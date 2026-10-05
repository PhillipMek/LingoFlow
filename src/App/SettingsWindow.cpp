#include "App/SettingsWindow.h"

#include <format>

#include "Config/ConfigSchema.h"

namespace liveai {
namespace {

juce::Font uiFont(float height = 15.0f)
{
    return juce::Font(juce::FontOptions().withHeight(height));
}

juce::Label& caption(juce::Label& label, const juce::String& text, juce::Component& parent)
{
    label.setFont(uiFont(12.0f));
    label.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    label.setText(text, juce::NotificationType::dontSendNotification);
    parent.addAndMakeVisible(label);
    return label;
}

juce::Slider& slider(juce::Slider& s, double min, double max, double interval,
                     juce::Component& parent)
{
    s.setRange(min, max, interval);
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 84, 22);
    parent.addAndMakeVisible(s);
    return s;
}

juce::TextEditor& editor(juce::TextEditor& e, juce::Component& parent)
{
    parent.addAndMakeVisible(e);
    return e;
}

std::string presenceLine(ApplicationController& controller)
{
    // Names and outcomes only. The value is not read here, and no path in this
    // file ever displays what the store holds (AGENTS.md 10).
    if (controller.hasApiSecret())
        return "API key: stored - " + controller.secretStoreName();

    return "API key: not stored - translation sessions will refuse until it is entered below";
}

} // namespace

// =========================================================================== SettingsContent

SettingsContent::SettingsContent(ApplicationController& controller)
    : controller_(controller)
{
    // ------------------------------------------------------------- credentials
    caption(credentialCaption_, "CREDENTIALS - stored in Windows secure storage, never in settings",
           *this);
    credStatusLabel_.setFont(uiFont(14.0f));
    credStatusLabel_.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
    addAndMakeVisible(credStatusLabel_);

    apiKeyEditor_.setPasswordCharacter(L'*');   // the field masks its echo (PASS criterion)
    addAndMakeVisible(apiKeyEditor_);

    saveKeyButton_.onClick = [this] { saveKeyPressed(); };
    removeKeyButton_.onClick = [this] { removeKeyPressed(); };
    addAndMakeVisible(saveKeyButton_);
    addAndMakeVisible(removeKeyButton_);

    keyHintLabel_.setFont(uiFont(11.0f));
    keyHintLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    keyHintLabel_.setText("A stored key survives restarts; the field clears after a successful "
                          "store and the value never reaches the settings file or the log.",
                          juce::NotificationType::dontSendNotification);
    addAndMakeVisible(keyHintLabel_);

    // ------------------------------------------------------------- translation
    // The editor is READ-ONLY on purpose (code review P1, 2026-10-05): the
    // current model ignores this text (protocol docs section 12.1), and making
    // the operator author a setting that does nothing is a fake contract. The
    // value stays visible - older installs may carry text - and the hint says
    // what the truth is.
    caption(instructionsCaption_, "Translation instructions - unsupported by gpt-realtime-translate", *this);
    instructionsEditor_.setMultiLine(true);
    instructionsEditor_.setScrollbarsShown(true);
    instructionsEditor_.setReadOnly(true);
    editor(instructionsEditor_, *this);

    instructionsHintLabel_.setFont(uiFont(11.0f));
    instructionsHintLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    instructionsHintLabel_.setText(
        "The translation provider accepts no custom prompting: it translates what it hears "
        "toward the target language and nothing else (protocol docs section 12.1). This field "
        "is stored for a future model that honours instructions, is never sent today, and the "
        "backend logs plainly that it was ignored. Empty means \"nothing configured\".",
        juce::NotificationType::dontSendNotification);
    addAndMakeVisible(instructionsHintLabel_);

    caption(modelHintCaption_, "Model hint (empty = backend default)", *this);
    editor(modelHintEditor_, *this);

    reconnectToggle_.setClickingTogglesState(true);
    addAndMakeVisible(reconnectToggle_);

    const auto [backoffMin, backoffMax] = config::reconnectBackoffRange();
    caption(initialBackoffCaption_, "First reconnect delay (ms)", *this);
    slider(initialBackoffSlider_, backoffMin, backoffMax, 100, *this);

    caption(maxBackoffCaption_, "Maximum reconnect backoff (ms)", *this);
    slider(maxBackoffSlider_, backoffMin, backoffMax, 100, *this);

    const auto [ageMin, ageMax] = config::sessionMaxAgeRange();
    caption(sessionAgeCaption_, "Session max age (s; 0 disables the proactive reopen)", *this);
    slider(sessionAgeSlider_, ageMin, ageMax, 60, *this);

    // --------------------------------------------------------------------- NDI
    caption(ndiCaption_, "NDI - timed text (TTML) metadata; receiver check is the venue run-sheet",
            *this);
    ndiToggle_.setClickingTogglesState(true);
    addAndMakeVisible(ndiToggle_);

    caption(streamNameCaption_, "NDI stream name", *this);
    editor(streamNameEditor_, *this);

    // -------------------------------------------------------------- diagnostics
    caption(logLevelCaption_, "Log level", *this);
    addAndMakeVisible(logLevelChoice_);   // part of the draft: Apply commits it
    logFileToggle_.setClickingTogglesState(true);
    addAndMakeVisible(logFileToggle_);

    restartHintLabel_.setFont(uiFont(11.0f));
    restartHintLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    restartHintLabel_.setText("Turning the log file on/off and recovery-policy edits take effect "
                              "when the application restarts.",
                              juce::NotificationType::dontSendNotification);
    addAndMakeVisible(restartHintLabel_);

    // ------------------------------------------------------- developer mode (019)
    caption(developerCaption_,
            "DEVELOPER / MOCK MODE - off is the real chain; while off, nothing below does anything",
            *this);

    developerToggle_.setClickingTogglesState(true);
    addAndMakeVisible(developerToggle_);

    caption(devSourceCaption_, "Audio source (no SoundGrid needed for wav/tone)", *this);
    addAndMakeVisible(devSourceChoice_);

    caption(devWavInCaption_, "WAV input file (its rate must equal the sample-rate setting)", *this);
    editor(devWavInEditor_, *this);

    caption(devWavOutCaption_, "Rehearsal recording WAV (empty = do not record)", *this);
    editor(devWavOutEditor_, *this);

    mockToggle_.setClickingTogglesState(true);
    addAndMakeVisible(mockToggle_);

    const auto [mockMin, mockMax] = config::mockLatencyRange();
    caption(mockLatencyCaption_, "Mock echo delay (ms) - known ground truth for the in-flight row", *this);
    slider(devMockLatencySlider_, mockMin, mockMax, 10, *this);

    caption(devToneFreqCaption_, "Test tone frequency (Hz)", *this);
    const auto [toneMin, toneMax] = config::toneFrequencyRange();
    slider(devToneFreqSlider_, toneMin, toneMax, 10, *this);

    caption(devToneLevelCaption_, "Test tone level (dBFS)", *this);
    const auto [levelMin, levelMax] = config::toneLevelRangeDb();
    slider(devToneLevelSlider_, levelMin, levelMax, 1, *this);

    loopbackToggle_.setClickingTogglesState(true);
    addAndMakeVisible(loopbackToggle_);

    developerHintLabel_.setFont(uiFont(11.0f));
    developerHintLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    developerHintLabel_.setText("Mounted at application start: source, mock and loopback edits take "
                                "effect on restart. Loopback plays the capture to the audience and "
                                "leaves the translator unfed - never enable it with a live room.",
                                juce::NotificationType::dontSendNotification);
    addAndMakeVisible(developerHintLabel_);

    applyButton_.onClick = [this] { applyPressed(); };
    addAndMakeVisible(applyButton_);

    noteLabel_.setFont(uiFont(12.0f));
    noteLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd29922u));
    addAndMakeVisible(noteLabel_);

    refreshFromSettings();
    startTimer(500);   // the presence line alone; the store can change outside this process
}

// --------------------------------------------------------------------------- commands

void SettingsContent::saveKeyPressed()
{
    // The exact bytes the operator typed: no trim, no case change. A key that
    // works is stored as it was entered; a key that works differently than it
    // looks would be a UI owning backend logic, and that is exactly what this
    // window may never do.
    std::string note;
    const bool stored = controller_.storeApiSecret(apiKeyEditor_.getText().toStdString(), note);
    actionNote_ = std::move(note);

    if (stored)
        apiKeyEditor_.clear();   // the value must not keep sitting on the screen

    showNote();
    refreshFromSettings();
}

void SettingsContent::removeKeyPressed()
{
    std::string note;
    controller_.removeApiSecret(note);
    actionNote_ = std::move(note);
    showNote();
    refreshFromSettings();
}

void SettingsContent::applyPressed()
{
    if (updatingWidgets_)
        return;

    AppConfig candidate = controller_.config().current();

    candidate.translation.instructions = instructionsEditor_.getText().toStdString();
    candidate.translation.modelHint = modelHintEditor_.getText().toStdString();
    candidate.translation.reconnectEnabled = reconnectToggle_.getToggleState();
    candidate.translation.reconnectInitialBackoffMs = static_cast<int> (initialBackoffSlider_.getValue());
    candidate.translation.reconnectMaxBackoffMs = static_cast<int> (maxBackoffSlider_.getValue());
    candidate.translation.sessionMaxAgeSeconds = static_cast<int> (sessionAgeSlider_.getValue());

    candidate.ndi.enabled = ndiToggle_.getToggleState();
    candidate.ndi.streamName = streamNameEditor_.getText().toStdString();

    if (const int index = logLevelChoice_.getSelectedItemIndex();
        index >= 0 && static_cast<std::size_t> (index) < logLevelCache_.size())
    {
        candidate.diagnostics.logLevel = logLevelCache_[static_cast<std::size_t> (index)].value;
    }

    candidate.diagnostics.writeLogFile = logFileToggle_.getToggleState();

    candidate.developer.enabled = developerToggle_.getToggleState();

    if (const int index = devSourceChoice_.getSelectedItemIndex();
        index >= 0 && static_cast<std::size_t> (index) < devSourceCache_.size())
    {
        candidate.developer.audioSource = devSourceCache_[static_cast<std::size_t> (index)];
    }

    candidate.developer.wavInputPath = devWavInEditor_.getText().toStdString();
    candidate.developer.wavOutputPath = devWavOutEditor_.getText().toStdString();
    candidate.developer.mockTranslation = mockToggle_.getToggleState();
    candidate.developer.mockLatencyMs = static_cast<int> (devMockLatencySlider_.getValue());
    candidate.developer.toneFrequencyHz = devToneFreqSlider_.getValue();
    candidate.developer.toneLevelDb = devToneLevelSlider_.getValue();
    candidate.developer.loopback = loopbackToggle_.getToggleState();

    std::string note;
    controller_.updateSettings(candidate, note);
    actionNote_ = std::move(note);

    // On refusal nothing was applied anywhere (ConfigManager's atomicity), and
    // the draft stays visible so the operator can fix the field the schema
    // named. On acceptance the config equals the draft already on screen.
    showNote();
}

void SettingsContent::showNote()
{
    const bool refused = actionNote_.find("refused") != std::string::npos
                         || actionNote_.find("NOT saved") != std::string::npos;
    noteLabel_.setText(juce::String(actionNote_), juce::NotificationType::dontSendNotification);
    noteLabel_.setColour(juce::Label::textColourId,
                         refused ? juce::Colour(0xfff85149u) : juce::Colour(0xffd29922u));
}

// -------------------------------------------------------------------------- refresh

void SettingsContent::refreshFromSettings()
{
    updatingWidgets_ = true;

    const auto& cfg = controller_.config().current();

    instructionsEditor_.setText(juce::String(cfg.translation.instructions),
                                juce::NotificationType::dontSendNotification);
    modelHintEditor_.setText(juce::String(cfg.translation.modelHint),
                             juce::NotificationType::dontSendNotification);
    reconnectToggle_.setToggleState(cfg.translation.reconnectEnabled,
                                    juce::NotificationType::dontSendNotification);
    initialBackoffSlider_.setValue(cfg.translation.reconnectInitialBackoffMs,
                                   juce::NotificationType::dontSendNotification);
    maxBackoffSlider_.setValue(cfg.translation.reconnectMaxBackoffMs,
                               juce::NotificationType::dontSendNotification);
    sessionAgeSlider_.setValue(cfg.translation.sessionMaxAgeSeconds,
                               juce::NotificationType::dontSendNotification);

    ndiToggle_.setToggleState(cfg.ndi.enabled, juce::NotificationType::dontSendNotification);
    streamNameEditor_.setText(juce::String(cfg.ndi.streamName),
                              juce::NotificationType::dontSendNotification);

    const auto choices = logLevelChoices();   // Utils/Log's own list, never one of ours

    if (logLevelCache_ != choices)
    {
        logLevelChoice_.clear(juce::NotificationType::dontSendNotification);
        for (std::size_t i = 0; i < choices.size(); ++i)
            logLevelChoice_.addItem(juce::String(choices[i].label), static_cast<int> (i) + 1);
        logLevelCache_ = choices;
    }

    for (std::size_t i = 0; i < logLevelCache_.size(); ++i)
    {
        if (logLevelCache_[i].value == cfg.diagnostics.logLevel)
        {
            logLevelChoice_.setSelectedId(static_cast<int> (i) + 1,
                                          juce::NotificationType::dontSendNotification);
            break;
        }
    }

    logFileToggle_.setToggleState(cfg.diagnostics.writeLogFile,
                                  juce::NotificationType::dontSendNotification);

    // ------------------------------------------------------- developer mode (019)
    developerToggle_.setToggleState(cfg.developer.enabled, juce::NotificationType::dontSendNotification);
    mockToggle_.setToggleState(cfg.developer.mockTranslation, juce::NotificationType::dontSendNotification);
    loopbackToggle_.setToggleState(cfg.developer.loopback, juce::NotificationType::dontSendNotification);

    // The schema's own list builds the selector - the UI again owns no vocabulary.
    const auto sources = config::developerAudioSources();

    if (devSourceCache_ != sources)
    {
        devSourceChoice_.clear(juce::NotificationType::dontSendNotification);

        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            const std::string_view value = sources[i];
            devSourceChoice_.addItem(juce::String(value == "device" ? "ASIO device (real)"
                                                       : value == "wav"   ? "WAV file (simulated)"
                                                                            : "Test tone (simulated)"),
                                     static_cast<int> (i) + 1);
        }

        devSourceCache_ = sources;
    }

    for (std::size_t i = 0; i < devSourceCache_.size(); ++i)
    {
        if (devSourceCache_[i] == cfg.developer.audioSource)
        {
            devSourceChoice_.setSelectedId(static_cast<int> (i) + 1,
                                           juce::NotificationType::dontSendNotification);
            break;
        }
    }

    devWavInEditor_.setText(juce::String(cfg.developer.wavInputPath),
                            juce::NotificationType::dontSendNotification);
    devWavOutEditor_.setText(juce::String(cfg.developer.wavOutputPath),
                             juce::NotificationType::dontSendNotification);
    devMockLatencySlider_.setValue(cfg.developer.mockLatencyMs, juce::NotificationType::dontSendNotification);
    devToneFreqSlider_.setValue(cfg.developer.toneFrequencyHz, juce::NotificationType::dontSendNotification);
    devToneLevelSlider_.setValue(cfg.developer.toneLevelDb, juce::NotificationType::dontSendNotification);

    credStatusLabel_.setText(juce::String(presenceLine(controller_)),
                             juce::NotificationType::dontSendNotification);

    updatingWidgets_ = false;
}

void SettingsContent::timerCallback()
{
    // Only the presence line rides the timer: settings drafts belong to the
    // operator's hand until Apply, and the store's answer to "is there a key"
    // can change outside this process (the Control Panel is a real neighbour).
    if (updatingWidgets_)
        return;

    const std::string line = presenceLine(controller_);

    if (credStatusLabel_.getText().toStdString() != line)
    {
        credStatusLabel_.setText(juce::String(line), juce::NotificationType::dontSendNotification);
        credStatusLabel_.setColour(juce::Label::textColourId,
                                   controller_.hasApiSecret()
                                       ? juce::Colour(0xff3fb950u)
                                       : juce::Colour(0xffd29922u));
    }
}

// ---------------------------------------------------------------------------- layout

void SettingsContent::resized()
{
    auto bounds = getLocalBounds().reduced(14);

    // credentials band across the top
    credentialCaption_.setBounds(bounds.removeFromTop(18));
    auto credRow = bounds.removeFromTop(30);
    removeKeyButton_.setBounds(credRow.removeFromRight(130));
    saveKeyButton_.setBounds(credRow.removeFromRight(110).withTrimmedLeft(4));
    apiKeyEditor_.setBounds(credRow.removeFromRight(300).withTrimmedLeft(4));
    credStatusLabel_.setBounds(credRow);
    keyHintLabel_.setBounds(bounds.removeFromTop(26));
    bounds.removeFromTop(6);

    // --- developer / mock band across the bottom (task 019)
    auto devBand = bounds.removeFromBottom(246);
    devBand.removeFromTop(6);
    developerCaption_.setBounds(devBand.removeFromTop(16));
    developerToggle_.setBounds(devBand.removeFromTop(24));
    developerHintLabel_.setBounds(devBand.removeFromBottom(28));

    auto devCols = devBand;
    auto devLeft = devCols.removeFromLeft(devCols.getWidth() / 2);
    auto devRight = devCols;
    devRight.removeFromLeft(12);

    devSourceCaption_.setBounds(devLeft.removeFromTop(16));
    devSourceChoice_.setBounds(devLeft.removeFromTop(26));
    devLeft.removeFromTop(4);
    devWavInCaption_.setBounds(devLeft.removeFromTop(16));
    devWavInEditor_.setBounds(devLeft.removeFromTop(26));
    devLeft.removeFromTop(4);
    devWavOutCaption_.setBounds(devLeft.removeFromTop(16));
    devWavOutEditor_.setBounds(devLeft.removeFromTop(26));

    mockToggle_.setBounds(devRight.removeFromTop(24));
    mockLatencyCaption_.setBounds(devRight.removeFromTop(16));
    devMockLatencySlider_.setBounds(devRight.removeFromTop(26));
    devToneFreqCaption_.setBounds(devRight.removeFromTop(16));
    devToneFreqSlider_.setBounds(devRight.removeFromTop(26));
    devToneLevelCaption_.setBounds(devRight.removeFromTop(16));
    devToneLevelSlider_.setBounds(devRight.removeFromTop(26));
    loopbackToggle_.setBounds(devRight.removeFromTop(24));

    auto body = bounds;
    auto left = body.removeFromLeft(body.getWidth() / 2);
    auto right = body;
    right.removeFromLeft(12);

    // --- translation column
    instructionsCaption_.setBounds(left.removeFromTop(16));
    instructionsEditor_.setBounds(left.removeFromTop(110));
    instructionsHintLabel_.setBounds(left.removeFromTop(48));
    left.removeFromTop(4);

    modelHintCaption_.setBounds(left.removeFromTop(16));
    modelHintEditor_.setBounds(left.removeFromTop(26));
    left.removeFromTop(6);

    reconnectToggle_.setBounds(left.removeFromTop(24));

    initialBackoffCaption_.setBounds(left.removeFromTop(16));
    initialBackoffSlider_.setBounds(left.removeFromTop(28));
    maxBackoffCaption_.setBounds(left.removeFromTop(20));
    maxBackoffSlider_.setBounds(left.removeFromTop(28));
    sessionAgeCaption_.setBounds(left.removeFromTop(20));
    sessionAgeSlider_.setBounds(left.removeFromTop(28));

    // --- NDI / diagnostics column
    ndiCaption_.setBounds(right.removeFromTop(16));
    ndiToggle_.setBounds(right.removeFromTop(24));
    streamNameCaption_.setBounds(right.removeFromTop(18));
    streamNameEditor_.setBounds(right.removeFromTop(26));
    right.removeFromTop(10);

    logLevelCaption_.setBounds(right.removeFromTop(16));
    logLevelChoice_.setBounds(right.removeFromTop(26));
    logFileToggle_.setBounds(right.removeFromTop(24));
    restartHintLabel_.setBounds(right.removeFromTop(30));
    right.removeFromTop(10);

    applyButton_.setBounds(right.removeFromTop(32).removeFromLeft(200));
    noteLabel_.setBounds(right);
}

// =========================================================================== SettingsWindow

SettingsWindow::SettingsWindow(ApplicationController& controller)
    : juce::DocumentWindow("LingoFlow Settings",
                           juce::Colour(0xff1e2124u),
                           juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    // (No delete-on-close in this JUCE: closeButtonPressed() hides, and the
    // owner window owns this object for its whole lifetime - see below.)

    auto* content = new SettingsContent(controller);
    content_ = content;
    setContentOwned(content, true);

    setResizable(true, true);
    // Task 019 grew the content: the credentials header, two columns and the
    // developer band across the bottom need the extra height.
    setResizeLimits(720, 830, 4000, 4000);
    setSize(820, 910);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

void SettingsWindow::reopen()
{
    if (content_ != nullptr)
        content_->refreshFromSettings();   // a dialog reopened never shows a stale draft

    setVisible(true);
    toFront(true);   ///< the component-level bring-to-front; no focus tricks needed
}

void SettingsWindow::closeButtonPressed()
{
    setVisible(false);
}

} // namespace liveai
