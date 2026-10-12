#pragma once
//
// DiagnosticsWindow - the dedicated engineering surface the UI redesign carved out of the
// main screen, structured by the UI redesign: five cards (Audio health, Translation
// health, Subtitles/NDI, Latency, Runtime) plus an expandable Raw details area,
// drawn as aligned label/value tables - not a wall of monospace text. Monospace
// survives in exactly one place: the raw event list, which IS a log.
//
// Since the venue layout pass (task 4 §5) the cards live in a
// ui::ScrollablePage: the surface is as tall as its content and scrolls
// vertically inside a window-sized frame, so nothing below the fold is cut
// off any more. The raw list prints EVERY ring line now - earlier only the
// tail that fit was drawn, and the events above the fold were unreachable.
// A reader who scrolled up into history stays there while new events arrive;
// a reader watching the live tail is followed (setFollowsNewContent).
//
// The buttons and the note line sit on a fixed strip OUTSIDE the scroll -
// Export and the raw toggle stay reachable wherever the reading position is.
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
#include "App/UiWidgets.h"

namespace liveai {

/// The scrolled surface: owns the panel snapshot and paints every card. Its
/// WIDTH and HEIGHT are managed by the ui::ScrollablePage that hosts it (the
/// page asks preferredHeight() for the width the viewport offers).
class DiagnosticsContent final : public ui::ScrollablePage::Body, private juce::Timer
{
public:
    explicit DiagnosticsContent(ApplicationController& controller);

    void paintContent(juce::Graphics& g, const juce::Rectangle<int>& area) override;
    void resized() override;

    /// Recompute every section from the model. Public because the frame calls
    /// it on reopen (the same rule the settings dialog uses for its refresh).
    void rebuild();

    /// The raw-details toggle lives on the fixed strip (the frame owns it);
    /// the expanded flag is this surface's layout fact.
    void setShowRaw(bool show);
    bool isShowingRaw() const noexcept { return showRaw_; }

    /// The scroll extent for the given width - the ui::ScrollablePage measure
    /// callback. Mirrors resized() by construction: both walk the same card
    /// heights from the same row counts.
    int preferredHeight(int width) const;

private:
    void timerCallback() override;

    /// One label/value row inside a card; state words colour themselves via
    /// the shared stateColour mapping (the same words, the same meanings).
    void drawRows(juce::Graphics& g, juce::Rectangle<int>& area,
                  const std::vector<std::pair<std::string, std::string>>& rows) const;
    void drawCard(juce::Graphics& g, const juce::Rectangle<int>& card,
                  const juce::String& title) const;

    ApplicationController& controller_;
    DiagnosticsPanel panel_;
    bool showRaw_ = false;

    juce::Rectangle<int> audioCard_, translationCard_, subtitlesCard_, runtimeCard_,
                         latencyCard_, rawCard_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiagnosticsContent)
};

/// The fixed strip around the scrolled surface: the Export / raw buttons and
/// the note line. They live OUTSIDE the viewport on purpose (venue task 4 §5:
/// "нижние элементы должны оставаться доступными") - the action buttons of a
/// window must not depend on where the reading position is.
class DiagnosticsFrame final : public juce::Component
{
public:
    explicit DiagnosticsFrame(ApplicationController& controller);

    DiagnosticsContent& surface() noexcept { return *content_; }

private:
    void exportPressed();
    void rawToggled();

    void resized() override;

    ApplicationController& controller_;
    ui::ScrollablePage* page_ = nullptr;         ///< owned by this
    DiagnosticsContent* content_ = nullptr;      ///< owned by the page's viewport

    juce::TextButton exportButton_ { "Export diagnostics" };
    juce::TextButton rawButton_ { "Show raw details" };
    juce::Label noteLabel_;
    std::string actionNote_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiagnosticsFrame)
};

/// Thin frame like the other two windows: closing hides, reopening refreshes.
/// Application-modal while open (venue task 4 §4) - the same mechanism and
/// the same no-nested-loop promise as the settings dialog.
class DiagnosticsWindow final : public juce::DocumentWindow
{
public:
    DiagnosticsWindow(ApplicationController& controller, juce::Component& ownerWindow);

    void reopen();
    void closeButtonPressed() override;

private:
    void becomeModal();

    DiagnosticsContent* content_ = nullptr;   ///< owned through the frame's viewport
    juce::Component& ownerWindow_;            ///< control returns here on dismissal

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiagnosticsWindow)
};

} // namespace liveai
