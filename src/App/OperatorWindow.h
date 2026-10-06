#pragma once
//
// OperatorWindow - the JUCE shell of task 014. It owns WIDGETS and nothing else:
// every value it paints comes from buildOperatorPanel() (App/UiModel), and every
// action it takes is exactly one call into ApplicationController. No device is
// opened, no audio computed, no socket touched from this file - the FAIL
// criterion "UI owns backend/audio logic" is kept out by construction.
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
/// UiMeterView and draws it; it owns no audio state.
class MeterBar final : public juce::Component
{
public:
    void setLevels(const UiMeterView& view);
    void paint(juce::Graphics& g) override;

private:
    static float positionForDb(float db) noexcept;   ///< -60..0 dBFS across the bar

    UiMeterView view_;
};

/// The operator screen itself: the UI-01 information architecture. Header ->
/// system status -> audio (routing, channels, meters, gains, mutes) ->
/// translation (pair, live text) -> compact live health -> commands. Raw
/// counters, the full latency accounting and the engineering text summary
/// moved to the Diagnostics window; sample rate, buffer size and jitter
/// pre-roll - set before a show, not during one - moved to Settings.
/// Owned as the content component of OperatorWindow; it lays itself out (the
/// JUCE 9 way) and polls the UiModel on a timer.
class OperatorContent final : public juce::Component, private juce::Timer
{
public:
    explicit OperatorContent(ApplicationController& controller);
    ~OperatorContent() override;   ///< defined where SettingsWindow is complete

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

    ApplicationController& controller_;
    std::string actionNote_;

    // ---------------------------------------------------------------- header
    juce::Label titleLabel_;
    juce::TextButton startButton_ { "Start" };
    juce::TextButton stopButton_ { "Stop" };
    juce::TextButton settingsButton_ { "Settings..." };
    juce::TextButton diagnosticsButton_ { "Diagnostics..." };
    juce::Label devBadge_;   ///< task 019: the developer-mode band, invisible in production

    // ---------------------------------------------------------- system status
    juce::Label statusHeader_;
    juce::Label appValue_, audioValue_, sessionValue_, ndiValue_;
    juce::Label detailLabel_;
    juce::Label credentialLabel_;

    // ------------------------------------------------------------------ audio
    juce::Label audioHeader_;
    juce::Label deviceCaption_, deviceNoteLabel_;
    juce::ComboBox deviceChoice_;
    juce::TextButton refreshDevicesButton_ { "Refresh devices" };
    /// Discrete by nature (UI-01 §4): channel is an index, so it gets a list,
    /// not a fader - the slider could be parked between two channels.
    juce::Label inputChannelCaption_, outputChannelCaption_, channelNoteLabel_;
    juce::ComboBox inputChannelChoice_, outputChannelChoice_;

    juce::Label inputMeterCaption_, outputMeterCaption_;
    MeterBar inputMeter_, outputMeter_;
    juce::Slider inputGainSlider_, outputGainSlider_;
    juce::Label appliedInputLabel_, appliedOutputLabel_;
    juce::TextButton inputMuteButton_ { "MUTE IN" };
    juce::TextButton outputMuteButton_ { "MUTE OUT" };

    // ------------------------------------------------------------- translation
    juce::Label translationHeader_;
    juce::Label sourceCaption_, targetCaption_, pairWarningLabel_;
    juce::ComboBox sourceChoice_, targetChoice_;
    juce::Label subtitleCaption_, currentSubtitleLabel_, historyLabel_;

    // -------------------------------------------------------------- live health
    juce::Label healthHeader_;
    juce::Label latencyLabel_, healthLabel_;

    juce::Label noteLabel_;

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
