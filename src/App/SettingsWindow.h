#pragma once
//
// SettingsWindow - the task 015 settings screen: everything the operator edits
// less than once a show (instructions, model hint, recovery policy, NDI,
// diagnostics) and the credential that must never live in settings (the API
// key). It follows the same rule as OperatorWindow (task 014): widgets only.
// Every value comes from and goes to the controller - updateSettings for the
// configuration (validate -> apply live what applies -> persist, one funnel),
// and the credential methods for the key (UI field -> secure store, the value
// skips settings, notes and logs entirely).
//
// The draft model: widgets are edited freely and reach the subsystems only at
// "Apply settings" - one atomic commit per click, so a half-typed instruction
// cannot land in the file, and a refused commit keeps the operator's draft on
// screen with the schema's reason in the note (nothing was applied - ConfigManager
// guarantees that, task 003).
//
// Credential rules visible right here (AGENTS.md 10, task 015 PASS criteria):
// the key field masks its echo, a successful store clears the field (the value
// must not sit on screen), and no label ever shows more than "stored/not".

#include <functional>
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

    /// Re-reads the configuration and the credential presence into the widgets.
    /// Called on every open, so a dialog that was only hidden never shows stale
    /// numbers, and after a credential action so the presence line is current.
    void refreshFromSettings();

    void resized() override;

private:
    void timerCallback() override;   ///< presence-only refresh

    /// One atomic commit of the whole draft through the controller funnel.
    void applyPressed();
    void saveKeyPressed();
    void removeKeyPressed();
    void showNote();

    ApplicationController& controller_;
    std::string actionNote_;

    // ------------------------------------------------------------ credentials
    juce::Label credentialCaption_, credStatusLabel_, keyHintLabel_;
    juce::TextEditor apiKeyEditor_;
    juce::TextButton saveKeyButton_ { "Store key" };
    juce::TextButton removeKeyButton_ { "Remove key" };

    // ------------------------------------------------------------ translation
    juce::Label instructionsCaption_, modelHintCaption_, reconnectCaption_, initialBackoffCaption_,
               maxBackoffCaption_, sessionAgeCaption_;
    juce::TextEditor instructionsEditor_, modelHintEditor_;
    juce::ToggleButton reconnectToggle_ { "Reconnect after a dropped session" };
    juce::Slider initialBackoffSlider_, maxBackoffSlider_, sessionAgeSlider_;

    // -------------------------------------------------------------------- NDI
    juce::Label ndiCaption_, streamNameCaption_;
    juce::ToggleButton ndiToggle_ { "Publish NDI output" };
    juce::TextEditor streamNameEditor_;

    // -------------------------------------------------------------- diagnostics
    juce::Label logLevelCaption_;
    juce::ComboBox logLevelChoice_;
    juce::ToggleButton logFileToggle_ { "Write the log file" };    juce::Label restartHintLabel_;

    // ------------------------------------------------- developer / mock mode (019)
    // The whole band renders inert unless developer mode is enabled - that is
    // the plan's job, not the widgets'; here the rule is only that the draft
    // carries exactly what is on screen and the hint states when edits count.
    juce::Label developerCaption_, devSourceCaption_, devWavInCaption_, devWavOutCaption_,
                mockLatencyCaption_, developerHintLabel_;
    juce::ToggleButton developerToggle_ { "Developer mode (simulated source / mock translation / loopback)" };
    juce::ToggleButton mockToggle_ { "Mock translation: echo + labelled mock text, no provider session" };
    juce::ToggleButton loopbackToggle_ { "Loopback: capture to the audience; the translator is NOT fed" };
    juce::ComboBox devSourceChoice_;
    juce::TextEditor devWavInEditor_, devWavOutEditor_;
    juce::Slider devMockLatencySlider_;
    juce::Label devToneFreqCaption_, devToneLevelCaption_;
    juce::Slider devToneFreqSlider_, devToneLevelSlider_;

    juce::TextButton applyButton_ { "Apply settings" };
    juce::Label noteLabel_;

    std::vector<UiOption> logLevelCache_;
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
