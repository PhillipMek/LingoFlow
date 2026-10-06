#pragma once
//
// UiModel - the thin layer of task 014. It is everything the operator window
// knows, computed from the controller's public read API and nothing else: the
// window paints these values and forwards commands through controller methods.
// The UI never touches engine internals, devices, protocols or config text
// (AGENTS.md 7, the task's FAIL criterion "UI owns backend/audio logic or
// freezes"), and because this file is portable, every mapping the window
// displays is testable without JUCE (tests/unit/TestUiModel.cpp).
//
// Formatting decisions that belong to the product and not to a widget live
// here on purpose: what a state word means, which options a selector offers
// (the registry, the schema - never a UI-local list), and what "estimated
// latency" honestly is: the pipeline's own buffer delay, computed from the
// live geometry, with the translation part named as unmeasured until task 018.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "App/ApplicationController.h"

namespace liveai {

/// One selectable item: `value` goes back to the controller (a device id, a
/// language code, a number as text), `label` is what the operator reads.
struct UiOption
{
    std::string value;
    std::string label;

    friend constexpr bool operator==(const UiOption&, const UiOption&) = default;
};

struct UiMeterView
{
    float peakDb = -120.0f;      ///< linearToDb floor: silence reads as -120, never -inf
    float rmsDb = -120.0f;
    bool signalPresent = false;  ///< the meter's documented -60 dBFS display threshold
    bool clipping = false;       ///< the engine's latched clip indicator, consumed by this build
};

/// One component row of the task 018 honest latency accounting (full contract at
/// `latencyAccounting` below the panel). The KIND is the point: a number's origin
/// (what the driver answered, what the config says, what the arithmetic derives,
/// what the live backlog computes, or nothing at all) is printed beside it and
/// can never be inferred away.
struct LatencyRow
{
    std::string component;   ///< "capture block in", "audio in the translator path", ...
    std::string value;       ///< formatted with its evidence, or the honest absence
    std::string kind;        ///< "driver-reported" | "configuration" | "arithmetic"
                             ///< | "live-computed" | "not measured"

    friend constexpr bool operator==(const LatencyRow&, const LatencyRow&) = default;
};

struct OperatorPanel
{
    // ------------------------------------------------------------- status
    std::string applicationState;
    std::string audioState;
    std::string sessionState;
    std::string ndiState;
    std::string audioBackendName;
    std::string detail;                ///< the freshest problem, or "ok"
    std::string credentialLine;        ///< task 015: where the API key stands, in one sentence
    std::string developerBadge;        ///< task 019: empty in production, unmistakable otherwise
    bool faulted = false;              ///< the Start button must offer Retry (clearFault)
    bool canStart = false;
    bool canStop = false;

    // ------------------------------------------------------------ devices
    std::vector<UiOption> devices;
    int selectedDevice = -1;           ///< index into devices, -1 = nothing selected
    std::string deviceNote;            ///< why the list can be empty, in plain words

    // ---------------------------------------------------------- languages
    // The provider auto-detects the spoken language and the wire carries only
    // the target (docs/openai-realtime-protocol.md section 5, R8). So the
    // main screen shows the target as a control and the source as a static,
    // honest line; sourceLanguages/selectedSource survive for the Settings
    // tab, where the operator records the EXPECTATION - LingoFlow-side
    // validation and diagnostics only, never a wire parameter.
    std::string sourceDisplay;         ///< "Automatic detection" - the truth of the wire
    std::vector<UiOption> sourceLanguages;
    std::vector<UiOption> targetLanguages;
    int selectedSource = -1;
    int selectedTarget = -1;
    std::string languagePairWarning;   ///< empty = the manifest supports this pair

    // ------------------------------------------------- format and geometry
    std::vector<UiOption> sampleRates;
    int selectedSampleRate = -1;       ///< index into sampleRates
    int bufferFrames = 0;
    int bufferMinFrames = 0;           ///< the schema's own range, exported so the
    int bufferMaxFrames = 0;           ///< selector cannot offer a refused value
    int maxChannel = 128;              ///< same source as validate()'s bound

    // ------------------------------------------------- channel selections
    // UI-01: channel is an INDEX - the main screen offers it as a discrete choice
    // list, never as a fader that could be parked between two channels. The
    // driver's own names drive the labels while a device is open; generic
    // "Channel N" numbering is used otherwise, and it says so (channelNote).
    std::vector<UiOption> inputChannelChoices;
    std::vector<UiOption> outputChannelChoices;
    int selectedInputChannel = -1;     ///< index into the choices, ghost included
    int selectedOutputChannel = -1;
    std::string channelNote;           ///< why the numbering is generic; empty = named
    int inputChannel = 1;              ///< the config truth, as text-in-list order too
    int outputChannel = 1;

    // ------------------------------------------------------------ live knobs
    float gainMinDb = 0.0f;            ///< config gainRange(): the UI never owns a window
    float gainMaxDb = 0.0f;
    float inputGainDb = 0.0f;          ///< the requested level (what the slider shows)
    float outputGainDb = 0.0f;
    float appliedInputGainDb = 0.0f;   ///< what the callback is applying right now (gliding)
    float appliedOutputGainDb = 0.0f;
    bool inputMuted = false;
    bool outputMuted = false;
    int jitterMs = 0;
    int jitterMaxMs = 0;
    UiMeterView inputMeter;
    UiMeterView outputMeter;

    // --------------------------------------------------------- live readouts
    // UI-01 shrank this to the operator's four scannable facts; the full 018
    // accounting and every raw counter live on in DiagnosticsPanel below,
    // rendered by the Diagnostics window. Relocated, never deleted.
    std::string latencySummary;        ///< the honest one-sentence buffer estimate
    std::vector<std::pair<std::string, std::string>> health;
                                       ///< latency/jitter/underruns/reconnects, compact

    std::string currentSubtitle;               ///< the open line from task 013
    std::vector<std::string> subtitleHistory;  ///< tail, oldest first

    std::string actionNote;                    ///< last updateSettings outcome, or empty
};

/// The engineering face of the same truth (UI-01 information architecture):
/// everything the main screen stopped showing. Built from the same controller
/// reads and the same formatting functions as the operator panel - the two
/// surfaces cannot tell two stories, they only decide who has to look.
/// Sections follow the task's diagnostics design: audio health, translation
/// health, subtitles (NDI + text pipeline), the full latency accounting with
/// its kinds (measured/estimated/absent stay distinguished), and runtime facts.
struct DiagnosticsPanel
{
    std::vector<std::pair<std::string, std::string>> audioHealth;
    std::vector<std::pair<std::string, std::string>> translationHealth;
    std::vector<std::pair<std::string, std::string>> subtitles;
    std::vector<LatencyRow> latency;
    std::vector<std::pair<std::string, std::string>> runtime;

    /// UI-03 §15: the un-formatted truth for the expandable "raw details"
    /// area - the event ring (newest last, eviction counted) and the
    /// configuration as it stands. Lines, not pairs: this is the one place a
    /// diagnostics surface may be a log. Built from the same public reads,
    /// and it carries no secret value (identifiers only, per the credential rules).
    std::vector<std::string> rawDetails;
};

/// How many history lines the panel carries to the window. A display depth,
/// not a product rule - the pipeline's own bound is the real one.
inline constexpr std::size_t kSubtitleHistoryTail = 8;

/// Log level names in the order Utils/Log defines them. The settings dialog's
/// selector is built from here and from nowhere else, so the names on screen
/// and the names validate() round-trips can never drift (task 015, same rule
/// as every other list on this screen).
std::vector<UiOption> logLevelChoices();

/// The product's one buffer-delay arithmetic (task 017): live engine geometry
/// first; before a device runs, the same arithmetic on the settings - what the
/// operator is told is then explicitly about what Start *will* run, never a
/// claim about sound that has not passed the pipeline. `source` names which
/// answered, because a number whose origin the reader cannot see is half a
/// number. This is NOT a latency measurement of any kind (mouth-to-ear is task
/// 018's); every user of these numbers carries that sentence.
struct LatencyEstimate
{
    int blockMs = 0;
    int totalMs = 0;   ///< block + block + jitter pre-roll
    bool fromEngine = false;
    std::string source;   ///< "live pipeline" | "settings (not running)"
};

/// Computes the estimate above. Non-realtime (reads atomics and config strings;
/// safe from the UI thread, pointless from anywhere else).
LatencyEstimate estimateBufferDelay(const AudioEngine& engine, const AppConfig& settings);

/// The whole accounting, in show order (input to audience), per task 018's
/// instruction: ASIO, network/server (one column honestly combined - this
/// product cannot split them without a provider-side timestamp, which would be
/// an invented field), output queue/jitter, and a total that adds up exactly the
/// labeled numbers and names what it excludes.
///
/// No number here is fabricated: driver latencies pass through as reported (or
/// "not reported by this driver"), buffers and pre-roll are configuration or
/// live atoms, the translator-path row is a COMPUTED backlog (submitted minus
/// everything that came back or was refused - the frames still in flight, at the
/// device rate). A nonzero backlog is the only defensible statement the sender
/// side can make about network+model time without a measurement rig; it is
/// labeled so and never summed into anything called measured. Mouth-to-ear
/// remains the venue's blank in docs/latency-budget.md.
///
/// `backend` may be null (no device story yet). Pure reads, non-realtime.
std::vector<LatencyRow> latencyAccounting(const AudioEngine& engine,
                                          const audio::IAudioBackend* backend,
                                          const DiagnosticsManager::Snapshot& diag,
                                          const AppConfig& settings);

/// Builds the panel from public reads. Non-realtime by construction (called on
/// the GUI thread); it allocates strings, which is exactly what a UI thread may
/// do and the audio callback may never. `actionNote` is carried in from the
/// window's last command so the operator sees their own result until it is
/// replaced.
///
/// The engine's clip latches are consumed here: the lamp is their only
/// consumer, and a poll that did not take them would show a clip from five
/// minutes ago as if it were now.
OperatorPanel buildOperatorPanel(ApplicationController& controller, const std::string& actionNote);

/// The diagnostics surface's counterpart, from the same public reads (UI-01).
/// The windows never duplicate the row-building: this is where relocated
/// engineering information lives now, and the Diagnostics window only paints it.
DiagnosticsPanel buildDiagnosticsPanel(ApplicationController& controller);

/// The discrete channel series for the selector (UI-01): `driverNames` are the
/// opened device's own labels - when present the list is exactly the channels
/// this device exposes, "1 - name" per entry, no invented maximum. An empty
/// `driverNames` means no device has been opened in this session, and the list
/// falls back to generic "Channel N" numbering up to the schema's validated
/// bound. Values are the one-based indices the config stores.
std::vector<UiOption> channelOptions(const std::vector<std::string>& driverNames,
                                     int schemaFallbackMax);

/// The configured channel stays selectable even when the (driver-named) list is
/// shorter than the stored index - the ghost keeps the honesty rule the devices
/// list uses: never silently drop what the settings file says. Appends the ghost
/// entry and returns the selection index for the list.
int selectChannelWithGhost(std::vector<UiOption>& options, int configuredChannel);

} // namespace liveai
