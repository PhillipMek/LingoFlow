#include <catch2/catch_test_macros.hpp>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "App/UiModel.h"
#include "Audio/AudioEngine.h"
#include "Config/ConfigSchema.h"
#include "Utils/Log.h"

using namespace liveai;

namespace {

constexpr int kFrames = 480;

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

std::string rowValue(const std::vector<std::pair<std::string, std::string>>& rows,
                     std::string_view label)
{
    for (const auto& [name, value] : rows)
    {
        if (name == label)
            return value;
    }
    return "<missing row>";
}

/// UI-01: the raw counters moved from the operator panel to the diagnostics
/// surface; tests address them through that section now.
std::string counterValue(ApplicationController& controller, std::string_view label)
{
    const DiagnosticsPanel panel = buildDiagnosticsPanel(controller);
    for (const auto* section : { &panel.audioHealth, &panel.translationHealth, &panel.subtitles,
                                 &panel.runtime })
    {
        for (const auto& [name, value] : *section)
        {
            if (name == label)
                return value;
        }
    }
    return "<missing row>";
}

/// One device callback of constant input; returns the played output for
/// inspection. Same driving style as the pipeline tests (manual callbacks).
void feedBlock(AudioEngine& engine, float level)
{
    std::vector<float> in(static_cast<std::size_t> (kFrames), level);
    std::vector<float> out(static_cast<std::size_t> (kFrames), -0.5f);

    const float* inPointers[1] = { in.data() };
    float* outPointers[1] = { out.data() };

    engine.processAudio(inPointers, outPointers, kFrames);
}

/// An in-memory UI-side stand-in for the credential store: presence is the only
/// fact the panel may display, so this store keeps exactly that.
class PresenceStore final : public security::ISecretStore
{
public:
    std::string_view name() const noexcept override { return "Presence test store"; }

    security::SecretStatus store(std::string_view, std::string_view) override
    {
        present = true;
        return security::SecretStatus::stored;
    }

    std::optional<std::string> load(std::string_view identifier) override
    {
        if (present && identifier == security::kOpenAiApiKey)
            return std::string("the-value");
        return std::nullopt;
    }

    security::SecretStatus remove(std::string_view) override
    {
        present = false;
        return security::SecretStatus::found;
    }

    std::vector<std::string> identifiers() const override
    {
        if (!present)
            return {};
        return { std::string(security::kOpenAiApiKey) };
    }

    bool present = false;
};

} // namespace

TEST_CASE("UiModel: the stopped panel states facts, not promises", "[app][ui][model]")
{
    QuietLog quiet;
    ApplicationController controller;

    const OperatorPanel panel = buildOperatorPanel(controller, {});

    CHECK(panel.applicationState == "stopped");
    CHECK(panel.audioState == "closed");
    CHECK(panel.sessionState == "closed");
    CHECK(panel.ndiState == "disabled");
    CHECK(panel.audioBackendName == "Null");   // the default device-shaped no-op is honest
    CHECK(panel.canStart);
    CHECK_FALSE(panel.canStop);
    CHECK_FALSE(panel.faulted);

    // Selector lists come from the modules that own them.
    const auto rates = config::supportedSampleRates();
    REQUIRE(panel.sampleRates.size() == rates.size());
    CHECK(panel.selectedSampleRate >= 0);
    CHECK(panel.sampleRates[static_cast<std::size_t> (panel.selectedSampleRate)].value
          == std::to_string(controller.config().current().audio.sampleRate));

    CHECK_FALSE(panel.sourceLanguages.empty());
    CHECK_FALSE(panel.targetLanguages.empty());
    CHECK(panel.selectedSource >= 0);   // the default "en" is in the list
    CHECK(panel.selectedTarget >= 0);   // the default "ru" is in the list
    CHECK(panel.languagePairWarning.empty());

    // Ranges are the schema's own, never a UI-side invention.
    const auto [gainMin, gainMax] = config::gainRange();
    CHECK(panel.gainMinDb == gainMin);
    CHECK(panel.gainMaxDb == gainMax);
    const auto [bufferMin, bufferMax] = config::bufferFramesRange();
    CHECK(panel.bufferMinFrames == bufferMin);
    CHECK(panel.bufferMaxFrames == bufferMax);
    CHECK(panel.maxChannel == config::channelRange().second);
    CHECK(panel.jitterMaxMs == config::jitterBufferRange().second);

    CHECK(counterValue(controller, "audio blocks") == "0");
}

TEST_CASE("UiModel: a configured device the scan cannot see stays visible and labelled",
          "[app][ui][model]")
{
    QuietLog quiet;
    ApplicationController controller;

    controller.setDeviceLister([]
    {
        return std::vector<asio::DeviceEntry>
        {
            { "A", "Device A", "ASIO", true, false, {} },
            { "B", "Device B", "ASIO", true, false, {} },
        };
    });

    auto cfg = controller.config().current();
    cfg.audio.inputDeviceId = "Ghost";
    cfg.audio.outputDeviceId = "Ghost";
    std::string note;
    REQUIRE(controller.updateSettings(cfg, note));

    controller.refreshDevices();   // what the operator window does on open and on Refresh
    const OperatorPanel panel = buildOperatorPanel(controller, {});

    REQUIRE(panel.devices.size() == 3);   // the two found plus the honest ghost
    CHECK(panel.selectedDevice == 2);
    CHECK(panel.devices[2].value == "Ghost");
    CHECK(panel.devices[2].label.find("not in the last scan") != std::string::npos);
    CHECK_FALSE(panel.deviceNote.empty());
}

TEST_CASE("UiModel: an unsupported language pair warns before Start and Start refuses honestly",
          "[app][ui][model][languages]")
{
    QuietLog quiet;
    ApplicationController controller;

    auto cfg = controller.config().current();
    cfg.translation.inputLanguage = "zz";   // well-shaped tag the manifest does not list
    std::string note;
    REQUIRE(controller.updateSettings(cfg, note));   // config validates shape only (011)

    {
        const OperatorPanel panel = buildOperatorPanel(controller, {});
        CHECK_FALSE(panel.languagePairWarning.empty());   // the UI says it before the fact
    }

    REQUIRE(controller.start());   // audio still comes up: a refused session is not fatal
    const OperatorPanel panel = buildOperatorPanel(controller, {});

    CHECK(panel.applicationState == "running");
    CHECK(panel.sessionState == "closed");
    CHECK(panel.detail.find("language pair not supported") != std::string::npos);
    CHECK(panel.languagePairWarning.empty() == false);

    controller.stop();
}

TEST_CASE("UiModel: live knobs are the engine's truth in both directions",
          "[app][ui][model][live]")
{
    QuietLog quiet;
    ApplicationController controller;
    REQUIRE(controller.start());

    controller.setGainsLive(-6.0f, 3.0f);
    controller.setMutesLive(true, false);
    controller.setJitterLive(80);

    auto panel = buildOperatorPanel(controller, {});
    CHECK(panel.inputGainDb == -6.0f);
    CHECK(panel.outputGainDb == 3.0f);
    CHECK(panel.inputMuted);
    CHECK_FALSE(panel.outputMuted);
    CHECK(panel.jitterMs == 80);

    // Out-of-window requests are clamped by the engine's own safety window
    // (GainStage, wider than the config validation window) and the panel shows
    // the clamped fact, not the fantasy (AGENTS.md 19): no UI-side invention
    // either. The UI slider bounds come from the config window; the floor the
    // engine may end up at is the GainStage one, and both are asserted here.
    controller.setGainsLive(500.0f, -500.0f);
    panel = buildOperatorPanel(controller, {});
    CHECK(panel.inputGainDb == controller.engine().inputGainDb());
    CHECK(panel.inputGainDb == audio::GainStage::kMaxGainDb);
    CHECK(panel.outputGainDb == audio::GainStage::kMinGainDb);

    // updateSettings persists and re-applies: one truth for slider, config, engine.
    auto cfg = controller.config().current();
    cfg.audio.inputGainDb = -12.0f;
    cfg.translation.jitterBufferMs = 40;
    std::string note;
    REQUIRE(controller.updateSettings(cfg, note));

    panel = buildOperatorPanel(controller, {});
    CHECK(panel.inputGainDb == -12.0f);
    CHECK(controller.config().current().audio.inputGainDb == -12.0f);
    CHECK(panel.jitterMs == 40);
    CHECK_FALSE(note.empty());

    controller.stop();
}

TEST_CASE("UiModel: meters and counters read the running pipeline", "[app][ui][model][meters]")
{
    QuietLog quiet;
    ApplicationController controller;
    REQUIRE(controller.start());

    feedBlock(controller.engine(), 0.5f);
    feedBlock(controller.engine(), 0.5f);

    const auto panel = buildOperatorPanel(controller, {});

    CHECK(panel.inputMeter.signalPresent);
    CHECK(panel.inputMeter.peakDb > -20.0f);    // 0.5 peak = -6 dB post-gain

    // The output meter sees pure silence: nothing has been delivered to the
    // jitter, and input audio has no route out (task 005's safety rule).
    CHECK_FALSE(panel.outputMeter.signalPresent);
    CHECK(panel.outputMeter.peakDb <= -100.0f);

    CHECK(counterValue(controller, "audio blocks") == "2");
    CHECK(std::stoull(counterValue(controller, "underruns")) >= 2);   // silence is played, counted

    // The clip latch is consumed by the build that shows it: one panel sees the
    // clip, the next (without a new one) shows the lamp dark - no stale red.
    feedBlock(controller.engine(), 1.0f);
    CHECK(buildOperatorPanel(controller, {}).inputMeter.clipping);
    CHECK_FALSE(buildOperatorPanel(controller, {}).inputMeter.clipping);

    controller.stop();
}

TEST_CASE("UiModel: the subtitle fields are the task 013 model, not a copy of some API",
          "[app][ui][model][text]")
{
    QuietLog quiet;
    ApplicationController controller;
    REQUIRE(controller.start());

    controller.onPartialText("Р В Р’В Р РЋРЎСџР В Р Р‹Р В РІР‚С™Р В Р’В Р РЋРІР‚ВР В Р’В Р В РІР‚В Р В Р’В Р вЂ™Р’ВµР В Р Р‹Р Р†Р вЂљРЎв„ў Р В Р’В Р РЋР’ВР В Р’В Р РЋРІР‚ВР В Р Р‹Р В РІР‚С™");

    auto panel = buildOperatorPanel(controller, {});
    CHECK(panel.currentSubtitle == "Р В Р’В Р РЋРЎСџР В Р Р‹Р В РІР‚С™Р В Р’В Р РЋРІР‚ВР В Р’В Р В РІР‚В Р В Р’В Р вЂ™Р’ВµР В Р Р‹Р Р†Р вЂљРЎв„ў Р В Р’В Р РЋР’ВР В Р’В Р РЋРІР‚ВР В Р Р‹Р В РІР‚С™");
    CHECK(panel.subtitleHistory.empty());

    controller.onFinalText("Р В Р’В Р РЋРЎСџР В Р Р‹Р В РІР‚С™Р В Р’В Р РЋРІР‚ВР В Р’В Р В РІР‚В Р В Р’В Р вЂ™Р’ВµР В Р Р‹Р Р†Р вЂљРЎв„ў Р В Р’В Р РЋР’ВР В Р’В Р РЋРІР‚ВР В Р Р‹Р В РІР‚С™");
    panel = buildOperatorPanel(controller, {});
    CHECK(panel.currentSubtitle.empty());
    REQUIRE(panel.subtitleHistory.size() == 1);
    CHECK(panel.subtitleHistory[0] == "Р В Р’В Р РЋРЎСџР В Р Р‹Р В РІР‚С™Р В Р’В Р РЋРІР‚ВР В Р’В Р В РІР‚В Р В Р’В Р вЂ™Р’ВµР В Р Р‹Р Р†Р вЂљРЎв„ў Р В Р’В Р РЋР’ВР В Р’В Р РЋРІР‚ВР В Р Р‹Р В РІР‚С™");
    CHECK(rowValue(buildDiagnosticsPanel(controller).subtitles,
                   "text partial/final/evicted/duplicates") == "1 / 1 / 0 / 0");

    controller.stop();
}

TEST_CASE("UiModel: the latency line is buffer arithmetic with a named source",
          "[app][ui][model][latency]")
{
    QuietLog quiet;
    ApplicationController controller;
    REQUIRE(controller.start());

    // Null device geometry is the settings default: 48 kHz / 480 frames -> 10 ms
    // blocks; pre-roll from settings (250 ms). 10 + 10 + 250 = 270.
    const auto panel = buildOperatorPanel(controller, {});

    CHECK(panel.latencySummary.find("270 ms") != std::string::npos);
    // The sentence that keeps the number honest: translation is NOT in it.
    CHECK(panel.latencySummary.find("NOT included") != std::string::npos);

    controller.stop();
}

TEST_CASE("UiModel: a refused update leaves memory, config and panel untouched",
          "[app][ui][model]")
{
    QuietLog quiet;
    ApplicationController controller;

    const auto before = controller.config().current();

    auto bad = before;
    bad.audio.sampleRate = 12345;

    std::string note;
    CHECK_FALSE(controller.updateSettings(bad, note));
    CHECK(note.rfind("settings refused", 0) == 0);
    CHECK(controller.config().current().audio.sampleRate == before.audio.sampleRate);

    const auto panel = buildOperatorPanel(controller, note);
    CHECK(panel.selectedSampleRate >= 0);
    CHECK(panel.sampleRates[static_cast<std::size_t> (panel.selectedSampleRate)].value
          == std::to_string(before.audio.sampleRate));
    CHECK(panel.actionNote == note);
}

TEST_CASE("UiModel: the credential line states the store, the absence and the remedy - never a value",
          "[app][ui][model][credentials]")
{
    QuietLog quiet;
    ApplicationController controller;

    auto panel = buildOperatorPanel(controller, {});
    CHECK(panel.credentialLine.rfind("API key: NOT stored", 0) == 0);
    CHECK(panel.credentialLine.find("Settings") != std::string::npos);   // the remedy is in the sentence
    CHECK(panel.credentialLine.find("Null") != std::string::npos);       // and so is the honest store name

    PresenceStore store;
    store.present = true;
    controller.setSecretStore(store);

    panel = buildOperatorPanel(controller, {});
    CHECK(panel.credentialLine.rfind("API key: stored", 0) == 0);
    CHECK(panel.credentialLine.find("Presence test store") != std::string::npos);
}

TEST_CASE("UiModel: the log-level list round-trips with its owner and the schema accepts every name",
          "[app][ui][model][logging]")
{
    // Single-source assertion (task 015): the dialog's combo and validate()
    // read the same lists - a selector can never offer what the schema refuses,
    // the same rule the language dropdowns have since task 011.
    const auto choices = logLevelChoices();
    REQUIRE_FALSE(choices.empty());

    for (const auto& choice : choices)
    {
        CHECK(log::nameOf(log::levelFromName(choice.value)) == choice.value);

        AppConfig cfg = config::defaults();
        cfg.diagnostics.logLevel = choice.value;
        std::string error;
        CHECK(config::validate(cfg, error));
    }

    // And the list is exactly the module's own, in its own order.
    REQUIRE(choices.size() == log::allLevels().size());
    for (std::size_t i = 0; i < choices.size(); ++i)
        CHECK(choices[i].value == log::nameOf(log::allLevels()[i]));
}

TEST_CASE("UiModel: faulted is visible and the retry path is the operator's, not automatic",
          "[app][ui][model][fault]")
{
    QuietLog quiet;
    ApplicationController controller;

    // A configured device that does not exist must fault the start by name
    // (never a silent fallback) - the UI has to show exactly that.
    auto cfg = controller.config().current();
    cfg.audio.inputDeviceId = "Nonexistent";
    cfg.audio.outputDeviceId = "Nonexistent";
    std::string note;
    REQUIRE(controller.updateSettings(cfg, note));

    // Without a factory the controller reports it honestly instead of pretending.
    CHECK_FALSE(controller.start());

    auto panel = buildOperatorPanel(controller, {});
    CHECK(panel.faulted);
    CHECK_FALSE(panel.canStart);          // the Retry button is faulted's own path
    CHECK(panel.canStop);
    CHECK(panel.detail.find("Nonexistent") != std::string::npos);

    controller.clearFault();
    panel = buildOperatorPanel(controller, {});
    CHECK_FALSE(panel.faulted);
    CHECK(panel.canStart);                // operator's decision to try again

    controller.stop();
}

TEST_CASE("UiModel UI-01: channel choices are discrete, named when the driver names them",
          "[app][ui][model][channels]")
{
    // The pure builder first: names in -> exactly those channels, numbered
    // one-based with the driver's own labels; no names -> generic numbering up
    // to the schema bound that validate() uses - never a UI-invented maximum.
    const std::vector<std::string> names = { "Main L", "", "Interpreter feed" };
    const auto named = channelOptions(names, 128);
    REQUIRE(named.size() == 3);
    CHECK(named[0].value == "1");
    CHECK(named[0].label == "1 - Main L");
    CHECK(named[1].label == "Channel 2");           // a nameless driver slot stays honest
    CHECK(named[2].label == "3 - Interpreter feed");

    const auto generic = channelOptions({}, 6);
    REQUIRE(generic.size() == 6);
    CHECK(generic[0].label == "Channel 1");
    CHECK(generic[5].value == "6");

    // The ghost rule (same honesty as the devices list): a configured index
    // outside the named series stays selectable and says what it is.
    std::vector<UiOption> shortList = channelOptions(names, 128);
    const int selected = selectChannelWithGhost(shortList, 40);
    CHECK(selected == 3);
    CHECK(shortList[3].value == "40");
    CHECK(shortList[3].label.find("from settings") != std::string::npos);
    CHECK(selectChannelWithGhost(shortList, 2) == 1);   // in range: no extra entry

    // Through the panel: the default null device exposes no names, so the
    // operator sees generic numbering plus the honest note.
    QuietLog quiet;
    ApplicationController controller;
    const auto panel = buildOperatorPanel(controller, {});
    CHECK_FALSE(panel.inputChannelChoices.empty());
    CHECK(panel.inputChannelChoices[0].label.rfind("Channel 1", 0) == 0);
    CHECK_FALSE(panel.channelNote.empty());
    CHECK(panel.selectedInputChannel >= 0);   // the configured 1 is in the list
    CHECK(panel.inputChannelChoices[static_cast<std::size_t> (panel.selectedInputChannel)].value
          == "1");
}

TEST_CASE("UiModel UI-01: the operator strip is four facts; the wall moved to diagnostics",
          "[app][ui][model][sections]")
{
    QuietLog quiet;
    ApplicationController controller;
    REQUIRE(controller.start());
    feedBlock(controller.engine(), 0.5f);

    const auto panel = buildOperatorPanel(controller, {});
    REQUIRE(panel.health.size() == 4);
    CHECK(panel.health[0].first == "Latency");
    CHECK(panel.health[0].second.find("estimated") != std::string::npos);  // never a bare number
    CHECK(panel.health[1].first == "Jitter fill");
    CHECK(panel.health[2].first == "Underruns");
    CHECK(panel.health[3].first == "Reconnects");

    const DiagnosticsPanel diag = buildDiagnosticsPanel(controller);
    CHECK_FALSE(diag.audioHealth.empty());
    CHECK_FALSE(diag.translationHealth.empty());
    CHECK_FALSE(diag.subtitles.empty());
    CHECK_FALSE(diag.latency.empty());
    CHECK_FALSE(diag.runtime.empty());

    // Relocated, not deleted: rows the old counter wall showed read the same.
    CHECK(rowValue(diag.audioHealth, "audio blocks") == "1");
    CHECK_FALSE(rowValue(diag.audioHealth, "malformed callbacks").empty());
    CHECK(rowValue(diag.subtitles, "NDI state") == std::string(nameOf(ndi::OutputState::disabled)));

    // Runtime facts stay runtime facts: the stopped-with-null-store sentence.
    CHECK(rowValue(diag.runtime, "application") == "running");
    CHECK_FALSE(rowValue(diag.runtime, "API key").empty());

    // UI-02: the technical detail the operator screen gave up is here now -
    // the gain glide, and the full buffer-delay sentence.
    CHECK(rowValue(diag.audioHealth, "applied gain in / out").find("gliding") != std::string::npos);
    CHECK(rowValue(diag.runtime, "buffer delay detail").find("NOT included") != std::string::npos);

    controller.stop();
}
TEST_CASE("UiModel UI-03: raw details carry the truth and never a secret",
          "[app][ui][model][diagnostics]")
{
    QuietLog quiet;
    ApplicationController controller;

    // A store that keeps values, so the leakage assertion below is about a
    // real secret having been through the real controller path.
    struct ValueStore final : public security::ISecretStore
    {
        std::string_view name() const noexcept override { return "Value test store"; }
        security::SecretStatus store(std::string_view id, std::string_view secret) override
        {
            items[std::string(id)] = std::string(secret);
            return security::SecretStatus::stored;
        }
        std::optional<std::string> load(std::string_view id) override
        {
            const auto it = items.find(std::string(id));
            return it == items.end() ? std::nullopt : std::optional<std::string>(it->second);
        }
        security::SecretStatus remove(std::string_view id) override
        {
            return items.erase(std::string(id)) > 0 ? security::SecretStatus::found
                                                    : security::SecretStatus::notFound;
        }
        std::vector<std::string> identifiers() const override
        {
            std::vector<std::string> out;
            for (const auto& [id, value] : items)
                out.push_back(id);
            return out;
        }
        std::map<std::string, std::string> items;
    };

    ValueStore store;
    controller.setSecretStore(store);

    std::string note;
    REQUIRE(controller.storeApiSecret("sk-DO-NOT-LEAK-42", note));

    const DiagnosticsPanel diag = buildDiagnosticsPanel(controller);

    // Section 14's honesty: named rows, not invented numbers.
    CHECK(rowValue(diag.runtime, "application uptime") == "not measured");
    CHECK(rowValue(diag.runtime, "CPU / memory") == "not measured");
    CHECK(rowValue(diag.runtime, "API key") == "stored");

    // Section 15's content: the event ring and the configuration, as lines.
    REQUIRE_FALSE(diag.rawDetails.empty());
    // UI-03 ordering: configuration first, events last (the raw window shows
    // its tail, and the tail should be the newest events).
    CHECK(diag.rawDetails[0].find("configuration") != std::string::npos);
    const std::string lastLine = diag.rawDetails.back();
    const bool endsWithEvent = lastLine.rfind("  #", 0) == 0
                               || lastLine == "  (none recorded since startup)";
    CHECK(endsWithEvent);
    bool sawSecurityEvent = false;
    for (const auto& line : diag.rawDetails)
    {
        if (line.find("operator stored the API key") != std::string::npos)
            sawSecurityEvent = true;
    }
    CHECK(sawSecurityEvent);

    // Section 16's promise: no surface of the diagnostics model - structured
    // rows or raw lines - carries the secret value.
    const std::string secret = "sk-DO-NOT-LEAK-42";
    const auto checkNoSecret = [&](const std::vector<std::pair<std::string, std::string>>& rows)
    {
        for (const auto& [label, value] : rows)
        {
            CHECK(label.find(secret) == std::string::npos);
            CHECK(value.find(secret) == std::string::npos);
        }
    };
    checkNoSecret(diag.audioHealth);
    checkNoSecret(diag.translationHealth);
    checkNoSecret(diag.subtitles);
    checkNoSecret(diag.runtime);
    for (const auto& row : diag.latency)
        CHECK(row.value.find(secret) == std::string::npos);
    for (const auto& line : diag.rawDetails)
        CHECK(line.find(secret) == std::string::npos);
}
