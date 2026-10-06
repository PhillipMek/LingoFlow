#include "App/DiagnosticsWindow.h"

#include <algorithm>
#include <filesystem>
#include <format>

#include "App/UiWidgets.h"

namespace liveai {
namespace {

constexpr int kRowHeight = 14;

juce::Label& makeRowBlock(juce::Label& label, juce::Component& parent)
{
    label.setFont(ui::monoFont());
    label.setColour(juce::Label::textColourId, juce::Colour(0xffc9d1d9u));
    label.setJustificationType(juce::Justification::topLeft);
    label.setInterceptsMouseClicks(false, false);
    parent.addAndMakeVisible(label);
    return label;
}

std::size_t lineCount(const std::string& text)
{
    return static_cast<std::size_t>(
        std::count(text.begin(), text.end(), '\n'))
         + (text.empty() ? 0u : 1u);
}

} // namespace

// ======================================================================= DiagnosticsContent

DiagnosticsContent::DiagnosticsContent(ApplicationController& controller)
    : controller_(controller)
{
    ui::sectionHeader(audioHeader_, "AUDIO HEALTH", *this);
    ui::sectionHeader(translationHeader_, "TRANSLATION HEALTH", *this);
    ui::sectionHeader(subtitlesHeader_, "SUBTITLES - NDI AND TEXT PIPELINE", *this);
    ui::sectionHeader(latencyHeader_,
                      "LATENCY ACCOUNTING - every row keeps its kind: measured, estimated, or not measured",
                      *this);
    ui::sectionHeader(runtimeHeader_, "RUNTIME", *this);

    makeRowBlock(audioRows_, *this);
    makeRowBlock(translationRows_, *this);
    makeRowBlock(subtitlesRows_, *this);
    makeRowBlock(latencyRows_, *this);
    makeRowBlock(runtimeRows_, *this);

    exportButton_.onClick = [this] { exportPressed(); };
    addAndMakeVisible(exportButton_);

    noteLabel_.setFont(ui::uiFont(12.0f));
    noteLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd29922u));
    noteLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(noteLabel_);

    rebuild();
    startTimer(500);   // engineering numbers on an engineering surface: half a second is honest
}

void DiagnosticsContent::exportPressed()
{
    // Task 017's funnel, relocated by UI-01 to the surface its receipts belong
    // to. The note is the receipt either way - success names the file, failure
    // is as talkable as success.
    std::filesystem::path written;
    std::string note;
    controller_.exportDiagnostics({}, written, note);
    actionNote_ = std::move(note);
    rebuild();
}

void DiagnosticsContent::timerCallback()
{
    rebuild();
}

std::string DiagnosticsContent::rows(
    const std::vector<std::pair<std::string, std::string>>& metricRows)
{
    return ui::formatMetricRows(metricRows);
}

void DiagnosticsContent::rebuild()
{
    const DiagnosticsPanel panel = buildDiagnosticsPanel(controller_);

    const std::string audioText = rows(panel.audioHealth);
    const std::string translationText = rows(panel.translationHealth);
    const std::string subtitlesText = rows(panel.subtitles);
    const std::string runtimeText = rows(panel.runtime);

    // The 018 accounting keeps the kind beside every number - here, of all
    // places, nobody may read an estimate as a measurement. The row text is
    // what the main screen used to print: relocated presentation.
    std::string latency;
    for (const auto& row : panel.latency)
        latency += std::format("{:<26}{} [{}]\n", row.component, row.value, row.kind);

    audioRows_.setText(juce::String(audioText), juce::NotificationType::dontSendNotification);
    translationRows_.setText(juce::String(translationText), juce::NotificationType::dontSendNotification);
    subtitlesRows_.setText(juce::String(subtitlesText), juce::NotificationType::dontSendNotification);
    latencyRows_.setText(juce::String(latency), juce::NotificationType::dontSendNotification);
    runtimeRows_.setText(juce::String(runtimeText), juce::NotificationType::dontSendNotification);

    audioLines_ = lineCount(audioText);
    translationLines_ = lineCount(translationText);
    subtitlesLines_ = lineCount(subtitlesText);
    latencyLines_ = lineCount(latency);
    runtimeLines_ = lineCount(runtimeText);

    noteLabel_.setText(juce::String(actionNote_), juce::NotificationType::dontSendNotification);
}

void DiagnosticsContent::resized()
{
    auto bounds = getLocalBounds().reduced(14);

    // Bottom-up: reserve the full-width latency block and the export row
    // FIRST, then split what remains into the two metric columns. The first
    // cut of this window laid the columns over the block and proved, loudly,
    // what overlapping text looks like at a venue. The +2 lines of slack are
    // for the long accounting sentences, which wrap inside their row.
    const int bottomHeight = 18 + kRowHeight * (static_cast<int>(latencyLines_) + 2)
                           + 8 + 30 + 40;   // header, rows, gap, export, note
    auto bottom = bounds.removeFromBottom(bottomHeight);
    auto columns = bounds;

    auto left = columns.removeFromLeft(columns.getWidth() / 2);
    auto right = columns;
    right.removeFromLeft(12);

    audioHeader_.setBounds(left.removeFromTop(18));
    audioRows_.setBounds(left.removeFromTop(kRowHeight * static_cast<int>(audioLines_)));
    left.removeFromTop(6);
    runtimeHeader_.setBounds(left.removeFromTop(18));
    runtimeRows_.setBounds(left);

    translationHeader_.setBounds(right.removeFromTop(18));
    translationRows_.setBounds(right.removeFromTop(kRowHeight * static_cast<int>(translationLines_)));
    right.removeFromTop(6);
    subtitlesHeader_.setBounds(right.removeFromTop(18));
    subtitlesRows_.setBounds(right);

    latencyHeader_.setBounds(bottom.removeFromTop(18));
    latencyRows_.setBounds(bottom.removeFromTop(kRowHeight * (static_cast<int>(latencyLines_) + 2)));
    bottom.removeFromTop(8);
    exportButton_.setBounds(bottom.removeFromTop(30).removeFromLeft(200));
    noteLabel_.setBounds(bottom);
}

// ======================================================================== DiagnosticsWindow

DiagnosticsWindow::DiagnosticsWindow(ApplicationController& controller)
    : juce::DocumentWindow("LingoFlow Diagnostics",
                           juce::Colour(0xff1e2124u),
                           juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    // No delete-on-close: close hides, the operator window owns the lifetime
    // (the same rule the settings dialog established in task 015).

    auto* content = new DiagnosticsContent(controller);
    content_ = content;
    setContentOwned(content, true);

    setResizable(true, true);
    setResizeLimits(900, 620, 4000, 4000);
    setSize(1040, 700);
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
