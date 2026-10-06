#include "App/DiagnosticsWindow.h"

#include <algorithm>
#include <filesystem>
#include <format>

#include "App/UiWidgets.h"

namespace liveai {
namespace {

// The card palette mirrors the operator window (UI-02): the surfaces match,
// the density is allowed to be higher here.
namespace dink {

const juce::Colour background  { 0xff1a1d20u };
const juce::Colour card        { 0xff22262bu };
const juce::Colour cardBorder  { 0xff2d333au };
const juce::Colour header      { 0xff8b949eu };
const juce::Colour caption     { 0xff9aa4afu };
const juce::Colour value       { 0xffe6edf3u };
const juce::Colour muted       { 0xff6e7681u };
const juce::Colour green       { 0xff3fb950u };
const juce::Colour amber       { 0xffd29922u };
const juce::Colour red         { 0xfff85149u };
const juce::Colour control     { 0xff30363du };

} // namespace dink

constexpr int kMargin = 16;
constexpr int kCardPad = 14;
constexpr int kCardGap = 12;
constexpr int kSectionGap = 24;   // between groups of cards, not just cards
constexpr int kRowH = 20;
constexpr int kLatencyRowH = 30;
constexpr int kHeaderH = 22;

/// The state words the row colouring knows. The words come from the modules'
/// own nameOf() - the tested contract is the word (UiModel tests assert it);
/// colour is presentation, never a second vocabulary.
juce::Colour valueColour(const std::string& label, const std::string& value)
{
    const bool stateRow = label == "session" || label == "NDI state"
                          || label == "audio (independent path)" || label == "application";
    if (!stateRow)
    {
        // Non-state rows still speak loudly when they moved: underruns,
        // overruns, drops and malformed counts are amber at non-zero, red at
        // the actionable ones. Zero stays neutral - a healthy row needs no
        // colour (UI-02 В§12).
        const bool counting = label == "underruns" || label == "overruns"
                              || label == "ring dropped (samples)"
                              || label == "output silence (samples)"
                              || label == "malformed callbacks" || label == "oversized callbacks"
                              || label == "reconnects"
                              || label == "translation errors (fatal)"
                              || label == "NDI dropped" || label == "NDI transport errors"
                              || label == "NDI errors (counted)"
                              || label == "text partial/final/evicted/duplicates"
                              || label == "text dispatch delivered/dropped"
                              || label == "capture gap-refused" || label == "rejected (wrong rate)"
                              || label == "dropped (buffer full)"
                              || label == "clip samples in / out";
        if (!counting)
            return dink::value;

        // A dispatch row reads "delivered / dropped": only the second number
        // is an alarm, so that one row's colour checks the tail after the
        // slash. Every other "a / b" row (clips, text totals) alarms on
        // either half, and single values simply check themselves.
        std::string watched = value;
        if (label == "text dispatch delivered/dropped")
        {
            const auto tail = value.rfind('/');
            if (tail != std::string::npos)
                watched = value.substr(tail + 1);
        }
        return watched.find_first_not_of(" 0()") == std::string::npos ? dink::value : dink::amber;
    }

    if (value == "running" || value == "connected" || value == "publishing")
        return dink::green;
    if (value == "faulted")
        return dink::red;
    if (value == "reconnecting" || value == "connecting" || value == "starting"
        || value == "stopping" || value == "opened" || value == "ready")
        return dink::amber;
    return dink::muted;   // stopped / closed / disabled / off
}

int cardHeightFor(const std::vector<std::pair<std::string, std::string>>& rows)
{
    return kHeaderH + kCardPad + static_cast<int>(rows.size()) * kRowH + kCardPad / 2;
}

void styleQuiet(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, dink::control);
    button.setColour(juce::TextButton::textColourOffId, dink::value);
    button.setColour(juce::TextButton::textColourOnId, dink::value);
}

} // namespace

// ======================================================================= DiagnosticsContent

DiagnosticsContent::DiagnosticsContent(ApplicationController& controller)
    : controller_(controller)
{
    exportButton_.onClick = [this] { exportPressed(); };
    styleQuiet(exportButton_);
    addAndMakeVisible(exportButton_);

    rawButton_.onClick = [this] { rawToggled(); };
    styleQuiet(rawButton_);
    addAndMakeVisible(rawButton_);

    noteLabel_.setFont(ui::uiFont(12.0f));
    noteLabel_.setColour(juce::Label::textColourId, dink::amber);
    addAndMakeVisible(noteLabel_);

    rebuild();
    startTimer(500);   // engineering numbers on an engineering surface
}

void DiagnosticsContent::exportPressed()
{
    // Task 017's funnel; the note is the receipt either way - success names
    // the file, failure is as talkable as the success. The export itself is
    // secret-free by construction (identifiers only, task 015's rule, pinned
    // by the export tests).
    std::filesystem::path written;
    std::string note;
    controller_.exportDiagnostics({}, written, note);
    actionNote_ = std::move(note);
    rebuild();
}

void DiagnosticsContent::rawToggled()
{
    showRaw_ = !showRaw_;
    rawButton_.setButtonText(showRaw_ ? "Hide raw details" : "Show raw details");
    resized();
    repaint();
}

void DiagnosticsContent::timerCallback()
{
    rebuild();
}

void DiagnosticsContent::rebuild()
{
    panel_ = buildDiagnosticsPanel(controller_);
    noteLabel_.setText(juce::String(actionNote_), juce::NotificationType::dontSendNotification);
    resized();   // card heights follow the row counts
    repaint();
}

void DiagnosticsContent::drawCard(juce::Graphics& g, const juce::Rectangle<int>& card,
                                  const juce::String& title) const
{
    g.setColour(dink::card);
    g.fillRoundedRectangle(card.toFloat(), 6.0f);
    g.setColour(dink::cardBorder);
    g.drawRoundedRectangle(card.toFloat().reduced(0.5f), 6.0f, 1.0f);

    g.setFont(ui::uiFont(11.0f));
    g.setColour(dink::header);
    g.drawText(title, card.reduced(kCardPad, 6).withHeight(16),
               juce::Justification::left);
}

void DiagnosticsContent::drawRows(juce::Graphics& g, juce::Rectangle<int>& area,
                                  const std::vector<std::pair<std::string, std::string>>& rows) const
{
    g.setFont(ui::uiFont(13.0f));

    for (const auto& [label, value] : rows)
    {
        auto row = area.removeFromTop(kRowH);
        g.setColour(dink::caption);
        g.drawText(juce::String(label), row.withWidth(row.getWidth() - 230),
                   juce::Justification::left);
        g.setColour(valueColour(label, value));
        g.drawText(juce::String(value), row.withX(row.getRight() - 230).withWidth(230),
                   juce::Justification::right);
    }
}

void DiagnosticsContent::paint(juce::Graphics& g)
{
    g.fillAll(dink::background);

    drawCard(g, audioCard_, "AUDIO HEALTH");
    drawCard(g, translationCard_, "TRANSLATION HEALTH");
    drawCard(g, subtitlesCard_, "SUBTITLES / NDI");
    drawCard(g, runtimeCard_, "RUNTIME");
    drawCard(g, latencyCard_, "LATENCY - every row keeps its kind: measured, estimated, or not measured");

    {
        auto inner = audioCard_.reduced(kCardPad, kHeaderH + 2);
        drawRows(g, inner, panel_.audioHealth);
    }
    {
        auto inner = translationCard_.reduced(kCardPad, kHeaderH + 2);
        drawRows(g, inner, panel_.translationHealth);
    }
    {
        auto inner = subtitlesCard_.reduced(kCardPad, kHeaderH + 2);
        drawRows(g, inner, panel_.subtitles);
    }
    {
        auto inner = runtimeCard_.reduced(kCardPad, kHeaderH + 2);
        drawRows(g, inner, panel_.runtime);
    }

    // The latency table: component | value | kind. The kind column is the
    // whole point of task 018's design and it stays visible on every row -
    // nobody may read an estimate as a measurement here.
    {
        auto inner = latencyCard_.reduced(kCardPad, kHeaderH + 2);
        g.setFont(ui::uiFont(13.0f));
        for (const auto& row : panel_.latency)
        {
            auto r = inner.removeFromTop(kLatencyRowH);   // long values may wrap over two lines
            g.setColour(dink::caption);
            g.drawFittedText(juce::String(row.component), r.withWidth(210), r.getHeight(),
                       juce::Justification::topLeft, 2);
            g.setColour(dink::value);
            g.drawFittedText(juce::String(row.value), r.withX(r.getX() + 218).withWidth(r.getWidth() - 218 - 270),
                             r.getHeight(), juce::Justification::topLeft, 2);
            g.setColour(dink::muted);
            g.drawFittedText("[" + juce::String(row.kind) + "]", r.withX(r.getRight() - 262).withWidth(262), r.getHeight(),
                       juce::Justification::topRight, 2);
        }
    }

    if (showRaw_)
    {
        drawCard(g, rawCard_, "RAW DETAILS - event ring and configuration (full history in the export)");

        auto inner = rawCard_.reduced(kCardPad, kHeaderH + 2);
        g.setFont(ui::monoFont(12.0f));
        g.setColour(dink::value);

        // Newest last; when the area is short, the newest lines are the ones
        // that must be visible, so the tail is what gets drawn.
        const std::size_t lines = std::min<std::size_t>(
            static_cast<std::size_t>(std::max(0, inner.getHeight() / 16)),
            panel_.rawDetails.size());
        std::string text;
        for (std::size_t i = panel_.rawDetails.size() - lines;
             i < panel_.rawDetails.size(); ++i)
            text += panel_.rawDetails[i] + "\n";
        g.drawMultiLineText(juce::String(text), inner.getX(),
                            inner.getY() + 12, inner.getWidth());
    }
}

void DiagnosticsContent::resized()
{
    auto bounds = getLocalBounds().reduced(kMargin);

    // Bottom-up stacking: buttons + note, then (when expanded) the raw window,
    // then the latency table, then the two columns of metric cards take what
    // is left. Every card's rect is disjoint by construction.
    auto bottom = bounds.removeFromBottom(kCardGap + 30 + kCardGap + 18);   // buttons + note
    if (showRaw_)
    {
        const int rawHeight = kHeaderH + kCardPad + 8 * 16 + kCardPad / 2;   // a window into the ring
        bounds.removeFromBottom(kSectionGap);
        rawCard_ = bounds.removeFromBottom(rawHeight);
    }

    const int latencyHeight = kHeaderH + kCardPad
                            + static_cast<int>(panel_.latency.size()) * kLatencyRowH + kCardPad / 2;
    bounds.removeFromBottom(kSectionGap);
    latencyCard_ = bounds.removeFromBottom(latencyHeight);

    // The four metric cards in two columns; each column stacks two cards,
    // the shorter column's second card absorbs the remainder so no gap lies.
    auto columns = bounds;
    auto left = columns.removeFromLeft(columns.getWidth() / 2);
    auto right = columns;
    right.removeFromLeft(kCardGap);

    const int leftFirst = cardHeightFor(panel_.audioHealth);
    audioCard_ = left.removeFromTop(leftFirst);
    left.removeFromTop(kCardGap);
    runtimeCard_ = left;

    const int rightFirst = cardHeightFor(panel_.translationHealth);
    translationCard_ = right.removeFromTop(rightFirst);
    right.removeFromTop(kCardGap);
    subtitlesCard_ = right;

    auto buttons = bottom.removeFromTop(30);
    exportButton_.setBounds(buttons.removeFromLeft(180));
    buttons.removeFromLeft(kCardGap);
    rawButton_.setBounds(buttons.removeFromLeft(160));
    noteLabel_.setBounds(bottom.withHeight(18));
}

// ======================================================================== DiagnosticsWindow

DiagnosticsWindow::DiagnosticsWindow(ApplicationController& controller)
    : juce::DocumentWindow("LingoFlow Diagnostics",
                           dink::background,
                           juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    // No delete-on-close: close hides, the operator window owns the lifetime
    // (the same rule the settings dialog established in task 015).

    auto* content = new DiagnosticsContent(controller);
    content_ = content;
    setContentOwned(content, true);

    setResizable(true, true);
    setResizeLimits(900, 900, 4000, 4000);
    setSize(1100, 1120);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

void DiagnosticsWindow::reopen()
{
    if (content_ != nullptr)
        content_->rebuild();   // reopened never shows stale numbers

    setVisible(true);
    toFront(true);
}

void DiagnosticsWindow::closeButtonPressed()
{
    setVisible(false);
}

} // namespace liveai
