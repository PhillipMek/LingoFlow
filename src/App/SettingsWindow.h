#pragma once
//
// SettingsWindow - the configuration dialog, sectioned by the UI redesign and given its
// final shape by the UI redesign: six tabs (Audio, Translation, Subtitles / NDI,
// Credentials, Diagnostics, Advanced), one Apply button, one note line. The
// tab bar is a plain juce::TabbedComponent - the natural navigation the task
// asks for, not a framework. Since the venue layout pass (task 4 §3) each tab
// is a ui::ScrollablePage: a page taller than the window scrolls vertically
// instead of hiding its bottom rows, and the Apply row lives outside the
// scroll. The dialog is application-modal while open (task 4 §4) - see the
// frame's comment for the mechanism and the no-nested-loop promise.
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
// ConfigManager guarantees that). The Audio tab's device, channel
// and gain fields are the same configuration the operator screen edits live;
// both routes go through the one funnel and the one config object, so there
// is no second state to drift - the last Apply simply wins, as it must.
//
// Credential rules visible right here (the project rules, the credential PASS criteria,
// the UI redesign §5 wording): the key field masks its echo, a successful store clears
// the field (the value must not sit on screen), and the status line names
// where the key actually lives - "stored securely in <writable store>" only
// when the writable store holds it, the development-environment sentence
// otherwise. No label ever shows the value.
//
// Advanced (the UI redesign §7) carries what an operator never needs: developer/test
// mode with its simulated-source controls, and the instructions field the
// current provider ignores (labelled as such, read-only - the An earlier review
// decision stands). The loopback toggle's danger is spelled out in red the
// moment it is checked.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <JuceHeader.h>

#include "App/ApplicationController.h"
#include "App/UiModel.h"
#include "App/UiWidgets.h"
#include "Subtitles/SubtitleStyle.h"

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

    /// The dialog is going away (the close button, any dismissal): a running
    /// subtitle test leaves WITH the dialog - the request queues on the
    /// lifecycle worker, which is drained before the app can die, so the
    /// test sender never outlives the window that raised it.
    void stopNdiTestIfActive();

    void resized() override;

private:
    void timerCallback() override;   ///< presence + NDI state + test button truth

    /// One atomic commit of the whole draft through the controller funnel.
    void applyPressed();
    void saveKeyPressed();
    void removeKeyPressed();
    void exportPressed();
    void showNote();
    void loopbackVisuals();   ///< the red sentence while loopback is checked

    /// The Test NDI button's one handler: while no test runs it asks the
    /// controller to latch the typed caption (an on-the-spot refusal shows
    /// its note immediately, a queued start is confirmed by the timer);
    /// while a test runs the same button ends it.
    void ndiTestPressed();

    /// The R/G/B sliders edit whichever of the two colours the swatch buttons
    /// last aimed at (editingTextColour_); no new JUCE module is spent on a
    /// colour wheel - three sliders and two swatches are the same service,
    /// and the hex precision the venue operator actually wants.
    void applyColourEdit();
    void aimColourSelector(bool forText);

    /// Loads the schema defaults into the appearance widgets (still a draft -
    /// Apply commits it, exactly as every other field on this dialog).
    void resetAppearancePressed();

    /// The subtitles draft as the widgets currently carry it. A combo with no
    /// selection (a hand-edited config value outside the presets) keeps the
    /// committed value instead of silently rewriting it to a preset.
    subtitles::SubtitleSettings readSubtitleDraft() const;

    /// Loads one settings object into all subtitles-tab widgets (refresh and
    /// reset share this single writer).
    void fillSubtitleWidgets(const subtitles::SubtitleSettings&);

    // One layout function per tab. The page now lives inside a
    // ui::ScrollablePage: the function measures the content height for the
    // width the viewport offers AND positions the widgets in the same pass
    // (the ScrollablePage contract), so the page scrolls exactly as far as
    // its content is tall and never drifts (venue task 4 §3).
    int layoutAudioPage(int width);
    int layoutTranslationPage(int width);
    int layoutSubtitlesPage(int width);
    int layoutCredentialsPage(int width);
    int layoutDiagnosticsPage(int width);
    int layoutAdvancedPage(int width);

    ApplicationController& controller_;
    std::string actionNote_;

    std::unique_ptr<juce::TabbedComponent> tabs_;
    ui::ScrollablePage* audioPage_ = nullptr;         // owned by tabs_
    ui::ScrollablePage* translationPage_ = nullptr;
    ui::ScrollablePage* subtitlesPage_ = nullptr;
    ui::ScrollablePage* credentialsPage_ = nullptr;
    ui::ScrollablePage* diagnosticsPage_ = nullptr;
    ui::ScrollablePage* advancedPage_ = nullptr;

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
    juce::Label offBehaviorCaption_;      ///< "When the target is Off, the output:"
    juce::Label offBehaviorNoteLabel_;    ///< what the two behaviors do, in plain words
    juce::ComboBox sourceChoice_, targetChoice_, offBehaviorChoice_;
    juce::Label modelHintCaption_;
    juce::TextEditor modelHintEditor_;
    juce::ToggleButton reconnectToggle_ { "Reconnect after a dropped session" };
    juce::Label initialBackoffCaption_, maxBackoffCaption_, sessionAgeCaption_, jitterCaption_;
    juce::Slider initialBackoffSlider_, maxBackoffSlider_, sessionAgeSlider_, jitterSlider_;

    // ---------------------------------------------------------- subtitles/NDI
    juce::ToggleButton ndiToggle_ { "Publish NDI output" };
    juce::Label streamNameCaption_, ndiStateLabel_, ndiHintLabel_;
    juce::TextEditor streamNameEditor_;

    /// The subtitle test broadcast row (ApplicationController::requestNdiTest):
    /// the typed caption latches onto the wire through the real transport; the
    /// same button ends the test. ndiTestAwaiting_ remembers the click that
    /// queued a start; the timer reconciles the button with the controller's
    /// truth and surfaces a worker-side refusal as the note. ndiTestShown_ is
    /// the state the title currently shows, so the button is retitled only
    /// when the truth actually moved.
    juce::Label ndiTestCaption_, ndiTestHintLabel_;
    juce::TextEditor ndiTestEditor_;
    juce::TextButton ndiTestButton_ { "Test NDI" };
    bool ndiTestAwaiting_ = false;
    bool ndiTestShown_ = false;

    /// Transport draft selections: output mode and the video geometry. Lists
    /// are fixed presets (docs/ndi-protocol.md facts); a hand-edited config
    /// value that misses every preset arrives as one extra "custom" entry, so
    /// Apply never silently rewrites what the operator did not touch.
    juce::ComboBox ndiModeChoice_;
    juce::ComboBox ndiResolutionChoice_;
    juce::ComboBox ndiFpsChoice_;

    /// Appearance draft: the fields of subtitles::SubtitleStyle, edited
    /// against a live preview. Colors travel as "#RRGGBB" values picked in a
    /// modal ColourSelector (OK/Cancel - the draft changes only on OK).
    juce::Label fontFamilyCaption_, fontSizeCaption_, outlineWidthCaption_,
                shadowOffsetCaption_, shadowOpacityCaption_, alignCaption_,
                marginCaption_, blockWidthCaption_,
                lineSpacingCaption_, previewCaption_, ndiModeCaption_,
                ndiResolutionCaption_, ndiFpsCaption_;
    juce::TextEditor fontFamilyEditor_;
    juce::Slider fontSizeSlider_, outlineWidthSlider_, shadowOffsetSlider_,
                 shadowOpacitySlider_, marginSlider_, blockWidthSlider_,
                 lineSpacingSlider_;
    juce::ToggleButton shadowToggle_ { "Drop shadow" };
    juce::ToggleButton backgroundToggle_ { "Background box behind the text" };
    juce::ComboBox hAlignChoice_;
    juce::ComboBox vAnchorChoice_;
    juce::TextButton textColorButton_ { "Text colour" };
    juce::TextButton outlineColorButton_ { "Outline colour" };
    juce::TextButton resetAppearanceButton_ { "Reset appearance to defaults" };

    /// The full subtitle draft the widgets currently carry (mode + geometry +
    /// appearance), committed only at Apply. The preview and the colour
    /// selector read and write it; applyPressed copies it into the candidate,
    /// so the three always agree. editingTextColour_ says which colour the
    /// shared selector is aimed at.
    subtitles::SubtitleSettings subtitleDraft_;
    bool editingTextColour_ = true;   ///< which colour the R/G/B sliders edit

    /// The colour editor: three RGB sliders write the aimed colour (text or
    /// outline) into the draft; the swatch shows the live result and the two
    /// buttons aim the sliders at the chosen colour. Built on juce_gui_basics
    /// only - no juce_gui_extra / ColourSelector dependency.
    juce::Slider colourRSlider_, colourGSlider_, colourBSlider_;
    juce::Label colourSwatch_, colourEditorCaption_;

    class SubtitlePreview;
    std::unique_ptr<SubtitlePreview> subtitlePreview_;   ///< checkerboard + live render of the draft

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
    // Instructions: read-only and parked here, not in
    // Translation (the UI redesign §3): the current model ignores it; the label and the
    // hint say exactly that, and the value stays visible for a future model.
    juce::Label instructionsCaption_, instructionsHintLabel_;
    juce::TextEditor instructionsEditor_;

    // Developer / test mode: inert unless enabled - the plan's job,
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
    std::vector<UiOption> deviceCache_, sourceCache_, targetCache_, offBehaviorCache_;
    std::vector<UiOption> inputChannelCache_, outputChannelCache_;
    std::vector<std::string> devSourceCache_;   ///< developer.audioSource values, schema's list
    bool updatingWidgets_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsContent)
};

/// Thin frame. Closing hides it (the owner may reopen it and the values are
/// re-read); quitting the application happens only from the operator window.
/// The window is APPLICATION-MODAL while open (venue task 4 §4):
/// enterModalState blocks input to every other window, but it returns
/// immediately and runs no nested event loop - the message thread keeps
/// ticking, so the audio engine, the network threads and the operator
/// window's own refresh are untouched. Closing ends the modal state
/// explicitly and the JUCE dismissal callback returns control to the
/// parent window on the next message-loop pass.
class SettingsWindow final : public juce::DocumentWindow
{
public:
    SettingsWindow(ApplicationController& controller, juce::Component& ownerWindow);

    /// Re-reads the configuration, the option lists and the credential
    /// presence into the widgets, brings the window up and takes the
    /// application-modal state it always holds while open.
    void reopen();

    void closeButtonPressed() override;

private:
    void becomeModal();

    SettingsContent* content_ = nullptr;   ///< owned by the DocumentWindow
    juce::Component& ownerWindow_;         ///< control returns here on dismissal

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsWindow)
};

} // namespace liveai
