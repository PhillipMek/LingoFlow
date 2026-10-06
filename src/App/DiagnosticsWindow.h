#pragma once
//
// DiagnosticsWindow - the dedicated engineering surface UI-01 carved out of the
// main screen. The operator's window shows four health facts and a button to
// this one; this window shows everything the raw-counter wall used to:
// audio health, translation health, subtitles (NDI + text pipeline), the full
// task 018 latency accounting with its kinds, and runtime facts. Nothing was
// deleted to get here (AGENTS.md 19): information moved to the place an
// operator opens deliberately, next to the Export diagnostics receipt button
// that already lived in the header.
//
// Same widget discipline as every other window (tasks 014/015/019): paint-only.
// Every row is pre-built by buildDiagnosticsPanel() from public controller
// reads; this file formats lists of pairs and never computes a value.

#include <string>
#include <utility>
#include <vector>

#include <JuceHeader.h>

#include "App/ApplicationController.h"
#include "App/UiModel.h"

namespace liveai {

class DiagnosticsContent final : public juce::Component, private juce::Timer
{
public:
    explicit DiagnosticsContent(ApplicationController& controller);
    void resized() override;

    /// Recompute every section from the model. Public because the frame calls
    /// it on reopen (the same rule the settings dialog uses for its refresh).
    void rebuild();

private:
    void timerCallback() override;
    void exportPressed();

    /// The three metric sections render through the same 22-row block so the
    /// columns line up across the window; the row content is model-built.
    static std::string rows(const std::vector<std::pair<std::string, std::string>>& metricRows);

    ApplicationController& controller_;
    std::string actionNote_;   ///< the export receipt lives here now (UI-01)

    juce::Label audioHeader_, translationHeader_, subtitlesHeader_, latencyHeader_, runtimeHeader_;
    juce::Label audioRows_, translationRows_, subtitlesRows_, latencyRows_, runtimeRows_;
    juce::Label noteLabel_;
    juce::TextButton exportButton_ { "Export diagnostics" };

    std::size_t audioLines_ = 1, translationLines_ = 1, subtitlesLines_ = 1;
    std::size_t latencyLines_ = 1, runtimeLines_ = 1;   ///< sized by the model's rows

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiagnosticsContent)
};

/// Thin frame like the other two windows: closing hides, reopening refreshes.
class DiagnosticsWindow final : public juce::DocumentWindow
{
public:
    explicit DiagnosticsWindow(ApplicationController& controller);

    void reopen();
    void closeButtonPressed() override;

private:
    DiagnosticsContent* content_ = nullptr;   ///< owned by the DocumentWindow

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiagnosticsWindow)
};

} // namespace liveai
