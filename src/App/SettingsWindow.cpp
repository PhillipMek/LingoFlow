#include "App/SettingsWindow.h"

#include <filesystem>
#include <format>

#include "App/UiWidgets.h"
#include "Config/ConfigSchema.h"

namespace liveai {
namespace {

// The dialog palette matches the operator window (UI-02 Р’В§12): state carries
// colour, everything else stays neutral.
namespace sink {

const juce::Colour textPrimary   { 0xffe6edf3u };
const juce::Colour textSecondary { 0xff9aa4afu };
const juce::Colour textMuted     { 0xff6e7681u };
const juce::Colour green         { 0xff3fb950u };
const juce::Colour amber         { 0xffd29922u };
const juce::Colour red           { 0xfff85149u };
const juce::Colour control       { 0xff30363du };
const juce::Colour cardBorder    { 0xff2d333au };
const juce::Colour blue          { 0xff2f81f7u };

} // namespace sink

constexpr int kPad = 16;
constexpr int kRowGap = 12;
constexpr int kControlH = 28;
constexpr int kCaptionH = 16;
constexpr int kButtonH = 32;

juce::Label& caption(juce::Label& label, const juce::String& text, juce::Component& parent)
{
    label.setFont(ui::uiFont(12.0f));
    label.setColour(juce::Label::textColourId, sink::textSecondary);
    label.setText(text, juce::NotificationType::dontSendNotification);
    parent.addAndMakeVisible(label);
    return label;
}

juce::Label& hint(juce::Label& label, const juce::String& text, juce::Component& parent)
{
    label.setFont(ui::uiFont(11.0f));
    label.setColour(juce::Label::textColourId, sink::textMuted);
    label.setText(text, juce::NotificationType::dontSendNotification);
    parent.addAndMakeVisible(label);
    return label;
}

juce::Slider& slider(juce::Slider& s, double min, double max, double interval,
                     juce::Component& parent)
{
    s.setRange(min, max, interval);
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 84, 24);
    s.setColour(juce::Slider::trackColourId, sink::blue.withAlpha(0.55f));
    s.setColour(juce::Slider::thumbColourId, sink::textPrimary);
    parent.addAndMakeVisible(s);
    return s;
}

void styleBox(juce::ComboBox& box, juce::Component& parent)
{
    box.setColour(juce::ComboBox::backgroundColourId, sink::control);
    box.setColour(juce::ComboBox::outlineColourId, sink::cardBorder);
    box.setColour(juce::ComboBox::textColourId, sink::textPrimary);
    box.setColour(juce::ComboBox::arrowColourId, sink::textSecondary);
    box.setColour(juce::ComboBox::focusedOutlineColourId, sink::blue);
    parent.addAndMakeVisible(box);
}

void styleButton(juce::TextButton& button, juce::Component& parent)
{
    button.setColour(juce::TextButton::buttonColourId, sink::control);
    button.setColour(juce::TextButton::textColourOffId, sink::textPrimary);
    button.setColour(juce::TextButton::textColourOnId, sink::textPrimary);
    parent.addAndMakeVisible(button);
}

juce::ToggleButton& toggle(juce::ToggleButton& t, juce::Component& parent)
{
    t.setClickingTogglesState(true);
    t.setColour(juce::ToggleButton::textColourId, sink::textPrimary);
    t.setColour(juce::ToggleButton::tickColourId, sink::green);
    parent.addAndMakeVisible(t);
    return t;
}

/// Rebuild a ComboBox's items only when the list actually changed, then
/// select `selectedIndex` (-1 clears). The same rule the operator window uses,
/// so a dialog refresh can never disturb an open dropdown mid-click.
void syncBox(juce::ComboBox& box, std::vector<UiOption>& cache,
             const std::vector<UiOption>& fresh, int selectedIndex)
{
    if (cache != fresh)
    {
        box.clear(juce::NotificationType::dontSendNotification);
        for (std::size_t i = 0; i < fresh.size(); ++i)
            box.addItem(juce::String(fresh[i].label), static_cast<int> (i) + 1);
        cache = fresh;
    }

    box.setSelectedId(selectedIndex >= 0 ? selectedIndex + 1 : 0,
                      juce::NotificationType::dontSendNotification);
}

/// The store sentence for the Credentials status line (UI-03 Р’В§5): built from
/// the same controller reads as the main screen's credential line - names of
/// stores, never values of secrets.
std::string presenceLine(ApplicationController& controller)
{
    switch (controller.apiSecretLocation())
    {
        case security::ISecretStore::Location::primary:
            return "Status: stored securely in " + controller.secretStoreWritableName();

        case security::ISecretStore::Location::fallbackOnly:
            return "Status: found only in the development environment - press Store key "
                   "to move it into " + controller.secretStoreWritableName();

        case security::ISecretStore::Location::none:
        default:
            return "Status: not stored - translation sessions will refuse until a key "
                   "is entered here";
    }
}

} // namespace

// =========================================================================== SettingsContent

SettingsContent::SettingsContent(ApplicationController& controller)
    : controller_(controller)
{
    tabs_ = std::make_unique<juce::TabbedComponent>(juce::TabbedButtonBar::TabsAtTop);
    tabs_->setTabBarDepth(30);
    tabs_->setColour(juce::TabbedButtonBar::tabTextColourId, sink::textSecondary);
    tabs_->setColour(juce::TabbedButtonBar::frontTextColourId, sink::textPrimary);
    tabs_->setColour(juce::TabbedButtonBar::tabOutlineColourId, sink::cardBorder);
    tabs_->setColour(juce::TabbedButtonBar::frontOutlineColourId, sink::cardBorder);
    addAndMakeVisible(*tabs_);

    // ------------------------------------------------------------------ audio
    audioPage_ = new juce::Component();
    caption(deviceCaption_, "ASIO device (one in/out device)", *audioPage_);
    styleBox(deviceChoice_, *audioPage_);   // no onChange: the draft owns the widget until Apply
    refreshDevicesButton_.onClick = [this]
    {
        // A rescan updates the LIST only - the rest of the draft on screen is
        // the operator's work and stays untouched.
        controller_.refreshDevices();
        const OperatorPanel panel = buildOperatorPanel(controller_, {});
        updatingWidgets_ = true;
        syncBox(deviceChoice_, deviceCache_, panel.devices, panel.selectedDevice);
        deviceNoteLabel_.setText(juce::String(panel.deviceNote),
                                 juce::NotificationType::dontSendNotification);
        updatingWidgets_ = false;
    };
    styleButton(refreshDevicesButton_, *audioPage_);
    hint(deviceNoteLabel_, "", *audioPage_);
    caption(sampleRateCaption_, "Sample rate", *audioPage_);
    styleBox(sampleRateChoice_, *audioPage_);
    caption(bufferCaption_, "Buffer size (frames)", *audioPage_);
    const auto [bufferMin, bufferMax] = config::bufferFramesRange();
    slider(bufferSlider_, bufferMin, bufferMax, 16, *audioPage_);
    caption(inputChannelCaption_, "Input channel", *audioPage_);
    styleBox(inputChannelChoice_, *audioPage_);
    caption(outputChannelCaption_, "Output channel", *audioPage_);
    styleBox(outputChannelChoice_, *audioPage_);
    hint(channelNoteLabel_, "", *audioPage_);
    caption(inputGainCaption_, "Input gain (dB)", *audioPage_);
    caption(outputGainCaption_, "Output gain (dB)", *audioPage_);
    const auto [gainMin, gainMax] = config::gainRange();
    slider(inputGainSlider_, gainMin, gainMax, 0.5, *audioPage_);
    slider(outputGainSlider_, gainMin, gainMax, 0.5, *audioPage_);
    hint(audioRestartHintLabel_,
         "Device, sample rate, buffer, channels and languages take effect on the next Start.",
         *audioPage_);

    // -------------------------------------------------------------- translation
    translationPage_ = new juce::Component();
    caption(sourceCaption_, "Input language", *translationPage_);
    styleBox(sourceChoice_, *translationPage_);
    caption(targetCaption_, "Output language", *translationPage_);
    styleBox(targetChoice_, *translationPage_);
    pairWarningLabel_.setFont(ui::uiFont(12.0f));
    pairWarningLabel_.setColour(juce::Label::textColourId, sink::red);
    translationPage_->addAndMakeVisible(pairWarningLabel_);
    caption(modelHintCaption_, "Model hint (empty = backend default)", *translationPage_);
    translationPage_->addAndMakeVisible(modelHintEditor_);
    toggle(reconnectToggle_, *translationPage_);
    const auto [backoffMin, backoffMax] = config::reconnectBackoffRange();
    caption(initialBackoffCaption_, "First reconnect delay (ms)", *translationPage_);
    slider(initialBackoffSlider_, backoffMin, backoffMax, 100, *translationPage_);
    caption(maxBackoffCaption_, "Maximum reconnect backoff (ms)", *translationPage_);
    slider(maxBackoffSlider_, backoffMin, backoffMax, 100, *translationPage_);
    const auto [ageMin, ageMax] = config::sessionMaxAgeRange();
    caption(sessionAgeCaption_, "Session max age (s; 0 = no proactive reopen)", *translationPage_);
    slider(sessionAgeSlider_, ageMin, ageMax, 60, *translationPage_);
    caption(jitterCaption_, "Jitter pre-roll (ms)", *translationPage_);
    const auto [jitterMin, jitterMax] = config::jitterBufferRange();
    slider(jitterSlider_, jitterMin, jitterMax, 10, *translationPage_);

    // ----------------------------------------------------------- subtitles/NDI
    subtitlesPage_ = new juce::Component();
    toggle(ndiToggle_, *subtitlesPage_);
    caption(streamNameCaption_, "NDI stream name", *subtitlesPage_);
    subtitlesPage_->addAndMakeVisible(streamNameEditor_);
    ndiStateLabel_.setFont(ui::uiFont(13.0f));
    subtitlesPage_->addAndMakeVisible(ndiStateLabel_);
    hint(ndiHintLabel_,
         "Subtitles are timed text (TTML) for NDI receivers; the receiver check is "
         "the venue run-sheet. NDI never carries audio and its failure never stops it.",
         *subtitlesPage_);

    // -------------------------------------------------------------- credentials
    credentialsPage_ = new juce::Component();
    credStatusLabel_.setFont(ui::uiFont(14.0f));
    credStatusLabel_.setColour(juce::Label::textColourId, sink::textPrimary);
    credentialsPage_->addAndMakeVisible(credStatusLabel_);
    apiKeyEditor_.setPasswordCharacter(L'*');   // the field masks its echo (PASS criterion)
    credentialsPage_->addAndMakeVisible(apiKeyEditor_);
    saveKeyButton_.onClick = [this] { saveKeyPressed(); };
    removeKeyButton_.onClick = [this] { removeKeyPressed(); };
    styleButton(saveKeyButton_, *credentialsPage_);
    styleButton(removeKeyButton_, *credentialsPage_);
    hint(keyHintLabel_,
         "The key lives in Windows secure storage, never in the settings file or the "
         "log. The field clears after a successful store and the value is never shown "
         "again here.",
         *credentialsPage_);

    // -------------------------------------------------------------- diagnostics
    diagnosticsPage_ = new juce::Component();
    caption(logLevelCaption_, "Log level", *diagnosticsPage_);
    diagnosticsPage_->addAndMakeVisible(logLevelChoice_);   // part of the draft: Apply commits it
    logLevelChoice_.setColour(juce::ComboBox::backgroundColourId, sink::control);
    logLevelChoice_.setColour(juce::ComboBox::outlineColourId, sink::cardBorder);
    logLevelChoice_.setColour(juce::ComboBox::textColourId, sink::textPrimary);
    toggle(logFileToggle_, *diagnosticsPage_);
    hint(logHintLabel_,
         "The log file and recovery-policy edits take effect when the application "
         "restarts. Export writes a structured diagnostics report you can carry to "
         "a venue - it contains counters and events, never the API key.",
         *diagnosticsPage_);
    exportDiagnosticsButton_.onClick = [this] { exportPressed(); };
    styleButton(exportDiagnosticsButton_, *diagnosticsPage_);

    // ------------------------------------------------------------------ advanced
    advancedPage_ = new juce::Component();
    caption(instructionsCaption_,
            "Translation instructions - unsupported by the current model (kept for a future one)",
            *advancedPage_);
    instructionsEditor_.setMultiLine(true);
    instructionsEditor_.setScrollbarsShown(true);
    instructionsEditor_.setReadOnly(true);   // code review P1: no authoring of a dead setting
    advancedPage_->addAndMakeVisible(instructionsEditor_);
    hint(instructionsHintLabel_,
         "The provider accepts no custom prompting (protocol docs section 12.1). This "
         "value is stored for a future model, is never sent today, and the backend logs "
         "plainly that it was ignored. Empty means \"nothing configured\".",
         *advancedPage_);
    toggle(developerToggle_, *advancedPage_);
    caption(devSourceCaption_, "Audio source (no SoundGrid needed for wav/tone)", *advancedPage_);
    devSourceChoice_.setColour(juce::ComboBox::backgroundColourId, sink::control);
    devSourceChoice_.setColour(juce::ComboBox::outlineColourId, sink::cardBorder);
    devSourceChoice_.setColour(juce::ComboBox::textColourId, sink::textPrimary);
    advancedPage_->addAndMakeVisible(devSourceChoice_);
    caption(devWavInCaption_, "WAV input file (its rate must equal the sample-rate setting)", *advancedPage_);
    advancedPage_->addAndMakeVisible(devWavInEditor_);
    caption(devWavOutCaption_, "Rehearsal recording WAV (empty = do not record)", *advancedPage_);
    advancedPage_->addAndMakeVisible(devWavOutEditor_);
    toggle(mockToggle_, *advancedPage_);
    const auto [mockMin, mockMax] = config::mockLatencyRange();
    caption(mockLatencyCaption_, "Mock echo delay (ms)", *advancedPage_);
    slider(devMockLatencySlider_, mockMin, mockMax, 10, *advancedPage_);
    caption(devToneFreqCaption_, "Test tone frequency (Hz)", *advancedPage_);
    const auto [toneMin, toneMax] = config::toneFrequencyRange();
    slider(devToneFreqSlider_, toneMin, toneMax, 10, *advancedPage_);
    caption(devToneLevelCaption_, "Test tone level (dBFS)", *advancedPage_);
    const auto [levelMin, levelMax] = config::toneLevelRangeDb();
    slider(devToneLevelSlider_, levelMin, levelMax, 1, *advancedPage_);
    loopbackToggle_.onClick = [this] { loopbackVisuals(); };
    toggle(loopbackToggle_, *advancedPage_);
    hint(developerHintLabel_,
         "Mounted at application start: source, mock and loopback edits take effect on "
         "restart. Loopback plays the capture to the audience and leaves the translator "
         "unfed - never enable it with a live room.",
         *advancedPage_);

    // --------------------------------------------------------------- bottom bar
    applyButton_.onClick = [this] { applyPressed(); };
    applyButton_.setColour(juce::TextButton::buttonColourId, sink::green.withAlpha(0.25f));
    applyButton_.setColour(juce::TextButton::textColourOffId, sink::textPrimary);
    addAndMakeVisible(applyButton_);
    noteLabel_.setFont(ui::uiFont(12.0f));
    noteLabel_.setColour(juce::Label::textColourId, sink::amber);
    addAndMakeVisible(noteLabel_);

    tabs_->addTab("Audio", juce::Colour(0xff1a1d20u), audioPage_, true);
    tabs_->addTab("Translation", juce::Colour(0xff1a1d20u), translationPage_, true);
    tabs_->addTab("Subtitles / NDI", juce::Colour(0xff1a1d20u), subtitlesPage_, true);
    tabs_->addTab("Credentials", juce::Colour(0xff1a1d20u), credentialsPage_, true);
    tabs_->addTab("Diagnostics", juce::Colour(0xff1a1d20u), diagnosticsPage_, true);
    tabs_->addTab("Advanced", juce::Colour(0xff1a1d20u), advancedPage_, true);

    refreshFromSettings();
    startTimer(500);   // presence + NDI state; the store can change outside this process
}

// --------------------------------------------------------------------------- commands

namespace {

/// Green only for the secure answer; the development-environment state and
/// the absence are both amber - the colour must not outrun the sentence.
juce::Colour credentialColour(security::ISecretStore::Location location)
{
    return location == security::ISecretStore::Location::primary ? sink::green : sink::amber;
}

} // namespace

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

void SettingsContent::exportPressed()
{
    // Task 017's funnel, offered here as well as in the Diagnostics window
    // (UI-03 Р’В§6): one call, and the note is the receipt either way.
    std::filesystem::path written;
    std::string note;
    controller_.exportDiagnostics({}, written, note);
    actionNote_ = std::move(note);
    showNote();
}

void SettingsContent::applyPressed()
{
    if (updatingWidgets_)
        return;

    AppConfig candidate = controller_.config().current();

    // Audio tab: the same keys the operator screen edits live; the last
    // commit wins and one validation funnel guards both routes.
    if (const int index = deviceChoice_.getSelectedItemIndex();
        index >= 0 && static_cast<std::size_t> (index) < deviceCache_.size())
    {
        candidate.audio.inputDeviceId = deviceCache_[static_cast<std::size_t> (index)].value;
        candidate.audio.outputDeviceId = candidate.audio.inputDeviceId;   // SPEC: one ASIO device
    }
    if (const int index = sampleRateChoice_.getSelectedItemIndex();
        index >= 0 && static_cast<std::size_t> (index) < sampleRateCache_.size())
    {
        candidate.audio.sampleRate = std::stoi(sampleRateCache_[static_cast<std::size_t> (index)].value);
    }
    candidate.audio.bufferFrames = static_cast<int> (bufferSlider_.getValue());
    if (const int index = inputChannelChoice_.getSelectedItemIndex();
        index >= 0 && static_cast<std::size_t> (index) < inputChannelCache_.size())
    {
        candidate.audio.inputChannel = std::stoi(inputChannelCache_[static_cast<std::size_t> (index)].value);
    }
    if (const int index = outputChannelChoice_.getSelectedItemIndex();
        index >= 0 && static_cast<std::size_t> (index) < outputChannelCache_.size())
    {
        candidate.audio.outputChannel = std::stoi(outputChannelCache_[static_cast<std::size_t> (index)].value);
    }
    candidate.audio.inputGainDb = static_cast<float> (inputGainSlider_.getValue());
    candidate.audio.outputGainDb = static_cast<float> (outputGainSlider_.getValue());

    // Translation tab.
    if (const int index = sourceChoice_.getSelectedItemIndex();
        index >= 0 && static_cast<std::size_t> (index) < sourceCache_.size())
    {
        candidate.translation.inputLanguage = sourceCache_[static_cast<std::size_t> (index)].value;
    }
    if (const int index = targetChoice_.getSelectedItemIndex();
        index >= 0 && static_cast<std::size_t> (index) < targetCache_.size())
    {
        candidate.translation.outputLanguage = targetCache_[static_cast<std::size_t> (index)].value;
    }
    candidate.translation.instructions = instructionsEditor_.getText().toStdString();
    candidate.translation.modelHint = modelHintEditor_.getText().toStdString();
    candidate.translation.reconnectEnabled = reconnectToggle_.getToggleState();
    candidate.translation.reconnectInitialBackoffMs = static_cast<int> (initialBackoffSlider_.getValue());
    candidate.translation.reconnectMaxBackoffMs = static_cast<int> (maxBackoffSlider_.getValue());
    candidate.translation.sessionMaxAgeSeconds = static_cast<int> (sessionAgeSlider_.getValue());
    candidate.translation.jitterBufferMs = static_cast<int> (jitterSlider_.getValue());

    // Subtitles tab.
    candidate.ndi.enabled = ndiToggle_.getToggleState();
    candidate.ndi.streamName = streamNameEditor_.getText().toStdString();

    // Diagnostics tab.
    if (const int index = logLevelChoice_.getSelectedItemIndex();
        index >= 0 && static_cast<std::size_t> (index) < logLevelCache_.size())
    {
        candidate.diagnostics.logLevel = logLevelCache_[static_cast<std::size_t> (index)].value;
    }
    candidate.diagnostics.writeLogFile = logFileToggle_.getToggleState();

    // Advanced tab.
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
                         refused ? sink::red : sink::amber);
}

void SettingsContent::loopbackVisuals()
{
    // UI-03 Р’В§7: the safety sentence is red and explicit while loopback is
    // checked; otherwise the ordinary muted hint returns.
    if (loopbackToggle_.getToggleState())
    {
        developerHintLabel_.setText(
            "LOOPBACK IS ON: the audience hears the capture and the translator is NOT "
            "fed. Never enable it with a live room.",
            juce::NotificationType::dontSendNotification);
        developerHintLabel_.setColour(juce::Label::textColourId, sink::red);
    }
    else
    {
        developerHintLabel_.setText(
            "Mounted at application start: source, mock and loopback edits take effect "
            "on restart. Loopback plays the capture to the audience and leaves the "
            "translator unfed - never enable it with a live room.",
            juce::NotificationType::dontSendNotification);
        developerHintLabel_.setColour(juce::Label::textColourId, sink::textMuted);
    }
}

// -------------------------------------------------------------------------- refresh

void SettingsContent::refreshFromSettings()
{
    updatingWidgets_ = true;

    const auto& cfg = controller_.config().current();

    // The option lists come from the same builders the operator screen uses -
    // devices from the scan, rates and ranges from the schema, languages from
    // the registry, channels from the driver's names or the honest fallback.
    const OperatorPanel panel = buildOperatorPanel(controller_, {});

    syncBox(deviceChoice_, deviceCache_, panel.devices, panel.selectedDevice);
    syncBox(sampleRateChoice_, sampleRateCache_, panel.sampleRates, panel.selectedSampleRate);
    syncBox(sourceChoice_, sourceCache_, panel.sourceLanguages, panel.selectedSource);
    syncBox(targetChoice_, targetCache_, panel.targetLanguages, panel.selectedTarget);
    syncBox(inputChannelChoice_, inputChannelCache_, panel.inputChannelChoices,
            panel.selectedInputChannel);
    syncBox(outputChannelChoice_, outputChannelCache_, panel.outputChannelChoices,
            panel.selectedOutputChannel);

    deviceNoteLabel_.setText(juce::String(panel.deviceNote),
                             juce::NotificationType::dontSendNotification);
    channelNoteLabel_.setText(juce::String(panel.channelNote),
                              juce::NotificationType::dontSendNotification);
    pairWarningLabel_.setText(juce::String(panel.languagePairWarning),
                              juce::NotificationType::dontSendNotification);

    bufferSlider_.setValue(cfg.audio.bufferFrames, juce::NotificationType::dontSendNotification);
    inputGainSlider_.setValue(cfg.audio.inputGainDb, juce::NotificationType::dontSendNotification);
    outputGainSlider_.setValue(cfg.audio.outputGainDb, juce::NotificationType::dontSendNotification);
    inputGainCaption_.setText("Input gain (dB) - currently "
                                  + juce::String(std::format("{:+.1f}", cfg.audio.inputGainDb)),
                              juce::NotificationType::dontSendNotification);
    outputGainCaption_.setText("Output gain (dB) - currently "
                                   + juce::String(std::format("{:+.1f}", cfg.audio.outputGainDb)),
                               juce::NotificationType::dontSendNotification);

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
    jitterSlider_.setValue(cfg.translation.jitterBufferMs,
                           juce::NotificationType::dontSendNotification);

    ndiToggle_.setToggleState(cfg.ndi.enabled, juce::NotificationType::dontSendNotification);
    streamNameEditor_.setText(juce::String(cfg.ndi.streamName),
                              juce::NotificationType::dontSendNotification);
    ndiStateLabel_.setText("NDI state: " + juce::String(std::string(ndi::nameOf(controller_.status().ndi))),
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
    loopbackVisuals();

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
    credStatusLabel_.setColour(juce::Label::textColourId,
                               credentialColour(controller_.apiSecretLocation()));

    updatingWidgets_ = false;
}

void SettingsContent::timerCallback()
{
    // Presence and NDI state ride the timer: settings drafts belong to the
    // operator's hand until Apply, but the store's answer to "is there a key"
    // can change outside this process (the Control Panel is a real neighbour)
    // and the NDI line is live state, not draft.
    if (updatingWidgets_)
        return;

    const std::string line = presenceLine(controller_);

    if (credStatusLabel_.getText().toStdString() != line)
    {
        credStatusLabel_.setText(juce::String(line), juce::NotificationType::dontSendNotification);
        credStatusLabel_.setColour(juce::Label::textColourId,
                                   credentialColour(controller_.apiSecretLocation()));
    }

    const auto state = std::string(ndi::nameOf(controller_.status().ndi));

    if (ndiStateLabel_.getText().toStdString() != "NDI state: " + state)
    {
        ndiStateLabel_.setText("NDI state: " + juce::String(state),
                               juce::NotificationType::dontSendNotification);
        ndiStateLabel_.setColour(juce::Label::textColourId,
                                 state == "publishing" ? sink::green
                                                       : state == "faulted" ? sink::red
                                                                            : sink::textSecondary);
    }
}

// ---------------------------------------------------------------------------- layout

void SettingsContent::resized()
{
    auto bounds = getLocalBounds().reduced(kPad);

    // Breathing room between the tab content and the command row: the tabs
    // end, then the gap, then Apply - the first cut had them sharing an edge.
    auto bottom = bounds.removeFromBottom(kRowGap + kButtonH + kRowGap + 18);
    bottom.removeFromTop(kRowGap);
    applyButton_.setBounds(bottom.removeFromTop(kButtonH).withWidth(180));
    noteLabel_.setBounds(bottom.withHeight(18));

    tabs_->setBounds(bounds);

    layoutAudioPage();
    layoutTranslationPage();
    layoutSubtitlesPage();
    layoutCredentialsPage();
    layoutDiagnosticsPage();
    layoutAdvancedPage();
}

void SettingsContent::layoutAudioPage()
{
    auto b = audioPage_->getLocalBounds().reduced(kPad);

    deviceCaption_.setBounds(b.removeFromTop(kCaptionH));
    {
        auto row = b.removeFromTop(kControlH);
        refreshDevicesButton_.setBounds(row.removeFromRight(100).withTrimmedLeft(kRowGap));
        deviceChoice_.setBounds(row);
    }
    deviceNoteLabel_.setBounds(b.removeFromTop(28));
    b.removeFromTop(kRowGap);

    sampleRateCaption_.setBounds(b.removeFromTop(kCaptionH));
    sampleRateChoice_.setBounds(b.removeFromTop(kControlH).withWidth(300));
    b.removeFromTop(kRowGap);

    bufferCaption_.setBounds(b.removeFromTop(kCaptionH));
    bufferSlider_.setBounds(b.removeFromTop(kControlH));
    b.removeFromTop(kRowGap);

    // The paired columns consume a block of their own height from b as well -
    // a copy of the rect does not advance the original, and the first cut of
    // this page proved it by painting the note through the channel combos.
    const int columnsBlockH = kCaptionH + kControlH + kRowGap + kCaptionH + kControlH;
    auto cols = b.removeFromTop(columnsBlockH);
    auto left = cols.removeFromLeft(cols.getWidth() / 2);
    auto right = cols;
    right.removeFromLeft(24);

    inputChannelCaption_.setBounds(left.removeFromTop(kCaptionH));
    inputChannelChoice_.setBounds(left.removeFromTop(kControlH));
    left.removeFromTop(kRowGap);
    inputGainCaption_.setBounds(left.removeFromTop(kCaptionH));
    inputGainSlider_.setBounds(left.removeFromTop(kControlH));

    outputChannelCaption_.setBounds(right.removeFromTop(kCaptionH));
    outputChannelChoice_.setBounds(right.removeFromTop(kControlH));
    right.removeFromTop(kRowGap);
    outputGainCaption_.setBounds(right.removeFromTop(kCaptionH));
    outputGainSlider_.setBounds(right.removeFromTop(kControlH));

    b.removeFromTop(kRowGap);
    channelNoteLabel_.setBounds(b.removeFromTop(16));
    b.removeFromTop(kRowGap);
    audioRestartHintLabel_.setBounds(b.withHeight(28));
}

void SettingsContent::layoutTranslationPage()
{
    auto b = translationPage_->getLocalBounds().reduced(kPad);

    auto cols = b.removeFromTop(kCaptionH + kControlH + 4);   // consumed from b too
    auto left = cols.removeFromLeft(cols.getWidth() / 2);
    auto right = cols;
    right.removeFromLeft(24);
    sourceCaption_.setBounds(left.removeFromTop(kCaptionH));
    sourceChoice_.setBounds(left);
    targetCaption_.setBounds(right.removeFromTop(kCaptionH));
    targetChoice_.setBounds(right);
    b.removeFromTop(kRowGap);

    pairWarningLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(kRowGap);

    modelHintCaption_.setBounds(b.removeFromTop(kCaptionH));
    modelHintEditor_.setBounds(b.removeFromTop(kControlH));
    b.removeFromTop(kRowGap);

    reconnectToggle_.setBounds(b.removeFromTop(24));
    b.removeFromTop(4);

    initialBackoffCaption_.setBounds(b.removeFromTop(kCaptionH));
    initialBackoffSlider_.setBounds(b.removeFromTop(kControlH));
    maxBackoffCaption_.setBounds(b.removeFromTop(kCaptionH + 4));
    maxBackoffSlider_.setBounds(b.removeFromTop(kControlH));
    sessionAgeCaption_.setBounds(b.removeFromTop(kCaptionH + 4));
    sessionAgeSlider_.setBounds(b.removeFromTop(kControlH));
    b.removeFromTop(kRowGap);
    jitterCaption_.setBounds(b.removeFromTop(kCaptionH));
    jitterSlider_.setBounds(b.removeFromTop(kControlH));
}

void SettingsContent::layoutSubtitlesPage()
{
    auto b = subtitlesPage_->getLocalBounds().reduced(kPad);

    ndiToggle_.setBounds(b.removeFromTop(24));
    b.removeFromTop(kRowGap);
    streamNameCaption_.setBounds(b.removeFromTop(kCaptionH));
    streamNameEditor_.setBounds(b.removeFromTop(kControlH));
    b.removeFromTop(kRowGap);
    ndiStateLabel_.setBounds(b.removeFromTop(20));
    b.removeFromTop(kRowGap);
    ndiHintLabel_.setBounds(b.withHeight(44));
}

void SettingsContent::layoutCredentialsPage()
{
    auto b = credentialsPage_->getLocalBounds().reduced(kPad);

    credStatusLabel_.setBounds(b.removeFromTop(22));
    b.removeFromTop(kRowGap);
    {
        auto row = b.removeFromTop(kControlH + 2);
        removeKeyButton_.setBounds(row.removeFromRight(130));
        saveKeyButton_.setBounds(row.removeFromRight(110 + 2 * kRowGap)
                                    .withTrimmedLeft(kRowGap)
                                    .withTrimmedRight(kRowGap));
        apiKeyEditor_.setBounds(row.removeFromRight(320).withTrimmedLeft(kRowGap));
    }
    b.removeFromTop(kRowGap);
    keyHintLabel_.setBounds(b.removeFromTop(44));
}

void SettingsContent::layoutDiagnosticsPage()
{
    auto b = diagnosticsPage_->getLocalBounds().reduced(kPad);

    logLevelCaption_.setBounds(b.removeFromTop(kCaptionH));
    logLevelChoice_.setBounds(b.removeFromTop(kControlH).withWidth(200));
    b.removeFromTop(kRowGap);
    logFileToggle_.setBounds(b.removeFromTop(24));
    b.removeFromTop(kRowGap);
    exportDiagnosticsButton_.setBounds(b.removeFromTop(kButtonH).withWidth(200));
    b.removeFromTop(kRowGap);
    logHintLabel_.setBounds(b.withHeight(44));
}

void SettingsContent::layoutAdvancedPage()
{
    auto b = advancedPage_->getLocalBounds().reduced(kPad);

    developerToggle_.setBounds(b.removeFromTop(24));
    b.removeFromTop(kRowGap);

    const int devBlockH = kCaptionH + 70 + 4 + 56 + kRowGap + kCaptionH + kControlH
                        + 4 + kCaptionH + kControlH + 4 + kCaptionH + kControlH;
    auto cols = b.removeFromTop(devBlockH);
    auto left = cols.removeFromLeft(cols.getWidth() / 2);
    auto right = cols;
    right.removeFromLeft(24);

    instructionsCaption_.setBounds(left.removeFromTop(kCaptionH));
    instructionsEditor_.setBounds(left.removeFromTop(70));
    left.removeFromTop(4);
    instructionsHintLabel_.setBounds(left.removeFromTop(56));
    left.removeFromTop(kRowGap);
    devSourceCaption_.setBounds(left.removeFromTop(kCaptionH));
    devSourceChoice_.setBounds(left.removeFromTop(kControlH));
    left.removeFromTop(4);
    devWavInCaption_.setBounds(left.removeFromTop(kCaptionH));
    devWavInEditor_.setBounds(left.removeFromTop(kControlH));
    left.removeFromTop(4);
    devWavOutCaption_.setBounds(left.removeFromTop(kCaptionH));
    devWavOutEditor_.setBounds(left.removeFromTop(kControlH));

    mockToggle_.setBounds(right.removeFromTop(24));
    mockLatencyCaption_.setBounds(right.removeFromTop(kCaptionH));
    devMockLatencySlider_.setBounds(right.removeFromTop(kControlH));
    devToneFreqCaption_.setBounds(right.removeFromTop(kCaptionH + 4));
    devToneFreqSlider_.setBounds(right.removeFromTop(kControlH));
    devToneLevelCaption_.setBounds(right.removeFromTop(kCaptionH + 4));
    devToneLevelSlider_.setBounds(right.removeFromTop(kControlH));
    right.removeFromTop(4);
    loopbackToggle_.setBounds(right.removeFromTop(24));
    right.removeFromTop(kRowGap);
    developerHintLabel_.setBounds(right.withHeight(56));
}

// =========================================================================== SettingsWindow

SettingsWindow::SettingsWindow(ApplicationController& controller)
    : juce::DocumentWindow("LingoFlow Settings",
                           juce::Colour(0xff1a1d20u),
                           juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    // No delete-on-close: close hides, the owner window owns the lifetime
    // (the rule established in task 015).

    auto* content = new SettingsContent(controller);
    content_ = content;
    setContentOwned(content, true);

    setResizable(true, true);
    // UI-03: tabs made the old column-stacking height unnecessary; the widest
    // page (Audio) and the tallest (Advanced) fit comfortably here.
    setResizeLimits(640, 560, 4000, 4000);
    setSize(820, 640);
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
