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
    caption(instructionsCaption_, "Translation instructions", *this);
    instructionsEditor_.setMultiLine(true);
    instructionsEditor_.setScrollbarsShown(true);
    editor(instructionsEditor_, *this);

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

    auto body = bounds;
    auto left = body.removeFromLeft(body.getWidth() / 2);
    auto right = body;
    right.removeFromLeft(12);

    // --- translation column
    instructionsCaption_.setBounds(left.removeFromTop(16));
    instructionsEditor_.setBounds(left.removeFromTop(110));
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
    setResizeLimits(680, 520, 4000, 4000);
    setSize(780, 620);
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
