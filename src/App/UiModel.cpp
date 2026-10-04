#include "App/UiModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <string_view>

#include "Audio/LevelMeter.h"
#include "Config/ConfigSchema.h"
#include "Translation/LanguageRegistry.h"
#include "Utils/Log.h"

namespace liveai {
namespace {

std::string lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

int indexOfValue(const std::vector<UiOption>& options, std::string_view value)
{
    for (std::size_t i = 0; i < options.size(); ++i)
    {
        if (options[i].value == value)
            return static_cast<int>(i);
    }
    return -1;
}

std::string formatCount(std::uint64_t value)
{
    return std::to_string(value);
}

UiMeterView meterView(const AudioEngine& engine, int channel, bool inputSide)
{
    UiMeterView view;

    const audio::LevelMeter* meter = inputSide ? engine.inputMeter(channel)
                                               : engine.outputMeter(channel);
    if (meter == nullptr)
        return view;   // no pipeline yet: silence is the honest reading

    view.peakDb = audio::linearToDb(meter->peakLinear());
    view.rmsDb = audio::linearToDb(meter->rmsLinear());
    view.signalPresent = meter->signalPresent();
    return view;
}

} // namespace

OperatorPanel buildOperatorPanel(ApplicationController& controller, const std::string& actionNote)
{
    OperatorPanel panel;
    const AppStatus status = controller.status();
    const auto& cfg = controller.config().current();
    AudioEngine& engine = controller.engine();

    // ---------------------------------------------------------------- status
    panel.applicationState = std::string(nameOf(status.application));
    panel.audioState = std::string(audio::nameOf(status.audio));
    panel.sessionState = std::string(translation::nameOf(status.session));
    panel.ndiState = std::string(ndi::nameOf(status.ndi));
    panel.audioBackendName = status.audioBackendName.empty() ? "none" : status.audioBackendName;
    panel.detail = status.detail.empty() ? "ok" : status.detail;   // silence reads as ok
    panel.faulted = status.application == ApplicationState::faulted;
    panel.canStart = status.application == ApplicationState::stopped;
    panel.canStop = status.application == ApplicationState::running
                    || status.application == ApplicationState::faulted;

    // Task 015. Presence is read from the store's identifier list (names, never
    // values), and the sentence points the operator at the exact remedy before
    // a session ever refuses for a missing key.
    panel.credentialLine = controller.hasApiSecret()
        ? "API key: stored - " + controller.secretStoreName()
        : "API key: NOT stored - sessions will refuse until it is entered in Settings (store: "
              + controller.secretStoreName() + ")";

    // --------------------------------------------------------------- devices
    for (const auto& device : controller.devices())
        panel.devices.push_back({ device.id, device.name });

    const std::string& selected = cfg.audio.inputDeviceId;

    panel.selectedDevice = indexOfValue(panel.devices, selected);

    if (selected.empty() && panel.devices.empty())
    {
        panel.deviceNote = "No device selected and no scan yet: press Refresh devices.";
    }
    else if (selected.empty())
    {
        panel.deviceNote = "No device selected - Start runs on the null device (silent) until one is.";
    }
    else if (panel.selectedDevice < 0)
    {
        // The configured device is not in the latest scan. It stays selectable and
        // labelled as what it is: the operator may know a thing the registry does
        // not (a server that was asleep during the scan). Never dropped silently.
        panel.devices.push_back({ selected, selected + "  (from settings; not in the last scan)" });
        panel.selectedDevice = static_cast<int>(panel.devices.size()) - 1;
        panel.deviceNote = "Configured device was not found by the last scan.";
    }

    // ------------------------------------------------------------- languages
    const auto& capabilities = translation::openAiManifest().capabilities();

    for (const auto& language : capabilities.sources)
        panel.sourceLanguages.push_back({ language.code,
                                          language.code + " - " + language.englishName });
    for (const auto& language : capabilities.targets)
        panel.targetLanguages.push_back({ language.code,
                                          language.code + " - " + language.englishName });

    panel.selectedSource = indexOfValue(panel.sourceLanguages, lower(cfg.translation.inputLanguage));
    panel.selectedTarget = indexOfValue(panel.targetLanguages, lower(cfg.translation.outputLanguage));

    if (const auto pair = translation::openAiManifest().checkPair(cfg.translation.inputLanguage,
                                                                  cfg.translation.outputLanguage);
        !pair)
    {
        panel.languagePairWarning = pair.detail;
    }

    // ------------------------------------------------- format and geometry
    for (const int rate : config::supportedSampleRates())
        panel.sampleRates.push_back({ std::to_string(rate), std::to_string(rate) + " Hz" });

    panel.selectedSampleRate = indexOfValue(panel.sampleRates, std::to_string(cfg.audio.sampleRate));

    const auto [bufferMin, bufferMax] = config::bufferFramesRange();
    const auto [gainMin, gainMax] = config::gainRange();
    const auto [channelMin, channelMax] = config::channelRange();
    const auto [jitterMin, jitterMax] = config::jitterBufferRange();

    panel.bufferFrames = cfg.audio.bufferFrames;
    panel.bufferMinFrames = bufferMin;
    panel.bufferMaxFrames = bufferMax;
    panel.inputChannel = cfg.audio.inputChannel;
    panel.outputChannel = cfg.audio.outputChannel;
    panel.maxChannel = channelMax;
    (void)channelMin;   // one-based is structural; the UI clamps at the bounds validate uses

    // ------------------------------------------------------------ live knobs
    panel.gainMinDb = gainMin;
    panel.gainMaxDb = gainMax;
    panel.jitterMaxMs = jitterMax;
    (void)jitterMin;

    // Engine state, not the file: what the callback is doing is what the operator
    // should see while dragging; the config catches up at release.
    panel.inputGainDb = engine.inputGainDb();
    panel.outputGainDb = engine.outputGainDb();
    panel.appliedInputGainDb = engine.appliedInputGainDb();
    panel.appliedOutputGainDb = engine.appliedOutputGainDb();
    panel.inputMuted = engine.inputMuted();
    panel.outputMuted = engine.outputMuted();
    panel.jitterMs = engine.jitterBufferMs();

    panel.inputMeter = meterView(engine, 0, true);
    panel.outputMeter = meterView(engine, 0, false);

    // Consumed by this build - see the header: the lamp is the only consumer.
    panel.inputMeter.clipping = engine.takeInputClipIndicator();
    panel.outputMeter.clipping = engine.takeOutputClipIndicator();

    // ------------------------------------------------------------- readouts
    {
        // The pipeline's own delay, from the product's one arithmetic (task 017),
        // and explicitly only that: translation latency is a measured property of
        // the rig (task 018), and inventing a number for it here would be the
        // AGENTS.md 19 kind of lie.
        const LatencyEstimate latency = estimateBufferDelay(engine, cfg);

        if (latency.blockMs == 0)
        {
            panel.latencySummary = "Buffer delay: unknown until a device or settings give a sample rate.";
        }
        else
        {
            panel.latencySummary = std::format(
                "Pipeline buffer ~{} ms ({} in + {} out + {} pre-roll, {}); translation latency "
                "is NOT included (measured in task 018)",
                latency.totalMs, latency.blockMs, latency.blockMs,
                latency.totalMs - latency.blockMs * 2, latency.source);
        }
    }

    const auto diag = controller.diagnostics().snapshot();

    panel.counters = {
        { "audio blocks", formatCount(diag.audioBlocks) },
        { "underruns", formatCount(diag.underruns) },
        { "overruns", formatCount(diag.overruns) },
        { "ring dropped", formatCount(engine.inputRingDroppedFrames()) },
        { "capture submitted", formatCount(diag.translationSubmittedFrames) },
        { "capture gap-refused", formatCount(diag.translationGapFrames) },
        { "translated audio frames", formatCount(diag.translatedAudioFrames) },
        { "rejected (wrong rate)", formatCount(diag.rejectedAudioFrames) },
        { "dropped (buffer full)", formatCount(diag.translatedAudioDroppedFrames) },
        { "clip frames in / out", formatCount(engine.inputClippedFrames())
                                      + " / " + formatCount(engine.outputGainClippedFrames()) },
        { "translation errors (fatal)", formatCount(diag.translationErrors)
                                            + " (" + formatCount(diag.translationFatalErrors) + ")" },
        { "reconnects", formatCount(diag.reconnects) },
        { "jitter fill", std::format("{:.0f} ms",
                                     engine.sampleRate() > 0
                                         ? engine.jitterFillFrames() * 1000.0 / static_cast<double>(engine.sampleRate())
                                         : 0.0) },
        { "malformed callbacks", formatCount(engine.malformedCallbacks()) },
        { "NDI errors", formatCount(diag.ndiErrors) },
    };

    const auto text = controller.textPipeline().snapshot();
    panel.currentSubtitle = text.currentLine;

    for (const auto& event : controller.textPipeline().recentHistory(kSubtitleHistoryTail))
        panel.subtitleHistory.push_back(event.text);

    panel.textSummary = std::format("partial {} - final {} - evicted {} - duplicates {}",
                                    controller.textPipeline().partialEvents(),
                                    controller.textPipeline().finalEvents(),
                                    controller.textPipeline().evictedLines(),
                                    controller.textPipeline().ignoredDuplicates());

    panel.actionNote = actionNote;
    return panel;
}

std::vector<UiOption> logLevelChoices()
{
    std::vector<UiOption> choices;
    choices.reserve(log::allLevels().size());

    for (const auto level : log::allLevels())
        choices.push_back({ std::string(log::nameOf(level)), std::string(log::nameOf(level)) });

    return choices;
}

LatencyEstimate estimateBufferDelay(const AudioEngine& engine, const AppConfig& settings)
{
    LatencyEstimate estimate;

    const int liveBlock = engine.bufferBlockMs();

    if (liveBlock > 0)
    {
        estimate.blockMs = liveBlock;
        estimate.totalMs = engine.pipelineBufferDelayMs();
        estimate.fromEngine = true;
        estimate.source = "live pipeline";
        return estimate;
    }

    // Nothing running: the same arithmetic on the settings, labelled as such.
    const auto& audio = settings.audio;

    if (audio.sampleRate > 0 && audio.bufferFrames > 0)
    {
        estimate.blockMs = static_cast<int>(
            std::llround(static_cast<double>(audio.bufferFrames) * 1000.0 / audio.sampleRate));
        estimate.totalMs = estimate.blockMs * 2 + settings.translation.jitterBufferMs;
        estimate.source = "settings (not running)";
    }
    else
    {
        estimate.source = "unknown";
    }

    return estimate;
}

} // namespace liveai
