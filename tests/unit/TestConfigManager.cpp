#include <catch2/catch_test_macros.hpp>

#include <string>

#include "Config/ConfigManager.h"
#include "Config/ConfigStore.h"
#include "TestTempDir.h"

using namespace liveai;

namespace {

class RecordingListener final : public IConfigListener
{
public:
    void onConfigChanged(const AppConfig& updated) override
    {
        ++calls;
        lastSampleRate = updated.audio.sampleRate;
        lastLanguage = updated.translation.outputLanguage;
    }

    int calls = 0;
    int lastSampleRate = 0;
    std::string lastLanguage;
};

} // namespace

TEST_CASE("ConfigManager: starts from the operator defaults", "[config][manager]")
{
    ConfigManager manager;

    CHECK(manager.current() == config::defaults());
    CHECK_FALSE(manager.hasStore());
    CHECK(manager.current().audio.sampleRate == 48000);
    CHECK(manager.current().translation.instructions == config::defaults().translation.instructions);
}

TEST_CASE("ConfigManager: without a store load and save are refused explicitly", "[config][manager]")
{
    ConfigManager manager;
    std::string message;

    CHECK_FALSE(manager.load(message));
    CHECK(message.find("no configuration store") != std::string::npos);

    message.clear();
    CHECK_FALSE(manager.save(message));
    CHECK_FALSE(message.empty());
}

TEST_CASE("ConfigManager: an invalid candidate is refused in full", "[config][manager]")
{
    ConfigManager manager;
    RecordingListener listener;
    manager.addListener(listener);
    std::string error;

    auto candidate = manager.current();
    candidate.audio.sampleRate = 12345;              // invalid
    candidate.audio.bufferFrames = 64;               // valid, but must not be applied either
    candidate.translation.outputLanguage = "fr";

    CHECK_FALSE(manager.update(candidate, error));
    CHECK(error.find("audio.sampleRate") != std::string::npos);

    // Nothing partial was applied and nobody was notified.
    CHECK(manager.current().audio.sampleRate == 48000);
    CHECK(manager.current().audio.bufferFrames == 480);
    CHECK(manager.current().translation.outputLanguage == "ru");
    CHECK(listener.calls == 0);
}

TEST_CASE("ConfigManager: a valid update applies and notifies listeners", "[config][manager]")
{
    ConfigManager manager;
    RecordingListener listener;
    manager.addListener(listener);
    std::string error;

    auto candidate = manager.current();
    candidate.audio.sampleRate = 96000;
    candidate.translation.outputLanguage = "de";

    REQUIRE(manager.update(candidate, error));
    CHECK(error.empty());
    CHECK(listener.calls == 1);
    CHECK(listener.lastSampleRate == 96000);
    CHECK(listener.lastLanguage == "de");
    CHECK(manager.current().audio.sampleRate == 96000);
}

TEST_CASE("ConfigManager: updateWith validates the merged result", "[config][manager]")
{
    ConfigManager manager;
    RecordingListener listener;
    manager.addListener(listener);
    std::string error;

    CHECK(manager.updateWith(error, [](AppConfig& cfg) { cfg.audio.bufferFrames = 512; }));
    CHECK(manager.current().audio.bufferFrames == 512);
    CHECK(listener.calls == 1);

    CHECK_FALSE(manager.updateWith(error, [](AppConfig& cfg) { cfg.translation.jitterBufferMs = 100000; }));
    CHECK(manager.current().translation.jitterBufferMs == 120);   // unchanged
    CHECK(listener.calls == 1);
}

TEST_CASE("ConfigManager: load reads the store and reports the outcome", "[config][manager]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");

    // Pre-write a file with one bad field and one good one.
    config::ConfigStore writer(file);
    livetest::writeFile(file, R"({"schemaVersion":1,"audio":{"sampleRate":12345,"bufferFrames":256}})");

    ConfigManager manager(writer);
    std::string note;
    REQUIRE(manager.load(note));

    CHECK(manager.current().audio.sampleRate == 48000);   // repaired
    CHECK(manager.current().audio.bufferFrames == 256);   // kept
    CHECK(manager.lastLoad().outcome == config::LoadOutcome::repaired);
    CHECK_FALSE(manager.lastLoad().problems.empty());
    CHECK(note.find("repaired") != std::string::npos);
}

TEST_CASE("ConfigManager: a missing file yields defaults without writing anything", "[config][manager]")
{
    livetest::TempDirectory temp;
    const auto file = temp.path() / "settings" / "config.json";

    config::ConfigStore store(file);
    ConfigManager manager(std::move(store));
    std::string note;
    REQUIRE(manager.load(note));

    CHECK(manager.current() == config::defaults());
    CHECK(manager.lastLoad().outcome == config::LoadOutcome::fileAbsent);
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK(note.find("file-absent") != std::string::npos);
}

TEST_CASE("ConfigManager: updateAndSave persists and survives a fresh manager", "[config][manager][roundtrip]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");

    std::string error;
    {
        config::ConfigStore store(file);
    ConfigManager manager(std::move(store));
        auto candidate = manager.current();
        candidate.audio.inputDeviceId = "Waves SoundGrid ASIO:1";
        candidate.translation.outputLanguage = "ru";
        REQUIRE(manager.updateAndSave(candidate, error));
    }

    config::ConfigStore reopenedStore(file);
    ConfigManager reopened(std::move(reopenedStore));
    std::string note;
    REQUIRE(reopened.load(note));

    CHECK(reopened.lastLoad().outcome == config::LoadOutcome::loaded);
    CHECK(reopened.current().audio.inputDeviceId == "Waves SoundGrid ASIO:1");
    CHECK(std::filesystem::exists(file));
}

TEST_CASE("ConfigManager: an invalid update never touches the saved file", "[config][manager][corrupt]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");

    config::ConfigStore store(file);
    ConfigManager manager(std::move(store));
    std::string error;
    REQUIRE(manager.save(error));                       // write the defaults first
    const auto savedContent = livetest::readFile(file);

    auto broken = manager.current();
    broken.diagnostics.logLevel = "everything";
    CHECK_FALSE(manager.updateAndSave(broken, error));

    CHECK(livetest::readFile(file) == savedContent);
    CHECK(manager.current().diagnostics.logLevel == "info");
}

TEST_CASE("ConfigManager: a store created for the default location is usable", "[config][manager]")
{
    const auto path = config::ConfigStore::defaultFile();

    CHECK(path.filename() == "config.json");
    CHECK(path.parent_path().filename() == std::string(config::ConfigStore::applicationDirectoryName()));
}

TEST_CASE("ConfigManager: resetForTests returns to a pristine manager", "[config][manager]")
{
    livetest::TempDirectory temp;
    config::ConfigStore store(temp.file("config.json"));
    ConfigManager manager(std::move(store));
    RecordingListener listener;
    manager.addListener(listener);

    std::string error;
    REQUIRE(manager.updateWith(error, [](AppConfig& cfg) { cfg.audio.sampleRate = 44100; }));
    CHECK(listener.calls == 1);

    manager.resetForTests();
    CHECK_FALSE(manager.hasStore());
    CHECK(manager.current() == config::defaults());

    // No notification after reset.
    CHECK(listener.calls == 1);
}
