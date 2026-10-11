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
    cfg.translation.reconnectEnabled = false;
    cfg.translation.reconnectInitialBackoffMs = 500;
    cfg.translation.reconnectMaxBackoffMs = 9000;
    cfg.translation.sessionMaxAgeSeconds = 1800;
    cfg.translation.expirySafetyMarginSeconds = 420;
    cfg.ndi.enabled = true;
    cfg.ndi.streamName = "LingoFlow EN->RU";
    cfg.ndi.subtitles.mode = subtitles::OutputMode::both;
    cfg.ndi.subtitles.video.width = 1280;
    cfg.ndi.subtitles.video.height = 720;
    cfg.ndi.subtitles.video.fps = 30;
    cfg.ndi.subtitles.style.fontFamily = "Segoe UI";
    cfg.ndi.subtitles.style.fontSizePx = 60;
    cfg.ndi.subtitles.style.textColorRgb = 0x12AB34;
    cfg.ndi.subtitles.style.outlineColorRgb = 0x56CD78;
    cfg.ndi.subtitles.style.outlineWidthPx = 3;
    cfg.ndi.subtitles.style.shadowEnabled = true;
    cfg.ndi.subtitles.style.shadowOffsetPx = 4;
    cfg.ndi.subtitles.style.shadowOpacityPercent = 60;
    cfg.ndi.subtitles.style.horizontalAlign = subtitles::HorizontalAlign::right;
    cfg.ndi.subtitles.style.verticalAnchor = subtitles::VerticalAnchor::top;
    cfg.ndi.subtitles.style.edgeMarginPercent = 12;
    cfg.ndi.subtitles.style.blockWidthPercent = 70;
    cfg.ndi.subtitles.style.maxLines = 3;
    cfg.ndi.subtitles.style.lineSpacingPercent = 40;
    cfg.ndi.subtitles.style.backgroundEnabled = true;
    cfg.ndi.subtitles.style.backgroundOpacityPercent = 40;
    cfg.ndi.subtitles.style.backgroundPaddingPx = 8;
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
    CHECK(cfg.translation.jitterBufferMs == 250);  // provisional, from the live wire facts; rig sweep confirms
    CHECK(cfg.translation.reconnectEnabled);       // recovery is on unless the operator says otherwise
    CHECK(cfg.translation.reconnectInitialBackoffMs == 1000);
    CHECK(cfg.translation.reconnectMaxBackoffMs == 15000);
    CHECK(cfg.translation.sessionMaxAgeSeconds == 3300); // under the measured 3600 s ceiling (docs section 15)

    CHECK_FALSE(cfg.ndi.enabled);
    CHECK(cfg.diagnostics.logLevel == "info");
    CHECK(cfg.diagnostics.writeLogFile);
    CHECK(cfg.schemaVersion == kConfigSchemaVersion);

    // Subtitle video defaults (the task's numbers, persisted as the schema's):
    // OBS-ready video mode, 1080p at 25 fps, the reference look.
    CHECK(cfg.ndi.subtitles.mode == subtitles::OutputMode::video);
    CHECK(cfg.ndi.subtitles.video.width == 1920);
    CHECK(cfg.ndi.subtitles.video.height == 1080);
    CHECK(cfg.ndi.subtitles.video.fps == 25);
    CHECK(cfg.ndi.subtitles.style.fontFamily == "Segoe UI Semibold");
    CHECK(cfg.ndi.subtitles.style.fontSizePx == 48);
    CHECK(cfg.ndi.subtitles.style.textColorRgb == 0xFFFFFF);
    CHECK(cfg.ndi.subtitles.style.outlineColorRgb == 0x000000);
    CHECK(cfg.ndi.subtitles.style.outlineWidthPx == 2);
    CHECK_FALSE(cfg.ndi.subtitles.style.shadowEnabled);
    CHECK(cfg.ndi.subtitles.style.horizontalAlign == subtitles::HorizontalAlign::center);
    CHECK(cfg.ndi.subtitles.style.verticalAnchor == subtitles::VerticalAnchor::bottom);
    CHECK(cfg.ndi.subtitles.style.edgeMarginPercent == 8);
    CHECK(cfg.ndi.subtitles.style.blockWidthPercent == 85);
    CHECK(cfg.ndi.subtitles.style.maxLines == 2);
    CHECK(cfg.ndi.subtitles.style.lineSpacingPercent == 25);
    CHECK_FALSE(cfg.ndi.subtitles.style.backgroundEnabled);
}

TEST_CASE("ConfigSchema: a v1 settings file grows the v2 defaults, not problems",
          "[config][schema][migration]")
{
    // The delivered ndi section is exactly two keys; loading such a file with
    // this build must keep what it says (enabled/streamName) and let every
    // subtitle-video field take its schema default - the no-fake-success rule
    // of every earlier schema section, repeated honestly for v2.
    const std::string v1Text = R"({
        "schemaVersion": 1,
        "ndi": { "enabled": true, "streamName": "Venue A" }
    })";

    AppConfig restored;
    ConfigProblems problems;
    std::string error;
    REQUIRE(config::fromJsonText(v1Text, restored, problems, error));

    CHECK(restored.ndi.enabled);
    CHECK(restored.ndi.streamName == "Venue A");
    CHECK(restored.ndi.subtitles == config::defaults().ndi.subtitles);

    const auto fresh = config::defaults();
    CHECK(restored.audio.sampleRate == fresh.audio.sampleRate);   // every other section defaults too
}

TEST_CASE("ConfigSchema: subtitle video values are validated and repaired field by field",
          "[config][schema][video]")
{
    // An odd NDI width is not a taste rule but a documented constraint (the
    // frame-type docs require widths divisible by two); every wrong field is
    // reported and restored, while the GOOD sibling values survive - the
    // repair granularity the audio section already promises.
    const std::string bad = R"({
        "schemaVersion": 2,
        "ndi": {
            "enabled": true,
            "streamName": "Venue A",
            "mode": "telepathy",
            "videoWidth": 1919,
            "videoFps": 240,
            "textColor": "#12G45Z",
            "maxLines": 9,
            "edgeMarginPercent": 8,
            "fontFamily": ""
        }
    })";

    AppConfig restored;
    ConfigProblems problems;
    std::string error;
    REQUIRE(config::fromJsonText(bad, restored, problems, error));

    const auto fields = [&problems](std::string_view field)
    {
        return std::any_of(problems.begin(), problems.end(),
                           [field](const ConfigProblem& p) { return p.field == field; });
    };

    CHECK(fields("ndi.mode"));
    CHECK(fields("ndi.videoWidth"));
    CHECK(fields("ndi.videoFps"));
    CHECK(fields("ndi.textColor"));
    CHECK(fields("ndi.maxLines"));
    CHECK(fields("ndi.fontFamily"));

    const auto& fresh = config::defaults();
    CHECK(restored.ndi.subtitles.mode == fresh.ndi.subtitles.mode);
    CHECK(restored.ndi.subtitles.video.width == fresh.ndi.subtitles.video.width);
    CHECK(restored.ndi.subtitles.video.fps == fresh.ndi.subtitles.video.fps);
    CHECK(restored.ndi.subtitles.style.textColorRgb == fresh.ndi.subtitles.style.textColorRgb);
    CHECK(restored.ndi.subtitles.style.maxLines == fresh.ndi.subtitles.style.maxLines);
    CHECK(restored.ndi.subtitles.style.fontFamily == fresh.ndi.subtitles.style.fontFamily);

    // And the fields that were valid are untouched by the repair.
    CHECK(restored.ndi.streamName == "Venue A");
    CHECK(restored.ndi.subtitles.style.edgeMarginPercent == 8);
}

TEST_CASE("ConfigSchema: colours travel as canonical #RRGGBB", "[config][schema][video]")
{
    AppConfig cfg = config::defaults();
    cfg.ndi.subtitles.style.textColorRgb = 0x0A141E;
    cfg.ndi.subtitles.style.outlineColorRgb = 0xF0E0D0;

    const std::string text = config::toJsonText(cfg);
    CHECK(text.find("\"#0A141E\"") != std::string::npos);
    CHECK(text.find("\"#F0E0D0\"") != std::string::npos);

    // Case-insensitive on the way in, canonical on the way back out.
    const std::string lower = R"({"schemaVersion": 2, "ndi": { "textColor": "#0a141e" }})";
    AppConfig restored;
    ConfigProblems problems;
    std::string error;
    REQUIRE(config::fromJsonText(lower, restored, problems, error));
    CHECK(restored.ndi.subtitles.style.textColorRgb == 0x0A141E);
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
    CHECK(restored.translation.reconnectEnabled == original.translation.reconnectEnabled);
    CHECK(restored.translation.reconnectInitialBackoffMs == original.translation.reconnectInitialBackoffMs);
    CHECK(restored.translation.reconnectMaxBackoffMs == original.translation.reconnectMaxBackoffMs);
    CHECK(restored.translation.sessionMaxAgeSeconds == original.translation.sessionMaxAgeSeconds);
    CHECK(restored.translation.expirySafetyMarginSeconds == original.translation.expirySafetyMarginSeconds);
    CHECK(restored.ndi.enabled == original.ndi.enabled);
    CHECK(restored.ndi.streamName == original.ndi.streamName);
    CHECK(restored.ndi.subtitles == original.ndi.subtitles);   // the whole v2 block, field for field
    CHECK(restored.diagnostics.logLevel == original.diagnostics.logLevel);
    CHECK(restored.diagnostics.writeLogFile == original.diagnostics.writeLogFile);
}

TEST_CASE("ConfigSchema: developer fields round-trip", "[config][schema][developer]")
{
    AppConfig cfg = config::defaults();
    cfg.developer.enabled = true;
    cfg.developer.audioSource = "wav";
    cfg.developer.wavInputPath = "rehearsal\\\\input.wav";
    cfg.developer.wavOutputPath = "rehearsal\\\\record.wav";
    cfg.developer.toneFrequencyHz = 440.0;
    cfg.developer.toneLevelDb = -18.0;
    cfg.developer.mockTranslation = true;
    cfg.developer.mockLatencyMs = 800;
    cfg.developer.loopback = true;

    const std::string text = config::toJsonText(cfg);

    AppConfig restored;
    ConfigProblems problems;
    std::string error;
    REQUIRE(config::fromJsonText(text, restored, problems, error));
    CHECK(problems.empty());
    CHECK(restored.developer == cfg.developer);
    CHECK(restored.developer.enabled);
    CHECK(restored.developer.audioSource == "wav");
    CHECK(restored.developer.mockLatencyMs == 800);
    CHECK(restored.developer.loopback);
}

TEST_CASE("ConfigSchema: a config file predating the developer section loads as developer-off",
          "[config][schema][developer][isolation]")
{
    // isolation runs down into the storage layer: an installation
    // that never touched developer mode keeps running the real chain after the
    // update - the absent section means "off", there is no absent-means-on path.
    const std::string text =
        R"({"schemaVersion":1,"audio":{"sampleRate":48000,"bufferFrames":480},)"
        R"("translation":{"inputLanguage":"en","outputLanguage":"ru"},)"
        R"("ndi":{"enabled":false,"streamName":"LingoFlow"},)"
        R"("diagnostics":{"logLevel":"info","writeLogFile":true}})";

    AppConfig restored;
    ConfigProblems problems;
    std::string error;
    REQUIRE(config::fromJsonText(text, restored, problems, error));
    CHECK(problems.empty());

    CHECK_FALSE(restored.developer.enabled);
    CHECK(restored.developer.audioSource == "device");
    CHECK_FALSE(restored.developer.mockTranslation);
    CHECK_FALSE(restored.developer.loopback);
}

TEST_CASE("ConfigSchema: developer values are validated and repaired per field",
          "[config][schema][developer]")
{
    // A nonsense source, an out-of-bounds mock delay and an incoherent
    // wav-without-file: every one gets its own report, defaults come back for
    // the offending fields, and the valid ones (enabled, mock) stay.
    const std::string text =
        R"({"developer":{"enabled":true,"audioSource":"flac","mockLatencyMs":99999,)"
        R"("toneFrequencyHz":0,"wavOutputPath":"ok.wav","mockTranslation":true}})";

    AppConfig restored;
    ConfigProblems problems;
    std::string error;
    REQUIRE(config::fromJsonText(text, restored, problems, error));

    CHECK(restored.developer.enabled);          // untouched: not a listed problem
    CHECK(restored.developer.mockTranslation);  // untouched
    CHECK(restored.developer.audioSource == "device");   // repaired
    CHECK(restored.developer.mockLatencyMs == 300);      // repaired
    CHECK(restored.developer.toneFrequencyHz == 1000.0); // repaired
    CHECK(restored.developer.wavOutputPath == "ok.wav"); // untouched

    const auto names = [](const ConfigProblems& list)
    {
        std::string joined;
        for (const auto& problem : list)
            joined += problem.field + ";";
        return joined;
    };

    const std::string reported = names(problems);
    CHECK(reported.find("developer.audioSource") != std::string::npos);
    CHECK(reported.find("developer.mockLatencyMs") != std::string::npos);
    CHECK(reported.find("developer.toneFrequencyHz") != std::string::npos);
}

TEST_CASE("ConfigStore: the developer section survives a real file round-trip",
          "[config][store][developer]")
{
    livetest::TempDirectory temp;
    const auto file = temp.file("config.json");

    AppConfig cfg = config::defaults();
    cfg.developer.enabled = true;
    cfg.developer.audioSource = "tone";
    cfg.developer.toneLevelDb = -12.0;

    config::ConfigStore store(file);
    std::string error;
    REQUIRE(store.save(cfg, error));

    const auto loaded = store.load();
    CHECK(loaded.outcome == config::LoadOutcome::loaded);
    CHECK(loaded.config.developer == cfg.developer);
    CHECK(loaded.config.developer.audioSource == "tone");
}

TEST_CASE("ConfigSchema: recovery policy fields validate and repair per field",
          "[config][schema][recovery]")
{
    // A maxBackoff below the initialBackoff is a contradiction: exactly that
    // field is repaired to its default, and the rest of the recovery section -
    // which is valid - stays as the operator set it.
    const std::string text =
        R"({"translation":{"reconnectEnabled":false,"reconnectInitialBackoffMs":2000,)"
        R"("reconnectMaxBackoffMs":100,"sessionMaxAgeSeconds":3600}})";

    AppConfig restored;
    ConfigProblems problems;
    std::string error;
    REQUIRE(config::fromJsonText(text, restored, problems, error));

    CHECK_FALSE(restored.translation.reconnectEnabled);            // kept
    CHECK(restored.translation.reconnectInitialBackoffMs == 2000); // kept
    CHECK(restored.translation.reconnectMaxBackoffMs
          == config::defaults().translation.reconnectMaxBackoffMs); // repaired
    CHECK(restored.translation.sessionMaxAgeSeconds == 3600);      // kept
    CHECK(std::any_of(problems.begin(), problems.end(),
                      [](const ConfigProblem& p)
                      { return p.field == "translation.reconnectMaxBackoffMs"; }));
    CHECK(config::validate(restored).empty()); // the repaired result is usable

    // Out-of-range numbers are problems too (the ceiling is two measured
    // provider hours).
    AppConfig broken = config::defaults();
    broken.translation.sessionMaxAgeSeconds = 7201;
    CHECK_FALSE(config::validate(broken).empty());
    broken.translation.sessionMaxAgeSeconds = 0; // 0 = the age reopen disabled, valid
    CHECK(config::validate(broken).empty());

    // Review P1: the safety margin for a server-announced expiry
    // (protocol docs section 4bis). Default five minutes; a value beyond the
    // half-ceiling boundary is a problem; 0 is valid-but-not-recommended; and
    // a config file written before this field existed must load on the
    // default, exactly as old installations keep working unchanged.
    CHECK(config::defaults().translation.expirySafetyMarginSeconds == 300);

    broken.translation.expirySafetyMarginSeconds = 3601;
    CHECK_FALSE(config::validate(broken).empty());
    broken.translation.expirySafetyMarginSeconds = 0;
    CHECK(config::validate(broken).empty());

    AppConfig preReview;
    ConfigProblems preProblems;
    std::string preError;
    REQUIRE(config::fromJsonText(R"({"translation":{"sessionMaxAgeSeconds":3600}})",
                                 preReview, preProblems, preError));
    CHECK(preProblems.empty());
    CHECK(preReview.translation.sessionMaxAgeSeconds == 3600);
    CHECK(preReview.translation.expirySafetyMarginSeconds == 300);   // the default answers

    AppConfig marginSet;
    ConfigProblems marginProblems;
    REQUIRE(config::fromJsonText(R"({"translation":{"expirySafetyMarginSeconds":600}})",
                                 marginSet, marginProblems, preError));
    CHECK(marginProblems.empty());
    CHECK(marginSet.translation.expirySafetyMarginSeconds == 600);
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
    CHECK(restored.translation.jitterBufferMs == 250);
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
    cfg.schemaVersion = kConfigSchemaVersion + 1;   // from the future: rejected as before
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

    // spec "Input Gain" suggests -24..+24 dB: both ends must be accepted.
    cfg = config::defaults();
    cfg.audio.inputGainDb = 24.0f;
    cfg.audio.outputGainDb = -24.0f;
    CHECK(countProblems(cfg) == 0);

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
    cfg.translation.instructions.clear();   // optional since review P1: "nothing configured" is legal
    CHECK(countProblems(cfg) == 0);         // and it is now the DEFAULT too

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

TEST_CASE("ConfigStore: the pre-rename location is a different folder, same file name",
          "[config][store][rename]")
{
    const auto current = config::ConfigStore::defaultFile();
    const auto legacy = config::ConfigStore::legacyFile();

    CHECK(current.filename() == legacy.filename());
    CHECK(current.parent_path() != legacy.parent_path());
    CHECK(std::string(config::ConfigStore::applicationDirectoryName()) == "LingoFlow");
    CHECK(std::string(config::ConfigStore::legacyApplicationDirectoryName()) == "Live AI Interpreter");
}

TEST_CASE("ConfigStore: startup file prefers the current location", "[config][store][rename]")
{
    livetest::TempDirectory temp;
    const auto current = temp.file("new/config.json");
    const auto legacy = temp.file("old/config.json");

    // Both exist -> the new one wins, and no migration is signalled.
    std::filesystem::create_directories(current.parent_path());
    std::filesystem::create_directories(legacy.parent_path());
    livetest::writeFile(current, "{}");
    livetest::writeFile(legacy, "{}");

    bool fromLegacy = false;
    CHECK(config::ConfigStore::resolveStartupFile(current, legacy, fromLegacy) == current);
    CHECK_FALSE(fromLegacy);

    // Only the legacy file -> read it, and say so.
    std::filesystem::remove(current);
    CHECK(config::ConfigStore::resolveStartupFile(current, legacy, fromLegacy) == legacy);
    CHECK(fromLegacy);

    // Neither -> first run, target is the new location, nothing to migrate.
    std::filesystem::remove(legacy);
    CHECK(config::ConfigStore::resolveStartupFile(current, legacy, fromLegacy) == current);
    CHECK_FALSE(fromLegacy);
}
