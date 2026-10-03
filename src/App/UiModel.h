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
    bool faulted = false;              ///< the Start button must offer Retry (clearFault)
    bool canStart = false;
    bool canStop = false;

    // ------------------------------------------------------------ devices
    std::vector<UiOption> devices;
    int selectedDevice = -1;           ///< index into devices, -1 = nothing selected
    std::string deviceNote;            ///< why the list can be empty, in plain words

    // ---------------------------------------------------------- languages
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
    int bufferMaxFrames = 0;           ///< slider cannot offer a refused value
    int inputChannel = 1;
    int outputChannel = 1;
    int maxChannel = 128;              ///< same source as validate()'s bound

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

    // ----------------------------------------------------------- readouts
    std::string latencySummary;
    std::vector<std::pair<std::string, std::string>> counters;

    std::string currentSubtitle;               ///< the open line from task 013
    std::vector<std::string> subtitleHistory;  ///< tail, oldest first
    std::string textSummary;                   ///< partial/final/evicted in one line

    std::string actionNote;                    ///< last updateSettings outcome, or empty
};

/// How many history lines the panel carries to the window. A display depth,
/// not a product rule - the pipeline's own bound is the real one.
inline constexpr std::size_t kSubtitleHistoryTail = 8;

/// Log level names in the order Utils/Log defines them. The settings dialog's
/// selector is built from here and from nowhere else, so the names on screen
/// and the names validate() round-trips can never drift (task 015, same rule
/// as every other list on this screen).
std::vector<UiOption> logLevelChoices();

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

} // namespace liveai
