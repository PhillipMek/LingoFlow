#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include "Config/ConfigSchema.h"
#include "Config/ConfigStore.h"
#include "TestTempDir.h"
#include "Utils/Log.h"

using namespace liveai;

namespace {

AppConfig makeConfig()
{
    AppConfig cfg = config::defaults();
    cfg.audio.inputDeviceId = "SG-A16-Rack:1";
    cfg.audio.outputDeviceId = "SG-A16-Rack:2";
    cfg.audio.sampleRate = 48000;
    cfg.audio.bufferFrames = 960;
    cfg.audio.inputChannel = 3;
    cfg.audio.outputChannel = 4;
    cfg.audio.inputGainDb = -2.5f;
    cfg.audio.outputGainDb = 1.5f;
    cfg.translation.inputLanguage = "en";
    cfg.translation.outputLanguage = "ru";
    cfg.translation.instructions = "custom instructions";
    cfg.translation.modelHint = "capability-model";
    cfg.translation.jitterBufferMs = 200;
    cfg.ndi.enabled = true;
    cfg.ndi.streamName = "LiveAI EN->RU";
    cfg.diagnostics.logLevel = "warning";
    cfg.diagnostics.writeLogFile = false;
    return cfg;
}

} // namespace

TEST_CASE("ConfigSchema: defaults validate cleanly", "[config][schema]")
{
    const auto cfg = config::defaults();
    CHECK(config::validate(cfg).empty());
    CHECK(cfg.schemaVersion == kConfigSchemaVersion);
    CHECK(cfg.audio.sampleRate == 48000);
    CHECK(cfg.audio.bufferFrames == 480);
    CHECK(cfg.translation.inputLanguage == "en");
    CHECK(cfg.translation.outputLanguage == "ru");
    CHECK_FALSE(cfg.ndi.enabled);
    CHECK(cfg.diagnostics.logLevel == "info");
}

TEST_CASE("ConfigSchema: documented defaults are actually the defaults", "[config][schema][defaults]")
{
    // docs/device-defaults.md fixes this table. Values here are settings, not
    // measurements: no SoundGrid server exists on this machine, so nothing in this
    // test may be read as verified hardware behaviour.
    const auto cfg = config::defaults();

    CHECK(cfg.audio.inputDeviceId.empty());        // "not selected yet"
    CHECK(cfg.audio.outputDeviceId.empty());
    CHECK(cfg.audio.sampleRate == 48000);
    CHECK(cfg.audio.bufferFrames == 480);
    CHECK(cfg.audio.inputChannel == 1);
    CHECK(cfg.audio.outputChannel == 1);
    CHECK(cfg.audio.inputGainDb == 0.0f);
    CHECK(cfg.audio.outputGainDb == 0.0f);

    CHECK(cfg.translation.inputLanguage == "en");
    CHECK(cfg.translation.outputLanguage == "ru");
    CHECK(cfg.translation.modelHint.empty());      // backend default, not invented here
    CHECK(cfg.translation.jitterBufferMs == 120);

    CHECK_FALSE(cfg.ndi.enabled);
    CHECK(cfg.diagnostics.logLevel == "info");
    CHECK(cfg.diagnostics.writeLogFile);
    CHECK(cfg.schemaVersion == kConfigSchemaVersion);
}

TEST_CASE("ConfigSchema: JSON round-trip preserves every field", "[config][schema][roundtrip]")
{
    const auto original = makeConfig();
    const std::string text = config::toJsonText(original);

    AppConfig restored;
    ConfigProblems problems;
    std::string error;
    REQUIRE(config::fromJsonText(text, restored, problems, error));
    CHECK(problems.empty());
    CHECK(error.empty());

    CHECK(restored.schemaVersion == original.schemaVersion);
    CHECK(restored.audio.inputDeviceId == original.audio.inputDeviceId);
    CHECK(restored.audio.outputDeviceId == original.audio.outputDeviceId);
    CHECK(restored.audio.sampleRate == original.audio.sampleRate);
    CHECK(restored.audio.bufferFrames == original.audio.bufferFrames);
    CHECK(restored.audio.inputChannel == original.audio.inputChannel);
    CHECK(restored.audio.outputChannel == original.audio.outputChannel);
    CHECK(restored.audio.inputGainDb == original.audio.inputGainDb);
    CHECK(restored.audio.outputGainDb == original.audio.outputGainDb);
    CHECK(restored.translation.inputLanguage == original.translation.inputLanguage);
    CHECK(restored.translation.outputLanguage == original.translation.outputLanguage);
    CHECK(restored.translation.instructions == original.translation.instructions);
    CHECK(restored.translation.modelHint == original.translation.modelHint);
    CHECK(restored.translation.jitterBufferMs == original.translation.jitterBufferMs);
    CHECK(restored.ndi.enabled == original.ndi.enabled);
    CHECK(restored.ndi.streamName == original.ndi.streamName);
    CHECK(restored.diagnostics.logLevel == original.diagnostics.logLevel);
    CHECK(restored.diagnostics.writeLogFile == original.diagnostics.writeLogFile);
}

TEST_CASE("ConfigSchema: absent sections and fields keep the defaults", "[config][schema]")
{
    AppConfig restored;
    ConfigProblems problems;
    std::string error;

    REQUIRE(config::fromJsonText("{}", restored, problems, error));
    CHECK(problems.empty());
    CHECK(restored.audio.sampleRate == 48000);
    CHECK(restored.translation.outputLanguage == "ru");

    // Partial document: only what is present is overwritten.
    REQUIRE(config::fromJsonText(R"({"audio":{"sampleRate":96000}})", restored, problems, error));
    CHECK(restored.audio.sampleRate == 96000);
    CHECK(restored.audio.bufferFrames == 480);
    CHECK(restored.translation.instructions == config::defaults().translation.instructions);
}

TEST_CASE("ConfigSchema: unknown fields are reported and dropped, not stored", "[config][schema]")
{
    AppConfig restored;
    ConfigProblems problems;
    std::string error;

    const std::string text = R"({
        "schemaVersion": 1,
        "audio": { "sampleRate": 48000, "mixerPreset": "broadcast" },
        "mystery": { "a": 1 }
    })";

    REQUIRE(config::fromJsonText(text, restored, problems, error));

    bool reportedUnknownAudioField = false;
    bool reportedUnknownSection = false;
    for (const auto& problem : problems)
    {
        if (problem.field == "audio.mixerPreset")
            reportedUnknownAudioField = true;
        if (problem.field == "mystery")
            reportedUnknownSection = true;
    }

    CHECK(reportedUnknownAudioField);
    CHECK(reportedUnknownSection);

    // Re-serializing must not carry the unknown keys back out.
    const auto roundTrip = config::toJsonText(restored);
    CHECK(roundTrip.find("mixerPreset") == std::string::npos);
    CHECK(roundTrip.find("mystery") == std::string::npos);
}

TEST_CASE("ConfigSchema: an invalid value is repaired field by field and reported", "[config][schema]")
{
    AppConfig restored;
    ConfigProblems problems;
    std::string error;

    const std::string text = R"({
        "schemaVersion": 1,
        "audio": { "sampleRate": 12345, "bufferFrames": 480, "inputGainDb": "loud" },
        "translation": { "inputLanguage": "en", "outputLanguage": "en", "jitterBufferMs": 99999 },
        "diagnostics": { "logLevel": "verbose" }
    })";

    REQUIRE(config::fromJsonText(text, restored, problems, error));

    // Every offending field falls back to its default, the rest is kept.
    CHECK(restored.audio.sampleRate == 48000);
    CHECK(restored.audio.bufferFrames == 480);
    CHECK(restored.audio.inputGainDb == 0.0f);
    CHECK(restored.translation.inputLanguage != restored.translation.outputLanguage);
    CHECK(restored.translation.jitterBufferMs == 120);
    CHECK(restored.diagnostics.logLevel == "info");

    // The language clash is repaired consistently: both end up at their defaults.
    CHECK(restored.translation.inputLanguage == "en");
    CHECK(restored.translation.outputLanguage == "ru");

    // And each repair is visible to the caller.
    std::string allProblems;
    for (const auto& problem : problems)
        allProblems += problem.field + ";";
    INFO(allProblems);

    CHECK(allProblems.find("audio.sampleRate") != std::string::npos);
    CHECK(allProblems.find("audio.inputGainDb") != std::string::npos);
    CHECK(allProblems.find("translation.jitterBufferMs") != std::string::npos);
    CHECK(allProblems.find("diagnostics.logLevel") != std::string::npos);

    // The repaired result is itself valid.
    CHECK(config::validate(restored).empty());
}

TEST_CASE("ConfigSchema: non-JSON text is rejected", "[config][schema][corrupt]")
{
    AppConfig restored = makeConfig();
    ConfigProblems problems;
    std::string error;

    CHECK_FALSE(config::fromJsonText("this is not json at all", restored, problems, error));
    CHECK_FALSE(error.empty());

    CHECK_FALSE(config::fromJsonText("", restored, problems, error));
    CHECK_FALSE(config::fromJsonText("{", restored, problems, error));
    CHECK_FALSE(config::fromJsonText("[1,2,3]", restored, problems, error));

    // A section with the wrong type is recoverable: defaults are kept for it and
    // the problem is reported, the rest of the file still loads.
    problems.clear();
    restored = makeConfig();
    REQUIRE(config::fromJsonText(R"({"audio": 5, "translation": {"inputLanguage": "de"}})", restored, problems, error));
    CHECK(restored.audio.sampleRate == 48000);
    CHECK(restored.translation.inputLanguage == "de");
    bool reportedBadSection = false;
    for (const auto& problem : problems)
        reportedBadSection = reportedBadSection || problem.field == "audio";
    CHECK(reportedBadSection);
}

TEST_CASE("ConfigSchema: schema version handling", "[config][schema]")
{
    CHECK(config::schemaVersionOf(R"({"schemaVersion":1})") == 1);
    CHECK(config::schemaVersionOf(R"({"schemaVersion":7})") == 7);
    CHECK(config::schemaVersionOf("{}") == 0);
    CHECK(config::schemaVersionOf("not json") == 0);

    // A file from the future is not parsed into a config this build cannot write back.
    AppConfig restored;
    ConfigProblems problems;
    std::string error;
    REQUIRE(config::fromJsonText(R"({"schemaVersion":99,"audio":{"sampleRate":96000}})", restored, problems, error));
    CHECK(config::validate(restored).empty());   // the repaired value is usable again

    bool reported = false;
    for (const auto& problem : problems)
        reported = reported || problem.field == "schemaVersion";
    CHECK(reported);
    CHECK(restored.schemaVersion == kConfigSchemaVersion);
    CHECK(restored.audio.sampleRate == 96000);   // valid fields are kept
}

TEST_CASE("ConfigSchema: credential-like keys are never read and never written", "[config][schema][security]")
{
    CHECK(config::isSecretFieldName("api_key"));
    CHECK(config::isSecretFieldName("OPENAI_API_KEY"));
    CHECK(config::isSecretFieldName("apikey"));
    CHECK(config::isSecretFieldName("access token"));
    CHECK(config::isSecretFieldName("authToken"));
    CHECK(config::isSecretFieldName("db_password"));
    CHECK(config::isSecretFieldName("clientSecret"));
    CHECK(config::isSecretFieldName("credential"));
    CHECK(config::isSecretFieldName("authorization"));
    CHECK(config::isSecretFieldName("bearer"));

    CHECK_FALSE(config::isSecretFieldName("sampleRate"));
    CHECK_FALSE(config::isSecretFieldName("bufferFrames"));
    CHECK_FALSE(config::isSecretFieldName("streamName"));
    CHECK_FALSE(config::isSecretFieldName("instructions"));
    CHECK_FALSE(config::isSecretFieldName(""));

    // Reading a file that contains a key: reported, and not applied anywhere.
    AppConfig restored = config::defaults();
    ConfigProblems problems;
    std::string error;
    const std::string text = R"({
        "schemaVersion": 1,
        "api_key": "sk-secret-value",
        "audio": { "sampleRate": 48000, "output_api_key": "sk-nested-secret" }
    })";

    REQUIRE(config::fromJsonText(text, restored, problems, error));

    int secretReports = 0;
    for (const auto& problem : problems)
        secretReports += problem.message.find("ignored") != std::string::npos ? 1 : 0;
    CHECK(secretReports == 2);

    // Nothing was copied into the configuration: it has no field that could hold it,
    // and the serialized form stays clean.
    const auto written = config::toJsonText(restored);
    CHECK(written.find("sk-secret-value") == std::string::npos);
    CHECK(written.find("sk-nested-secret") == std::string::npos);
    CHECK(written.find("api_key") == std::string::npos);
}

TEST_CASE("ConfigSchema: validation covers the documented bounds", "[config][schema][validation]")
{
    auto countProblems = [](const AppConfig& cfg) { return config::validate(cfg).size(); };

    CHECK(countProblems(config::defaults()) == 0);

    auto cfg = config::defaults();
    cfg.schemaVersion = 2;
    CHECK(countProblems(cfg) == 1);
    cfg.schemaVersion = 0;
    CHECK(countProblems(cfg) == 1);

    cfg = config::defaults();
    cfg.audio.sampleRate = 32000;
    CHECK(countProblems(cfg) == 1);

    cfg = config::defaults();
    cfg.audio.bufferFrames = 32;
    CHECK(countProblems(cfg) == 1);
    cfg.audio.bufferFrames = 4096;
    CHECK(countProblems(cfg) == 1);
    cfg.audio.bufferFrames = 512;
    CHECK(countProblems(cfg) == 0);

    cfg = config::defaults();
    cfg.audio.inputChannel = 0;
    CHECK(countProblems(cfg) == 1);
    cfg.audio.outputChannel = -1;
    CHECK(countProblems(cfg) == 1);

    cfg = config::defaults();
    cfg.audio.inputGainDb = 80.0f;
    CHECK(countProblems(cfg) == 1);
    cfg.audio.outputGainDb = -80.0f;
    CHECK(countProblems(cfg) == 1);

    cfg = config::defaults();
    cfg.translation.inputLanguage = "english-language";   // not a tag: too long
    CHECK(countProblems(cfg) >= 1);

    cfg = config::defaults();
    cfg.translation.inputLanguage = "en-US";   // a real tag shape stays acceptable
    CHECK(countProblems(cfg) == 0);

    cfg = config::defaults();
    cfg.translation.inputLanguage = "en;drop-table";   // punctuation is not a tag
    CHECK(countProblems(cfg) >= 1);

    cfg = config::defaults();
    cfg.translation.inputLanguage = "";
    CHECK(countProblems(cfg) >= 1);

    cfg = config::defaults();
    cfg.translation.instructions.clear();
    CHECK(countProblems(cfg) == 1);

    cfg = config::defaults();
    cfg.translation.instructions = std::string(5000, 'x');
    CHECK(countProblems(cfg) == 1);

    cfg = config::defaults();
    cfg.translation.modelHint = std::string(100, 'm');
    CHECK(countProblems(cfg) == 1);

    cfg = config::defaults();
    cfg.translation.jitterBufferMs = -1;
    CHECK(countProblems(cfg) == 1);

    cfg = config::defaults();
    cfg.ndi.enabled = true;
    cfg.ndi.streamName.clear();
    CHECK(countProblems(cfg) == 1);

    cfg = config::defaults();
    cfg.diagnostics.logLevel = "trace";
    CHECK(countProblems(cfg) == 0);
    cfg.diagnostics.logLevel = "nonsense";
    CHECK(countProblems(cfg) == 1);

    // Control characters are refused: they would corrupt logs and file content.
    cfg = config::defaults();
    cfg.translation.instructions = std::string("bad") + static_cast<char>(7);
    CHECK(countProblems(cfg) == 1);
}

TEST_CASE("ConfigSchema: gains are validated as finite numbers", "[config][schema][validation]")
{
    auto cfg = config::defaults();
    cfg.audio.inputGainDb = std::numeric_limits<float>::infinity();
    CHECK(config::validate(cfg).size() == 1);

    cfg.audio.inputGainDb = 0.0f;
    cfg.audio.outputGainDb = -std::numeric_limits<float>::infinity();
    CHECK(config::validate(cfg).size() == 1);
}

TEST_CASE("ConfigSchema: log level names come from the logger itself", "[config][schema]")
{
    for (const auto level : { LogLevel::trace, LogLevel::debug, LogLevel::info, LogLevel::warning,
                              LogLevel::error, LogLevel::critical, LogLevel::off })
    {
        auto cfg = config::defaults();
        cfg.diagnostics.logLevel = std::string(log::nameOf(level));
        INFO(cfg.diagnostics.logLevel);
        CHECK(config::validate(cfg).empty());
    }
}

TEST_CASE("ConfigStore: round-trip through a real file", "[config][store][roundtrip]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");

    config::ConfigStore store(file);
    const auto settings = makeConfig();

    std::string error;
    REQUIRE(store.save(settings, error));
    CHECK(error.empty());
    CHECK(std::filesystem::exists(file));

    // The temporary file must not survive a successful save.
    CHECK_FALSE(std::filesystem::exists(store.temporaryFile()));

    const auto loaded = store.load();
    CHECK(loaded.outcome == config::LoadOutcome::loaded);
    CHECK(loaded.problems.empty());
    CHECK(loaded.config.audio.inputDeviceId == settings.audio.inputDeviceId);
    CHECK(loaded.config.translation.instructions == settings.translation.instructions);
    CHECK(loaded.config.diagnostics.logLevel == "warning");
    CHECK(loaded.config.ndi.streamName == settings.ndi.streamName);
}

TEST_CASE("ConfigStore: missing file means defaults, and nothing is written", "[config][store]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("nested") / "config.json";

    config::ConfigStore store(file);
    const auto loaded = store.load();

    CHECK(loaded.outcome == config::LoadOutcome::fileAbsent);
    CHECK(loaded.config.audio.sampleRate == 48000);
    CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("ConfigStore: a corrupt file is quarantined, not deleted", "[config][store][corrupt]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");
    livetest::writeFile(file, "{ this is not json ");

    config::ConfigStore store(file);
    const auto loaded = store.load();

    CHECK(loaded.outcome == config::LoadOutcome::invalidJson);
    CHECK(loaded.config == config::defaults());
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK_FALSE(loaded.quarantinedTo.empty());
    CHECK(std::filesystem::exists(loaded.quarantinedTo));

    // The broken content is still recoverable by a human.
    CHECK(livetest::readFile(loaded.quarantinedTo).find("this is not json") != std::string::npos);
}

TEST_CASE("ConfigStore: a valid backup is restored when the live file is broken", "[config][store][corrupt]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");

    config::ConfigStore store(file);
    auto settings = makeConfig();
    settings.audio.bufferFrames = 640;

    std::string error;
    REQUIRE(store.save(settings, error));

    // Second save creates the .bak of the previous good file.
    settings.audio.bufferFrames = 1024;
    REQUIRE(store.save(settings, error));
    REQUIRE(std::filesystem::exists(store.backupFile()));

    livetest::writeFile(file, "}{{{ not json at all");

    const auto loaded = store.load();
    CHECK(loaded.outcome == config::LoadOutcome::restoredFromBackup);
    CHECK(loaded.config.audio.bufferFrames == 640);   // the backup, not the broken file

    // The broken original was moved aside, and the next save repairs the tree.
    REQUIRE(store.save(settings, error));
    const auto again = store.load();
    CHECK(again.outcome == config::LoadOutcome::loaded);
    CHECK(again.config.audio.bufferFrames == 1024);
}

TEST_CASE("ConfigStore: a file from a newer schema version is refused untouched", "[config][store][corrupt]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");
    const std::string future = R"({"schemaVersion": 42, "audio": {"sampleRate": 48000}})";
    livetest::writeFile(file, future);

    config::ConfigStore store(file);
    const auto loaded = store.load();

    CHECK(loaded.outcome == config::LoadOutcome::refusedFutureVersion);
    CHECK(loaded.config == config::defaults());
    REQUIRE(std::filesystem::exists(file));
    CHECK(livetest::readFile(file) == future);   // the operator's newer file was not touched
    CHECK(loaded.quarantinedTo.empty());
}

TEST_CASE("ConfigStore: per-field problems in an otherwise valid file are reported", "[config][store][corrupt]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");
    livetest::writeFile(file, R"({"schemaVersion":1,"audio":{"sampleRate":12345,"bufferFrames":512}})");

    config::ConfigStore store(file);
    const auto loaded = store.load();

    CHECK(loaded.outcome == config::LoadOutcome::repaired);
    CHECK(loaded.config.audio.sampleRate == 48000);
    CHECK(loaded.config.audio.bufferFrames == 512);
    REQUIRE_FALSE(loaded.problems.empty());
    CHECK(loaded.problems[0].field == "audio.sampleRate");
    CHECK(std::filesystem::exists(file));   // repairable file stays in place
}

TEST_CASE("ConfigStore: an invalid configuration is never written", "[config][store][validation]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");

    config::ConfigStore store(file);
    auto settings = config::defaults();
    settings.audio.sampleRate = 12345;

    std::string error;
    CHECK_FALSE(store.save(settings, error));
    CHECK(error.find("audio.sampleRate") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK_FALSE(std::filesystem::exists(store.temporaryFile()));
}

TEST_CASE("ConfigStore: a file without schemaVersion is migrated, not rejected", "[config][store]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");
    livetest::writeFile(file, R"({"audio":{"bufferFrames":256}})");

    config::ConfigStore store(file);
    const auto loaded = store.load();

    CHECK(loaded.outcome == config::LoadOutcome::repaired);
    CHECK(loaded.config.schemaVersion == kConfigSchemaVersion);
    CHECK(loaded.config.audio.bufferFrames == 256);

    bool migrated = false;
    for (const auto& problem : loaded.problems)
        migrated = migrated || problem.field == "schemaVersion";
    CHECK(migrated);
}

TEST_CASE("ConfigStore: the written file never contains a credential", "[config][store][security]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");

    // An operator who pasted a key into the file must not have it persisted, and
    // must be told about it.
    livetest::writeFile(file, R"({"schemaVersion":1,"api_key":"sk-abc123","audio":{"sampleRate":48000}})");

    config::ConfigStore store(file);
    auto loaded = store.load();
    REQUIRE_FALSE(loaded.problems.empty());

    std::string error;
    REQUIRE(store.save(loaded.config, error));

    const auto content = livetest::readFile(file);
    CHECK(content.find("sk-abc123") == std::string::npos);
    CHECK(content.find("api_key") == std::string::npos);
    CHECK(content.find("schemaVersion") != std::string::npos);
}

TEST_CASE("ConfigStore: defaultFile points into the application folder", "[config][store]")
{
    const auto path = config::ConfigStore::defaultFile();
    const auto filename = path.filename().string();
    const auto parent = path.parent_path().filename().string();

    CHECK(filename == "config.json");
    CHECK(parent == std::string(config::ConfigStore::applicationDirectoryName()));
}
