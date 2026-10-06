#pragma once
//
// SettingsWindow - the configuration dialog, sectioned by UI-01 and given its
// final shape by UI-03: six tabs (Audio, Translation, Subtitles / NDI,
// Credentials, Diagnostics, Advanced), one Apply button, one note line. The
// tab bar is a plain juce::TabbedComponent - the natural navigation the task
// asks for, not a framework.
//
// It follows the same rule as every other window (tasks 014/015): widgets
// only. Every value comes from and goes to the controller - updateSettings for
// the configuration (validate -> apply live what applies -> persist, one
// funnel), and the credential methods for the key (UI field -> secure store,
// the value skips settings, notes and logs entirely).
//
// The draft model: widgets are edited freely and reach the subsystems only at
// "Apply settings" - one atomic commit per click, so a half-typed instruction
// cannot land in the file, and a refused commit keeps the operator's draft on
// screen with the schema's reason in the note (nothing was applied -
// ConfigManager guarantees that, task 003). The Audio tab's device, channel
// and gain fields are the same configuration the operator screen edits live;
// both routes go through the one funnel and the one config object, so there
// is no second state to drift - the last Apply simply wins, as it must.
//
// Credential rules visible right here (AGENTS.md 10, task 015 PASS criteria,
// UI-03 §5 wording): the key field masks its echo, a successful store clears
// the field (the value must not sit on screen), and the status line names
// where the key actually lives - "stored securely in <writable store>" only
// when the writable store holds it, the development-environment sentence
// otherwise. No label ever shows the value.
//
// Advanced (UI-03 §7) carries what an operator never needs: developer/test
// mode with its simulated-source controls, and the instructions field the
// current provider ignores (labelled as such, read-only - the code review P1
// decision stands). The loopback toggle's danger is spelled out in red the
// moment it is checked.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <JuceHeader.h>

#include "App/ApplicationController.h"
#include "App/UiModel.h"

namespace liveai {

class SettingsContent final : public juce::Component, private juce::Timer
{
public:
    explicit SettingsContent(ApplicationController& controller);

    /// Re-reads the configuration, the option lists and the credential
    /// presence into the widgets. Called on every open, so a dialog that was
    /// only hidden never shows stale numbers, and after a credential action so
    /// the presence line is current.
    void refreshFromSettings();

    void resized() override;

private:
    void timerCallback() override;   ///< presence + NDI state only

    /// One atomic commit of the whole draft through the controller funnel.
    void applyPressed();
    void saveKeyPressed();
    void removeKeyPressed();
    void exportPressed();
    void showNote();
    void loopbackVisuals();   ///< the red sentence while loopback is checked

    // One layout function per tab; resized() calls them all (JUCE lays out
    // hidden tab pages too, and they are cheap).
    void layoutAudioPage();
    void layoutTranslationPage();
    void layoutSubtitlesPage();
    void layoutCredentialsPage();
    void layoutDiagnosticsPage();
    void layoutAdvancedPage();

    ApplicationController& controller_;
    std::string actionNote_;

    std::unique_ptr<juce::TabbedComponent> tabs_;
    juce::Component* audioPage_ = nullptr;         // owned by tabs_
    juce::Component* translationPage_ = nullptr;
    juce::Component* subtitlesPage_ = nullptr;
    juce::Component* credentialsPage_ = nullptr;
    juce::Component* diagnosticsPage_ = nullptr;
    juce::Component* advancedPage_ = nullptr;

    // ------------------------------------------------------------------ audio
    juce::Label deviceCaption_, deviceNoteLabel_, sampleRateCaption_, bufferCaption_;
    juce::ComboBox deviceChoice_;
    juce::TextButton refreshDevicesButton_ { "Refresh" };
    juce::ComboBox sampleRateChoice_;
    juce::Slider bufferSlider_;
    juce::Label inputChannelCaption_, outputChannelCaption_, channelNoteLabel_;
    juce::ComboBox inputChannelChoice_, outputChannelChoice_;
    juce::Label inputGainCaption_, outputGainCaption_;
    juce::Slider inputGainSlider_, outputGainSlider_;
    juce::Label audioRestartHintLabel_;

    // ------------------------------------------------------------ translation
    juce::Label sourceCaption_, targetCaption_, pairWarningLabel_;
    juce::Label sourceNoteLabel_;         ///< the expectation-vs-wire truth, in one sentence
    juce::ComboBox sourceChoice_, targetChoice_;
    juce::Label modelHintCaption_;
    juce::TextEditor modelHintEditor_;
    juce::ToggleButton reconnectToggle_ { "Reconnect after a dropped session" };
    juce::Label initialBackoffCaption_, maxBackoffCaption_, sessionAgeCaption_, jitterCaption_;
    juce::Slider initialBackoffSlider_, maxBackoffSlider_, sessionAgeSlider_, jitterSlider_;

    // ---------------------------------------------------------- subtitles/NDI
    juce::ToggleButton ndiToggle_ { "Publish NDI output" };
    juce::Label streamNameCaption_, ndiStateLabel_, ndiHintLabel_;
    juce::TextEditor streamNameEditor_;

    // ------------------------------------------------------------ credentials
    juce::Label credStatusLabel_, keyHintLabel_;
    juce::TextEditor apiKeyEditor_;
    juce::TextButton saveKeyButton_ { "Store key" };
    juce::TextButton removeKeyButton_ { "Remove key" };

    // -------------------------------------------------------------- diagnostics
    juce::Label logLevelCaption_, logHintLabel_;
    juce::ComboBox logLevelChoice_;
    juce::ToggleButton logFileToggle_ { "Write the log file" };
    juce::TextButton exportDiagnosticsButton_ { "Export diagnostics" };

    // ---------------------------------------------------------------- advanced
    // Instructions: read-only (code review P1) and parked here, not in
    // Translation (UI-03 §3): the current model ignores it; the label and the
    // hint say exactly that, and the value stays visible for a future model.
    juce::Label instructionsCaption_, instructionsHintLabel_;
    juce::TextEditor instructionsEditor_;

    // Developer / test mode (task 019): inert unless enabled - the plan's job,
    // not the widgets'. The draft carries what is on screen; the hint states
    // when edits count.
    juce::Label devSourceCaption_, devWavInCaption_, devWavOutCaption_,
                mockLatencyCaption_, developerHintLabel_;
    juce::ToggleButton developerToggle_ { "DEVELOPER / TEST MODE (simulated source, mock translation, loopback)" };
    juce::ToggleButton mockToggle_ { "Mock translation: echo + labelled mock text, no provider session" };
    juce::ToggleButton loopbackToggle_ { "Loopback: capture to the audience; the translator is NOT fed" };
    juce::ComboBox devSourceChoice_;
    juce::TextEditor devWavInEditor_, devWavOutEditor_;
    juce::Slider devMockLatencySlider_;
    juce::Label devToneFreqCaption_, devToneLevelCaption_;
    juce::Slider devToneFreqSlider_, devToneLevelSlider_;

    // ------------------------------------------------------------- bottom bar
    juce::TextButton applyButton_ { "Apply settings" };
    juce::Label noteLabel_;

    std::vector<UiOption> logLevelCache_, sampleRateCache_;
    std::vector<UiOption> deviceCache_, sourceCache_, targetCache_;
    std::vector<UiOption> inputChannelCache_, outputChannelCache_;
    std::vector<std::string> devSourceCache_;   ///< developer.audioSource values, schema's list
    bool updatingWidgets_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsContent)
};

/// Thin frame. Closing hides it (the owner may reopen it and the values are
/// re-read); quitting the application happens only from the operator window.
class SettingsWindow final : public juce::DocumentWindow
{
public:
    explicit SettingsWindow(ApplicationController& controller);

    /// Re-reads the settings into the widgets and brings the window up.
    void reopen();

    void closeButtonPressed() override;

private:
    SettingsContent* content_ = nullptr;   ///< owned by the DocumentWindow

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsWindow)
};

} // namespace liveai
