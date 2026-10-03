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
// Option lists (devices, languages, rates) live only as caches mirrored from
// the panel - the window holds no list of its own, so there is nothing that
// could drift out of sync with the registry or the schema.
//
// Slider wiring (this JUCE version exposes onValueChange/onDragStart/onDragEnd):
// dragging fires the LIVE call only (no disk); the commit that validates and
// persists happens once at drag end - or immediately for a typed text value,
// which arrives while no drag is active.

#include <functional>
#include <string>
#include <vector>

#include <JuceHeader.h>

#include "App/ApplicationController.h"
#include "App/UiModel.h"

namespace liveai {

class SettingsWindow;   // the task 015 dialog, owned while hidden and shown

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

/// The operator screen itself. Owned as the content component of OperatorWindow;
/// it lays itself out (the JUCE 9 way) and polls the UiModel on a timer.
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
    void refreshDevicesPressed();
    void deviceSelected();
    void sourceSelected();
    void targetSelected();
    void rateSelected();
    void bufferChanged();
    void channelChanged();
    void gainMoved();
    void gainCommit();
    void jitterMoved();
    void jitterCommit();
    void muteToggled();

    /// Copy current settings, mutate, and push through controller.updateSettings;
    /// whatever it says becomes the visible action note. This is the ONLY way the
    /// window changes configuration - one funnel, no half-applied edits.
    void commitSettings(std::function<void(AppConfig&)> mutate);

    static juce::Colour stateColour(std::string_view state) noexcept;

    ApplicationController& controller_;
    std::string actionNote_;

    // ---------------------------------------------------------- header/status
    juce::Label titleLabel_;
    juce::TextButton startButton_ { "Start" };
    juce::TextButton stopButton_ { "Stop" };
    juce::TextButton settingsButton_ { "Settings..." };
    juce::Label appValue_, audioValue_, sessionValue_, ndiValue_;
    juce::Label detailLabel_;
    juce::Label credentialLabel_;

    // ---------------------------------------------------------- settings column
    juce::Label deviceCaption_, deviceNoteLabel_;
    juce::ComboBox deviceChoice_;
    juce::TextButton refreshDevicesButton_ { "Refresh devices" };
    juce::Label rateCaption_;
    juce::ComboBox rateChoice_;
    juce::Label bufferCaption_;
    juce::Slider bufferSlider_;
    juce::Label inputChannelCaption_, outputChannelCaption_;
    juce::Slider inputChannelSlider_, outputChannelSlider_;
    juce::Label sourceCaption_, targetCaption_, pairWarningLabel_;
    juce::ComboBox sourceChoice_, targetChoice_;

    // ---------------------------------------------------------- meters column
    juce::Label inputMeterCaption_, outputMeterCaption_;
    MeterBar inputMeter_, outputMeter_;
    juce::Slider inputGainSlider_, outputGainSlider_, jitterSlider_;
    juce::Label jitterCaption_;
    juce::Label appliedInputLabel_, appliedOutputLabel_;
    juce::TextButton inputMuteButton_ { "MUTE IN" };
    juce::TextButton outputMuteButton_ { "MUTE OUT" };
    juce::Label latencyLabel_;

    // ---------------------------------------------------------- readout column
    juce::Label countersLabel_;
    juce::Label subtitleCaption_, currentSubtitleLabel_, historyLabel_, textSummaryLabel_;
    juce::Label noteLabel_;

    // Option caches: the window's only lists, mirrored from the panel.
    std::vector<UiOption> deviceCache_, sourceCache_, targetCache_, rateCache_;

    /// The task 015 dialog: created on first open, hidden on close, always
    /// re-read before it is shown again - the settings funnel stays singular.
    std::unique_ptr<SettingsWindow> settingsWindow_;

    bool updatingWidgets_ = false;
    bool gainDragging_ = false;
    bool jitterDragging_ = false;
    bool geometryDragging_ = false;   ///< buffer + channels share the commit-at-release rule

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
