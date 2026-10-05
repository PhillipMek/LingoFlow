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

    // Task 019. The badge is the mounted plan, extended with the worker's real
    // state: a banner that promised "loopback on" while the worker failed to
    // start would sell a developer fiction - the opposite of what this banner
    // exists to prevent. Production: the plan's badge is empty, so is this.
    {
        const auto& plan = controller.developerPlan();
        std::string badge = plan.badge;

        if (plan.loopback)
            badge += controller.loopbackActive()
                       ? " | loopback worker RUNNING, "
                         + std::to_string(controller.loopbackTransferredFrames()) + " frames moved"
                       : " | loopback NOT running (see log)";

        panel.developerBadge = std::move(badge);
    }

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

    // The 018 accounting rides the same panel as everything else - the screen and
    // the export render one function's rows, so they cannot tell two stories.
    panel.latencyRows = latencyAccounting(engine, controller.audioBackend(), diag, cfg);

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

std::vector<LatencyRow> latencyAccounting(const AudioEngine& engine,
                                          const audio::IAudioBackend* backend,
                                          const DiagnosticsManager::Snapshot& diag,
                                          const AppConfig& settings)
{
    std::vector<LatencyRow> rows;

    const int rate = engine.sampleRate() > 0 ? engine.sampleRate() : settings.audio.sampleRate;
    const LatencyEstimate buffers = estimateBufferDelay(engine, settings);

    const auto msOf = [rate](std::uint64_t frames)
    { return rate > 0 ? static_cast<double>(frames) * 1000.0 / rate : 0.0; };

    // 1) What the driver itself says, both directions. A device that answered
    // nothing (or with the ASIO query's inherent "0 = unknown") is stated as not
    // measured - it contributes 0.0 to the total below and says so in its row.
    const audio::DeviceCapabilities caps =
        backend != nullptr ? backend->capabilities() : audio::DeviceCapabilities{};

    double driverInMs = 0.0;
    double driverOutMs = 0.0;

    if (backend == nullptr)
    {
        rows.push_back({ "asio input latency", "no device open - nothing reported", "not measured" });
        rows.push_back({ "asio output latency", "no device open - nothing reported", "not measured" });
    }
    else if (caps.inputLatencySamples > 0 || caps.outputLatencySamples > 0)
    {
        if (rate > 0 && caps.inputLatencySamples > 0)
        {
            driverInMs = msOf(static_cast<std::uint64_t>(caps.inputLatencySamples));
            rows.push_back({ "asio input latency",
                             std::format("{:.1f} ms ({} frames, reported by the driver)",
                                         driverInMs, caps.inputLatencySamples),
                             "driver-reported" });
        }
        else if (caps.inputLatencySamples > 0)
        {
            rows.push_back({ "asio input latency",
                             std::to_string(caps.inputLatencySamples) + " frames (no sample rate yet)",
                             "driver-reported" });
        }
        else
        {
            rows.push_back({ "asio input latency", "not reported by this driver", "not measured" });
        }

        if (rate > 0 && caps.outputLatencySamples > 0)
        {
            driverOutMs = msOf(static_cast<std::uint64_t>(caps.outputLatencySamples));
            rows.push_back({ "asio output latency",
                             std::format("{:.1f} ms ({} frames, reported by the driver)",
                                         driverOutMs, caps.outputLatencySamples),
                             "driver-reported" });
        }
        else if (caps.outputLatencySamples > 0)
        {
            rows.push_back({ "asio output latency",
                             std::to_string(caps.outputLatencySamples) + " frames (no sample rate yet)",
                             "driver-reported" });
        }
        else
        {
            rows.push_back({ "asio output latency", "not reported by this driver", "not measured" });
        }
    }
    else
    {
        rows.push_back({ "asio input latency", "not reported by this driver", "not measured" });
        rows.push_back({ "asio output latency", "not reported by this driver", "not measured" });
    }

    // 2) The block arithmetic (the callback's granularity, in both directions).
    const std::string blockKind = buffers.fromEngine ? "arithmetic (live geometry)"
                                                     : "arithmetic (settings, not running)";
    if (buffers.blockMs > 0)
    {
        rows.push_back({ "capture block in", std::to_string(buffers.blockMs) + " ms", blockKind });
    }
    else
    {
        rows.push_back({ "capture block in", "no geometry yet", "not measured" });
    }

    // 3) Network + model, ONE combined row: the audio the translator has not
    // returned. gap-refused frames never entered the wire (012 counts them
    // separately, not here); everything that came back - accepted, rejected or
    // dropped - has left the path. This is a computed backlog, not a split of
    // wire time from server time: that split would need a provider-side
    // timestamp this API does not offer, and inventing one is AGENTS.md 19.
    const std::uint64_t returned = diag.translatedAudioFrames + diag.rejectedAudioFrames
                                   + diag.translatedAudioDroppedFrames;

    double inFlightMs = 0.0;

    if (diag.translationSubmittedFrames == 0 && returned == 0)
    {
        rows.push_back({ "network + model (audio in flight)", "nothing submitted yet", "not measured" });
    }
    else if (rate <= 0)
    {
        rows.push_back({ "network + model (audio in flight)", "no sample rate yet", "not measured" });
    }
    else
    {
        const auto signedInFlight = static_cast<long long>(diag.translationSubmittedFrames)
                                    - static_cast<long long>(returned);
        const long long inFlight = signedInFlight > 0 ? signedInFlight : 0;
        inFlightMs = msOf(static_cast<std::uint64_t>(inFlight));

        rows.push_back({ "network + model (audio in flight)",
                         std::format("{:.1f} ms ({} frames waiting to come back)", inFlightMs, inFlight),
                         "live-computed; network and model are NOT separated (no provider-side "
                         "timestamp exists); clamped at 0 across threads" });
    }

    // 4) The output queue, live and configured.
    const std::uint64_t fill = engine.jitterFillFrames();

    if (rate > 0 && fill > 0)
    {
        rows.push_back({ "jitter queue (live fill)",
                         std::format("{:.1f} ms ({} frames)", msOf(fill), fill),
                         "live-computed (atomic)" });
    }
    else
    {
        rows.push_back({ "jitter queue (live fill)", "empty", "live-computed (atomic)" });
    }

    rows.push_back({ "jitter pre-roll target", std::to_string(engine.jitterBufferMs()) + " ms",
                     "configuration" });

    if (buffers.blockMs > 0)
    {
        rows.push_back({ "playback block out", std::to_string(buffers.blockMs) + " ms", blockKind });
    }
    else
    {
        rows.push_back({ "playback block out", "no geometry yet", "not measured" });
    }

    // 5) The total of exactly the labeled rows. Driver latencies contribute when
    // reported and contribute 0.0 when not - visibly in this sentence, never as
    // an invisible zero. The jitter TARGET (not the live fill) is what the
    // audience waits on during steady playback. Mouth-to-ear stays the venue's
    // blank in docs/latency-budget.md; this sum is an accounting, not a
    // measurement, and its kind says so.
    const double totalMs = driverInMs + static_cast<double>(buffers.blockMs) + inFlightMs
                           + engine.jitterBufferMs() + static_cast<double>(buffers.blockMs)
                           + driverOutMs;

    rows.push_back({ "estimated total (labeled rows)",
                     std::format("{:.1f} ms", totalMs),
                     "sum of the rows above; unreported driver latencies count as 0.0; NOT "
                     "mouth-to-ear (see docs/latency-budget.md)" });

    return rows;
}

} // namespace liveai
