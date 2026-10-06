#pragma once
//
// DiagnosticsWindow - the dedicated engineering surface UI-01 carved out of the
// main screen, structured by UI-03: five cards (Audio health, Translation
// health, Subtitles/NDI, Latency, Runtime) plus an expandable Raw details area,
// drawn as aligned label/value tables - not a wall of monospace text. Monospace
// survives in exactly one place: the raw event list, which IS a log.
//
// Same widget discipline as every other window (tasks 014/015/019): paint-only.
// Every row is pre-built by buildDiagnosticsPanel() from public controller
// reads; this file positions text and never computes a value. The card
// rectangles are computed in resized() and consumed by paint().
//
// Section 12's promise lives in the data: the NDI card carries the audio state
// row beside it, so "NDI died, is the show over?" answers itself in one glance.
// Section 14's promise lives there too: uptime/CPU/memory read "not measured"
// because this application does not measure them - no cosmetic sampler was
// added to make the row look nicer.

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
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Recompute every section from the model. Public because the frame calls
    /// it on reopen (the same rule the settings dialog uses for its refresh).
    void rebuild();

private:
    void timerCallback() override;
    void exportPressed();
    void rawToggled();

    /// One label/value row inside a card; state words colour themselves via
    /// the shared stateColour mapping (the same words, the same meanings).
    void drawRows(juce::Graphics& g, juce::Rectangle<int>& area,
                  const std::vector<std::pair<std::string, std::string>>& rows) const;
    void drawCard(juce::Graphics& g, const juce::Rectangle<int>& card,
                  const juce::String& title) const;

    ApplicationController& controller_;
    DiagnosticsPanel panel_;
    std::string actionNote_;
    bool showRaw_ = false;

    juce::Rectangle<int> audioCard_, translationCard_, subtitlesCard_, runtimeCard_,
                         latencyCard_, rawCard_;

    juce::Label noteLabel_;
    juce::TextButton exportButton_ { "Export diagnostics" };
    juce::TextButton rawButton_ { "Show raw details" };

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
