#include "App/OperatorWindow.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

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
    addAndMakeVisible(startButton_);
    addAndMakeVisible(stopButton_);

    for (auto* chip : { &appValue_, &audioValue_, &sessionValue_, &ndiValue_ })
    {
        chip->setFont(uiFont(15.0f));
        addAndMakeVisible(*chip);
    }

    detailLabel_.setFont(uiFont(13.0f));
    detailLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd29922u));
    addAndMakeVisible(detailLabel_);

    // ------------------------------------------------------------- settings
    caption(deviceCaption_, "Audio device (single ASIO in/out)", *this);
    deviceChoice_.onChange = [this] { deviceSelected(); };
    addAndMakeVisible(deviceChoice_);

    refreshDevicesButton_.onClick = [this] { refreshDevicesPressed(); };
    addAndMakeVisible(refreshDevicesButton_);

    deviceNoteLabel_.setFont(uiFont(11.0f));
    deviceNoteLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    addAndMakeVisible(deviceNoteLabel_);

    caption(rateCaption_, "Sample rate", *this);
    rateChoice_.onChange = [this] { rateSelected(); };
    addAndMakeVisible(rateChoice_);

    caption(bufferCaption_, "Buffer size (frames)", *this);
    const auto [bufferMin, bufferMax] = config::bufferFramesRange();
    fader(bufferSlider_, bufferMin, bufferMax, 16, *this);
    // Geometry fields commit at the end of a drag or right after a typed value;
    // they are device-restart fields, so there is nothing to stream live.
    bufferSlider_.onDragStart = [this] { geometryDragging_ = true; };
    bufferSlider_.onDragEnd = [this]
    {
        geometryDragging_ = false;
        bufferChanged();
    };
    bufferSlider_.onValueChange = [this]
    {
        if (updatingWidgets_ || geometryDragging_)
            return;
        bufferChanged();   // typed text arrives with no active drag
    };

    caption(inputChannelCaption_, "Input channel (one-based)", *this);
    const auto [channelMin, channelMax] = config::channelRange();
    fader(inputChannelSlider_, channelMin, channelMax, 1, *this);
    caption(outputChannelCaption_, "Output channel (one-based)", *this);
    fader(outputChannelSlider_, channelMin, channelMax, 1, *this);

    for (auto* slider : { &inputChannelSlider_, &outputChannelSlider_ })
    {
        slider->onDragStart = [this] { geometryDragging_ = true; };
        slider->onDragEnd = [this]
        {
            geometryDragging_ = false;
            channelChanged();
        };
        slider->onValueChange = [this]
        {
            if (updatingWidgets_ || geometryDragging_)
                return;
            channelChanged();
        };
    }

    caption(sourceCaption_, "Input language", *this);
    sourceChoice_.onChange = [this] { sourceSelected(); };
    addAndMakeVisible(sourceChoice_);

    caption(targetCaption_, "Output language", *this);
    targetChoice_.onChange = [this] { targetSelected(); };
    addAndMakeVisible(targetChoice_);

    pairWarningLabel_.setFont(uiFont(12.0f));
    pairWarningLabel_.setColour(juce::Label::textColourId, juce::Colour(0xfff85149u));
    addAndMakeVisible(pairWarningLabel_);

    // --------------------------------------------------------------- meters
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

    caption(jitterCaption_, "Jitter pre-roll (ms) - live", *this);
    const auto [jitterMin, jitterMax] = config::jitterBufferRange();
    fader(jitterSlider_, jitterMin, jitterMax, 10, *this);
    jitterSlider_.onValueChange = [this] { jitterMoved(); };
    jitterSlider_.onDragStart = [this] { jitterDragging_ = true; };
    jitterSlider_.onDragEnd = [this]
    {
        jitterDragging_ = false;
        jitterCommit();
    };

    latencyLabel_.setFont(uiFont(12.0f));
    latencyLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    addAndMakeVisible(latencyLabel_);

    // ------------------------------------------------------------- readouts
    countersLabel_.setFont(monoFont());
    countersLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffc9d1d9u));
    addAndMakeVisible(countersLabel_);

    caption(subtitleCaption_, "SUBTITLES - the typed text model (task 013)", *this);

    currentSubtitleLabel_.setFont(uiFont(20.0f));
    currentSubtitleLabel_.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
    currentSubtitleLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(currentSubtitleLabel_);

    historyLabel_.setFont(uiFont(13.0f));
    historyLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    historyLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(historyLabel_);

    textSummaryLabel_.setFont(uiFont(11.0f));
    textSummaryLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff6e7681u));
    addAndMakeVisible(textSummaryLabel_);

    noteLabel_.setFont(uiFont(12.0f));
    addAndMakeVisible(noteLabel_);

    controller_.refreshDevices();   // the first list the window can honestly show
    rebuild();
    startTimer(100);
}

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

void OperatorContent::rateSelected()
{
    if (updatingWidgets_)
        return;

    const int index = rateChoice_.getSelectedItemIndex();

    if (index < 0 || static_cast<std::size_t> (index) >= rateCache_.size())
        return;

    const int rate = std::stoi(rateCache_[static_cast<std::size_t> (index)].value);

    commitSettings([rate](AppConfig& cfg) { cfg.audio.sampleRate = rate; });
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

void OperatorContent::bufferChanged()
{
    const int frames = static_cast<int> (bufferSlider_.getValue());

    commitSettings([frames](AppConfig& cfg) { cfg.audio.bufferFrames = frames; });
}

void OperatorContent::channelChanged()
{
    const int in = static_cast<int> (inputChannelSlider_.getValue());
    const int out = static_cast<int> (outputChannelSlider_.getValue());

    commitSettings([in, out](AppConfig& cfg)
    {
        cfg.audio.inputChannel = in;
        cfg.audio.outputChannel = out;
    });
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

void OperatorContent::jitterMoved()
{
    if (updatingWidgets_)
        return;

    controller_.setJitterLive(static_cast<int> (jitterSlider_.getValue()));

    if (!jitterDragging_)
        jitterCommit();
}

void OperatorContent::jitterCommit()
{
    const int ms = static_cast<int> (jitterSlider_.getValue());

    commitSettings([ms](AppConfig& cfg) { cfg.translation.jitterBufferMs = ms; });
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

    startButton_.setEnabled(panel.canStart || panel.faulted);
    startButton_.setButtonText(panel.faulted ? "Retry" : "Start");
    stopButton_.setEnabled(panel.canStop);

    syncOptions(deviceChoice_, deviceCache_, panel.devices, panel.selectedDevice);
    syncOptions(rateChoice_, rateCache_, panel.sampleRates, panel.selectedSampleRate);
    syncOptions(sourceChoice_, sourceCache_, panel.sourceLanguages, panel.selectedSource);
    syncOptions(targetChoice_, targetCache_, panel.targetLanguages, panel.selectedTarget);

    deviceNoteLabel_.setText(juce::String(panel.deviceNote),
                             juce::NotificationType::dontSendNotification);
    pairWarningLabel_.setText(juce::String(panel.languagePairWarning),
                              juce::NotificationType::dontSendNotification);

    bufferSlider_.setValue(panel.bufferFrames, juce::NotificationType::dontSendNotification);
    inputChannelSlider_.setValue(panel.inputChannel,
                                 juce::NotificationType::dontSendNotification);
    outputChannelSlider_.setValue(panel.outputChannel,
                                  juce::NotificationType::dontSendNotification);

    if (!gainDragging_)
    {
        inputGainSlider_.setValue(panel.inputGainDb,
                                  juce::NotificationType::dontSendNotification);
        outputGainSlider_.setValue(panel.outputGainDb,
                                   juce::NotificationType::dontSendNotification);
    }

    if (!jitterDragging_)
        jitterSlider_.setValue(panel.jitterMs, juce::NotificationType::dontSendNotification);

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

    latencyLabel_.setText(juce::String(panel.latencySummary),
                          juce::NotificationType::dontSendNotification);

    std::string counters;
    for (const auto& [label, value] : panel.counters)
        counters += std::format("{:<24}{:>12}\n", label, value);
    countersLabel_.setText(juce::String(counters), juce::NotificationType::dontSendNotification);

    currentSubtitleLabel_.setText(panel.currentSubtitle.empty()
                                      ? juce::String(".")
                                      : juce::String(panel.currentSubtitle),
                                  juce::NotificationType::dontSendNotification);

    std::string history;
    for (const auto& line : panel.subtitleHistory)
        history += line + "\n";
    historyLabel_.setText(juce::String(history), juce::NotificationType::dontSendNotification);

    textSummaryLabel_.setText(juce::String(panel.textSummary),
                              juce::NotificationType::dontSendNotification);
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

    auto header = bounds.removeFromTop(32);
    stopButton_.setBounds(header.removeFromRight(96));
    startButton_.setBounds(header.removeFromRight(96).withTrimmedRight(6));
    titleLabel_.setBounds(header);

    auto chips = bounds.removeFromTop(26);
    appValue_.setBounds(chips.removeFromLeft(220));
    audioValue_.setBounds(chips.removeFromLeft(340));
    sessionValue_.setBounds(chips.removeFromLeft(260));
    ndiValue_.setBounds(chips.removeFromLeft(220));

    detailLabel_.setBounds(bounds.removeFromTop(20));
    bounds.removeFromTop(6);

    auto content = bounds;
    auto left = content.removeFromLeft(350);
    auto middle = content.removeFromLeft(420);
    auto right = content;
    left.removeFromTop(4);
    middle.removeFromTop(4);

    // --- settings column
    deviceCaption_.setBounds(left.removeFromTop(16));
    deviceChoice_.setBounds(left.removeFromTop(26));
    refreshDevicesButton_.setBounds(left.removeFromTop(28));
    deviceNoteLabel_.setBounds(left.removeFromTop(30));
    left.removeFromTop(6);

    rateCaption_.setBounds(left.removeFromTop(16));
    rateChoice_.setBounds(left.removeFromTop(26));
    left.removeFromTop(6);

    bufferCaption_.setBounds(left.removeFromTop(16));
    bufferSlider_.setBounds(left.removeFromTop(28));
    left.removeFromTop(2);

    inputChannelCaption_.setBounds(left.removeFromTop(16));
    inputChannelSlider_.setBounds(left.removeFromTop(28));
    outputChannelCaption_.setBounds(left.removeFromTop(16));
    outputChannelSlider_.setBounds(left.removeFromTop(28));
    left.removeFromTop(6);

    sourceCaption_.setBounds(left.removeFromTop(16));
    sourceChoice_.setBounds(left.removeFromTop(26));
    targetCaption_.setBounds(left.removeFromTop(20));
    targetChoice_.setBounds(left.removeFromTop(26));
    pairWarningLabel_.setBounds(left);

    // --- meters column
    inputMeterCaption_.setBounds(middle.removeFromTop(16));
    inputMeter_.setBounds(middle.removeFromTop(22));
    {
        auto row = middle.removeFromTop(30);
        inputMuteButton_.setBounds(row.removeFromRight(96).withTrimmedLeft(6));
        inputGainSlider_.setBounds(row);
    }
    appliedInputLabel_.setBounds(middle.removeFromTop(16));
    middle.removeFromTop(8);

    outputMeterCaption_.setBounds(middle.removeFromTop(16));
    outputMeter_.setBounds(middle.removeFromTop(22));
    {
        auto row = middle.removeFromTop(30);
        outputMuteButton_.setBounds(row.removeFromRight(96).withTrimmedLeft(6));
        outputGainSlider_.setBounds(row);
    }
    appliedOutputLabel_.setBounds(middle.removeFromTop(16));
    middle.removeFromTop(8);

    jitterCaption_.setBounds(middle.removeFromTop(16));
    jitterSlider_.setBounds(middle.removeFromTop(30));
    latencyLabel_.setBounds(middle.removeFromTop(40));

    // --- readout column
    countersLabel_.setBounds(right.removeFromTop(360));
    right.removeFromTop(4);
    subtitleCaption_.setBounds(right.removeFromTop(16));
    currentSubtitleLabel_.setBounds(right.removeFromTop(60));
    historyLabel_.setBounds(right.removeFromTop(150));
    textSummaryLabel_.setBounds(right.removeFromTop(16));
    noteLabel_.setBounds(right);
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
    setResizeLimits(1040, 700, 4000, 4000);
    setSize(1200, 780);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

void OperatorWindow::closeButtonPressed()
{
    if (auto* app = juce::JUCEApplication::getInstance())
        app->systemRequestedQuit();
}

} // namespace liveai
