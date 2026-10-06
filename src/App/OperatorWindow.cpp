#include "App/OperatorWindow.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "App/DiagnosticsWindow.h"
#include "App/SettingsWindow.h"
#include "App/UiWidgets.h"
#include "Config/ConfigSchema.h"

namespace liveai {
namespace {

juce::Font uiFont(float height = 15.0f)
{
    return juce::Font(juce::FontOptions().withHeight(height));
}

juce::Font monoFont(float height = 13.0f)
{
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                        height, juce::Font::plain));
}

juce::Label& caption(juce::Label& label, const juce::String& text, juce::Component& parent)
{
    label.setFont(uiFont(12.0f));
    label.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    label.setText(text, juce::NotificationType::dontSendNotification);
    parent.addAndMakeVisible(label);
    return label;
}

juce::Slider& fader(juce::Slider& slider, double min, double max, double interval,
                    juce::Component& parent)
{
    slider.setRange(min, max, interval);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 74, 22);
    parent.addAndMakeVisible(slider);
    return slider;
}

} // namespace

// ================================================================================ MeterBar

float MeterBar::positionForDb(float db) noexcept
{
    // -60 dBFS at the left edge, 0 at the right: a display window, the same
    // reading convention the meters' own -60 threshold documents.
    const double scaled = (static_cast<double> (db) + 60.0) / 60.0;
    return static_cast<float> (std::clamp(scaled, 0.0, 1.0));
}

void MeterBar::setLevels(const UiMeterView& view)
{
    view_ = view;
    repaint();
}

void MeterBar::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    g.setColour(juce::Colour(0xff16191cu));
    g.fillRoundedRectangle(bounds, 3.0f);

    const float peakX = bounds.getX() + positionForDb(view_.peakDb) * bounds.getWidth();
    const float rmsX = bounds.getX() + positionForDb(view_.rmsDb) * bounds.getWidth();

    g.setColour(juce::Colour(0xff2f81f7u).withAlpha(0.85f));   // RMS body
    g.fillRoundedRectangle({ bounds.getX() + 1.0f, bounds.getY() + 1.0f,
                             jmax(0.0f, rmsX - bounds.getX() - 1.0f),
                             bounds.getHeight() - 2.0f }, 2.0f);

    g.setColour(view_.clipping ? juce::Colour(0xfff85149u)      // peak line
                               : juce::Colour(0xffe6edf3u));
    g.fillRect(juce::Rectangle<float> (peakX - 1.0f, bounds.getY() + 1.0f, 2.5f,
                                       bounds.getHeight() - 2.0f));

    g.setColour(juce::Colour(0xff8b949eu));                     // scale ticks
    for (int db = -60; db <= 0; db += 12)
    {
        const float x = bounds.getX() + positionForDb(static_cast<float> (db)) * bounds.getWidth();
        g.fillRect(juce::Rectangle<float> (x, bounds.getY() + bounds.getHeight() - 3.0f, 1.0f, 3.0f));
    }

    if (view_.clipping)
    {
        g.setColour(juce::Colour(0xfff85149u));                 // the latched clip lamp
        g.fillEllipse(bounds.getRight() - 13.0f, bounds.getCentreY() - 4.0f, 8.0f, 8.0f);
    }
    else if (!view_.signalPresent)
    {
        g.setFont(uiFont(10.0f));
        g.setColour(juce::Colour(0xff8b949eu));
        g.drawFittedText("no signal", getLocalBounds(), juce::Justification::centredRight, 1,
                         static_cast<float> (getWidth()) - 20.0f);
    }
}

// ============================================================================ OperatorContent

OperatorContent::OperatorContent(ApplicationController& controller)
    : controller_(controller)
{
    // ---------------------------------------------------------------- header
    titleLabel_.setFont(uiFont(17.0f));
    titleLabel_.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
    titleLabel_.setText(juce::String(std::format("{} {}",
                                                 JUCE_APPLICATION_NAME_STRING,
                                                 JUCE_APPLICATION_VERSION_STRING)),
                        juce::NotificationType::dontSendNotification);
    addAndMakeVisible(titleLabel_);

    startButton_.onClick = [this] { startPressed(); };
    stopButton_.onClick = [this] { stopPressed(); };
    settingsButton_.onClick = [this] { settingsPressed(); };
    diagnosticsButton_.onClick = [this] { diagnosticsPressed(); };
    addAndMakeVisible(startButton_);
    addAndMakeVisible(stopButton_);
    addAndMakeVisible(settingsButton_);
    addAndMakeVisible(diagnosticsButton_);

    devBadge_.setFont(uiFont(13.0f));
    devBadge_.setColour(juce::Label::textColourId, juce::Colour(0xfff85149u));
    devBadge_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff3c1416u));
    devBadge_.setJustificationType(juce::Justification::centredLeft);
    devBadge_.setVisible(false);   // empty in production - the band appears only when real
    addAndMakeVisible(devBadge_);

    // ---------------------------------------------------------- system status
    ui::sectionHeader(statusHeader_, "SYSTEM STATUS", *this);

    for (auto* chip : { &appValue_, &audioValue_, &sessionValue_, &ndiValue_ })
    {
        chip->setFont(uiFont(15.0f));
        addAndMakeVisible(*chip);
    }

    detailLabel_.setFont(uiFont(13.0f));
    detailLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd29922u));
    addAndMakeVisible(detailLabel_);

    credentialLabel_.setFont(uiFont(12.0f));
    credentialLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    addAndMakeVisible(credentialLabel_);

    // ------------------------------------------------------------------ audio
    ui::sectionHeader(audioHeader_, "AUDIO - routing and levels", *this);

    caption(deviceCaption_, "Audio device (single ASIO in/out)", *this);
    deviceChoice_.onChange = [this] { deviceSelected(); };
    addAndMakeVisible(deviceChoice_);

    refreshDevicesButton_.onClick = [this] { refreshDevicesPressed(); };
    addAndMakeVisible(refreshDevicesButton_);

    deviceNoteLabel_.setFont(uiFont(11.0f));
    deviceNoteLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    addAndMakeVisible(deviceNoteLabel_);

    // Discrete channel selection (UI-01 §4): the list IS the device's channel
    // space (driver names once opened), so there is no in-between value to
    // land on and no invented maximum to scroll past. Sample rate, buffer and
    // jitter are Settings-window fields now - chosen before a show, not in it.
    caption(inputChannelCaption_, "Input channel", *this);
    inputChannelChoice_.onChange = [this] { channelChanged(); };
    addAndMakeVisible(inputChannelChoice_);

    caption(outputChannelCaption_, "Output channel", *this);
    outputChannelChoice_.onChange = [this] { channelChanged(); };
    addAndMakeVisible(outputChannelChoice_);

    channelNoteLabel_.setFont(uiFont(11.0f));
    channelNoteLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff6e7681u));
    addAndMakeVisible(channelNoteLabel_);

    caption(inputMeterCaption_, "INPUT - what the translator hears", *this);
    addAndMakeVisible(inputMeter_);

    const auto [gainMin, gainMax] = config::gainRange();
    fader(inputGainSlider_, gainMin, gainMax, 0.5, *this);
    inputGainSlider_.onValueChange = [this] { gainMoved(); };
    inputGainSlider_.onDragStart = [this] { gainDragging_ = true; };
    inputGainSlider_.onDragEnd = [this]
    {
        gainDragging_ = false;
        gainCommit();
    };

    appliedInputLabel_.setFont(uiFont(11.0f));
    appliedInputLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    addAndMakeVisible(appliedInputLabel_);

    inputMuteButton_.setClickingTogglesState(true);
    inputMuteButton_.onClick = [this] { muteToggled(); };
    addAndMakeVisible(inputMuteButton_);

    caption(outputMeterCaption_, "OUTPUT - what the audience hears", *this);
    addAndMakeVisible(outputMeter_);

    fader(outputGainSlider_, gainMin, gainMax, 0.5, *this);
    outputGainSlider_.onValueChange = [this] { gainMoved(); };
    outputGainSlider_.onDragStart = [this] { gainDragging_ = true; };
    outputGainSlider_.onDragEnd = [this]
    {
        gainDragging_ = false;
        gainCommit();
    };

    appliedOutputLabel_.setFont(uiFont(11.0f));
    appliedOutputLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    addAndMakeVisible(appliedOutputLabel_);

    outputMuteButton_.setClickingTogglesState(true);
    outputMuteButton_.onClick = [this] { muteToggled(); };
    addAndMakeVisible(outputMuteButton_);

    // ------------------------------------------------------------ translation
    ui::sectionHeader(translationHeader_, "TRANSLATION - pair and live text", *this);

    caption(sourceCaption_, "Input language", *this);
    sourceChoice_.onChange = [this] { sourceSelected(); };
    addAndMakeVisible(sourceChoice_);

    caption(targetCaption_, "Output language", *this);
    targetChoice_.onChange = [this] { targetSelected(); };
    addAndMakeVisible(targetChoice_);

    pairWarningLabel_.setFont(uiFont(12.0f));
    pairWarningLabel_.setColour(juce::Label::textColourId, juce::Colour(0xfff85149u));
    addAndMakeVisible(pairWarningLabel_);

    caption(subtitleCaption_, "Subtitles", *this);

    currentSubtitleLabel_.setFont(uiFont(20.0f));
    currentSubtitleLabel_.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
    currentSubtitleLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(currentSubtitleLabel_);

    historyLabel_.setFont(uiFont(13.0f));
    historyLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    historyLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(historyLabel_);

    // ------------------------------------------------------------ live health
    // The compact four-fact strip (UI-01): the raw counter wall and the full
    // 018 accounting moved to the Diagnostics window behind this button.
    ui::sectionHeader(healthHeader_, "LIVE HEALTH", *this);

    latencyLabel_.setFont(uiFont(12.0f));
    latencyLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    latencyLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(latencyLabel_);

    healthLabel_.setFont(monoFont());
    healthLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffc9d1d9u));
    addAndMakeVisible(healthLabel_);

    noteLabel_.setFont(uiFont(12.0f));
    addAndMakeVisible(noteLabel_);

    controller_.refreshDevices();   // the first list the window can honestly show
    rebuild();
    startTimer(100);
}

OperatorContent::~OperatorContent() = default;   // SettingsWindow is complete here

// --------------------------------------------------------------------------- commands

void OperatorContent::startPressed()
{
    if (controller_.state() == ApplicationState::faulted)
        controller_.clearFault();   // the original failure stays in the log

    if (controller_.start())
        actionNote_.clear();
    else
        actionNote_ = "Start failed: " + controller_.status().detail;

    rebuild();
}

void OperatorContent::stopPressed()
{
    controller_.stop();
    actionNote_ = "Stopped. Device, rate, channel, language and NDI changes apply on Start.";
    rebuild();
}

void OperatorContent::settingsPressed()
{
    // One dialog object for the lifetime of the window; close hides it, and a
    // reopen always re-reads the settings first - a second stale copy of the
    // configuration is exactly the thing this product refuses to have.
    if (settingsWindow_ == nullptr)
        settingsWindow_ = std::make_unique<SettingsWindow>(controller_);
    else
        settingsWindow_->reopen();
}

void OperatorContent::diagnosticsPressed()
{
    // UI-01: the engineering surface opens on request, owns the Export button,
    // and follows the same hide/reopen lifetime rule as the settings dialog.
    if (diagnosticsWindow_ == nullptr)
        diagnosticsWindow_ = std::make_unique<DiagnosticsWindow>(controller_);
    else
        diagnosticsWindow_->reopen();
}

void OperatorContent::refreshDevicesPressed()
{
    const int count = controller_.refreshDevices();
    actionNote_ = "Device scan: " + std::to_string(count) + " device(s).";
    rebuild();
}

void OperatorContent::commitSettings(std::function<void(AppConfig&)> mutate)
{
    if (updatingWidgets_)
        return;

    AppConfig candidate = controller_.config().current();
    mutate(candidate);

    std::string note;
    controller_.updateSettings(candidate, note);
    actionNote_ = std::move(note);
    rebuild();
}

void OperatorContent::deviceSelected()
{
    if (updatingWidgets_)
        return;

    const int index = deviceChoice_.getSelectedItemIndex();

    if (index < 0 || static_cast<std::size_t> (index) >= deviceCache_.size())
        return;

    const std::string id = deviceCache_[static_cast<std::size_t> (index)].value;

    commitSettings([id](AppConfig& cfg)
    {
        cfg.audio.inputDeviceId = id;
        cfg.audio.outputDeviceId = id;   // SPEC: one ASIO device carries input and output
    });
}

void OperatorContent::channelChanged()
{
    if (updatingWidgets_)
        return;

    // The discrete choices (UI-01): the value list is the panel's, mirrored
    // into the caches; a combo can only land on a real channel index.
    const int inIndex = inputChannelChoice_.getSelectedItemIndex();
    const int outIndex = outputChannelChoice_.getSelectedItemIndex();

    if (inIndex < 0 || static_cast<std::size_t> (inIndex) >= inputChannelCache_.size()
        || outIndex < 0 || static_cast<std::size_t> (outIndex) >= outputChannelCache_.size())
        return;

    const int in = std::stoi(inputChannelCache_[static_cast<std::size_t> (inIndex)].value);
    const int out = std::stoi(outputChannelCache_[static_cast<std::size_t> (outIndex)].value);

    commitSettings([in, out](AppConfig& cfg)
    {
        cfg.audio.inputChannel = in;
        cfg.audio.outputChannel = out;
    });
}
void OperatorContent::sourceSelected()
{
    if (updatingWidgets_)
        return;

    const int index = sourceChoice_.getSelectedItemIndex();

    if (index < 0 || static_cast<std::size_t> (index) >= sourceCache_.size())
        return;

    const std::string code = sourceCache_[static_cast<std::size_t> (index)].value;

    commitSettings([code](AppConfig& cfg) { cfg.translation.inputLanguage = code; });
}

void OperatorContent::targetSelected()
{
    if (updatingWidgets_)
        return;

    const int index = targetChoice_.getSelectedItemIndex();

    if (index < 0 || static_cast<std::size_t> (index) >= targetCache_.size())
        return;

    const std::string code = targetCache_[static_cast<std::size_t> (index)].value;

    commitSettings([code](AppConfig& cfg) { cfg.translation.outputLanguage = code; });
}

void OperatorContent::gainMoved()
{
    if (updatingWidgets_)
        return;

    controller_.setGainsLive(static_cast<float> (inputGainSlider_.getValue()),
                             static_cast<float> (outputGainSlider_.getValue()));

    // A typed value arrives while no drag is active: commit it immediately.
    if (!gainDragging_)
        gainCommit();
}

void OperatorContent::gainCommit()
{
    const float in = static_cast<float> (inputGainSlider_.getValue());
    const float out = static_cast<float> (outputGainSlider_.getValue());

    commitSettings([in, out](AppConfig& cfg)
    {
        cfg.audio.inputGainDb = in;
        cfg.audio.outputGainDb = out;
    });
}

void OperatorContent::muteToggled()
{
    controller_.setMutesLive(inputMuteButton_.getToggleState(),
                             outputMuteButton_.getToggleState());
    actionNote_ = "Mutes are live only and were not saved (task 006: a show never starts muted).";
    rebuild();
}

// --------------------------------------------------------------------------- refresh

void OperatorContent::timerCallback()
{
    rebuild();
}

juce::Colour OperatorContent::stateColour(std::string_view state) noexcept
{
    // The words come from the modules' own nameOf(); mapping them to meaning is
    // cosmetic - the tested contract is the word (UiModel tests assert it).
    if (state == "running" || state == "connected" || state == "publishing")
        return juce::Colour(0xff3fb950u);   // the show is working
    if (state == "starting" || state == "stopping" || state == "opened"
        || state == "connecting" || state == "reconnecting" || state == "ready")
        return juce::Colour(0xffd29922u);   // on its way, not there yet
    if (state == "faulted")
        return juce::Colour(0xfff85149u);   // operator, look here
    return juce::Colour(0xff8b949eu);       // stopped / off / disabled
}

void OperatorContent::syncOptions(juce::ComboBox& box, std::vector<UiOption>& cache,
                                  const std::vector<UiOption>& fresh, int selectedIndex)
{
    if (cache != fresh)
    {
        box.clear(juce::NotificationType::dontSendNotification);

        for (std::size_t i = 0; i < fresh.size(); ++i)
            box.addItem(juce::String(fresh[i].label), static_cast<int> (i) + 1);

        cache = fresh;
    }

    if (selectedIndex >= 0)
        box.setSelectedId(selectedIndex + 1, juce::NotificationType::dontSendNotification);
    else
        box.setSelectedId(0, juce::NotificationType::dontSendNotification);   // nothing chosen
}

void OperatorContent::rebuild()
{
    updatingWidgets_ = true;

    const OperatorPanel panel = buildOperatorPanel(controller_, actionNote_);

    appValue_.setText("app: " + juce::String(panel.applicationState),
                      juce::NotificationType::dontSendNotification);
    audioValue_.setText("audio: " + juce::String(panel.audioState) + " ("
                            + juce::String(panel.audioBackendName) + ")",
                        juce::NotificationType::dontSendNotification);
    sessionValue_.setText("translation: " + juce::String(panel.sessionState),
                          juce::NotificationType::dontSendNotification);
    ndiValue_.setText("NDI: " + juce::String(panel.ndiState),
                      juce::NotificationType::dontSendNotification);
    appValue_.setColour(juce::Label::textColourId, stateColour(panel.applicationState));
    audioValue_.setColour(juce::Label::textColourId, stateColour(panel.audioState));
    sessionValue_.setColour(juce::Label::textColourId, stateColour(panel.sessionState));
    ndiValue_.setColour(juce::Label::textColourId, stateColour(panel.ndiState));

    detailLabel_.setText(panel.detail == "ok" ? juce::String() : juce::String(panel.detail),
                         juce::NotificationType::dontSendNotification);

    // Task 015: the credential state belongs to the main screen too - an
    // operator must be able to see "no key" before pressing Start, not only
    // inside a dialog they have not opened.
    credentialLabel_.setText(juce::String(panel.credentialLine),
                             juce::NotificationType::dontSendNotification);
    credentialLabel_.setColour(juce::Label::textColourId,
                               panel.credentialLine.rfind("API key: stored", 0) == 0
                                   ? juce::Colour(0xff8b949eu)
                                   : juce::Colour(0xffd29922u));

    // Task 019: visible only when the mounted plan is a developer plan; the
    // text itself is the plan's badge, extended with the loopback worker's
    // real state (UiModel does the composing, this widget only paints it).
    devBadge_.setVisible(!panel.developerBadge.empty());
    devBadge_.setText(juce::String(panel.developerBadge), juce::NotificationType::dontSendNotification);

    startButton_.setEnabled(panel.canStart || panel.faulted);
    startButton_.setButtonText(panel.faulted ? "Retry" : "Start");
    stopButton_.setEnabled(panel.canStop);

    syncOptions(deviceChoice_, deviceCache_, panel.devices, panel.selectedDevice);
    syncOptions(sourceChoice_, sourceCache_, panel.sourceLanguages, panel.selectedSource);
    syncOptions(targetChoice_, targetCache_, panel.targetLanguages, panel.selectedTarget);
    syncOptions(inputChannelChoice_, inputChannelCache_, panel.inputChannelChoices,
                panel.selectedInputChannel);
    syncOptions(outputChannelChoice_, outputChannelCache_, panel.outputChannelChoices,
                panel.selectedOutputChannel);

    deviceNoteLabel_.setText(juce::String(panel.deviceNote),
                             juce::NotificationType::dontSendNotification);
    channelNoteLabel_.setText(juce::String(panel.channelNote),
                              juce::NotificationType::dontSendNotification);
    pairWarningLabel_.setText(juce::String(panel.languagePairWarning),
                              juce::NotificationType::dontSendNotification);

    if (!gainDragging_)
    {
        inputGainSlider_.setValue(panel.inputGainDb,
                                  juce::NotificationType::dontSendNotification);
        outputGainSlider_.setValue(panel.outputGainDb,
                                   juce::NotificationType::dontSendNotification);
    }

    inputMuteButton_.setToggleState(panel.inputMuted,
                                    juce::NotificationType::dontSendNotification);
    outputMuteButton_.setToggleState(panel.outputMuted,
                                     juce::NotificationType::dontSendNotification);

    appliedInputLabel_.setText(
        juce::String(std::format("applying {:+.1f} dB (gliding)", panel.appliedInputGainDb)),
        juce::NotificationType::dontSendNotification);
    appliedOutputLabel_.setText(
        juce::String(std::format("applying {:+.1f} dB (gliding)", panel.appliedOutputGainDb)),
        juce::NotificationType::dontSendNotification);

    inputMeter_.setLevels(panel.inputMeter);
    outputMeter_.setLevels(panel.outputMeter);

    // The one honest sentence about buffer delay (kept in full, kind and all -
    // UI-01 did not shorten the truth, it shortened the screen), plus the four
    // health facts. The detailed accounting is one button away, in Diagnostics.
    latencyLabel_.setText(juce::String(panel.latencySummary),
                          juce::NotificationType::dontSendNotification);

    std::string health;
    for (const auto& [label, value] : panel.health)
        health += std::format("{:<16}{}\n", label, value);
    healthLabel_.setText(juce::String(health), juce::NotificationType::dontSendNotification);

    currentSubtitleLabel_.setText(panel.currentSubtitle.empty()
                                      ? juce::String()
                                      : juce::String(panel.currentSubtitle),
                                  juce::NotificationType::dontSendNotification);

    std::string history;
    for (const auto& line : panel.subtitleHistory)
        history += line + "\n";
    historyLabel_.setText(juce::String(history), juce::NotificationType::dontSendNotification);

    noteLabel_.setText(juce::String(panel.actionNote),
                       juce::NotificationType::dontSendNotification);
    noteLabel_.setColour(juce::Label::textColourId,
                         panel.actionNote.rfind("settings refused", 0) == 0
                             ? juce::Colour(0xfff85149u)
                             : juce::Colour(0xffd29922u));

    updatingWidgets_ = false;
}

void OperatorContent::resized()
{
    auto bounds = getLocalBounds().reduced(14);

    // 1. Header: identity left, commands right (Start/Stop stay the prominent
    // pair; Settings opens configuration, Diagnostics opens the engineering
    // surface that took over the raw-counter wall).
    auto header = bounds.removeFromTop(32);
    stopButton_.setBounds(header.removeFromRight(96));
    startButton_.setBounds(header.removeFromRight(96).withTrimmedRight(6));
    settingsButton_.setBounds(header.removeFromRight(110).withTrimmedRight(6));
    diagnosticsButton_.setBounds(header.removeFromRight(120).withTrimmedRight(6));
    titleLabel_.setBounds(header);

    // Task 019: the developer band sits above everything the operator reads -
    // production hides it (empty label, no paint), a developer run cannot
    // scroll it away or overlook it.
    devBadge_.setBounds(bounds.removeFromTop(24));

    // 2. System status: one scannable block directly under the header.
    statusHeader_.setBounds(bounds.removeFromTop(18));
    {
        auto chips = bounds.removeFromTop(24);
        appValue_.setBounds(chips.removeFromLeft(200));
        audioValue_.setBounds(chips.removeFromLeft(360));
        sessionValue_.setBounds(chips.removeFromLeft(240));
        ndiValue_.setBounds(chips);
    }
    detailLabel_.setBounds(bounds.removeFromTop(20));
    credentialLabel_.setBounds(bounds.removeFromTop(18));
    bounds.removeFromTop(6);

    // 3. Audio: routing across the top, then INPUT and OUTPUT as paired
    // columns - channel combo, meter, gain, mute in the same order on both
    // sides so the two halves read as one control surface.
    audioHeader_.setBounds(bounds.removeFromTop(18));
    deviceCaption_.setBounds(bounds.removeFromTop(16));
    {
        auto row = bounds.removeFromTop(26);
        refreshDevicesButton_.setBounds(row.removeFromRight(150).withTrimmedLeft(6));
        deviceChoice_.setBounds(row);
    }
    deviceNoteLabel_.setBounds(bounds.removeFromTop(18));
    bounds.removeFromTop(2);

    auto audioCols = bounds.removeFromTop(150);
    auto inCol = audioCols.removeFromLeft(audioCols.getWidth() / 2);
    auto outCol = audioCols;
    outCol.removeFromLeft(10);

    inputChannelCaption_.setBounds(inCol.removeFromTop(16));
    inputChannelChoice_.setBounds(inCol.removeFromTop(26));
    inputMeterCaption_.setBounds(inCol.removeFromTop(18));
    inputMeter_.setBounds(inCol.removeFromTop(22));
    {
        auto row = inCol.removeFromTop(30);
        inputMuteButton_.setBounds(row.removeFromRight(96).withTrimmedLeft(6));
        inputGainSlider_.setBounds(row);
    }
    appliedInputLabel_.setBounds(inCol.removeFromTop(16));

    outputChannelCaption_.setBounds(outCol.removeFromTop(16));
    outputChannelChoice_.setBounds(outCol.removeFromTop(26));
    outputMeterCaption_.setBounds(outCol.removeFromTop(18));
    outputMeter_.setBounds(outCol.removeFromTop(22));
    {
        auto row = outCol.removeFromTop(30);
        outputMuteButton_.setBounds(row.removeFromRight(96).withTrimmedLeft(6));
        outputGainSlider_.setBounds(row);
    }
    appliedOutputLabel_.setBounds(outCol.removeFromTop(16));

    channelNoteLabel_.setBounds(bounds.removeFromTop(16));
    bounds.removeFromTop(4);

    // 4. Translation: the pair on one line, then the live text underneath.
    translationHeader_.setBounds(bounds.removeFromTop(18));
    {
        auto pairRow = bounds.removeFromTop(44);
        auto leftCol = pairRow.removeFromLeft(240);
        auto rightCol = pairRow.removeFromLeft(260);
        sourceCaption_.setBounds(leftCol.removeFromTop(16));
        sourceChoice_.setBounds(leftCol.removeFromTop(26));
        targetCaption_.setBounds(rightCol.removeFromTop(16));
        targetChoice_.setBounds(rightCol.removeFromTop(26));
        pairWarningLabel_.setBounds(pairRow);
    }
    subtitleCaption_.setBounds(bounds.removeFromTop(16));
    currentSubtitleLabel_.setBounds(bounds.removeFromTop(34));
    historyLabel_.setBounds(bounds.removeFromTop(120));
    bounds.removeFromTop(4);

    // 5. Live health: the four facts, the honest latency sentence - and the
    // door to everything else (the Diagnostics button up in the header).
    healthHeader_.setBounds(bounds.removeFromTop(18));
    latencyLabel_.setBounds(bounds.removeFromTop(34));
    healthLabel_.setBounds(bounds.removeFromTop(62));

    bounds.removeFromTop(4);
    noteLabel_.setBounds(bounds);
}

// ============================================================================ OperatorWindow

OperatorWindow::OperatorWindow(ApplicationController& controller)
    : juce::DocumentWindow(JUCE_APPLICATION_NAME_STRING,
                           juce::Colour(0xff1e2124u),
                           juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    setContentOwned(new OperatorContent(controller), true);
    setResizable(true, true);
    // UI-01 reshaped the content vertically (status -> audio -> translation ->
    // health); the counter wall's width is gone, the hierarchy needs the height.
    setResizeLimits(900, 760, 4000, 4000);
    setSize(1140, 860);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

void OperatorWindow::closeButtonPressed()
{
    if (auto* app = juce::JUCEApplication::getInstance())
        app->systemRequestedQuit();
}

} // namespace liveai
