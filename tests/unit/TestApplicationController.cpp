#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "App/ApplicationController.h"
#include "Audio/Null/NullAudioBackend.h"
#include "Config/ConfigSchema.h"
#include "Config/ConfigStore.h"
#include "NDI/Null/NullNdiOutput.h"
#include "TestTempDir.h"
#include "Translation/Null/NullTranslationBackend.h"
#include "Utils/Log.h"
#include "support/MockTranslationBackend.h"

using namespace liveai;
using liveai::audio::BackendState;
using liveai::audio::IAudioBackend;
using liveai::ndi::OutputState;
using liveai::translation::ITranslationBackend;
using liveai::translation::SessionState;
using liveai::test::MockTranslationBackend;

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

TEST_CASE("ApplicationController: a cleared fault can be retried into a running show",
          "[app][faults]")
{
    // The recovery path the windowed startup now depends on (fix 2026-10-06):
    // Main.cpp shows the window even when this start() fails, so the operator
    // can press Retry (OperatorContent::startPressed: clearFault + start) and
    // reach running without a relaunch. The UI half is pinned by TestUiModel's
    // retry case; this pins the controller half end to end.
    QuietLog quiet;
    ApplicationController controller;
    controller.setAudioBackend(std::make_unique<FailingAudioBackend>());

    // The busy-device startup: failure recorded as a fault, reason kept alive
    // for the status line (the log said it when it happened - nothing hidden).
    CHECK_FALSE(controller.start());
    CHECK(controller.state() == ApplicationState::faulted);
    CHECK(controller.status().detail.find("SoundGrid server not reachable") != std::string::npos);

    // Retry step one: the operator's clearFault. stopped, not haunted.
    controller.clearFault();
    CHECK(controller.state() == ApplicationState::stopped);
    CHECK(controller.faultReason().empty());
    CHECK(controller.status().detail.empty());

    // Retry step two: the external cause is gone (here: a device that opens).
    // The same controller object reaches running - no second state machine.
    controller.setAudioBackend(std::make_unique<liveai::audio::NullAudioBackend>());
    REQUIRE(controller.start());
    CHECK(controller.state() == ApplicationState::running);
    CHECK(controller.isRunning());
    CHECK(controller.status().detail.empty());   // the old failure must not haunt the new status

    controller.stop();
    CHECK(controller.state() == ApplicationState::stopped);
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

    // The composition root normally installs a factory that turns a device name into
    // a backend. Here it hands out a null backend, so the test exercises the rule
    // about device *selection* without needing a driver.
    controller.setAudioBackendFactory([](const audio::DeviceRequest&, std::string&)
                                      { return std::make_unique<audio::NullAudioBackend>(); });

    // spec "ASIO Device Selection": one ASIO device carries both directions, and a
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

TEST_CASE("ApplicationController: a selected device without a backend factory is an error",
          "[app][audio][spec]")
{
    QuietLog quiet;
    ApplicationController controller;   // no factory installed

    std::string error;
    auto cfg = controller.config().current();
    cfg.audio.inputDeviceId = "Waves SoundGrid ASIO";
    cfg.audio.outputDeviceId = "Waves SoundGrid ASIO";
    REQUIRE(controller.config().update(std::move(cfg), error));

    // SPEC: "ASIO device unavailable" - not "we quietly started the null device
    // instead". A running application with silent inputs is worse than a refused start.
    CHECK_FALSE(controller.start());
    CHECK(controller.state() == ApplicationState::faulted);
    CHECK(controller.status().detail.find("no audio backend factory") != std::string::npos);

    controller.stop();

    // And with no device selected at all the null backend is allowed, because that is
    // the documented state "nothing chosen yet", not a substitution.
    cfg = controller.config().current();
    cfg.audio.inputDeviceId.clear();
    cfg.audio.outputDeviceId.clear();
    REQUIRE(controller.config().update(std::move(cfg), error));
    CHECK(controller.start());
    controller.stop();
}

TEST_CASE("ApplicationController: the gains in settings are the gains in effect",
          "[app][audio][gain]")
{
    QuietLog quiet;
    ApplicationController controller;

    auto cfg = controller.config().current();
    cfg.audio.inputGainDb = -6.0f;
    cfg.audio.outputGainDb = 3.0f;

    std::string error;
    REQUIRE(controller.config().update(cfg, error));

    REQUIRE(controller.start());

    // spec "Configuration": the Settings area holds the gain, and what the operator reads
    // there is what the pipeline applies - not what the code happened to default to.
    CHECK(controller.engine().inputGainDb() == -6.0f);
    CHECK(controller.engine().outputGainDb() == 3.0f);

    // activate() lands the coefficient on those values rather than gliding into them, so
    // the very first block of the run is already at the configured level.
    CHECK(std::fabs(controller.engine().appliedInputGainDb() + 6.0f) < 0.01f);
    CHECK(std::fabs(controller.engine().appliedOutputGainDb() - 3.0f) < 0.01f);

    // Mute is live stage state: a start never arrives muted.
    CHECK_FALSE(controller.engine().inputMuted());
    CHECK_FALSE(controller.engine().outputMuted());

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

    // Async delivery: the pipeline hands its listener to a
    // dispatch worker, so the barrier replaces the old synchronous fire.
    controller.textPipeline().waitForDispatch();
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

TEST_CASE("ApplicationController: a pair outside the capability manifest is refused before the backend",
          "[app][languages]")
{
    // Design note: the operator cannot start what the product cannot
    // deliver. The gate reads the versioned manifest - the controller holds no
    // list of its own - and the backend is never opened. Audio keeps running:
    // a translation-side refusal is recorded, not fatal.
    QuietLog quiet;
    ApplicationController controller;

    auto cfg = controller.config().current();
    cfg.translation.inputLanguage = "en";
    cfg.translation.outputLanguage = "nl"; // a documented source, never a target
    std::string error;
    REQUIRE(controller.config().update(cfg, error));

    auto backend = std::make_unique<translation::NullTranslationBackend>();
    auto* backendRef = backend.get();
    controller.setTranslationBackend(std::move(backend));

    REQUIRE(controller.start()); // the app runs - translation failure is recoverable
    CHECK(backendRef->lastRequest().pair.output.empty()); // openSession was never reached
    CHECK(controller.sessionState() == translation::SessionState::closed);

    // And the reason is visible to the operator, not swallowed:
    // the recorded diagnostics error carries the manifest's own sentence.
    const auto snap = controller.diagnostics().snapshot();
    CHECK(snap.lastErrorSubsystem == "translation");
    CHECK(snap.lastErrorMessage.find("target language") != std::string::npos);
    CHECK(snap.lastErrorMessage.find("nl") != std::string::npos);

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

// ------------------------------------------------------------- stop lifecycle
//
// The venue report pinned three facts this section guards: Start/Stop must not
// freeze the calling thread, the unheard translated audio must die at the Stop
// (not play out at speed), and the fast Start->Stop->Start sequence must not
// mix sessions. The instrument is a backend whose open and close can be held
// on a leash, plus the controller's own lifecycle worker.

namespace {

/// A latch the test controls: armed doors make the backend method wait;
/// release opens every armed door at once.
struct StallGate
{
    std::mutex mutex;
    std::condition_variable cv;
    bool armed = false;
    bool open = true;

    void arm()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        armed = true;
        open = false;
    }

    void release()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            armed = false;
            open = true;
        }
        cv.notify_all();
    }

    void waitIfArmed()
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return open; });
    }
};

/// An ITranslationBackend whose openSession() and closeSession() can be held
/// inside the call, recording which close VERB the application used.
class StallingBackend final : public ITranslationBackend
{
public:
    std::string_view name() const noexcept override { return "Stalling"; }
    translation::SessionState state() const noexcept override { return state_.load(); }
    void setSink(translation::ITranslationSink& sink) noexcept override { sink_ = &sink; }

    bool openSession(const translation::SessionRequest&, std::string& error) override
    {
        ++opens;
        report(translation::SessionState::connecting);
        openGate.waitIfArmed();

        if (refuseOpen)
        {
            error = "stalling: open refused";
            return false;
        }

        state_ = translation::SessionState::connected;
        ++connectedOpens;
        report(translation::SessionState::connected);
        return true;
    }

    bool submitAudio(const float*, int, std::string& error) override
    {
        if (state_.load() != translation::SessionState::connected)
        {
            error = "stalling: not connected";
            return false;
        }
        ++submits;
        return true;
    }

    void closeSession() noexcept override
    {
        ++closes;
        closeGate.waitIfArmed();

        if (state_.exchange(translation::SessionState::closed) != translation::SessionState::closed)
            report(translation::SessionState::closed);
    }

    void abandonSession() noexcept override
    {
        ++abandons;
        closeSession();   // the production backends distinguish; this one records and reuses
    }

    StallGate openGate;
    StallGate closeGate;
    std::atomic<int> opens { 0 };
    std::atomic<int> connectedOpens { 0 };
    std::atomic<int> closes { 0 };
    std::atomic<int> abandons { 0 };
    std::atomic<int> submits { 0 };
    std::atomic<bool> refuseOpen { false };

private:
    void report(translation::SessionState next)
    {
        if (sink_ != nullptr)
            sink_->onSessionStateChanged(next);
    }

    translation::ITranslationSink* sink_ = nullptr;
    std::atomic<translation::SessionState> state_ { translation::SessionState::closed };
};

/// Drive the engine's callback from the test side (the null backend has no
/// thread of its own): returns once any block left the output non-silent.
int pumpBlocks(ApplicationController& controller, int count)
{
    const int frames = controller.engine().bufferFrames();
    std::vector<float> in(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> out(static_cast<std::size_t>(frames), 0.0f);
    float* inPtr[] = { in.data() };
    float* outPtr[] = { out.data() };

    int audible = 0;
    for (int block = 0; block < count; ++block)
    {
        std::fill(out.begin(), out.end(), 0.0f);
        controller.engine().processAudio(inPtr, outPtr, frames);
        for (const float sample : out)
        {
            if (sample != 0.0f)
            {
                ++audible;
                break;
            }
        }
    }
    return audible;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

} // namespace

TEST_CASE("ApplicationController: the queue drains in order - a Stop queued behind a stalled Start runs after it",
          "[app][lifecycle][stop]")
{
    QuietLog quiet;
    auto backend = std::make_unique<StallingBackend>();
    auto* stall = backend.get();
    ApplicationController controller;
    controller.setTranslationBackend(std::move(backend));

    stall->openGate.arm();
    controller.requestStart();          // the worker parks inside openSession()
    controller.requestStop();           // queued BEHIND the still-running start

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    stall->openGate.release();

    // The queue owes both actions, in order: the session connected and then
    // was closed - never the reverse, never dropped.
    REQUIRE(waitUntil([&] { return stall->connectedOpens.load() >= 1; }));
    REQUIRE(waitUntil([&] { return controller.state() == ApplicationState::stopped; }));
    CHECK(stall->abandons.load() >= 1);
    CHECK(stall->state() == SessionState::closed);   // (backend view agrees)

    controller.stop();   // idempotent, safe
}

TEST_CASE("ApplicationController: Stop silences the output while a slow session close is still running",
          "[app][lifecycle][stop]")
{
    QuietLog quiet;
    auto backend = std::make_unique<StallingBackend>();
    auto* stall = backend.get();
    ApplicationController controller;
    controller.setTranslationBackend(std::move(backend));

    REQUIRE(controller.start());

    // Half a second of translated audio is queued on the output.
    std::vector<float> audio(24000, 0.25f);
    controller.onTranslatedAudio(audio.data(), static_cast<int>(audio.size()), controller.engine().sampleRate());
    REQUIRE(controller.engine().outputJitter(0) != nullptr);
    REQUIRE(controller.engine().outputJitter(0)->available() > 0u);

    stall->closeGate.arm();
    const auto t0 = std::chrono::steady_clock::now();
    controller.requestStop();
    const auto returnedIn = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    CHECK(returnedIn.count() < 100);   // the button thread is not an accessory to the teardown

    // The worker parks inside the abandoning close. The discard Stop requests
    // is a real-time decision, executed by the callback - so the test drives
    // the callback and watches the moment it takes effect. The buffer must go
    // empty and the output must be silence while the close is still running.
    REQUIRE(waitUntil([&] { return controller.state() == ApplicationState::stopping; }));
    bool discarded = false;
    for (int attempt = 0; attempt < 50 && !discarded; ++attempt)
    {
        pumpBlocks(controller, 1);
        discarded = controller.engine().outputDiscardedFrames() > 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(discarded);
    CHECK(pumpBlocks(controller, 4) == 0);
    CHECK(controller.engine().outputJitter(0)->available() == 0u);

    // Audio delivered mid-close (a backend finishing a callback it was legally
    // inside when the gate flipped) is counted and dropped, never buffered:
    // the blocks after it stay silent too.
    controller.onTranslatedAudio(audio.data(), 4800, controller.engine().sampleRate());
    CHECK(pumpBlocks(controller, 2) == 0);
    CHECK(controller.diagnostics().snapshot().lateAudioFrames > 0u);

    stall->closeGate.release();
    REQUIRE(waitUntil([&] { return controller.state() == ApplicationState::stopped; }));

    // The stop used the ABANDONING verb (not the draining one) exactly once,
    // and the engine is down with its buffers.
    CHECK(stall->abandons.load() == 1);
    CHECK(stall->closes.load() == 1);
    CHECK(controller.engine().outputJitter(0) == nullptr);

    controller.requestStop();   // idempotent through the queue too
}

TEST_CASE("ApplicationController: requestStart returns while the handshake runs and the session lands ready",
          "[app][lifecycle][start]")
{
    QuietLog quiet;
    auto backend = std::make_unique<StallingBackend>();
    auto* stall = backend.get();
    ApplicationController controller;
    controller.setTranslationBackend(std::move(backend));

    stall->openGate.arm();
    const auto t0 = std::chrono::steady_clock::now();
    controller.requestStart();
    const auto returnedIn = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    CHECK(returnedIn.count() < 100);   // no handshake minutes frozen into the UI

    CHECK(waitUntil([&] { return stall->opens.load() >= 1; }));          // the open really started
    stall->openGate.release();

    REQUIRE(waitUntil([&] { return controller.state() == ApplicationState::running; }));
    CHECK(controller.sessionState() == SessionState::connected);
    CHECK(stall->connectedOpens.load() == 1);

    controller.stop();   // synchronous path for the cleanup (gates unarmed)
    CHECK(stall->abandons.load() == 1);
}

TEST_CASE("ApplicationController: a refused session start leaves a running, stoppable, restartable app",
          "[app][lifecycle][partial-start]")
{
    QuietLog quiet;
    auto backend = std::make_unique<StallingBackend>();
    auto* stall = backend.get();
    stall->refuseOpen = true;
    ApplicationController controller;
    controller.setTranslationBackend(std::move(backend));

    // The partial-init path: audio up, session refused. The application keeps
    // running with the failure said, the streamer never exists, and Stop is
    // still the same clean door.
    CHECK(controller.start());
    CHECK(controller.isRunning());
    CHECK_FALSE(controller.translationStreamer());
    CHECK(controller.sessionState() == SessionState::closed);
    CHECK(controller.status().detail.find("not started") != std::string::npos);

    controller.stop();
    CHECK(controller.state() == ApplicationState::stopped);
    CHECK(stall->abandons.load() == 1);   // even a session that never connected
                                          // leaves through the abandoning verb

    // Restartable: the same controller, now that the refusal is gone.
    stall->refuseOpen = false;
    CHECK(controller.start());
    CHECK(stall->connectedOpens.load() == 1);
    CHECK(controller.sessionState() == SessionState::connected);
    CHECK(controller.translationStreamer() != nullptr);

    // And a fast stop-on-start with audio still buffered: the Stop discards
    // instead of playing out (the burst this whole section pins).
    std::vector<float> audio(9600, 0.5f);
    controller.onTranslatedAudio(audio.data(), static_cast<int>(audio.size()), controller.engine().sampleRate());
    controller.stop();
    CHECK(stall->abandons.load() == 2);
    CHECK(controller.engine().outputJitter(0) == nullptr);
}

TEST_CASE("ApplicationController: Start->Stop->Start keeps two sessions apart",
          "[app][lifecycle][restart]")
{
    QuietLog quiet;
    auto backend = std::make_unique<StallingBackend>();
    auto* stall = backend.get();
    ApplicationController controller;
    controller.setTranslationBackend(std::move(backend));

    // Session one: connect, buffer audio, stop hard (gates unarmed - the
    // abandoning close is still the one under test).
    CHECK(controller.start());
    std::vector<float> first(4800, 0.25f);
    controller.onTranslatedAudio(first.data(), static_cast<int>(first.size()), controller.engine().sampleRate());
    CHECK(controller.engine().outputJitter(0)->available() > 0u);
    controller.stop();

    // Session two: the fresh pipeline begins from silence - old audio had to
    // die at the Stop, and nothing of session one survives to leak out.
    CHECK(controller.start());
    CHECK(stall->connectedOpens.load() == 2);
    CHECK(controller.engine().outputJitter(0)->available() == 0u);
    CHECK(pumpBlocks(controller, 3) == 0);   // silence, not the old 0.25 queue

    // New deliveries work normally on the new session: feed a full pre-roll
    // (the fresh pipeline primes like any new one - that is the requested
    // delay, not a leftover) and the audience hears session two, not session one.
    std::vector<float> second(24000, 0.5f);
    controller.onTranslatedAudio(second.data(), static_cast<int>(second.size()), controller.engine().sampleRate());
    CHECK(pumpBlocks(controller, 40) > 0);

    controller.stop();
}

// ---------------------------------------------------- Off mode & live language
//
// Task 2 of the venue follow-up: the target list gained a real OFF entry with
// its own output behavior (bypass / silence), and the target language became
// a live control. These tests walk the whole transition matrix with the
// deterministic support mock: what must be TRUE after each click - session
// counts, routing, what the audience hears, what was discarded - is asserted,
// and the caller returning quickly is timed, because "the GUI must not freeze
// on a socket" is the requirement these paths exist for. Every controller here
// reads its settings from a TempDirectory file: updateSettings SAVES, and a
// test must never write into the real operator's store.

namespace {

/// Point the controller's settings at a disposable file (defaults when the
/// path holds nothing yet).
void useTempSettings(ApplicationController& controller, livetest::TempDirectory& temp)
{
    std::string note;
    REQUIRE(controller.loadSettings(temp.file("config.json"), note));
}

} // namespace

namespace {

/// Pump one block through the engine and report what the audience got:
/// `maxAbs` of the output and whether any sample differs from {0, expected}.
void pumpOneBlock(ApplicationController& controller, float inputLevel, std::vector<float>& out)
{
    const int frames = controller.engine().bufferFrames();
    std::vector<float> in(static_cast<std::size_t>(frames), inputLevel);
    float* inPtr[] = { in.data() };
    float* outPtr[] = { out.data() };
    std::fill(out.begin(), out.end(), 0.0f);
    controller.engine().processAudio(inPtr, outPtr, frames);
}

} // namespace

TEST_CASE("ApplicationController: Off/bypass opens no session, submits nothing, plays the original",
          "[app][off][bypass]")
{
    QuietLog quiet;
    livetest::TempDirectory temp;
    auto backend = std::make_unique<MockTranslationBackend>();
    auto* mock = backend.get();
    ApplicationController controller;
    useTempSettings(controller, temp);
    controller.setTranslationBackend(std::move(backend));

    auto candidate = controller.config().current();
    candidate.translation.outputLanguage = "off";
    candidate.translation.offOutputBehavior = "bypass";
    std::string note;
    REQUIRE(controller.updateSettings(candidate, note));   // before Start: stored, not applied by a worker

    REQUIRE(controller.start());

    // "Do not send new translation requests" - at the source: no session was
    // opened to accept anything, and no streaming worker exists to feed one.
    CHECK(mock->sessionsOpened() == 0);
    CHECK(mock->acceptedSubmits() == 0);
    CHECK(controller.translationStreamer() == nullptr);
    CHECK(controller.sessionState() == SessionState::closed);
    CHECK(controller.engine().outputRouting() == OutputRouting::bypassOriginal);

    // The original reaches the audience inside the same block it was captured:
    // default gains (0 dB) leave the samples exactly what the device handed in.
    const int frames = controller.engine().bufferFrames();
    std::vector<float> out(static_cast<std::size_t>(frames), -1.0f);
    pumpOneBlock(controller, 0.4f, out);
    for (const float sample : out)
        CHECK(sample == 0.4f);

    // And the original never mixes with an old translation: park audio in the
    // jitter buffer (a stray delivery, the shape an off-path bug would leave)
    // - bypass must not read it, not now and not later.
    std::vector<float> stale(static_cast<std::size_t>(24000), 0.6f);
    REQUIRE(controller.engine().outputJitter(0)->write(stale.data(), stale.size()) == stale.size());
    pumpOneBlock(controller, 0.4f, out);
    for (const float sample : out)
        CHECK(sample == 0.4f);                                        // still only the room
    CHECK(controller.engine().outputJitter(0)->available() == stale.size());  // never consumed

    controller.stop();
    CHECK(controller.state() == ApplicationState::stopped);
}

TEST_CASE("ApplicationController: Off/silence stops the output and never counts as an underrun alarm",
          "[app][off][silence]")
{
    QuietLog quiet;
    livetest::TempDirectory temp;
    auto backend = std::make_unique<MockTranslationBackend>();
    auto* mock = backend.get();
    ApplicationController controller;
    useTempSettings(controller, temp);
    controller.setTranslationBackend(std::move(backend));

    auto candidate = controller.config().current();
    candidate.translation.outputLanguage = "off";
    candidate.translation.offOutputBehavior = "silence";
    std::string note;
    REQUIRE(controller.updateSettings(candidate, note));
    REQUIRE(controller.start());

    CHECK(mock->sessionsOpened() == 0);
    CHECK(controller.engine().outputRouting() == OutputRouting::silence);

    const int frames = controller.engine().bufferFrames();
    std::vector<float> out(static_cast<std::size_t>(frames), -1.0f);

    // Even a stray queue in the jitter buffer cannot leak: the routing does
    // not read it, and the deliberate silence is NOT counted in
    // outputSilenceSamples - that row answers "the buffer came up short",
    // and a stopped output was always going to be short on purpose.
    std::vector<float> stale(static_cast<std::size_t>(24000), 0.25f);
    REQUIRE(controller.engine().outputJitter(0)->write(stale.data(), stale.size()) == stale.size());

    const std::uint64_t before = controller.engine().outputSilenceSamples();
    for (int block = 0; block < 5; ++block)
    {
        pumpOneBlock(controller, 0.4f, out);
        for (const float sample : out)
            CHECK(sample == 0.0f);
    }
    CHECK(controller.engine().outputSilenceSamples() == before);

    controller.stop();
}

TEST_CASE("ApplicationController: entering Off discards the unheard translation; leaving it reopens with the CURRENT target",
          "[app][off][transition]")
{
    QuietLog quiet;
    livetest::TempDirectory temp;
    auto backend = std::make_unique<MockTranslationBackend>();
    auto* mock = backend.get();
    ApplicationController controller;
    useTempSettings(controller, temp);
    controller.setTranslationBackend(std::move(backend));

    REQUIRE(controller.start());
    REQUIRE(mock->sessionsOpened() == 1);
    REQUIRE(mock->openRequests()[0].pair.output == "ru");   // the default candidate

    // Half a second of translated audio is queued unheard.
    std::vector<float> oldLanguage(static_cast<std::size_t>(24000), 0.25f);
    controller.onTranslatedAudio(oldLanguage.data(), 24000, controller.engine().sampleRate());
    REQUIRE(controller.engine().outputJitter(0)->available() > 0u);

    // Step into Off/bypass. The call must return while the session teardown
    // still runs somewhere else - this is the GUI thread of the real app.
    auto candidate = controller.config().current();
    candidate.translation.outputLanguage = "off";
    std::string note;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(controller.updateSettings(candidate, note));
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
          < 100);

    REQUIRE(waitUntil([&] { return mock->sessionsAbandoned() >= 1; }));
    REQUIRE(waitUntil([&] { return controller.translationStreamer() == nullptr; }));
    CHECK(controller.engine().outputRouting() == OutputRouting::bypassOriginal);
    CHECK(controller.sessionState() == SessionState::closed);

    // The queue died at the switch: what the audience hears from now is the
    // room (0.4), and NEVER the old 0.25 tail.
    const int frames = controller.engine().bufferFrames();
    std::vector<float> out(static_cast<std::size_t>(frames), -1.0f);
    bool discarded = false;
    for (int block = 0; block < 10 && !discarded; ++block)
    {
        pumpOneBlock(controller, 0.4f, out);
        discarded = controller.engine().outputDiscardedFrames() > 0;
    }
    CHECK(discarded);
    for (int block = 0; block < 3; ++block)
    {
        pumpOneBlock(controller, 0.4f, out);
        for (const float sample : out)
            CHECK(sample == 0.4f);
    }

    // Leave Off with a DIFFERENT target in the same click: the reopened
    // session must carry the current language, not the one from before Off.
    candidate.translation.outputLanguage = "de";
    REQUIRE(controller.updateSettings(candidate, note));
    REQUIRE(waitUntil([&] { return mock->sessionsOpened() >= 2; }));
    REQUIRE(mock->openRequests().size() >= 2);
    CHECK(mock->openRequests()[1].pair.output == "de");
    CHECK(controller.engine().outputRouting() == OutputRouting::translation);
    CHECK(controller.sessionState() == SessionState::connected);
    CHECK(controller.translationStreamer() != nullptr);

    // The fresh session starts from silence (its queue was never filled), and
    // deliveries work normally. First burn a few blocks: the transition's
    // discards are consumed BEFORE the first real delivery arrives - which is
    // exactly the real order, since the session can only deliver after it
    // opened, and the discard was requested before that.
    for (int block = 0; block < 3; ++block)
        pumpOneBlock(controller, 0.0f, out);

    std::vector<float> freshDelivery(static_cast<std::size_t>(24000), 0.5f);
    controller.onTranslatedAudio(freshDelivery.data(), 24000, controller.engine().sampleRate());
    int audibleBlocks = 0;
    for (int block = 0; block < 40; ++block)
    {
        pumpOneBlock(controller, 0.0f, out);
        for (const float sample : out)
        {
            if (sample == 0.5f)
            {
                ++audibleBlocks;
                break;
            }
        }
    }
    CHECK(audibleBlocks > 0);

    controller.stop();
}

TEST_CASE("ApplicationController: live target change rotates the session and buries the old language's queue",
          "[app][translation][live]")
{
    QuietLog quiet;
    livetest::TempDirectory temp;
    auto backend = std::make_unique<MockTranslationBackend>();
    auto* mock = backend.get();
    ApplicationController controller;
    useTempSettings(controller, temp);
    controller.setTranslationBackend(std::move(backend));

    REQUIRE(controller.start());
    const auto firstRequest = mock->openRequests()[0];
    CHECK(firstRequest.pair.output == "ru");

    std::vector<float> queued(static_cast<std::size_t>(24000), 0.25f);
    controller.onTranslatedAudio(queued.data(), 24000, controller.engine().sampleRate());
    REQUIRE(controller.engine().outputJitter(0)->available() > 0u);

    auto candidate = controller.config().current();
    candidate.translation.outputLanguage = "de";
    std::string note;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(controller.updateSettings(candidate, note));
    const auto returnedIn = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(returnedIn < 100);   // the rotation was dispatched, not endured

    REQUIRE(waitUntil([&] { return mock->sessionsOpened() >= 2; }));
    CHECK(mock->sessionsAbandoned() >= 1);              // the old one died by ABANDON
    CHECK(mock->openRequests()[1].pair.output == "de"); // the new one carries the new target
    CHECK(controller.sessionState() == SessionState::connected);
    CHECK(controller.engine().outputRouting() == OutputRouting::translation);

    // Nothing of the previous language survives to the audience: the first
    // pumped block eats the discard, and no block ever shows 0.25.
    const int frames = controller.engine().bufferFrames();
    std::vector<float> out(static_cast<std::size_t>(frames), -1.0f);
    for (int block = 0; block < 20; ++block)
    {
        pumpOneBlock(controller, 0.0f, out);
        for (const float sample : out)
            CHECK(sample != 0.25f);
    }
    CHECK(controller.engine().outputDiscardedFrames() > 0u);

    controller.stop();
}

TEST_CASE("ApplicationController: fast Off/behavior/target clicks land on the last decision, never hang",
          "[app][off][matrix]")
{
    QuietLog quiet;
    livetest::TempDirectory temp;
    auto backend = std::make_unique<MockTranslationBackend>();
    auto* mock = backend.get();
    ApplicationController controller;
    useTempSettings(controller, temp);
    controller.setTranslationBackend(std::move(backend));

    REQUIRE(controller.start());

    struct Step { const char* target; const char* behavior; };
    const std::vector<Step> matrix = {
        { "off", "bypass" },   // Translation -> Off/bypass
        { "off", "silence" },  // Bypass -> Stop output
        { "de",  "silence" },  // Stop output -> Translation (new target)
        { "off", "silence" },  // Translation -> Off/silence
        { "off", "bypass" },   // Stop output -> Bypass
        { "ru",  "bypass" },   // Bypass -> Translation (back to the old default)
    };

    long long worstUpdateMs = 0;
    for (const auto& step : matrix)
    {
        auto candidate = controller.config().current();
        candidate.translation.outputLanguage = step.target;
        candidate.translation.offOutputBehavior = step.behavior;
        std::string note;
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE(controller.updateSettings(candidate, note));
        worstUpdateMs = std::max<long long>(
            worstUpdateMs,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());

        // The routing (a flag the callback reads) follows immediately...
        const auto want = isTranslationOff(step.target)
                              ? (std::string(step.behavior) == "silence" ? OutputRouting::silence
                                                                          : OutputRouting::bypassOriginal)
                              : OutputRouting::translation;
        CHECK(controller.engine().outputRouting() == want);

        // ...and the session side lands on the same truth via the worker.
        if (isTranslationOff(step.target))
        {
            REQUIRE(waitUntil([&] { return controller.sessionState() == SessionState::closed; }));
            REQUIRE(waitUntil([&] { return controller.translationStreamer() == nullptr; }));
        }
        else
        {
            REQUIRE(waitUntil([&] { return controller.sessionState() == SessionState::connected; }));
            REQUIRE(waitUntil([&] { return controller.translationStreamer() != nullptr; }));
        }
    }

    // Every click returned well under a blocking window; the show is still
    // exactly what the LAST click said.
    CHECK(worstUpdateMs < 100);
    CHECK(controller.isRunning());
    CHECK(controller.engine().outputRouting() == OutputRouting::translation);

    // And the session ledger is consistent: each landing on a language opened
    // its own session (three in this matrix: Start, the Off->de step, the
    // Off->ru step), and by the final Stop nothing is left open.
    const int opens = mock->sessionsOpened();
    CHECK(opens == 3);
    controller.stop();
    CHECK(mock->state() == SessionState::closed);
    CHECK(mock->sessionsAbandoned() >= opens);   // every crossing left through the
                                                 // abandoning verb, no stragglers
    CHECK(controller.sessionState() == SessionState::closed);
}
