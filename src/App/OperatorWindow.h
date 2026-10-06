#pragma once
//
// OperatorWindow - the JUCE shell of task 014, laid out for UI-02's operator
// screen. It owns WIDGETS and nothing else: every value it paints comes from
// buildOperatorPanel() (App/UiModel), and every action it takes is exactly one
// call into ApplicationController. No device is opened, no audio computed, no
// socket touched from this file - the FAIL criterion "UI owns backend/audio
// logic" is kept out by construction.
//
// The visual contract (UI-02, on top of UI-01's information architecture):
//   header -> SYSTEM STATUS card -> AUDIO card (paired INPUT/OUTPUT columns)
//   -> TRANSLATION card -> HEALTH card (with the door to Diagnostics)
//   -> command bar (START/STOP). Colour is state only; the spacing scale is
//   4/8/12/16/24/32; no monospace and no engineering explanations on this
//   screen - the applied-glide detail and the arithmetic behind the latency
//   line live in the Diagnostics window.
//
// Threading: everything runs on the message thread. The timer (100 ms) is a
// repaint clock over atomic snapshots - a slow paint stalls paint only, never
// audio. The updatingWidgets_ guard is why a programmatic refresh cannot echo
// back as an operator action.
//
// Option lists (devices, languages, channels) live only as caches mirrored from
// the panel - the window holds no list of its own, so there is nothing that
// could drift out of sync with the registry or the schema.
//
// Slider wiring (this JUCE version exposes onValueChange/onDragStart/onDragEnd):
// only the gains remain sliders on this screen - dragging fires the LIVE call
// only (no disk); the commit that validates and persists happens once at drag
// end - or immediately for a typed text value, which arrives while no drag is
// active. Channels are discrete combo choices; they commit directly on change.

#include <functional>
#include <string>
#include <vector>

#include <JuceHeader.h>

#include "App/ApplicationController.h"
#include "App/UiModel.h"

namespace liveai {

class SettingsWindow;      // the task 015 dialog, owned while hidden and shown
class DiagnosticsWindow;   // the UI-01 engineering surface, same ownership rule

/// A horizontal peak/RMS meter with a clipping lamp. Pure paint: it receives a
/// UiMeterView and draws it; it owns no audio state. The numeric readout lives
/// beside it (a Label the content window fills from the same view), never
/// inside the bar.
class MeterBar final : public juce::Component
{
public:
    void setLevels(const UiMeterView& view);
    void paint(juce::Graphics& g) override;

    /// Natural height of the card column for the current width - the
    /// viewport's scroll extent (see resized()).
    int preferredHeight() const;

private:
    static float positionForDb(float db) noexcept;   ///< -60..0 dBFS across the bar

    UiMeterView view_;
};

/// The operator screen itself: the UI-01 information architecture wearing the
/// UI-02 visual hierarchy. Raw counters, the full latency accounting and the
/// technical gain-glide detail are the Diagnostics window's business; this
/// screen answers "ready? routed? levels? translating? act?" in one glance.
class OperatorContent final : public juce::Component, private juce::Timer
{
public:
    explicit OperatorContent(ApplicationController& controller);
    ~OperatorContent() override;   ///< defined where SettingsWindow is complete

    void paint(juce::Graphics& g) override;

    /// Natural height of the card column for the current width - the
    /// viewport's scroll extent (see resized()).
    int preferredHeight() const;
    void resized() override;

private:
    // ------------------------------------------------------------ refresh
    void timerCallback() override;
    void rebuild();

    /// Replaces a ComboBox's items only when the list actually changed, so the
    /// 100 ms clock never disturbs an open dropdown mid-click. Selects
    /// selectedIndex (or clears the selection when it is -1).
    void syncOptions(juce::ComboBox& box, std::vector<UiOption>& cache,
                     const std::vector<UiOption>& fresh, int selectedIndex);

    // ------------------------------------------------------------- commands
    void startPressed();
    void stopPressed();
    void settingsPressed();
    void diagnosticsPressed();
    void refreshDevicesPressed();
    void deviceSelected();
    void sourceSelected();
    void targetSelected();
    void channelChanged();
    void gainMoved();
    void gainCommit();
    void muteToggled();

    /// Copy current settings, mutate, and push through controller.updateSettings;
    /// whatever it says becomes the visible action note. This is the ONLY way the
    /// window changes configuration - one funnel, no half-applied edits.
    void commitSettings(std::function<void(AppConfig&)> mutate);

    static juce::Colour stateColour(std::string_view state) noexcept;
    static juce::String stateGlyph(std::string_view state) noexcept;

    /// One status row: "Audio  РІвЂ”РЏ Running" - dot and word share the state's
    /// colour, the caption stays neutral. Painted as two labels per row.
    void layoutStatusRow(juce::Rectangle<int>& area, juce::Label& caption,
                         juce::Label& value) const;

    ApplicationController& controller_;
    std::string actionNote_;

    // ---------------------------------------------------------------- header
    juce::Label titleLabel_;
    juce::Label devBadge_;         ///< compact "DEVELOPER MODE" chip, hidden in production
    juce::TextButton settingsButton_ { "Settings" };

    // ---------------------------------------------------------- system status
    juce::Label statusHeader_;
    juce::Label appCaption_, audioCaption_, sessionCaption_, ndiCaption_;
    juce::Label appValue_, audioValue_, sessionValue_, ndiValue_;
    juce::Label detailLabel_;          ///< the actionable warning line (amber/red), empty when healthy
    juce::Label credentialLabel_;
    juce::Label devDetailLabel_;       ///< the plan's full truth, small, only when developer mode is on

    // ------------------------------------------------------------------ audio
    juce::Label audioHeader_;
    juce::Label deviceCaption_, deviceNoteLabel_;
    juce::ComboBox deviceChoice_;
    juce::TextButton refreshDevicesButton_ { "Refresh" };
    /// Discrete by nature (UI-01 Р’В§4): channel is an index, so it gets a list,
    /// not a fader - and a visibly different control from the gain slider.
    juce::Label inputChannelCaption_, outputChannelCaption_, channelNoteLabel_;
    juce::ComboBox inputChannelChoice_, outputChannelChoice_;

    juce::Label inputMeterCaption_, outputMeterCaption_;
    MeterBar inputMeter_, outputMeter_;
    juce::Label inputLevelLabel_, outputLevelLabel_;   ///< numeric level + peak, from the same view
    juce::Label inputGainCaption_, outputGainCaption_;
    juce::Slider inputGainSlider_, outputGainSlider_;
    juce::TextButton inputMuteButton_ { "MUTE INPUT" };
    juce::TextButton outputMuteButton_ { "MUTE OUTPUT" };

    // ------------------------------------------------------------- translation
    juce::Label translationHeader_;
    juce::Label sourceCaption_, targetCaption_, arrowLabel_, pairWarningLabel_;
    juce::ComboBox sourceChoice_, targetChoice_;
    juce::Label sessionLineLabel_;         ///< "Connected" beside the pair - the same state word
    juce::Label currentSubtitleLabel_;     ///< the live line, quoted
    juce::Label historyLabel_;             ///< compact tail of recent lines

    // -------------------------------------------------------------- live health
    juce::Label healthHeader_;
    juce::Label latencyCaption_, latencyValue_;
    juce::Label jitterCaption_, jitterValue_;
    juce::Label underrunCaption_, underrunValue_;
    juce::Label reconnectCaption_, reconnectValue_;
    juce::TextButton diagnosticsButton_ { "Diagnostics" };

    juce::Label noteLabel_;                ///< the operator's own last result

    // ------------------------------------------------------------- command bar
    juce::TextButton startButton_ { "Start" };
    juce::TextButton stopButton_ { "Stop" };

    // Card rectangles, computed in resized() and painted in paint().
    juce::Rectangle<int> statusCard_, audioCard_, translationCard_, healthCard_;

    // Option caches: the window's only lists, mirrored from the panel.
    std::vector<UiOption> deviceCache_, sourceCache_, targetCache_;
    std::vector<UiOption> inputChannelCache_, outputChannelCache_;

    /// The task 015 dialog: created on first open, hidden on close, always
    /// re-read before it is shown again - the settings funnel stays singular.
    std::unique_ptr<SettingsWindow> settingsWindow_;

    /// The UI-01 diagnostics surface, same lifetime rule.
    std::unique_ptr<DiagnosticsWindow> diagnosticsWindow_;

    bool updatingWidgets_ = false;
    bool gainDragging_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OperatorContent)
};

/// The DocumentWindow wrapper: nothing but a frame, the title bar and the quit
/// route - all substance lives in OperatorContent.
class OperatorWindow final : public juce::DocumentWindow
{
public:
    explicit OperatorWindow(ApplicationController& controller);

    void closeButtonPressed() override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OperatorWindow)
};

} // namespace liveai
