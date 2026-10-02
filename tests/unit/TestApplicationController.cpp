#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "App/ApplicationController.h"
#include "Audio/Null/NullAudioBackend.h"
#include "Config/ConfigSchema.h"
#include "Config/ConfigStore.h"
#include "NDI/Null/NullNdiOutput.h"
#include "TestTempDir.h"
#include "Translation/Null/NullTranslationBackend.h"
#include "Utils/Log.h"

using namespace liveai;
using liveai::audio::BackendState;
using liveai::audio::IAudioBackend;
using liveai::ndi::OutputState;
using liveai::translation::ITranslationBackend;
using liveai::translation::SessionState;

namespace {

struct QuietLog
{
    QuietLog()
    {
        LogConfig cfg;
        cfg.level = LogLevel::off;
        cfg.writeConsole = false;
        log::configure(cfg);
    }
    ~QuietLog() { log::resetForTests(); }
};

/// Audio backend whose start() fails, standing in for a busy/absent device.
class FailingAudioBackend final : public IAudioBackend
{
public:
    std::string_view name() const noexcept override { return "Failing"; }
    BackendState state() const noexcept override { return state_; }

    bool open(audio::IAudioProcessor&, const audio::DeviceRequest&, std::string& error) override
    {
        state_ = BackendState::opened;
        error.clear();
        return true;
    }

    bool start(std::string& error) override
    {
        error = "SoundGrid server not reachable";
        state_ = BackendState::faulted;
        return false;
    }

    bool stop(std::string&) override { state_ = BackendState::closed; return true; }
    void close() noexcept override { state_ = BackendState::closed; }
    audio::DeviceCapabilities capabilities() const noexcept override { return {}; }

private:
    BackendState state_ = BackendState::closed;
};

/// Translation backend that cannot open a session (stands in for auth/network failure).
class FailingTranslationBackend final : public ITranslationBackend
{
public:
    std::string_view name() const noexcept override { return "Failing"; }
    SessionState state() const noexcept override { return SessionState::faulted; }
    void setSink(translation::ITranslationSink&) noexcept override {}

    bool openSession(const translation::SessionRequest&, std::string& error) override
    {
        error = "websocket rejected (401)";
        return false;
    }

    bool submitAudio(const float*, int, std::string& error) override
    {
        error = "no session";
        return false;
    }

    void closeSession() noexcept override {}
};

/// NDI output whose start() fails.
class FailingNdiOutput final : public ndi::INdiOutput
{
public:
    std::string_view name() const noexcept override { return "Failing"; }
    OutputState state() const noexcept override { return OutputState::faulted; }

    bool start(std::string_view, std::string& error) override
    {
        error = "no NDI sender available";
        return false;
    }

    void stop() noexcept override {}

    bool publish(const ndi::SubtitleFrame&, std::string& error) override
    {
        error = "not started";
        return false;
    }
};

} // namespace

TEST_CASE("ApplicationController: state names are stable", "[app]")
{
    CHECK(nameOf(ApplicationState::stopped) == "stopped");
    CHECK(nameOf(ApplicationState::running) == "running");
    CHECK(nameOf(ApplicationState::faulted) == "faulted");
}

TEST_CASE("ApplicationController: start/stop with the null subsystems", "[app]")
{
    QuietLog quiet;
    ApplicationController controller;

    REQUIRE(controller.state() == ApplicationState::stopped);
    REQUIRE(controller.start());
    CHECK(controller.isRunning());
    CHECK(controller.engine().backend() != nullptr);

    auto status = controller.status();
    status.detail.clear();   // detail is not part of the expectation
    CHECK(status.application == ApplicationState::running);
    CHECK(status.audio == BackendState::running);
    CHECK(status.session == SessionState::connected);
    CHECK(status.ndi == OutputState::disabled);       // NDI is off by default
    CHECK(status.audioBackendName == "Null");

    CHECK_FALSE(controller.start());                  // already running
    CHECK(controller.state() == ApplicationState::running);

    controller.stop();
    CHECK(controller.state() == ApplicationState::stopped);
    CHECK(controller.engine().backend() == nullptr);
    CHECK(controller.status().session == SessionState::closed);
}

TEST_CASE("ApplicationController: audio failure faults the application", "[app][faults]")
{
    QuietLog quiet;
    ApplicationController controller;
    controller.setAudioBackend(std::make_unique<FailingAudioBackend>());

    std::string unused;
    CHECK_FALSE(controller.start());
    CHECK(controller.state() == ApplicationState::faulted);

    const auto status = controller.status();
    // A device whose start() failed is closed again by the engine: the operator
    // must never be left with a half-opened SoundGrid device. The reason is kept
    // in detail/diagnostics, not in the device state.
    CHECK(status.audio == BackendState::closed);
    CHECK(controller.engine().backend() == nullptr);
    CHECK(status.detail.find("SoundGrid server not reachable") != std::string::npos);
    CHECK(controller.diagnostics().snapshot().lastErrorSubsystem == "audio");

    // A faulted application refuses to start until it is stopped (recovered).
    CHECK_FALSE(controller.start());
    controller.stop();
    CHECK(controller.state() == ApplicationState::stopped);
    CHECK(controller.faultReason().empty());
}

TEST_CASE("ApplicationController: translation failure does not stop audio", "[app][faults]")
{
    QuietLog quiet;
    ApplicationController controller;
    controller.setTranslationBackend(std::make_unique<FailingTranslationBackend>());

    REQUIRE(controller.start());                      // audio keeps running
    CHECK(controller.isRunning());
    CHECK(controller.engine().backend() != nullptr);
    CHECK(controller.status().audio == BackendState::running);
    CHECK(controller.status().session == SessionState::faulted);

    const auto snapshot = controller.diagnostics().snapshot();
    CHECK(snapshot.lastErrorSubsystem == "translation");
    CHECK(snapshot.lastErrorMessage.find("401") != std::string::npos);

    controller.stop();
}

TEST_CASE("ApplicationController: NDI failure does not stop audio", "[app][faults]")
{
    QuietLog quiet;
    ApplicationController controller;

    auto cfg = controller.config().current();
    cfg.ndi.enabled = true;
    std::string error;
    REQUIRE(controller.config().update(cfg, error));

    controller.setNdiOutput(std::make_unique<FailingNdiOutput>());

    REQUIRE(controller.start());
    CHECK(controller.isRunning());
    CHECK(controller.status().audio == BackendState::running);
    CHECK(controller.diagnostics().snapshot().ndiErrors == 0);   // start failure, not a publish error
    CHECK(controller.diagnostics().snapshot().lastErrorSubsystem == "ndi");

    controller.stop();
}

TEST_CASE("ApplicationController: two different ASIO devices in settings are refused", "[app][audio][spec]")
{
    QuietLog quiet;
    ApplicationController controller;

    // SPEC "ASIO Device Selection": one ASIO device carries both directions, and a
    // configured device must never be silently replaced.
    std::string error;
    auto cfg = controller.config().current();
    cfg.audio.inputDeviceId = "Waves SoundGrid ASIO";
    cfg.audio.outputDeviceId = "Some Other Device";
    REQUIRE(controller.config().update(std::move(cfg), error));

    CHECK_FALSE(controller.start());
    CHECK(controller.state() == ApplicationState::faulted);
    CHECK(controller.status().detail.find("single device for input and output") != std::string::npos);

    controller.stop();

    // Naming the same device on both sides is fine.
    cfg = controller.config().current();
    cfg.audio.outputDeviceId = "Waves SoundGrid ASIO";
    REQUIRE(controller.config().update(std::move(cfg), error));
    CHECK(controller.start());
    controller.stop();
}

TEST_CASE("ApplicationController: text events reach NDI and diagnostics", "[app][ndi]")
{
    QuietLog quiet;
    ApplicationController controller;

    auto cfg = controller.config().current();
    cfg.ndi.enabled = true;
    cfg.ndi.streamName = "LingoFlow EN->RU";
    std::string error;
    REQUIRE(controller.config().update(cfg, error));

    auto ndi = std::make_unique<ndi::NullNdiOutput>();
    auto* ndiRef = ndi.get();
    controller.setNdiOutput(std::move(ndi));

    REQUIRE(controller.start());

    controller.onPartialText("good evening");
    controller.onFinalText("good evening, welcome");

    CHECK(ndiRef->publishedFrames() == 2);
    CHECK(ndiRef->state() == OutputState::publishing);

    const auto snapshot = controller.diagnostics().snapshot();
    CHECK(snapshot.partialTextEvents == 1);
    CHECK(snapshot.finalTextEvents == 1);

    controller.stop();
    CHECK(ndiRef->state() == OutputState::disabled);
}

TEST_CASE("ApplicationController: text is dropped while NDI is off, without errors", "[app][ndi]")
{
    QuietLog quiet;
    ApplicationController controller;

    auto ndi = std::make_unique<ndi::NullNdiOutput>();
    auto* ndiRef = ndi.get();
    controller.setNdiOutput(std::move(ndi));

    REQUIRE(controller.start());          // NDI stays disabled (default config)
    controller.onFinalText("hello");

    CHECK(ndiRef->publishedFrames() == 0);
    CHECK(controller.diagnostics().snapshot().ndiErrors == 0);
    CHECK(controller.diagnostics().snapshot().finalTextEvents == 1);

    controller.stop();
}

TEST_CASE("ApplicationController: session uses the configured languages and instructions", "[app]")
{
    QuietLog quiet;
    ApplicationController controller;

    auto cfg = controller.config().current();
    cfg.translation.inputLanguage = "de";
    cfg.translation.outputLanguage = "ja";
    cfg.translation.instructions = "custom instructions";
    std::string error;
    REQUIRE(controller.config().update(cfg, error));

    auto backend = std::make_unique<translation::NullTranslationBackend>();
    auto* backendRef = backend.get();
    controller.setTranslationBackend(std::move(backend));

    REQUIRE(controller.start());

    CHECK(backendRef->lastRequest().pair.input == "de");
    CHECK(backendRef->lastRequest().pair.output == "ja");
    CHECK(backendRef->lastRequest().instructions == "custom instructions");

    controller.stop();
}

TEST_CASE("ApplicationController: audio engine counts blocks driven by the backend", "[app][audio]")
{
    QuietLog quiet;
    ApplicationController controller;

    auto backend = std::make_unique<audio::NullAudioBackend>();
    auto* backendRef = backend.get();
    controller.setAudioBackend(std::move(backend));

    REQUIRE(controller.start());

    CHECK(backendRef->renderOneBlock());
    CHECK(backendRef->renderOneBlock());
    CHECK_FALSE(backendRef->renderOneBlock() == false);   // still running

    CHECK(controller.engine().blockCount() == 3);
    CHECK(controller.diagnostics().snapshot().audioBlocks == 3);

    controller.stop();
    CHECK_FALSE(backendRef->renderOneBlock());            // backend closed after stop
}

TEST_CASE("ApplicationController: start after stop works again", "[app]")
{
    QuietLog quiet;
    ApplicationController controller;

    REQUIRE(controller.start());
    controller.stop();
    CHECK(controller.start());
    CHECK(controller.isRunning());
    controller.stop();
    CHECK(controller.state() == ApplicationState::stopped);
}

TEST_CASE("ApplicationController: settings from the pre-rename folder migrate once",
          "[app][config][rename]")
{
    QuietLog quiet;
    livetest::TempDirectory temp;

    const auto legacy = temp.file("old/config.json");
    const auto current = temp.file("new/config.json");

    // A real installation's file, produced by the store itself, not hand-written.
    {
        auto stored = config::defaults();
        stored.audio.sampleRate = 96000;
        std::string error;
        REQUIRE(config::ConfigStore(legacy).save(stored, error));
    }

    ApplicationController controller;
    std::string note;
    REQUIRE(controller.loadSettings(legacy, note, current));

    CHECK(controller.config().current().audio.sampleRate == 96000);
    CHECK(std::filesystem::exists(current));   // written into the new location
    CHECK(std::filesystem::exists(legacy));    // the old file is never deleted

    // And the store is re-pointed: a later save must not write to the old path again.
    const auto oldContent = livetest::readFile(legacy);

    auto cfg = controller.config().current();
    cfg.audio.bufferFrames = 512;
    std::string error;
    REQUIRE(controller.config().update(std::move(cfg), error));
    REQUIRE(controller.saveSettings(error));

    CHECK(livetest::readFile(legacy) == oldContent);
    CHECK(livetest::readFile(current).find("96000") != std::string::npos);
    CHECK(livetest::readFile(current).find("512") != std::string::npos);
}
