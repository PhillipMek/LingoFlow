// Venue task 5 asks one Win32 question (IsZoomed on the native frame - see
// OperatorWindow.h). Same rule as Main.cpp: the Windows headers come before
// JUCE pulls its own, and NOMINMAX keeps the min/max macros away from the
// std::clamp in this file.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "App/OperatorWindow.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <utility>

#include "App/DiagnosticsWindow.h"
#include "App/SettingsWindow.h"
#include "Config/ConfigSchema.h"
#include "Utils/Log.h"

namespace liveai {
namespace {

// ---------------------------------------------------------------- the palette
// Colour communicates state and nothing else (the UI redesign Р В РІР‚в„ўР вЂ™Р’В§12): surfaces stay
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
// 4 / 8 / 12 / 16 / 24 / 32 (the UI redesign Р В РІР‚в„ўР вЂ™Р’В§11). Component heights are constants, not
// per-widget negotiations.

constexpr int kMargin    = 16;   // window edge -> card
constexpr int kCardPad   = 16;   // card edge -> content
constexpr int kCardGap   = 12;   // between cards
constexpr int kRowGap    = 8;    // between rows inside a card
constexpr int kControlH  = 30;   // ComboBox height, everywhere
constexpr int kSmallBtnH = 30;   // Refresh / Diagnostics
constexpr int kCommandH  = 46;   // START / STOP - the tallest controls on screen
constexpr int kMeterH    = 22;
constexpr int kStatusRowH = 24;  // one row of the 2x2 status grid (task 4 §1)
constexpr int kHealthRowH = 22;  // one row of the 2x2 health grid (task 4 §1)

/// The height CardsSurface::layout measures against - taller than the card
/// column ever is, so the removeFromTop walk consumes its true total and the
/// returned content height is exact (the same contract the settings pages
/// use).
constexpr int kMeasureHeight = 20000;

/// Inline caption widths (task 4 §1): a caption ABOVE its control cost a 14 px
/// row each; the same words BESIDE it cost nothing and still say what the
/// control is. Sized to the longest string each carries at its font.
constexpr int kChannelCapW = 78;    ///< "Channel"
constexpr int kLevelCapW   = 90;    ///< "Input level" / "Output level"
constexpr int kGainCapW    = 110;   ///< "Gain  +2.0 dB"
constexpr int kStatusCapW  = 110;   ///< "Translation" - the longest status caption
constexpr int kDeviceCapW  = 210;   ///< "Device (one ASIO in/out)"

/// One AUDIO column: channel row, level line, meter, gain row, mute.
/// The same block height on both sides is what makes the pair read as a pair.
/// Task 4 §1: the three captions joined their controls' rows - 184 became 144.
constexpr int kColumnH = kControlH + 4 + 18 + kMeterH + 4 + 28 + kRowGap + 30;

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
    // The application-wide wheel rule now lives in ui::wheelSafeSlider (venue
    // task 4 §2): with the wheel disabled a JUCE Slider hands the event back
    // to its parent chain, so scrolling over a gain fader scrolls the card
    // column - the live audio value never moves by accident. Drag, click,
    // the fader's own text box and keyboard editing are untouched.
    ui::wheelSafeSlider(slider);
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

/// How many history lines this screen shows: a tail, not a transcript - the
/// full bounded history stays in the model (and readable in Diagnostics).
constexpr int kOperatorHistoryTail = 3;

/// One status cell of the 2x2 grid: caption left (fixed width, shared so the
/// two columns read as columns), value right of it (venue task 4 §1 - the
/// four rows were the card's biggest mouth).
void layoutStatusCell(juce::Rectangle<int> cell, juce::Label& caption, juce::Label& value)
{
    caption.setBounds(cell.removeFromLeft(kStatusCapW).reduced(0, 2));
    value.setBounds(cell.reduced(0, 2));
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
    // The scrollable card column first (venue task 4 §1): everything that
    // INFORMS lives inside it; the header above and the command bar below are
    // pinned outside, so Start/Stop/Settings are reachable at any scroll
    // position and on any screen. The surface measures and places the cards
    // in one pass - the ui::ScrollablePage contract.
    auto surface = std::make_unique<CardsSurface>(*this);
    cards_ = surface.get();
    cardsPage_ = std::make_unique<ui::ScrollablePage>(
        std::move(surface), [this](int width) { return cards_->layout(width); });
    addAndMakeVisible(*cardsPage_);

    // ---------------------------------------------------------------- header
    titleLabel_.setFont(uiFont(20.0f, true));
    titleLabel_.setColour(juce::Label::textColourId, ink::textPrimary);
    titleLabel_.setText(juce::String(std::format("{} {}",
                                                 JUCE_APPLICATION_NAME_STRING,
                                                 JUCE_APPLICATION_VERSION_STRING)),
                        juce::NotificationType::dontSendNotification);
    addAndMakeVisible(titleLabel_);

    // the UI redesign Р В РІР‚в„ўР вЂ™Р’В§4: developer mode is a compact, unmistakable chip - not the red
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
    caption(statusHeader_, "SYSTEM STATUS", *cards_, ink::textMuted, 11.0f);

    caption(appCaption_, "App", *cards_, ink::textSecondary, 14.0f);
    caption(audioCaption_, "Audio", *cards_, ink::textSecondary, 14.0f);
    caption(sessionCaption_, "Translation", *cards_, ink::textSecondary, 14.0f);
    caption(ndiCaption_, "NDI", *cards_, ink::textSecondary, 14.0f);

    for (auto* chip : { &appValue_, &audioValue_, &sessionValue_, &ndiValue_ })
    {
        chip->setFont(uiFont(14.0f));
        cards_->addAndMakeVisible(*chip);
    }

    detailLabel_.setFont(uiFont(13.0f));
    detailLabel_.setColour(juce::Label::textColourId, ink::amber);
    cards_->addAndMakeVisible(detailLabel_);

    credentialLabel_.setFont(uiFont(12.0f));
    credentialLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    cards_->addAndMakeVisible(credentialLabel_);

    devDetailLabel_.setFont(uiFont(11.0f));
    devDetailLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    devDetailLabel_.setVisible(false);
    cards_->addAndMakeVisible(devDetailLabel_);

    // ------------------------------------------------------------------ audio
    caption(audioHeader_, "AUDIO", *cards_, ink::textMuted, 11.0f);

    caption(deviceCaption_, "Device (one ASIO in/out)", *cards_);
    deviceChoice_.onChange = [this] { deviceSelected(); };
    styleBox(deviceChoice_);
    cards_->addAndMakeVisible(deviceChoice_);

    refreshDevicesButton_.onClick = [this] { refreshDevicesPressed(); };
    styleQuietButton(refreshDevicesButton_);
    cards_->addAndMakeVisible(refreshDevicesButton_);

    deviceNoteLabel_.setFont(uiFont(11.0f));
    deviceNoteLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    cards_->addAndMakeVisible(deviceNoteLabel_);

    caption(inputChannelCaption_, "Channel", *cards_);
    inputChannelChoice_.onChange = [this] { channelChanged(); };
    styleBox(inputChannelChoice_);
    cards_->addAndMakeVisible(inputChannelChoice_);

    caption(outputChannelCaption_, "Channel", *cards_);
    outputChannelChoice_.onChange = [this] { channelChanged(); };
    styleBox(outputChannelChoice_);
    cards_->addAndMakeVisible(outputChannelChoice_);

    channelNoteLabel_.setFont(uiFont(11.0f));
    channelNoteLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    cards_->addAndMakeVisible(channelNoteLabel_);

    caption(inputMeterCaption_, "Input level", *cards_);
    cards_->addAndMakeVisible(inputMeter_);
    // The numeric readout rides the caption's row now (task 4 §1): one line
    // "Input level      -24.3 dB   peak -12.0 dB" above the bar. Right-aligned
    // so the digits form a column against the bar's right edge.
    inputLevelLabel_.setFont(uiFont(12.0f));
    inputLevelLabel_.setColour(juce::Label::textColourId, ink::textSecondary);
    inputLevelLabel_.setJustificationType(juce::Justification::centredRight);
    cards_->addAndMakeVisible(inputLevelLabel_);

    caption(inputGainCaption_, "Gain", *cards_);
    const auto [gainMin, gainMax] = config::gainRange();
    fader(inputGainSlider_, gainMin, gainMax, 0.5, *cards_);
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
    cards_->addAndMakeVisible(inputMuteButton_);

    caption(outputMeterCaption_, "Output level", *cards_);
    cards_->addAndMakeVisible(outputMeter_);
    outputLevelLabel_.setFont(uiFont(12.0f));
    outputLevelLabel_.setColour(juce::Label::textColourId, ink::textSecondary);
    outputLevelLabel_.setJustificationType(juce::Justification::centredRight);
    cards_->addAndMakeVisible(outputLevelLabel_);

    caption(outputGainCaption_, "Gain", *cards_);
    fader(outputGainSlider_, gainMin, gainMax, 0.5, *cards_);
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
    cards_->addAndMakeVisible(outputMuteButton_);

    // ------------------------------------------------------------ translation
    caption(translationHeader_, "TRANSLATION", *cards_, ink::textMuted, 11.0f);

    // The pair as the wire knows it (docs section 5, R8): the target is the
    // operator's choice, the source is what the provider decides by itself.
    // The old "From [combo] -> To [combo]" implied a declaration that never
    // leaves LingoFlow; the expectation lives in Settings, the truth lives
    // here. Task 4 §1 put the source truth and the session state on the
    // target's row: one line says "Target [ru]  ● running  Source: automatic".
    caption(targetCaption_, "Target", *cards_);
    targetChoice_.onChange = [this] { targetSelected(); };
    styleBox(targetChoice_);
    cards_->addAndMakeVisible(targetChoice_);

    caption(sourceCaption_, "Source", *cards_);
    sourceValueLabel_.setFont(uiFont(14.0f));
    sourceValueLabel_.setColour(juce::Label::textColourId, ink::textSecondary);
    sourceValueLabel_.setJustificationType(juce::Justification::centredLeft);
    cards_->addAndMakeVisible(sourceValueLabel_);

    sessionLineLabel_.setFont(uiFont(13.0f));
    cards_->addAndMakeVisible(sessionLineLabel_);

    pairWarningLabel_.setFont(uiFont(12.0f));
    pairWarningLabel_.setColour(juce::Label::textColourId, ink::red);
    cards_->addAndMakeVisible(pairWarningLabel_);

    currentSubtitleLabel_.setFont(uiFont(18.0f));
    currentSubtitleLabel_.setColour(juce::Label::textColourId, ink::textPrimary);
    currentSubtitleLabel_.setJustificationType(juce::Justification::centredLeft);
    cards_->addAndMakeVisible(currentSubtitleLabel_);

    historyLabel_.setFont(uiFont(12.0f));
    historyLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    historyLabel_.setJustificationType(juce::Justification::topLeft);
    cards_->addAndMakeVisible(historyLabel_);

    // ------------------------------------------------------------ live health
    caption(healthHeader_, "HEALTH", *cards_, ink::textMuted, 11.0f);

    caption(latencyCaption_, "Latency", *cards_, ink::textSecondary, 13.0f);
    caption(jitterCaption_, "Jitter fill", *cards_, ink::textSecondary, 13.0f);
    caption(underrunCaption_, "Underruns", *cards_, ink::textSecondary, 13.0f);
    caption(reconnectCaption_, "Reconnects", *cards_, ink::textSecondary, 13.0f);

    for (auto* value : { &latencyValue_, &jitterValue_, &underrunValue_, &reconnectValue_ })
    {
        value->setFont(uiFont(13.0f));
        value->setColour(juce::Label::textColourId, ink::textSecondary);
        cards_->addAndMakeVisible(*value);
    }

    diagnosticsButton_.onClick = [this] { diagnosticsPressed(); };
    styleQuietButton(diagnosticsButton_);
    cards_->addAndMakeVisible(diagnosticsButton_);

    noteLabel_.setFont(uiFont(12.0f));
    noteLabel_.setColour(juce::Label::textColourId, ink::textMuted);
    addAndMakeVisible(noteLabel_);

    // ------------------------------------------------------------ command bar
    // The two tallest, widest controls on the screen (the UI redesign Р В РІР‚в„ўР вЂ™Р’В§10). Start is
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

    // The async front on purpose: what used to freeze this window - the
    // handshake, the device open, the session teardown - now runs on the
    // controller's lifecycle thread, and the 100 ms refresh below carries the
    // truth (starting / running / faulted + detail) as the action completes.
    // The export, the log and the operator screen all read the same atomics,
    // so nothing here has to block to be honest about the outcome.
    controller_.requestStart();
    actionNote_.clear();
    rebuild();
}

void OperatorContent::stopPressed()
{
    controller_.requestStop();
    actionNote_ = "Stopping. Unplayed translated audio is discarded; device, rate, "
                  "channel and NDI changes apply on Start.";
    rebuild();
}

void OperatorContent::settingsPressed()
{
    // One dialog object for the lifetime of the window; close hides it, and a
    // reopen always re-reads the settings first - a second stale copy of the
    // configuration is exactly the thing this product refuses to have. The
    // dialog is modal while open (venue task 4 §4) and this screen is the
    // window it hands control back to.
    if (settingsWindow_ == nullptr)
        settingsWindow_ = std::make_unique<SettingsWindow>(controller_, *this);
    else
        settingsWindow_->reopen();
}

void OperatorContent::diagnosticsPressed()
{
    // the UI redesign: the engineering surface opens on request, owns the Export button,
    // and follows the same hide/reopen lifetime rule as the settings dialog -
    // and the same modal rule (venue task 4 §4).
    if (diagnosticsWindow_ == nullptr)
        diagnosticsWindow_ = std::make_unique<DiagnosticsWindow>(controller_, *this);
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

    // The discrete choices: the value list is the panel's, mirrored
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
    actionNote_ = "Mutes are live only and were not saved (a show never starts muted).";
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

    OperatorPanel panel = buildOperatorPanel(controller_, actionNote_);

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

    // Warnings only when they ask something (the UI redesign Р В РІР‚в„ўР вЂ™Р’В§9): "ok" paints nothing.
    const juce::String detailText = panel.detail == "ok"
                                        ? juce::String()
                                        : juce::String(panel.detail);
    detailLabel_.setText(detailText, juce::NotificationType::dontSendNotification);
    detailLabel_.setColour(juce::Label::textColourId,
                           panel.faulted ? ink::red : ink::amber);
    // Task 4 §1: an empty line reserves no row - the card shrinks to what it
    // actually says (visibility is read by CardsSurface::layout, which runs at
    // the end of this function through resized()).
    detailLabel_.setVisible(!detailText.isEmpty());

    // Design note: the credential state belongs to the main screen too - an
    // operator must be able to see "no key" before pressing Start, not only
    // inside a dialog they have not opened.
    credentialLabel_.setText(juce::String(panel.credentialLine),
                             juce::NotificationType::dontSendNotification);
    credentialLabel_.setColour(juce::Label::textColourId,
                               panel.credentialLine.rfind("API key: stored", 0) == 0
                                   ? ink::textMuted
                                   : ink::amber);

    // Design note: the UI redesign Р В РІР‚в„ўР вЂ™Р’В§4: the chip is the compact, unmistakable flag; the
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
    syncOptions(targetChoice_, targetCache_, panel.targetLanguages, panel.selectedTarget);
    // The source line is the panel's words, not a widget's guess: it says
    // what the wire actually does (R8), and it is set here, from the tested
    // model, so a future change of provider behaviour moves one string in
    // UiModel and this screen follows.
    sourceValueLabel_.setText(juce::String(panel.sourceDisplay),
                              juce::NotificationType::dontSendNotification);
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
    // Task 4 §1 again: the warning row is reserved only while a bad pair is
    // actually on screen - a healthy show does not pay 18 px for silence.
    pairWarningLabel_.setVisible(!panel.languagePairWarning.empty());

    if (!gainDragging_)
    {
        inputGainSlider_.setValue(panel.inputGainDb,
                                  juce::NotificationType::dontSendNotification);
        outputGainSlider_.setValue(panel.outputGainDb,
                                   juce::NotificationType::dontSendNotification);
    }

    // The caption carries the requested number (the slider's own value); the
    // technical "applying ... (gliding)" line moved to Diagnostics (the UI redesign Р В РІР‚в„ўР вЂ™Р’В§6).
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

    // ---- meter display (venue task 3 §3). The raw measurement is the engine's
    // fact; the smoothing is this view's own, here in the message thread - never
    // in the callback, never in UiModel: the export and the diagnostics rows keep
    // printing raw numbers, only the screen glides. dt comes from the real clock,
    // so the visual speed is independent of how often rebuild() happens to run.
    {
        const auto now = std::chrono::steady_clock::now();
        const double dtMs =
            lastMeterUpdate_.time_since_epoch().count() == 0
                ? 100.0   // first glance: the timer's own interval is the honest assumed pace
                : std::chrono::duration<double, std::milli>(now - lastMeterUpdate_).count();
        lastMeterUpdate_ = now;

        const auto smooth = [dtMs](const UiMeterView& raw, std::uint64_t& lastBlocks,
                                   audio::MeterSmoother& smoother)
        {
            UiMeterView shown = raw;

            // No progress in the meter's own block counter means no data: the
            // honest raw reading is silence, not the frozen last block. The
            // smoother then releases toward it - the bar ends quietly instead
            // of holding a level that no longer exists.
            const bool fresh = raw.blocksMeasured != lastBlocks;
            lastBlocks = raw.blocksMeasured;

            const auto levels =
                smoother.update(fresh ? raw.peakDb : audio::kMeterFloorDb,
                                fresh ? raw.rmsDb : audio::kMeterFloorDb, dtMs);
            shown.peakDb = levels.peakDb;
            shown.rmsDb = levels.rmsDb;
            shown.signalPresent = fresh && raw.signalPresent;

            // The clipping lamp is deliberately NOT smoothed: it is this
            // block's hazard flag, and delaying a full-scale fact is hiding,
            // not polish.
            return shown;
        };

        panel.inputMeter = smooth(panel.inputMeter, lastInputMeterBlocks_, inputMeterSmoothing_);
        panel.outputMeter = smooth(panel.outputMeter, lastOutputMeterBlocks_, outputMeterSmoothing_);
    }

    inputMeter_.setLevels(panel.inputMeter);
    outputMeter_.setLevels(panel.outputMeter);

    // Numeric readout beside the bar (the UI redesign Р В РІР‚в„ўР вЂ™Р’В§6): level and peak from the same
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
    resized();   // the developer row and the detail lines change the height mid-run
    repaint();
    cards_->repaint();   // the card panels follow the visibility of those lines
}

// ------------------------------------------------------------------------------ paint

void OperatorContent::paint(juce::Graphics& g)
{
    // The pinned frame: header above, command bar below. The card column paints
    // itself inside its own surface (CardsSurface::paintContent) - the two
    // halves share the house background, so the split is invisible.
    g.fillAll(ink::background);
}

// ---------------------------------------------------------------------------- layout

namespace {

/// One AUDIO column laid out inside its rectangle (the UI redesign Р В РІР‚в„ўР вЂ™Р’В§5: the same block on
/// both sides - device is shared above them, channel/meter/gain/mute are not).
void layoutAudioColumn(juce::Rectangle<int>& col,
                       juce::Label& channelCaption, juce::ComboBox& channelBox,
                       juce::Label& levelCaption, MeterBar& meter, juce::Label& levelLabel,
                       juce::Label& gainCaption, juce::Slider& gainSlider,
                       juce::TextButton& muteButton)
{
    // Venue task 4 §1: the three captions moved INTO their control's row -
    // "Channel [combo]", "Input level   -24.3 dB peak", "Gain +2.0 dB [====]".
    // Same words, same controls, 40 px less per column, and nothing got
    // smaller to achieve it.
    auto channelRow = col.removeFromTop(kControlH);
    channelCaption.setBounds(channelRow.removeFromLeft(kChannelCapW));
    channelBox.setBounds(channelRow.withTrimmedLeft(8));
    col.removeFromTop(4);

    auto levelRow = col.removeFromTop(18);
    levelCaption.setBounds(levelRow.removeFromLeft(kLevelCapW));
    levelLabel.setBounds(levelRow);
    meter.setBounds(col.removeFromTop(kMeterH));
    col.removeFromTop(4);

    auto gainRow = col.removeFromTop(28);
    gainCaption.setBounds(gainRow.removeFromLeft(kGainCapW));
    gainSlider.setBounds(gainRow.withTrimmedLeft(6));
    col.removeFromTop(kRowGap);

    muteButton.setBounds(col.removeFromTop(30));
}

/// One cell of a 2x2 grid: fixed caption column, value beside it.
void layoutGridCell(juce::Rectangle<int> cell, juce::Label& caption, juce::Label& value,
                    int captionWidth)
{
    caption.setBounds(cell.removeFromLeft(captionWidth));
    value.setBounds(cell);
}

} // namespace

// =========================================================================== CardsSurface

int OperatorContent::CardsSurface::layout(int width)
{
    // One pass measures AND places: the removeFromTop walk consumes exactly the
    // height the cards need, and the answer is what the viewport scrolls (the
    // ui::ScrollablePage contract - no mirrored preferredHeight() anymore).
    auto bounds = juce::Rectangle<int>(0, 0, width, kMeasureHeight).reduced(kMargin);
    const bool stacked = bounds.getWidth() < 760;   // narrow: the audio pair stacks

    // 1. SYSTEM STATUS card: the four states as a 2x2 grid (task 4 §1 - four
    // stacked rows were 96 px of mostly-quiet labels). The lines that ask
    // attention reserve their rows only while they carry text.
    {
        const bool hasDetail = owner_.detailLabel_.isVisible();
        const bool hasDev = owner_.devDetailLabel_.isVisible();
        const int height = kCardPad * 2 + 16 + 6 + 2 * kStatusRowH + 6 + 16
                         + (hasDetail ? 18 : 0) + (hasDev ? 16 : 0);
        statusCard_ = bounds.removeFromTop(height);
        bounds.removeFromTop(kCardGap);

        auto inner = statusCard_.reduced(kCardPad);
        owner_.statusHeader_.setBounds(inner.removeFromTop(16));
        inner.removeFromTop(6);

        auto row1 = inner.removeFromTop(kStatusRowH);
        layoutStatusCell(row1.removeFromLeft(row1.getWidth() / 2),
                         owner_.appCaption_, owner_.appValue_);
        row1.removeFromLeft(12);
        layoutStatusCell(row1, owner_.audioCaption_, owner_.audioValue_);

        auto row2 = inner.removeFromTop(kStatusRowH);
        layoutStatusCell(row2.removeFromLeft(row2.getWidth() / 2),
                         owner_.sessionCaption_, owner_.sessionValue_);
        row2.removeFromLeft(12);
        layoutStatusCell(row2, owner_.ndiCaption_, owner_.ndiValue_);
        inner.removeFromTop(6);

        if (hasDetail)
            owner_.detailLabel_.setBounds(inner.removeFromTop(18));
        owner_.credentialLabel_.setBounds(inner.removeFromTop(16));
        if (hasDev)
            owner_.devDetailLabel_.setBounds(inner.removeFromTop(16));
    }

    // 2. AUDIO card: shared device row on top, then the paired columns. The
    //    device caption shares the combo's row now (task 4 §1, same rule as
    //    the columns).
    {
        const int columnsHeight = stacked ? 2 * kColumnH + kRowGap : kColumnH;
        const int height = kCardPad * 2 + 16 + kRowGap + kControlH + 14 + kRowGap
                         + columnsHeight + (stacked ? 0 : kRowGap + 14);
        audioCard_ = bounds.removeFromTop(height);
        bounds.removeFromTop(kCardGap);

        auto inner = audioCard_.reduced(kCardPad);
        owner_.audioHeader_.setBounds(inner.removeFromTop(16));
        inner.removeFromTop(kRowGap);
        {
            auto row = inner.removeFromTop(kControlH);
            owner_.deviceCaption_.setBounds(row.removeFromLeft(kDeviceCapW));
            row.removeFromLeft(8);
            owner_.refreshDevicesButton_.setBounds(row.removeFromRight(90));
            owner_.deviceChoice_.setBounds(row.withTrimmedLeft(8));
        }
        owner_.deviceNoteLabel_.setBounds(inner.removeFromTop(14));
        inner.removeFromTop(kRowGap);

        auto columns = inner;
        if (stacked)
        {
            auto inCol = columns.removeFromTop(kColumnH);
            columns.removeFromTop(kRowGap);
            auto outCol = columns;   // full width, under the input column

            layoutAudioColumn(inCol, owner_.inputChannelCaption_, owner_.inputChannelChoice_,
                              owner_.inputMeterCaption_, owner_.inputMeter_,
                              owner_.inputLevelLabel_,
                              owner_.inputGainCaption_, owner_.inputGainSlider_,
                              owner_.inputMuteButton_);
            layoutAudioColumn(outCol, owner_.outputChannelCaption_, owner_.outputChannelChoice_,
                              owner_.outputMeterCaption_, owner_.outputMeter_,
                              owner_.outputLevelLabel_,
                              owner_.outputGainCaption_, owner_.outputGainSlider_,
                              owner_.outputMuteButton_);
            owner_.channelNoteLabel_.setBounds({});
        }
        else
        {
            auto inCol = columns.removeFromLeft(columns.getWidth() / 2);
            auto outCol = columns;
            outCol.removeFromLeft(24);

            layoutAudioColumn(inCol, owner_.inputChannelCaption_, owner_.inputChannelChoice_,
                              owner_.inputMeterCaption_, owner_.inputMeter_,
                              owner_.inputLevelLabel_,
                              owner_.inputGainCaption_, owner_.inputGainSlider_,
                              owner_.inputMuteButton_);
            layoutAudioColumn(outCol, owner_.outputChannelCaption_, owner_.outputChannelChoice_,
                              owner_.outputMeterCaption_, owner_.outputMeter_,
                              owner_.outputLevelLabel_,
                              owner_.outputGainCaption_, owner_.outputGainSlider_,
                              owner_.outputMuteButton_);
            owner_.channelNoteLabel_.setBounds(outCol.withHeight(14));
        }
    }

    // 3. TRANSLATION card: one row for the pair as the wire knows it -
    //    Target [combo], the session state, and the source truth beside them
    //    (task 4 §1 merged the two stacked rows; R8's sentence still reads in
    //    full, just horizontally). No arrow - an arrow drew a pair the wire
    //    never carries. The live line and the compact tail below.
    {
        const bool hasWarning = owner_.pairWarningLabel_.isVisible();
        const int height = kCardPad * 2 + 16 + kRowGap + kControlH + 8
                         + (hasWarning ? 18 : 0) + 30 + 3 * 16;
        translationCard_ = bounds.removeFromTop(height);
        bounds.removeFromTop(kCardGap);

        auto inner = translationCard_.reduced(kCardPad);
        owner_.translationHeader_.setBounds(inner.removeFromTop(16));
        inner.removeFromTop(kRowGap);

        auto pairRow = inner.removeFromTop(kControlH);
        owner_.targetCaption_.setBounds(pairRow.removeFromLeft(52));
        owner_.targetChoice_.setBounds(pairRow.removeFromLeft(208).withTrimmedLeft(6));
        owner_.sessionLineLabel_.setBounds(pairRow.removeFromLeft(160).withTrimmedLeft(16));
        pairRow.removeFromLeft(16);
        owner_.sourceCaption_.setBounds(pairRow.removeFromLeft(64));
        owner_.sourceValueLabel_.setBounds(pairRow);

        if (hasWarning)
            owner_.pairWarningLabel_.setBounds(inner.removeFromTop(18));
        owner_.currentSubtitleLabel_.setBounds(inner.removeFromTop(30));
        owner_.historyLabel_.setBounds(inner);   // kOperatorHistoryTail * 16, reserved above
    }

    // 4. HEALTH card: four numbers as a 2x2 grid, the (deliberately quiet)
    //    door to detail riding the header row (task 4 §1 - the card lost its
    //    four stacked rows and its dedicated button row).
    {
        const int height = kCardPad * 2 + kSmallBtnH + 6 + 2 * kHealthRowH;
        healthCard_ = bounds.removeFromTop(height);

        auto inner = healthCard_.reduced(kCardPad);
        {
            auto headerRow = inner.removeFromTop(kSmallBtnH);
            owner_.diagnosticsButton_.setBounds(headerRow.removeFromRight(140));
            owner_.healthHeader_.setBounds(headerRow.reduced(0, 8));
        }
        inner.removeFromTop(6);

        auto row1 = inner.removeFromTop(kHealthRowH);
        layoutGridCell(row1.removeFromLeft(row1.getWidth() / 2),
                       owner_.latencyCaption_, owner_.latencyValue_, 92);
        row1.removeFromLeft(16);
        layoutGridCell(row1, owner_.jitterCaption_, owner_.jitterValue_, 92);

        auto row2 = inner.removeFromTop(kHealthRowH);
        layoutGridCell(row2.removeFromLeft(row2.getWidth() / 2),
                       owner_.underrunCaption_, owner_.underrunValue_, 92);
        row2.removeFromLeft(16);
        layoutGridCell(row2, owner_.reconnectCaption_, owner_.reconnectValue_, 92);
    }

    // The stack consumed its cards plus the two margins - that is the scroll
    // extent. No trailing gap: the last card ends the page.
    return kMeasureHeight - bounds.getHeight();
}

void OperatorContent::CardsSurface::paintContent(juce::Graphics& g, const juce::Rectangle<int>&)
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

void OperatorContent::resized()
{
    // Pinned top, pinned bottom, scroll in between (venue task 4 §1). This
    // function positions the pinned parts and hands the middle to the
    // ui::ScrollablePage, which measures and lays out the cards for the width
    // it is given - the cards never see a window resize directly.
    auto bounds = getLocalBounds().reduced(kMargin);

    // 1. Header: identity left, the compact developer chip, Settings right.
    auto header = bounds.removeFromTop(36);
    settingsButton_.setBounds(header.removeFromRight(110).withHeight(kSmallBtnH));
    devBadge_.setBounds(header.removeFromRight(160).withTrimmedRight(12).reduced(0, 5));
    titleLabel_.setBounds(header);
    bounds.removeFromTop(kCardGap);

    // 2. The command bar and the action note, built from the bottom so they
    // are ALWAYS on screen: the buttons a live show runs on are not allowed
    // to sit below the fold.
    auto commands = bounds.removeFromBottom(kCommandH);
    bounds.removeFromBottom(kRowGap);
    auto noteRow = bounds.removeFromBottom(18);
    bounds.removeFromBottom(kCardGap);

    // 3. Everything between is the scrollable card column.
    cardsPage_->setBounds(bounds);

    const int half = commands.getWidth() / 2;
    startButton_.setBounds(commands.removeFromLeft(half).reduced(4, 0));
    stopButton_.setBounds(commands.reduced(4, 0));
    noteLabel_.setBounds(noteRow.reduced(4, 0));
}

// ============================================================================ OperatorWindow

namespace {

/// Snapshot the current displays into the portable DisplayRect list the
/// restore policy reads. Converting juce::Displays::Display here - and only
/// here - keeps every off-screen / multi-monitor decision in WindowGeometry,
/// where it is unit-testable with a synthetic monitor layout (the real thing
/// cannot be fabricated in an automated run).
std::vector<DisplayRect> currentDisplayRects()
{
    const auto& desktop = juce::Desktop::getInstance();
    const auto& displays = desktop.getDisplays();

    std::vector<DisplayRect> rects;
    rects.reserve(static_cast<std::size_t>(displays.displays.size()));

    for (const auto& display : displays.displays)
    {
        // userBounds is the part of the display the window may occupy (task
        // bar excluded), in logical pixels - the same space Component bounds
        // and getBounds() use.
        const juce::Rectangle<int> area = display.userBounds.toNearestInt();

        rects.push_back(DisplayRect{ area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                                     display.isMain });
    }

    return rects;
}

/// True while the native frame is maximised or minimised. JUCE 9.0.3 has no
/// public isMaximised() (checked against the vendored tree), so the one place
/// that must not record a special-state rectangle as the "saved size" asks
/// the OS via IsZoomed; minimisation needs no OS question - the peer's own
/// documented flag covers it, and a minimised window's bounds are the system
/// parking place, not the operator's geometry. No peer yet (the constructor's
/// first layout pass) means no special state: the placement being applied IS
/// the geometry worth recording.
bool frameInSpecialState(const juce::Component& window)
{
    auto* peer = window.getPeer();
    if (peer == nullptr)
        return false;

#ifdef _WIN32
    if (IsZoomed(static_cast<HWND>(peer->getNativeHandle())) != FALSE)
        return true;
#endif
    return peer->isMinimised();
}

} // namespace

OperatorWindow::OperatorWindow(ApplicationController& controller)
    : juce::DocumentWindow(JUCE_APPLICATION_NAME_STRING,
                           ink::background,
                           juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);

    // Venue task 4 §1: the scroll moved INSIDE the content - OperatorContent
    // pins the header and the command bar and lets only the card column
    // scroll (ui::ScrollablePage). The frame is therefore just the content;
    // DocumentWindow lays it out, resized() does the rest.
    setContentOwned(new OperatorContent(controller), true);

    setResizable(true, true);
    // The compacted card column (2x2 status and health grids, inline captions,
    // the merged translation row) needs about 700 px instead of the old 1128,
    // so the default 980x660 - outer ~691 with the title bar - fits a
    // 1366x768 work area, and the 540 floor keeps header, note and START/STOP
    // on screen everywhere: the cards scroll, the command bar never does. The
    // numbers are the shared contract in App/WindowGeometry.h, so the restore
    // below clamps against exactly what the frame enforces.
    setResizeLimits(kMainWindowMinWidth, kMainWindowMinHeight, kMainWindowMaxWidth, kMainWindowMaxHeight);

    // Venue task 5: come back where the operator left the window. The stored
    // geometry (single Settings area, config.json) is resolved against the
    // displays that exist RIGHT NOW - a remembered spot with no monitor under
    // it, or a size outside the app limits, comes back on-screen and legal
    // instead of invisible. Everything happens BEFORE setVisible: the window
    // appears once, at its final rectangle, with no post-show resize jump.
    const auto placement = resolveWindowPlacement(controller.config().current().window,
                                                  currentDisplayRects());

    if (placement.fromSavedGeometry)
        setBounds(placement.x, placement.y, placement.width, placement.height);
    else
    {
        setSize(placement.width, placement.height);
        centreWithSize(getWidth(), getHeight());
    }

    if (!placement.fromSavedGeometry || placement.sizeClamped || placement.movedOnScreen)
        log::info("ui", "main window: " + placement.describe());

    setVisible(true);

    // Measured on the live build (venue task 5): sizes above the floor round-trip
    // pixel-exact; a rectangle stored EXACTLY at the floor lands one frame width
    // smaller in CLIENT terms (the native peer re-derives component bounds from
    // the window rect after creation - the file then converges to that fixed
    // point and every later start is identical). The task-4 layout absorbs it:
    // the pinned header, note and START/STOP were verified on screen at that
    // size (UIA bounding rects inside the frame), only the scrollable card
    // column gets a slightly shorter viewport. A synchronous re-assert here was
    // tried and cannot work - the peer's own correction arrives later, on the
    // message queue; the floor is the constrainer's job for every INTERACTIVE
    // resize, which is where it actually guards the layout.
}

void OperatorWindow::resized()
{
    juce::DocumentWindow::resized();   // mandatory: the superclass lays out the decorations
    recordNormalGeometry();
}

void OperatorWindow::moved()
{
    juce::Component::moved();
    recordNormalGeometry();
}

void OperatorWindow::recordNormalGeometry()
{
    // Track only the NORMAL-state rectangle. A maximised close would
    // otherwise store a whole-screen geometry that restores the next launch
    // as an oversized window covering the operator's other work - the
    // remembered intent is the size the window was before someone pressed
    // maximize, and that is exactly what survives the close.
    if (frameInSpecialState(*this))
        return;

    const auto bounds = getBounds();
    if (bounds.getWidth() <= 0 || bounds.getHeight() <= 0)
        return;   // the frame's first layout pass runs before any real size exists

    geometry_ = WindowSettings{ bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight() };
}

void OperatorWindow::closeButtonPressed()
{
    if (auto* app = juce::JUCEApplication::getInstance())
        app->systemRequestedQuit();
}

} // namespace liveai
