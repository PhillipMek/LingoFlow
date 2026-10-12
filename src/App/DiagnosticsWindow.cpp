#include "App/DiagnosticsWindow.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>

#include "App/UiWidgets.h"

namespace liveai {
namespace {

// The card palette mirrors the operator window: the surfaces match,
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
constexpr int kRawLineH = 16;
constexpr int kHeaderH = 22;
constexpr int kButtonH = 30;
constexpr int kNoteH = 18;

/// The height the measure passes run against (see the settings page's rule of
/// the same name): every stack here consumes fixed rows, so what is left at
/// the end is exactly the slack and the returned content height is exact.
constexpr int kMeasureHeight = 40000;

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
        // colour (the UI redesign, section 12).
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
    rebuild();
    startTimer(500);   // engineering numbers on an engineering surface
}

void DiagnosticsContent::setShowRaw(bool show)
{
    showRaw_ = show;
    if (auto* page = findParentComponentOfClass<ui::ScrollablePage>())
        page->relayout();   // the raw section changes the content height
    else
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

    // Row counts can move between refreshes (the raw ring grows; session
    // fields join or leave), so the scroll extent is re-measured, not just
    // repainted. The page's follow-at-bottom rule decides whether the reader
    // is carried along with new events (venue task 4 §5): a reader who
    // scrolled up into history is NOT yanked to the newest line.
    if (auto* page = findParentComponentOfClass<ui::ScrollablePage>())
        page->relayout();
    else
        resized();
    repaint();
}

int DiagnosticsContent::preferredHeight(int width) const
{
    // Mirrors resized() by construction: the same cardHeightFor()/latency/
    // raw helpers stack the same sections the layout walk places.
    auto b = juce::Rectangle<int>(0, 0, width, kMeasureHeight).reduced(kMargin);

    const int leftBlock = cardHeightFor(panel_.audioHealth) + kCardGap
                        + cardHeightFor(panel_.runtime);
    const int rightBlock = cardHeightFor(panel_.translationHealth) + kCardGap
                         + cardHeightFor(panel_.subtitles);
    b.removeFromTop(std::max(leftBlock, rightBlock) + kSectionGap);

    const int latencyHeight = kHeaderH + kCardPad
                            + static_cast<int>(panel_.latency.size()) * kLatencyRowH
                            + kCardPad / 2;
    b.removeFromTop(latencyHeight + kSectionGap);

    if (showRaw_)
    {
        const int rawHeight = kHeaderH + kCardPad
                            + static_cast<int>(panel_.rawDetails.size()) * kRawLineH
                            + kCardPad / 2;
        b.removeFromTop(rawHeight);
    }

    return kMeasureHeight - b.getHeight() - kMargin;
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

void DiagnosticsContent::paintContent(juce::Graphics& g, const juce::Rectangle<int>&)
{
    // The rectangles were recomputed by the last resized() - paint reads
    // them, never re-derives (the same discipline as before the scroll pass).
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
    // whole point of the pipeline's design and it stays visible on every row -
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

        // Newest last - and EVERY line is printed. Before the scrolling pass
        // this card drew only the tail that fit and the events above the fold
        // were unreachable; the body now grows to hold the whole ring, and
        // the reader scrolls back through it at their own position (venue
        // task 4 §5).
        juce::String text;
        for (const auto& line : panel_.rawDetails)
            text += juce::String(line) + "\n";
        g.drawMultiLineText(text, inner.getX(), inner.getY() + 12, inner.getWidth());
    }
}

void DiagnosticsContent::resized()
{
    // Top-down stacking inside the scroll body: the two columns of metric
    // cards, the latency table, and - expanded or not - the raw section was
    // the last card; every card keeps its NATURAL height (cardHeightFor) and
    // the page makes the body exactly preferredHeight() tall. Nothing is
    // stretched or cut; what does not fit the window scrolls.
    auto bounds = getLocalBounds().reduced(kMargin);

    auto columns = bounds;
    auto left = columns.removeFromLeft(columns.getWidth() / 2);
    auto right = columns;
    right.removeFromLeft(kCardGap);

    audioCard_ = left.removeFromTop(cardHeightFor(panel_.audioHealth));
    left.removeFromTop(kCardGap);
    runtimeCard_ = left.removeFromTop(cardHeightFor(panel_.runtime));

    translationCard_ = right.removeFromTop(cardHeightFor(panel_.translationHealth));
    right.removeFromTop(kCardGap);
    subtitlesCard_ = right.removeFromTop(cardHeightFor(panel_.subtitles));

    const int leftBlock = audioCard_.getHeight() + kCardGap + runtimeCard_.getHeight();
    const int rightBlock = translationCard_.getHeight() + kCardGap + subtitlesCard_.getHeight();
    bounds.removeFromTop(std::max(leftBlock, rightBlock) + kSectionGap);

    const int latencyHeight = kHeaderH + kCardPad
                            + static_cast<int>(panel_.latency.size()) * kLatencyRowH
                            + kCardPad / 2;
    latencyCard_ = bounds.removeFromTop(latencyHeight);
    bounds.removeFromTop(kSectionGap);

    if (showRaw_)
    {
        const int rawHeight = kHeaderH + kCardPad
                            + static_cast<int>(panel_.rawDetails.size()) * kRawLineH
                            + kCardPad / 2;
        rawCard_ = bounds.removeFromTop(rawHeight);
    }
    else
    {
        rawCard_ = {};
    }
}

// ============================================================================ DiagnosticsFrame

DiagnosticsFrame::DiagnosticsFrame(ApplicationController& controller)
    : controller_(controller)
{
    auto surface = std::make_unique<DiagnosticsContent>(controller);
    content_ = surface.get();

    page_ = new ui::ScrollablePage(std::move(surface),
                                   [this](int width) { return content_->preferredHeight(width); });
    // The live-tail rule (venue task 4 §5): new events pull the view along
    // ONLY while it already sits at the bottom. Scrolled up to read history?
    // the refresh keeps your position; nothing here resets to the top either -
    // a relayout never touches the scroll number.
    page_->setFollowsNewContent(true);
    addAndMakeVisible(*page_);

    exportButton_.onClick = [this] { exportPressed(); };
    styleQuiet(exportButton_);
    addAndMakeVisible(exportButton_);

    rawButton_.onClick = [this] { rawToggled(); };
    styleQuiet(rawButton_);
    addAndMakeVisible(rawButton_);

    noteLabel_.setFont(ui::uiFont(12.0f));
    noteLabel_.setColour(juce::Label::textColourId, dink::amber);
    addAndMakeVisible(noteLabel_);
}

void DiagnosticsFrame::exportPressed()
{
    // the export funnel; the note is the receipt either way - success names
    // the file, failure is as talkable as the success. The export itself is
    // secret-free by construction (identifiers only, the established rule, pinned
    // by the export tests).
    std::filesystem::path written;
    std::string note;
    controller_.exportDiagnostics({}, written, note);
    actionNote_ = std::move(note);
    noteLabel_.setText(juce::String(actionNote_), juce::NotificationType::dontSendNotification);
    content_->rebuild();
}

void DiagnosticsFrame::rawToggled()
{
    const bool show = !content_->isShowingRaw();
    content_->setShowRaw(show);
    rawButton_.setButtonText(show ? "Hide raw details" : "Show raw details");
}

void DiagnosticsFrame::resized()
{
    // The fixed strip at the bottom, the scroll above it: the buttons and the
    // note never scroll away (venue task 4 §5 - "нижние элементы должны
    // оставаться доступными"). The strip carries the same left/right inset as
    // the card column inside the viewport (kMargin, applied by the body's own
    // reduced()) - a review pass caught that the buttons were missing it and
    // sat flush to the window edge while the cards floated 16 px in.
    auto bounds = getLocalBounds();
    auto bottom = bounds.removeFromBottom(kMargin + kButtonH + kCardGap + kNoteH + kMargin);
    bottom.removeFromTop(kMargin);
    bottom = bottom.reduced(kMargin, 0);

    auto buttons = bottom.removeFromTop(kButtonH);
    exportButton_.setBounds(buttons.removeFromLeft(180));
    buttons.removeFromLeft(kCardGap);
    rawButton_.setBounds(buttons.removeFromLeft(160));
    bottom.removeFromTop(kCardGap);

    noteLabel_.setBounds(bottom.withHeight(kNoteH));

    page_->setBounds(bounds);
}

// ======================================================================== DiagnosticsWindow

DiagnosticsWindow::DiagnosticsWindow(ApplicationController& controller, juce::Component& ownerWindow)
    : juce::DocumentWindow("LingoFlow Diagnostics",
                           dink::background,
                           juce::DocumentWindow::closeButton),
      ownerWindow_(ownerWindow)
{
    setUsingNativeTitleBar(true);
    // No delete-on-close: close hides, the operator window owns the lifetime
    // (the same rule the settings dialog established earlier).

    auto* frame = new DiagnosticsFrame(controller);
    content_ = &frame->surface();
    setContentOwned(frame, true);

    setResizable(true, true);
    // Task 4 §5 made this surface scrollable: the window used to demand
    // 1100x1120 (and still cut the raw list off); now it shows an honest
    // viewport and the rest scrolls. 700 fits a 1366x768 work area with the
    // title bar; the card grid wants ~900 across to keep the value column.
    setResizeLimits(900, 560, 4000, 4000);
    setSize(1100, 700);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
    becomeModal();
}

void DiagnosticsWindow::reopen()
{
    if (content_ != nullptr)
        content_->rebuild();   // reopened never shows stale numbers

    setVisible(true);
    toFront(true);
    becomeModal();   // dismissal ended the modal state - a reopen begins it again
}

void DiagnosticsWindow::becomeModal()
{
    // Venue task 4 §4 - the same mechanism as the settings dialog, and the
    // same reasons: enterModalState returns immediately (no nested event
    // loop, so the audio engine, the network threads and this window's own
    // 500 ms refresh keep running while it is open, and quitting or Stopping
    // behind it cannot deadlock). Closing ends the modal state explicitly and
    // the callback returns control to the operator window one loop-pass later.
    if (isCurrentlyModal())
        return;   // a duplicate reopen never stacks modality on itself

    enterModalState(true,
                    juce::ModalCallbackFunction::create(
                        [this](int)
                        {
                            if (auto* topLevel = ownerWindow_.getTopLevelComponent())
                                topLevel->toFront(true);
                        }),
                    false);
}

void DiagnosticsWindow::closeButtonPressed()
{
    // Same close rule as the settings dialog: end the modal state explicitly,
    // then hide - the callback returns focus to the operator window on the
    // next message-loop pass.
    if (isCurrentlyModal())
        exitModalState();

    setVisible(false);
}

} // namespace liveai
