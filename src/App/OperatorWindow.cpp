#include "App/OperatorWindow.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "App/DiagnosticsWindow.h"
#include "App/SettingsWindow.h"
#include "Config/ConfigSchema.h"

namespace liveai {
namespace {

// ---------------------------------------------------------------- the palette
// Colour communicates state and nothing else (UI-02 Р В РІР‚в„ўР вЂ™Р’В§12): surfaces stay
// neutral, the four state colours appear only on dots, words and the buttons
// that mean them.

namespace ink {

const juce::Colour background    { 0xff1a1d20u };
const juce::Colour card          { 0xff22262bu };
const juce::Colour cardBorder    { 0xff2d333au };
const juce::Colour textPrimary   { 0xffe6edf3u };
const juce::Colour textSecondary { 0xff9aa4afu };
const juce::Colour textMuted     { 0xff6e7681u };
const juce::Colour green         { 0xff3fb950u };
const juce::Colour amber         { 0xffd29922u };
const juce::Colour red           { 0xfff85149u };
const juce::Colour blue          { 0xff2f81f7u };
const juce::Colour control       { 0xff30363du };

} // namespace ink

// ------------------------------------------------------------- the spacing grid
// 4 / 8 / 12 / 16 / 24 / 32 (UI-02 Р В РІР‚в„ўР вЂ™Р’В§11). Component heights are constants, not
// per-widget negotiations.

constexpr int kMargin    = 16;   // window edge -> card
constexpr int kCardPad   = 16;   // card edge -> content
constexpr int kCardGap   = 12;   // between cards
constexpr int kRowGap    = 8;    // between rows inside a card
constexpr int kControlH  = 30;   // ComboBox height, everywhere
constexpr int kSmallBtnH = 30;   // Refresh / Diagnostics
constexpr int kCommandH  = 46;   // START / STOP - the tallest controls on screen
constexpr int kMeterH    = 22;
constexpr int kStatusRowH = 24;
constexpr int kHealthRowH = 22;

/// One AUDIO column: caption, channel combo, meter, numeric level, gain, mute.
/// The same block height on both sides is what makes the pair read as a pair.
constexpr int kColumnH = 14 + kControlH + 4 + 14 + kMeterH + 16 + 4 + 14 + 28 + kRowGap + 30;

juce::Font uiFont(float height = 15.0f, bool bold = false)
{
    juce::Font font { juce::FontOptions().withHeight(height) };
    if (bold)
        font.setBold(true);
    return font;
}

juce::Label& caption(juce::Label& label, const juce::String& text, juce::Component& parent,
                     juce::Colour colour = ink::textSecondary, float height = 12.0f)
{
    label.setFont(uiFont(height));
    label.setColour(juce::Label::textColourId, colour);
    label.setText(text, juce::NotificationType::dontSendNotification);
    parent.addAndMakeVisible(label);
    return label;
}

juce::Slider& fader(juce::Slider& slider, double min, double max, double interval,
                    juce::Component& parent)
{
    slider.setRange(min, max, interval);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 74, 26);
    // The screen scrolls with the wheel, and a cursor crossing a gain fader on
    // its way must never ride that scroll into the live audio path: JUCE's
    // own rule - with the wheel disabled the Slider hands the event back to
    // the parent (the viewport), so scrolling keeps working and the gain
    // stays exactly where the operator left it. Drag, click and keyboard on
    // the fader are untouched.
    slider.setScrollWheelEnabled(false);
    slider.setColour(juce::Slider::backgroundColourId, ink::card);
    slider.setColour(juce::Slider::trackColourId, ink::blue.withAlpha(0.55f));
    slider.setColour(juce::Slider::thumbColourId, ink::textPrimary);
    slider.setColour(juce::Slider::textBoxTextColourId, ink::textPrimary);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, ink::control);
    slider.setColour(juce::Slider::textBoxOutlineColourId, ink::cardBorder);
    parent.addAndMakeVisible(slider);
    return slider;
}

/// The house style of every ComboBox on this screen: same height, same
/// colours - a channel selector must never resemble the gain fader below it.
void styleBox(juce::ComboBox& box)
{
    box.setColour(juce::ComboBox::backgroundColourId, ink::control);
    box.setColour(juce::ComboBox::outlineColourId, ink::cardBorder);
    box.setColour(juce::ComboBox::textColourId, ink::textPrimary);
    box.setColour(juce::ComboBox::arrowColourId, ink::textSecondary);
    box.setColour(juce::ComboBox::focusedOutlineColourId, ink::blue);
}

void styleQuietButton(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, ink::control);
    button.setColour(juce::TextButton::textColourOffId, ink::textPrimary);
    button.setColour(juce::TextButton::textColourOnId, ink::textPrimary);
}

/// The viewport pattern JUCE expects for a width-fitting, height-deciding
/// child: on every resize the viewport offers the width of its content
/// holder (getMaximumVisibleWidth - deliberately not getViewWidth, which
/// mirrors the child's own width and would deadlock at zero), and the child's
/// resized() then decides the scrollable height - see OperatorContent::resized.
/// Without this nudge the child would never hear about window resizes: a
/// Viewport does not touch its content's bounds.
class OperatorViewport final : public juce::Viewport
{
public:
    using juce::Viewport::Viewport;

    void resized() override
    {
        juce::Viewport::resized();

        if (auto* content = getViewedComponent())
            content->setSize(getMaximumVisibleWidth(), content->getHeight());
    }
};
/// How many history lines this screen shows: a tail, not a transcript - the
/// full bounded history stays in the model (and readable in Diagnostics).
constexpr int kOperatorHistoryTail = 3;

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
    const auto bounds = getLocalBounds().toFloat().reduced(0.5f);

    g.setColour(ink::background);
    g.fillRoundedRectangle(bounds, 3.0f);

    const float peakX = bounds.getX() + positionForDb(view_.peakDb) * bounds.getWidth();
    const float rmsX = bounds.getX() + positionForDb(view_.rmsDb) * bounds.getWidth();

    g.setColour(ink::blue.withAlpha(0.85f));                          // RMS body
    g.fillRoundedRectangle({ bounds.getX() + 1.0f, bounds.getY() + 1.0f,
                             jmax(0.0f, rmsX - bounds.getX() - 1.0f),
                             bounds.getHeight() - 2.0f }, 2.0f);

    g.setColour(view_.clipping ? ink::red : ink::textPrimary);        // peak line
    g.fillRect(juce::Rectangle<float> (peakX - 1.0f, bounds.getY() + 1.0f, 2.5f,
                                       bounds.getHeight() - 2.0f));

    g.setColour(ink::textMuted);                                      // scale ticks
    for (int db = -60; db <= 0; db += 12)
    {
        const float x = bounds.getX() + positionForDb(static_cast<float> (db)) * bounds.getWidth();
        g.fillRect(juce::Rectangle<float> (x, bounds.getY() + bounds.getHeight() - 3.0f, 1.0f, 3.0f));
    }

    if (view_.clipping)
    {
        g.setColour(ink::red);                                        // the latched clip lamp
        g.fillEllipse(bounds.getRight() - 13.0f, bounds.getCentreY() - 4.0f, 8.0f, 8.0f);
    }
    else if (!view_.signalPresent)
    {
        g.setFont(uiFont(10.0f));
        g.setColour(ink::textMuted);
        g.drawFittedText("no signal", getLocalBounds(), juce::Justification::centredRight, 1,
                         static_cast<float> (getWidth()) - 20.0f);
    }
}

// ============================================================================ OperatorContent

OperatorContent::OperatorContent(ApplicationController& controller)
    : controller_(controller)
{
    // ---------------------------------------------------------------- header
    titleLabel_.setFont(uiFont(20.0f, true));
    titleLabel_.setColour(juce::Label::textColourId, ink::textPrimary);
    titleLabel_.setText(juce::String(std::format("{} {}",
                                                 JUCE_APPLICATION_NAME_STRING,
                                                 JUCE_APPLICATION_VERSION_STRING)),
                        juce::NotificationType::dontSendNotification);
    addAndMakeVisible(titleLabel_);

    // UI-02 Р В РІР‚в„ўР вЂ™Р’В§4: developer mode is a compact, unmistakable chip - not the red
    // banner that used to make a rehearsal look like an incident. The plan's
    // full sentence stays readable below the status rows (devDetailLabel_).
    devBadge_.setFont(uiFont(11.0f, true));
    devBadge_.setColour(juce::Label::textColourId, ink::red);
    devBadge_.setColour(juce::Label::backgroundColourId, ink::red.withAlpha(0.12f));
    devBadge_.setColour(juce::Label::outlineColourId, ink::red.withAlpha(0.6f));
    devBadge_.setJustificationType(juce::Justification::centred);
    devBadge_.setText("DEVELOPER MODE", juce::NotificationType::dontSendNotification);
    devBadge_.setVisible(false);   // empty in production - the chip appears only when real
    addAndMakeVisible(devBadge_);

    settingsButton_.onClick = [this] { settingsPressed(); };
    styleQuietButton(settingsButton_);
    addAndMakeVisible(settingsButton_);

    // ---------------------------------------------------------- system status
    caption(statusHeader_, "SYSTEM STATUS", *this, ink::textMuted, 11.0f);

    caption(appCaption_, "App", *this, ink::textSecondary, 14.0f);
    caption(audioCaption_, "Audio", *this, ink::textSecondary, 14.0f);
    caption(sessionCaption_, "Translation", *this, ink::textSecondary, 14.0f);
    caption(ndiCaption_, "NDI", *this, ink::textSecondary, 14.0f);

    for (auto* chip : { &appValue_, &audioValue_, &sessionValue_, &ndiValue_ })
    {
        chip->setFont(uiFont(14.0f));
        addAndMakeVisible(*chip);
    }

    detailLabel_.setFont(uiFont(13.0f));
    detailLabel_.setColour(juce::Label::textColourId, ink::amber);
    addAndMakeVisible(detailLabel_);

    credentialLabel_.setFont(uiFont(12.0f));
    credentialLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    addAndMakeVisible(credentialLabel_);

    devDetailLabel_.setFont(uiFont(11.0f));
    devDetailLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    devDetailLabel_.setVisible(false);
    addAndMakeVisible(devDetailLabel_);

    // ------------------------------------------------------------------ audio
    caption(audioHeader_, "AUDIO", *this, ink::textMuted, 11.0f);

    caption(deviceCaption_, "Device (one ASIO in/out)", *this);
    deviceChoice_.onChange = [this] { deviceSelected(); };
    styleBox(deviceChoice_);
    addAndMakeVisible(deviceChoice_);

    refreshDevicesButton_.onClick = [this] { refreshDevicesPressed(); };
    styleQuietButton(refreshDevicesButton_);
    addAndMakeVisible(refreshDevicesButton_);

    deviceNoteLabel_.setFont(uiFont(11.0f));
    deviceNoteLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    addAndMakeVisible(deviceNoteLabel_);

    caption(inputChannelCaption_, "Channel", *this);
    inputChannelChoice_.onChange = [this] { channelChanged(); };
    styleBox(inputChannelChoice_);
    addAndMakeVisible(inputChannelChoice_);

    caption(outputChannelCaption_, "Channel", *this);
    outputChannelChoice_.onChange = [this] { channelChanged(); };
    styleBox(outputChannelChoice_);
    addAndMakeVisible(outputChannelChoice_);

    channelNoteLabel_.setFont(uiFont(11.0f));
    channelNoteLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    addAndMakeVisible(channelNoteLabel_);

    caption(inputMeterCaption_, "Input level", *this);
    addAndMakeVisible(inputMeter_);
    caption(inputLevelLabel_, "", *this, ink::textSecondary, 12.0f);

    caption(inputGainCaption_, "Gain", *this);
    const auto [gainMin, gainMax] = config::gainRange();
    fader(inputGainSlider_, gainMin, gainMax, 0.5, *this);
    inputGainSlider_.onValueChange = [this] { gainMoved(); };
    inputGainSlider_.onDragStart = [this] { gainDragging_ = true; };
    inputGainSlider_.onDragEnd = [this]
    {
        gainDragging_ = false;
        gainCommit();
    };

    inputMuteButton_.setClickingTogglesState(true);
    inputMuteButton_.onClick = [this] { muteToggled(); };
    styleQuietButton(inputMuteButton_);
    addAndMakeVisible(inputMuteButton_);

    caption(outputMeterCaption_, "Output level", *this);
    addAndMakeVisible(outputMeter_);
    caption(outputLevelLabel_, "", *this, ink::textSecondary, 12.0f);

    caption(outputGainCaption_, "Gain", *this);
    fader(outputGainSlider_, gainMin, gainMax, 0.5, *this);
    outputGainSlider_.onValueChange = [this] { gainMoved(); };
    outputGainSlider_.onDragStart = [this] { gainDragging_ = true; };
    outputGainSlider_.onDragEnd = [this]
    {
        gainDragging_ = false;
        gainCommit();
    };

    outputMuteButton_.setClickingTogglesState(true);
    outputMuteButton_.onClick = [this] { muteToggled(); };
    styleQuietButton(outputMuteButton_);
    addAndMakeVisible(outputMuteButton_);

    // ------------------------------------------------------------ translation
    caption(translationHeader_, "TRANSLATION", *this, ink::textMuted, 11.0f);

    caption(sourceCaption_, "From", *this);
    sourceChoice_.onChange = [this] { sourceSelected(); };
    styleBox(sourceChoice_);
    addAndMakeVisible(sourceChoice_);

    arrowLabel_.setFont(uiFont(18.0f));
    arrowLabel_.setColour(juce::Label::textColourId, ink::textSecondary);
    arrowLabel_.setJustificationType(juce::Justification::centred);
    arrowLabel_.setText(juce::String::fromUTF8("\u2192"), juce::NotificationType::dontSendNotification);
    addAndMakeVisible(arrowLabel_);

    caption(targetCaption_, "To", *this);
    targetChoice_.onChange = [this] { targetSelected(); };
    styleBox(targetChoice_);
    addAndMakeVisible(targetChoice_);

    sessionLineLabel_.setFont(uiFont(13.0f));
    addAndMakeVisible(sessionLineLabel_);

    pairWarningLabel_.setFont(uiFont(12.0f));
    pairWarningLabel_.setColour(juce::Label::textColourId, ink::red);
    addAndMakeVisible(pairWarningLabel_);

    currentSubtitleLabel_.setFont(uiFont(18.0f));
    currentSubtitleLabel_.setColour(juce::Label::textColourId, ink::textPrimary);
    currentSubtitleLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(currentSubtitleLabel_);

    historyLabel_.setFont(uiFont(12.0f));
    historyLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    historyLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(historyLabel_);

    // ------------------------------------------------------------ live health
    caption(healthHeader_, "HEALTH", *this, ink::textMuted, 11.0f);

    caption(latencyCaption_, "Latency", *this, ink::textSecondary, 13.0f);
    caption(jitterCaption_, "Jitter fill", *this, ink::textSecondary, 13.0f);
    caption(underrunCaption_, "Underruns", *this, ink::textSecondary, 13.0f);
    caption(reconnectCaption_, "Reconnects", *this, ink::textSecondary, 13.0f);

    for (auto* value : { &latencyValue_, &jitterValue_, &underrunValue_, &reconnectValue_ })
    {
        value->setFont(uiFont(13.0f));
        value->setColour(juce::Label::textColourId, ink::textSecondary);
        addAndMakeVisible(*value);
    }

    diagnosticsButton_.onClick = [this] { diagnosticsPressed(); };
    styleQuietButton(diagnosticsButton_);
    addAndMakeVisible(diagnosticsButton_);

    noteLabel_.setFont(uiFont(12.0f));
    noteLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    addAndMakeVisible(noteLabel_);

    // ------------------------------------------------------------ command bar
    // The two tallest, widest controls on the screen (UI-02 Р В РІР‚в„ўР вЂ™Р’В§10). Start is
    // green when it means "go", Stop stays blue-strong: ending a show is an
    // action, not an error. Retry (faulted) borrows amber - it asks attention.
    startButton_.onClick = [this] { startPressed(); };
    stopButton_.onClick = [this] { stopPressed(); };
    startButton_.setColour(juce::TextButton::textColourOffId, ink::textPrimary);
    startButton_.setColour(juce::TextButton::textColourOnId, ink::textPrimary);
    stopButton_.setColour(juce::TextButton::textColourOffId, ink::textPrimary);
    stopButton_.setColour(juce::TextButton::textColourOnId, ink::textPrimary);
    addAndMakeVisible(startButton_);
    addAndMakeVisible(stopButton_);

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
        return ink::green;        // the show is working
    if (state == "starting" || state == "stopping" || state == "opened"
        || state == "connecting" || state == "reconnecting" || state == "ready")
        return ink::amber;        // on its way, not there yet
    if (state == "faulted")
        return ink::red;          // operator, look here
    return ink::textMuted;        // stopped / off / disabled
}

juce::String OperatorContent::stateGlyph(std::string_view state) noexcept
{
    // Filled dot when something is on (in any health colour), hollow when it is
    // off/disabled/closed: the shape says "is it running", the colour says how.
    const bool off = state == "stopped" || state == "closed" || state == "disabled"
                     || state == "off" || state == "idle";
    return off ? juce::String::fromUTF8("\u25cb ") : juce::String::fromUTF8("\u25cf ");
}

void OperatorContent::layoutStatusRow(juce::Rectangle<int>& area, juce::Label& captionLabel,
                                      juce::Label& value) const
{
    auto row = area.removeFromTop(kStatusRowH);
    captionLabel.setBounds(row.removeFromLeft(140).reduced(0, 2));
    value.setBounds(row.reduced(0, 2));
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

    // ------------------------------------------------------------- status card
    appValue_.setText(stateGlyph(panel.applicationState) + juce::String(panel.applicationState),
                      juce::NotificationType::dontSendNotification);
    audioValue_.setText(stateGlyph(panel.audioState) + juce::String(panel.audioState)
                            + "  (" + juce::String(panel.audioBackendName) + ")",
                        juce::NotificationType::dontSendNotification);
    sessionValue_.setText(stateGlyph(panel.sessionState) + juce::String(panel.sessionState),
                          juce::NotificationType::dontSendNotification);
    ndiValue_.setText(stateGlyph(panel.ndiState) + juce::String(panel.ndiState),
                      juce::NotificationType::dontSendNotification);
    appValue_.setColour(juce::Label::textColourId, stateColour(panel.applicationState));
    audioValue_.setColour(juce::Label::textColourId, stateColour(panel.audioState));
    sessionValue_.setColour(juce::Label::textColourId, stateColour(panel.sessionState));
    ndiValue_.setColour(juce::Label::textColourId, stateColour(panel.ndiState));

    // Warnings only when they ask something (UI-02 Р В РІР‚в„ўР вЂ™Р’В§9): "ok" paints nothing.
    detailLabel_.setText(panel.detail == "ok" ? juce::String() : juce::String(panel.detail),
                         juce::NotificationType::dontSendNotification);
    detailLabel_.setColour(juce::Label::textColourId,
                           panel.faulted ? ink::red : ink::amber);

    // Task 015: the credential state belongs to the main screen too - an
    // operator must be able to see "no key" before pressing Start, not only
    // inside a dialog they have not opened.
    credentialLabel_.setText(juce::String(panel.credentialLine),
                             juce::NotificationType::dontSendNotification);
    credentialLabel_.setColour(juce::Label::textColourId,
                               panel.credentialLine.rfind("API key: stored", 0) == 0
                                   ? ink::textMuted
                                   : ink::amber);

    // Task 019 + UI-02 Р В РІР‚в„ўР вЂ™Р’В§4: the chip is the compact, unmistakable flag; the
    // plan's full sentence rides below the rows, muted. Production: empty,
    // both invisible.
    const bool dev = !panel.developerBadge.empty();
    devBadge_.setVisible(dev);
    devDetailLabel_.setVisible(dev);
    devDetailLabel_.setText(juce::String(panel.developerBadge),
                            juce::NotificationType::dontSendNotification);

    // ------------------------------------------------------------- command bar
    startButton_.setEnabled(panel.canStart || panel.faulted);
    startButton_.setButtonText(panel.faulted ? "Retry" : "Start");
    startButton_.setColour(juce::TextButton::buttonColourId,
                           (panel.faulted ? ink::amber : ink::green).withAlpha(0.28f));
    stopButton_.setEnabled(panel.canStop);
    stopButton_.setColour(juce::TextButton::buttonColourId,
                          panel.canStop ? ink::blue.withAlpha(0.25f) : ink::control);

    // ------------------------------------------------------------------- audio
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

    // The caption carries the requested number (the slider's own value); the
    // technical "applying ... (gliding)" line moved to Diagnostics (UI-02 Р В РІР‚в„ўР вЂ™Р’В§6).
    inputGainCaption_.setText(juce::String(std::format("Gain  {:+.1f} dB", panel.inputGainDb)),
                              juce::NotificationType::dontSendNotification);
    outputGainCaption_.setText(juce::String(std::format("Gain  {:+.1f} dB", panel.outputGainDb)),
                               juce::NotificationType::dontSendNotification);

    inputMuteButton_.setToggleState(panel.inputMuted,
                                    juce::NotificationType::dontSendNotification);
    outputMuteButton_.setToggleState(panel.outputMuted,
                                     juce::NotificationType::dontSendNotification);
    inputMuteButton_.setColour(juce::TextButton::buttonColourId,
                               panel.inputMuted ? ink::amber.withAlpha(0.30f) : ink::control);
    outputMuteButton_.setColour(juce::TextButton::buttonColourId,
                                panel.outputMuted ? ink::amber.withAlpha(0.30f) : ink::control);

    inputMeter_.setLevels(panel.inputMeter);
    outputMeter_.setLevels(panel.outputMeter);

    // Numeric readout beside the bar (UI-02 Р В РІР‚в„ўР вЂ™Р’В§6): level and peak from the same
    // view the bar draws - existing telemetry, no new processing.
    inputLevelLabel_.setText(juce::String(std::format("{:.1f} dB   peak {:.1f} dB",
                                                      panel.inputMeter.rmsDb,
                                                      panel.inputMeter.peakDb)),
                             juce::NotificationType::dontSendNotification);
    inputLevelLabel_.setColour(juce::Label::textColourId,
                               panel.inputMeter.clipping ? ink::red : ink::textSecondary);
    outputLevelLabel_.setText(juce::String(std::format("{:.1f} dB   peak {:.1f} dB",
                                                       panel.outputMeter.rmsDb,
                                                       panel.outputMeter.peakDb)),
                              juce::NotificationType::dontSendNotification);
    outputLevelLabel_.setColour(juce::Label::textColourId,
                                panel.outputMeter.clipping ? ink::red : ink::textSecondary);

    // -------------------------------------------------------------- translation
    sessionLineLabel_.setText(stateGlyph(panel.sessionState) + juce::String(panel.sessionState),
                              juce::NotificationType::dontSendNotification);
    sessionLineLabel_.setColour(juce::Label::textColourId, stateColour(panel.sessionState));

    currentSubtitleLabel_.setText(panel.currentSubtitle.empty()
                                      ? juce::String::fromUTF8("\u2014")
                                      : juce::String("\u201c" + panel.currentSubtitle + "\u201d"),
                                  juce::NotificationType::dontSendNotification);

    std::string history;
    const std::size_t total = panel.subtitleHistory.size();
    const std::size_t shown = total > static_cast<std::size_t>(kOperatorHistoryTail)
                                  ? total - static_cast<std::size_t>(kOperatorHistoryTail)
                                  : 0u;
    for (std::size_t i = shown; i < total; ++i)
        history += panel.subtitleHistory[i] + "\n";
    historyLabel_.setText(juce::String(history), juce::NotificationType::dontSendNotification);

    // -------------------------------------------------------------------- health
    // Four rows, values verbatim from the model (the "estimated" wording is the
    // model's, not this window's invention). Non-zero underruns/reconnects turn
    // amber: a number that stays 0 needs no colour, one that moved does.
    if (panel.health.size() == 4)
    {
        latencyValue_.setText(juce::String(panel.health[0].second),
                              juce::NotificationType::dontSendNotification);
        jitterValue_.setText(juce::String(panel.health[1].second),
                             juce::NotificationType::dontSendNotification);
        underrunValue_.setText(juce::String(panel.health[2].second),
                               juce::NotificationType::dontSendNotification);
        reconnectValue_.setText(juce::String(panel.health[3].second),
                                juce::NotificationType::dontSendNotification);
        underrunValue_.setColour(juce::Label::textColourId,
                                 panel.health[2].second == "0" ? ink::textSecondary : ink::amber);
        reconnectValue_.setColour(juce::Label::textColourId,
                                  panel.health[3].second == "0" ? ink::textSecondary : ink::amber);
    }

    noteLabel_.setText(juce::String(panel.actionNote),
                       juce::NotificationType::dontSendNotification);
    noteLabel_.setColour(juce::Label::textColourId,
                         panel.actionNote.rfind("settings refused", 0) == 0
                             ? ink::red
                             : ink::textMuted);

    updatingWidgets_ = false;
    resized();   // the developer row changes the content height mid-run
    repaint();
}

// ------------------------------------------------------------------------------ paint

void OperatorContent::paint(juce::Graphics& g)
{
    g.fillAll(ink::background);

    for (const auto& card : { statusCard_, audioCard_, translationCard_, healthCard_ })
    {
        g.setColour(ink::card);
        g.fillRoundedRectangle(card.toFloat(), 6.0f);
        g.setColour(ink::cardBorder);
        g.drawRoundedRectangle(card.toFloat().reduced(0.5f), 6.0f, 1.0f);
    }
}

// ---------------------------------------------------------------------------- layout

namespace {

/// One AUDIO column laid out inside its rectangle (UI-02 Р В РІР‚в„ўР вЂ™Р’В§5: the same block on
/// both sides - device is shared above them, channel/meter/gain/mute are not).
void layoutAudioColumn(juce::Rectangle<int>& col,
                       juce::Label& channelCaption, juce::ComboBox& channelBox,
                       juce::Label& meterCaption, MeterBar& meter, juce::Label& levelLabel,
                       juce::Label& gainCaption, juce::Slider& gainSlider,
                       juce::TextButton& muteButton)
{
    channelCaption.setBounds(col.removeFromTop(14));
    channelBox.setBounds(col.removeFromTop(kControlH));
    col.removeFromTop(4);
    meterCaption.setBounds(col.removeFromTop(14));
    meter.setBounds(col.removeFromTop(kMeterH));
    levelLabel.setBounds(col.removeFromTop(16));
    col.removeFromTop(4);
    gainCaption.setBounds(col.removeFromTop(14));
    gainSlider.setBounds(col.removeFromTop(28));
    col.removeFromTop(kRowGap);
    muteButton.setBounds(col.removeFromTop(30));
}

} // namespace

int OperatorContent::preferredHeight() const
{
    // The natural height of the card column: the same constants resized()
    // consumes, summed (kept in sync by construction - both read the stacked
    // flag off the same width rule). Inside the window's viewport this is the
    // scrollable content height: the window can be shorter than the screen
    // while every card keeps its size.
    const bool stacked = getWidth() - 2 * kMargin < 760;

    const int status = kCardPad * 2 + 16 + kRowGap + 4 * kStatusRowH + kRowGap + 18 + 16
                     + (devBadge_.isVisible() ? 16 : 0);
    const int columnsHeight = stacked ? 2 * kColumnH + kRowGap : kColumnH;
    const int audio = kCardPad * 2 + 16 + kRowGap + 14 + kControlH + 14 + kRowGap
                    + columnsHeight + 14;
    const int translation = kCardPad * 2 + 16 + kRowGap + kControlH + 14 + 12
                          + 18 + kRowGap + 30 + 3 * 16;
    const int health = kCardPad * 2 + 16 + kRowGap + 4 * kHealthRowH + kRowGap + kSmallBtnH;

    return 2 * kMargin + 36 + kCardGap + status + kCardGap + audio + kCardGap
         + translation + kCardGap + health + kRowGap + 18 + kRowGap + kCommandH;
}

void OperatorContent::resized()
{
    // Inside the window's viewport the rule is the documented one: the
    // viewport offers a width (its visible track), the viewed component
    // decides its own height. Both are applied only when they changed -
    // setSize from resized would otherwise recurse.
    auto* viewport = findParentComponentOfClass<juce::Viewport>();
    const int wantedWidth = viewport != nullptr
                                ? viewport->getMaximumVisibleWidth()
                                : getWidth();
    const int needed = preferredHeight();
    if (getWidth() != wantedWidth || getHeight() != needed)
    {
        setSize(wantedWidth, needed);
        return;   // the resize re-enters with final bounds; lay out then
    }

    auto bounds = getLocalBounds().reduced(kMargin);

    // 1. Header: identity left, the compact developer chip, Settings right.
    auto header = bounds.removeFromTop(36);
    settingsButton_.setBounds(header.removeFromRight(110).withHeight(kSmallBtnH));
    devBadge_.setBounds(header.removeFromRight(160).withTrimmedRight(12).reduced(0, 5));
    titleLabel_.setBounds(header);
    bounds.removeFromTop(kCardGap);

    const bool stacked = bounds.getWidth() < 760;   // narrow: the audio pair stacks

    // 2. SYSTEM STATUS card: header line (with the app state), three rows,
    // then the lines that ask attention.
    {
        const int height = kCardPad * 2 + 16 + kRowGap
                         + 4 * kStatusRowH + kRowGap + 18 + 16
                         + (devBadge_.isVisible() ? 16 : 0);
        statusCard_ = bounds.removeFromTop(height);
        bounds.removeFromTop(kCardGap);

        auto inner = statusCard_.reduced(kCardPad);
        statusHeader_.setBounds(inner.removeFromTop(16));
        inner.removeFromTop(kRowGap);
        layoutStatusRow(inner, appCaption_, appValue_);
        layoutStatusRow(inner, audioCaption_, audioValue_);
        layoutStatusRow(inner, sessionCaption_, sessionValue_);
        layoutStatusRow(inner, ndiCaption_, ndiValue_);
        inner.removeFromTop(kRowGap);
        detailLabel_.setBounds(inner.removeFromTop(18));
        credentialLabel_.setBounds(inner.removeFromTop(16));
        if (devBadge_.isVisible())
            devDetailLabel_.setBounds(inner.removeFromTop(16));
    }

    // 3. AUDIO card: shared device row on top, then the paired columns.
    {
        const int columnsHeight = stacked ? 2 * kColumnH + kRowGap : kColumnH;
        const int height = kCardPad * 2 + 16 + kRowGap + 14 + kControlH + 14 + kRowGap
                         + columnsHeight + 14;   // preferredHeight() mirrors this
        audioCard_ = bounds.removeFromTop(height);
        bounds.removeFromTop(kCardGap);

        auto inner = audioCard_.reduced(kCardPad);
        audioHeader_.setBounds(inner.removeFromTop(16));
        inner.removeFromTop(kRowGap);
        deviceCaption_.setBounds(inner.removeFromTop(14));
        {
            auto row = inner.removeFromTop(kControlH);
            refreshDevicesButton_.setBounds(row.removeFromRight(90).withTrimmedLeft(kRowGap));
            deviceChoice_.setBounds(row);
        }
        deviceNoteLabel_.setBounds(inner.removeFromTop(14));
        inner.removeFromTop(kRowGap);

        auto columns = inner;
        if (stacked)
        {
            auto inCol = columns.removeFromTop(kColumnH);
            columns.removeFromTop(kRowGap);
            auto outCol = columns;   // full width, under the input column

            layoutAudioColumn(inCol, inputChannelCaption_, inputChannelChoice_,
                              inputMeterCaption_, inputMeter_, inputLevelLabel_,
                              inputGainCaption_, inputGainSlider_, inputMuteButton_);
            layoutAudioColumn(outCol, outputChannelCaption_, outputChannelChoice_,
                              outputMeterCaption_, outputMeter_, outputLevelLabel_,
                              outputGainCaption_, outputGainSlider_, outputMuteButton_);
            channelNoteLabel_.setBounds({});
        }
        else
        {
            auto inCol = columns.removeFromLeft(columns.getWidth() / 2);
            auto outCol = columns;
            outCol.removeFromLeft(24);

            layoutAudioColumn(inCol, inputChannelCaption_, inputChannelChoice_,
                              inputMeterCaption_, inputMeter_, inputLevelLabel_,
                              inputGainCaption_, inputGainSlider_, inputMuteButton_);
            layoutAudioColumn(outCol, outputChannelCaption_, outputChannelChoice_,
                              outputMeterCaption_, outputMeter_, outputLevelLabel_,
                              outputGainCaption_, outputGainSlider_, outputMuteButton_);
            channelNoteLabel_.setBounds(outCol.withHeight(14));
        }
    }

    // 4. TRANSLATION card: the pair with an arrow, the state, the live line.
    {
        const int height = kCardPad * 2 + 16 + kRowGap + kControlH + 14 + 12
                         + 18 + kRowGap + 30 + 3 * 16;
        translationCard_ = bounds.removeFromTop(height);
        bounds.removeFromTop(kCardGap);

        auto inner = translationCard_.reduced(kCardPad);
        translationHeader_.setBounds(inner.removeFromTop(16));
        inner.removeFromTop(kRowGap);

        auto pairRow = inner.removeFromTop(kControlH + 14);
        auto fromCol = pairRow.removeFromLeft(260);
        sourceCaption_.setBounds(fromCol.removeFromTop(14));
        sourceChoice_.setBounds(fromCol);
        auto arrowCol = pairRow.removeFromLeft(48);
        arrowCol.removeFromTop(14);                       // align with the combos, not the captions
        arrowLabel_.setBounds(arrowCol.withHeight(kControlH));
        auto toCol = pairRow.removeFromLeft(260);
        targetCaption_.setBounds(toCol.removeFromTop(14));
        targetChoice_.setBounds(toCol);
        auto statusCol = pairRow;
        statusCol.removeFromTop(14 + 6);
        sessionLineLabel_.setBounds(statusCol.reduced(16, 0).withHeight(kControlH - 12));

        pairWarningLabel_.setBounds(inner.removeFromTop(18));
        currentSubtitleLabel_.setBounds(inner.removeFromTop(30));
        historyLabel_.setBounds(inner);
    }

    // 5. HEALTH card: four rows and the (deliberately quiet) door to detail.
    {
        const int height = kCardPad * 2 + 16 + kRowGap + 4 * kHealthRowH + kRowGap + kSmallBtnH;
        healthCard_ = bounds.removeFromTop(height);
        bounds.removeFromTop(kRowGap);

        auto inner = healthCard_.reduced(kCardPad);
        healthHeader_.setBounds(inner.removeFromTop(16));
        inner.removeFromTop(kRowGap);

        const auto layoutHealth = [](juce::Rectangle<int>& row, juce::Label& cap,
                                     juce::Label& val)
        {
            auto r = row.removeFromTop(kHealthRowH);
            cap.setBounds(r.removeFromLeft(140));
            val.setBounds(r);
        };
        layoutHealth(inner, latencyCaption_, latencyValue_);
        layoutHealth(inner, jitterCaption_, jitterValue_);
        layoutHealth(inner, underrunCaption_, underrunValue_);
        layoutHealth(inner, reconnectCaption_, reconnectValue_);

        diagnosticsButton_.setBounds(inner.withHeight(kSmallBtnH).withWidth(140));
    }

    // 6. Action note + the command bar: the last thing the eye lands on.
    noteLabel_.setBounds(bounds.removeFromTop(18).reduced(4, 0));
    bounds.removeFromTop(kRowGap);
    auto commands = bounds.removeFromTop(kCommandH);
    const int half = commands.getWidth() / 2;
    startButton_.setBounds(commands.removeFromLeft(half).reduced(4, 0));
    stopButton_.setBounds(commands.reduced(4, 0));
}

// ============================================================================ OperatorWindow

OperatorWindow::OperatorWindow(ApplicationController& controller)
    : juce::DocumentWindow(JUCE_APPLICATION_NAME_STRING,
                           ink::background,
                           juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);

    // UI-03 follow-up: the card column is taller than some venue laptops are
    // wide-tall, so the content lives in a vertical viewport - the window
    // itself fits any screen and the operator scrolls between sections.
    // Nothing about the layout changed: the same cards, the same sizes.
    auto* viewport = new OperatorViewport("OperatorScroll");
    viewport->setScrollBarsShown(true, false);            // vertical only
    viewport->setViewedComponent(new OperatorContent(controller), false);
    setContentOwned(viewport, true);

    setResizable(true, true);
    // UI-04: the minimum stays a sane operator width; the height may now be
    // smaller than the content because the viewport scrolls. The default
    // 980x700 (outer ~731) fits the 1366x768 work area, the 620 floor fits
    // 1280x720.
    setResizeLimits(720, 620, 4000, 4000);
    setSize(980, 700);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

void OperatorWindow::closeButtonPressed()
{
    if (auto* app = juce::JUCEApplication::getInstance())
        app->systemRequestedQuit();
}

} // namespace liveai
