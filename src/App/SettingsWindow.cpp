#include "App/SettingsWindow.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <utility>

#include "App/UiWidgets.h"
#include "Config/ConfigSchema.h"
#include "Subtitles/SubtitleRenderer.h"

namespace liveai {
namespace {

// The dialog palette matches the operator window (the UI redesign Р’В§12): state carries
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

/// The height a page layout function measures against: taller than any page
/// ever is, so every removeFromTop chain consumes its true total and the
/// returned content height is exact. The number itself carries no meaning -
/// it only has to dominate every layout below (venue task 4 §3, and the
/// monotone-measure contract of ui::ScrollablePage).
constexpr int kMeasureHeight = 20000;

/// The subtitle preview's strip on the (now measured) Subtitles page. It was
/// "whatever the column has left"; a scrolling page decides its own height,
/// so the preview keeps a fixed window - tall enough to read the caption
/// placement against the checkerboard.
constexpr int kPreviewH = 240;

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
    // The application-wide rule (venue task 4 §2, ui::wheelSafeSlider): the
    // wheel scrolls the page, it never changes a value - here it would also
    // fight the tab's own scrolling viewport, which is exactly the accidental
    // edit the task names. This factory is the ONE place a settings slider is
    // built; nothing here or later re-enables the wheel.
    ui::wheelSafeSlider(s);
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

/// The store sentence for the Credentials status line (the UI redesign Р’В§5): built from
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

// --------------------------------------------------------------- subtitles tab data

/// The three transport modes, worded as what the operator actually gets. The
/// combo item numbers are 1-based; this table is the ONE mapping shared by the
/// widget fill, the draft read and the settings refresh.
constexpr std::pair<const char*, subtitles::OutputMode> kSubtitleModes[] = {
    { "Video - burned-in subtitles (OBS-ready)", subtitles::OutputMode::video },
    { "Metadata only - TTML timed text", subtitles::OutputMode::metadata },
    { "Video + metadata (one source, both)", subtitles::OutputMode::both },
};

constexpr std::pair<const char*, std::pair<int, int>> kSubtitleResolutions[] = {
    { "1280 x 720", { 1280, 720 } },
    { "1920 x 1080", { 1920, 1080 } },
    { "2560 x 1440", { 2560, 1440 } },
    { "3840 x 2160", { 3840, 2160 } },
};

constexpr int kSubtitleFrameRates[] = { 10, 15, 24, 25, 30, 50, 60 };

juce::Colour colourFromRgb(std::uint32_t rgb)
{
    return juce::Colour(0xff000000u | ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu));
}

} // namespace

// =========================================================================== SubtitlePreview

/// The look of the caption draft, painted for real: a GDI+ render of the
/// current draft settings into the SAME straight-alpha BGRA buffer the NDI
/// transport ships, composited here over a checkerboard so "transparent" is a
/// thing the operator SEES before the show, not a thing the manual asserts.
///
/// Geometry: the preview renders the draft's aspect at one third the size, and
/// the rasterizer scales style px by frame height, so a 1920x1080 draft
/// previews 640x360 with the font's proportions intact. The conversion to a
/// JUCE image happens through setPixelAt(Colour) - JUCE stores ARGB
/// premultiplied while the wire is straight alpha (docs/ndi-protocol.md), and
/// the two conventions meet only in this one, explicit place. A failure to
/// render is drawn as its own sentence, never as a blank that could be
/// mistaken for "no text".
class SettingsContent::SubtitlePreview final : public juce::Component
{
public:
    /// Re-renders when the draft changed. Cheap to call from a timer: the
    /// compare is a struct equality over plain data, the render runs ~1 ms.
    void refresh()
    {
        if (subtitleDraft_ == lastRenderedDraft_)
            return;

        lastRenderedDraft_ = subtitleDraft_;

        subtitles::SubtitleSettings previewSettings = subtitleDraft_;
        previewSettings.video.width = (previewSettings.video.width / 3) & ~1;
        previewSettings.video.height = previewSettings.video.height / 3;
        if (previewSettings.video.width < 64 || previewSettings.video.height < 36)
        {
            previewSettings.video.width = 64;
            previewSettings.video.height = 36;
        }

        renderer_.applySettings(previewSettings);
        std::string error;
        subtitles::RenderedSubtitleFrame frame;

        static const std::string kSample =
            "\xd0\x9f\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb2\xd0\xbe\xd0\xb4 "
            "\xd0\xb7\xd0\xb2\xd1\x83\xd1\x87\xd0\xb8\xd1\x82\xd1\x81\xd1\x8f "
            "\xd0\xb2 \xd1\x80\xd0\xb5\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe\xd0\xbc "
            "\xd0\xb2\xd1\x80\xd0\xb5\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb8 - Hello!";
            // "Перевод звучит в реальном времени - Hello!": the preview must
            // prove Cyrillic, not merely survive it.

        if (!renderer_.render(kSample, frame, error))
        {
            image_ = {};
            note_ = error;
            repaint();
            return;
        }

        note_.clear();
        if (frame.familyUsed != subtitleDraft_.style.fontFamily)
            note_ = "font fallback: " + frame.familyUsed;   // said out loud, never silently

        buildImage(frame);
        repaint();
    }

    void setDraft(const subtitles::SubtitleSettings& draft)
    {
        subtitleDraft_ = draft;
    }

    void paint(juce::Graphics& g) override
    {
        // Checkerboard: the transparency truth, in the dialog's palette.
        const auto bounds = getLocalBounds();
        constexpr int kCell = 10;
        for (int y = 0; y < bounds.getHeight(); y += kCell)
        {
            for (int x = 0; x < bounds.getWidth(); x += kCell)
            {
                g.setColour(((x / kCell + y / kCell) % 2) == 0 ? juce::Colour(0xff3a3f46u)
                                                               : juce::Colour(0xff2a2f36u));
                g.fillRect(x, y, kCell, kCell);
            }
        }

        if (!image_.isNull())
            g.drawImage(image_, bounds.toFloat(), juce::RectanglePlacement::stretchToFit);

        if (!note_.empty())
        {
            g.setColour(sink::amber);
            g.setFont(ui::uiFont(11.0f));
            g.drawText(juce::String(note_), bounds.reduced(6), juce::Justification::topLeft);
        }
    }

private:
    /// Straight-alpha BGRA (the wire format) to a JUCE ARGB image: setPixelAt
    /// takes a straight Colour and stores it premultiplied, which is exactly
    /// the two conventions this product separates - the wire stays straight
    /// (NDI), the screen stays JUCE-correct, and the conversion is visible
    /// here rather than hidden in a cast.
    void buildImage(const subtitles::RenderedSubtitleFrame& frame)
    {
        juce::Image image(juce::Image::ARGB, frame.width, frame.height, false);
        for (int y = 0; y < frame.height; ++y)
        {
            const std::uint8_t* line = frame.bgra.data() + static_cast<std::size_t>(y) * frame.width * 4u;
            for (int x = 0; x < frame.width; ++x)
            {
                image.setPixelAt(x, y,
                                 juce::Colour(line[x * 4 + 2], line[x * 4 + 1], line[x * 4 + 0],
                                              line[x * 4 + 3]));
            }
        }
        image_ = image;
    }

    subtitles::SubtitleRenderer renderer_;
    subtitles::SubtitleSettings subtitleDraft_;
    subtitles::SubtitleSettings lastRenderedDraft_;
    juce::Image image_;
    std::string note_;
};

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
    audioPage_ = new ui::ScrollablePage([this](int width) { return layoutAudioPage(width); });
    auto& audioBody = audioPage_->body();
    caption(deviceCaption_, "ASIO device (one in/out device)", audioBody);
    styleBox(deviceChoice_, audioBody);   // no onChange: the draft owns the widget until Apply
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
    styleButton(refreshDevicesButton_, audioBody);
    hint(deviceNoteLabel_, "", audioBody);
    caption(sampleRateCaption_, "Sample rate", audioBody);
    styleBox(sampleRateChoice_, audioBody);
    caption(bufferCaption_, "Buffer size (frames)", audioBody);
    const auto [bufferMin, bufferMax] = config::bufferFramesRange();
    slider(bufferSlider_, bufferMin, bufferMax, 16, audioBody);
    caption(inputChannelCaption_, "Input channel", audioBody);
    styleBox(inputChannelChoice_, audioBody);
    caption(outputChannelCaption_, "Output channel", audioBody);
    styleBox(outputChannelChoice_, audioBody);
    hint(channelNoteLabel_, "", audioBody);
    caption(inputGainCaption_, "Input gain (dB)", audioBody);
    caption(outputGainCaption_, "Output gain (dB)", audioBody);
    const auto [gainMin, gainMax] = config::gainRange();
    // The wheel rule is the factory's now (ui::wheelSafeSlider via slider()):
    // every settings slider - these live gains included - ignores the wheel
    // and lets it scroll the page (venue task 4 §2 superseded the earlier
    // "only the live pair" decision).
    slider(inputGainSlider_, gainMin, gainMax, 0.5, audioBody);
    slider(outputGainSlider_, gainMin, gainMax, 0.5, audioBody);
    hint(audioRestartHintLabel_,
         "Device, sample rate, buffer and channels take effect on the next Start; the target "
         "language (and the Off behavior) applies while running.",
         audioBody);

    // -------------------------------------------------------------- translation
    translationPage_ = new ui::ScrollablePage(
        [this](int width) { return layoutTranslationPage(width); });
    auto& translationBody = translationPage_->body();
    // The provider auto-detects the spoken language (docs section 5, R8):
    // what the operator picks here is the EXPECTATION LingoFlow validates
    // and reports with - not a parameter OpenAI is given. The labels say
    // exactly that, so the screen cannot imply a declaration the wire never
    // carries.
    caption(sourceCaption_, "Expected source language", translationBody);
    styleBox(sourceChoice_, translationBody);
    caption(targetCaption_, "Target language", translationBody);
    styleBox(targetChoice_, translationBody);
    // §3: Off is a point in the target list; what the audience hears while
    // off is this separate, persisted choice - not a third list entry and not
    // a voice switch. Both values come from the panel's closed vocabulary, so
    // the selector cannot offer what the schema would refuse.
    caption(offBehaviorCaption_, "When the target is Off, the output:", translationBody);
    styleBox(offBehaviorChoice_, translationBody);
    hint(offBehaviorNoteLabel_,
         "Bypass sends your live audio straight to the room (no translation, no added delay); "
         "silence stops the output. The device stays open in both.",
         translationBody);
    hint(sourceNoteLabel_,
         "OpenAI detects the spoken language automatically. The expectation above is used "
         "by LingoFlow for validation and diagnostics only.",
         translationBody);
    pairWarningLabel_.setFont(ui::uiFont(12.0f));
    pairWarningLabel_.setColour(juce::Label::textColourId, sink::red);
    translationBody.addAndMakeVisible(pairWarningLabel_);
    caption(modelHintCaption_, "Model hint (empty = backend default)", translationBody);
    translationBody.addAndMakeVisible(modelHintEditor_);
    toggle(reconnectToggle_, translationBody);
    const auto [backoffMin, backoffMax] = config::reconnectBackoffRange();
    caption(initialBackoffCaption_, "First reconnect delay (ms)", translationBody);
    slider(initialBackoffSlider_, backoffMin, backoffMax, 100, translationBody);
    caption(maxBackoffCaption_, "Maximum reconnect backoff (ms)", translationBody);
    slider(maxBackoffSlider_, backoffMin, backoffMax, 100, translationBody);
    const auto [ageMin, ageMax] = config::sessionMaxAgeRange();
    caption(sessionAgeCaption_, "Session max age (s; 0 = no proactive reopen)", translationBody);
    slider(sessionAgeSlider_, ageMin, ageMax, 60, translationBody);
    caption(jitterCaption_, "Jitter pre-roll (ms)", translationBody);
    const auto [jitterMin, jitterMax] = config::jitterBufferRange();
    slider(jitterSlider_, jitterMin, jitterMax, 10, translationBody);

    // ----------------------------------------------------------- subtitles/NDI
    subtitlesPage_ = new ui::ScrollablePage(
        [this](int width) { return layoutSubtitlesPage(width); });
    auto& subtitlesBody = subtitlesPage_->body();
    toggle(ndiToggle_, subtitlesBody);
    caption(streamNameCaption_, "NDI stream name", subtitlesBody);
    subtitlesBody.addAndMakeVisible(streamNameEditor_);
    ndiStateLabel_.setFont(ui::uiFont(13.0f));
    subtitlesBody.addAndMakeVisible(ndiStateLabel_);

    caption(ndiModeCaption_, "Output (one source, chosen media)", subtitlesBody);
    for (std::size_t i = 0; i < std::size(kSubtitleModes); ++i)
        ndiModeChoice_.addItem(juce::String(kSubtitleModes[i].first), static_cast<int> (i) + 1);
    styleBox(ndiModeChoice_, subtitlesBody);

    caption(ndiResolutionCaption_, "Video resolution", subtitlesBody);
    for (std::size_t i = 0; i < std::size(kSubtitleResolutions); ++i)
        ndiResolutionChoice_.addItem(juce::String(kSubtitleResolutions[i].first), static_cast<int> (i) + 1);
    styleBox(ndiResolutionChoice_, subtitlesBody);

    caption(ndiFpsCaption_, "Frame rate (fps)", subtitlesBody);
    for (std::size_t i = 0; i < std::size(kSubtitleFrameRates); ++i)
        ndiFpsChoice_.addItem(juce::String(kSubtitleFrameRates[i]), static_cast<int> (i) + 1);
    styleBox(ndiFpsChoice_, subtitlesBody);

    caption(fontFamilyCaption_, "Font family", subtitlesBody);
    subtitlesBody.addAndMakeVisible(fontFamilyEditor_);
    caption(fontSizeCaption_, "Font size (px at 1080p; scales with resolution)", subtitlesBody);
    slider(fontSizeSlider_, 16, 200, 1, subtitlesBody);

    textColorButton_.onClick = [this] { aimColourSelector(true); };
    outlineColorButton_.onClick = [this] { aimColourSelector(false); };
    styleButton(textColorButton_, subtitlesBody);
    styleButton(outlineColorButton_, subtitlesBody);

    caption(outlineWidthCaption_, "Outline width (px at 1080p; 0 = none)", subtitlesBody);
    slider(outlineWidthSlider_, 0, 8, 1, subtitlesBody);
    toggle(shadowToggle_, subtitlesBody);
    caption(shadowOffsetCaption_, "Shadow offset (px at 1080p)", subtitlesBody);
    slider(shadowOffsetSlider_, 0, 16, 1, subtitlesBody);
    caption(shadowOpacityCaption_, "Shadow opacity (%)", subtitlesBody);
    slider(shadowOpacitySlider_, 0, 100, 5, subtitlesBody);
    toggle(backgroundToggle_, subtitlesBody);

    caption(alignCaption_, "Alignment (horizontal / vertical edge)", subtitlesBody);
    hAlignChoice_.addItem("left", 1);
    hAlignChoice_.addItem("center", 2);
    hAlignChoice_.addItem("right", 3);
    vAnchorChoice_.addItem("bottom", 1);
    vAnchorChoice_.addItem("top", 2);
    styleBox(hAlignChoice_, subtitlesBody);
    styleBox(vAnchorChoice_, subtitlesBody);

    caption(marginCaption_, "Edge margin (%) of frame height", subtitlesBody);
    slider(marginSlider_, 0, 40, 1, subtitlesBody);
    caption(blockWidthCaption_, "Max text block width (%)", subtitlesBody);
    slider(blockWidthSlider_, 25, 100, 5, subtitlesBody);
    caption(maxLinesCaption_, "Max lines (NDI video shows one line)", subtitlesBody);
    slider(maxLinesSlider_, 1, 6, 1, subtitlesBody);
    caption(lineSpacingCaption_, "Line spacing (%)", subtitlesBody);
    slider(lineSpacingSlider_, 0, 100, 5, subtitlesBody);

    resetAppearanceButton_.onClick = [this] { resetAppearancePressed(); };
    styleButton(resetAppearanceButton_, subtitlesBody);

    auto colourSlider = [this, &subtitlesBody](juce::Slider& s)
    {
        // slider() carries the wheel rule (venue task 4 §2) - the colour
        // editor's RGB sliders are sliders like any other.
        slider(s, 0, 255, 1, subtitlesBody);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 18);
        s.onValueChange = [this] { applyColourEdit(); };
    };
    colourSlider(colourRSlider_);
    colourSlider(colourGSlider_);
    colourSlider(colourBSlider_);
    caption(colourEditorCaption_, "Colour editor (R / G / B) - aim it with the two buttons above",
            subtitlesBody);
    colourSwatch_.setColour(juce::Label::backgroundColourId, colourFromRgb(0xFFFFFF));
    subtitlesBody.addAndMakeVisible(colourSwatch_);

    subtitlePreview_ = std::make_unique<SubtitlePreview>();
    caption(previewCaption_, "Preview (checkerboard = the transparent background; sample caption)",
            subtitlesBody);
    subtitlesBody.addAndMakeVisible(*subtitlePreview_);

    hint(ndiHintLabel_,
         "Video mode is a real NDI video source with a transparent background - add it in OBS "
         "as an ordinary NDI Video Source. Metadata mode sends TTML timed text (the delivered "
         "behaviour). Appearance and geometry apply live; the stream name takes effect on "
         "Stop + Start. NDI never carries audio and its failure never stops it - seeing the "
         "text on a receiver is the venue checkpoint; counters only prove the sender sent.",
         subtitlesBody);

    // -------------------------------------------------------------- credentials
    credentialsPage_ = new ui::ScrollablePage(
        [this](int width) { return layoutCredentialsPage(width); });
    auto& credentialsBody = credentialsPage_->body();
    credStatusLabel_.setFont(ui::uiFont(14.0f));
    credStatusLabel_.setColour(juce::Label::textColourId, sink::textPrimary);
    credentialsBody.addAndMakeVisible(credStatusLabel_);
    apiKeyEditor_.setPasswordCharacter(L'*');   // the field masks its echo (PASS criterion)
    credentialsBody.addAndMakeVisible(apiKeyEditor_);
    saveKeyButton_.onClick = [this] { saveKeyPressed(); };
    removeKeyButton_.onClick = [this] { removeKeyPressed(); };
    styleButton(saveKeyButton_, credentialsBody);
    styleButton(removeKeyButton_, credentialsBody);
    hint(keyHintLabel_,
         "The key lives in Windows secure storage, never in the settings file or the "
         "log. The field clears after a successful store and the value is never shown "
         "again here.",
         credentialsBody);

    // -------------------------------------------------------------- diagnostics
    diagnosticsPage_ = new ui::ScrollablePage(
        [this](int width) { return layoutDiagnosticsPage(width); });
    auto& diagnosticsBody = diagnosticsPage_->body();
    caption(logLevelCaption_, "Log level", diagnosticsBody);
    diagnosticsBody.addAndMakeVisible(logLevelChoice_);   // part of the draft: Apply commits it
    logLevelChoice_.setColour(juce::ComboBox::backgroundColourId, sink::control);
    logLevelChoice_.setColour(juce::ComboBox::outlineColourId, sink::cardBorder);
    logLevelChoice_.setColour(juce::ComboBox::textColourId, sink::textPrimary);
    toggle(logFileToggle_, diagnosticsBody);
    hint(logHintLabel_,
         "The log file and recovery-policy edits take effect when the application "
         "restarts. Export writes a structured diagnostics report you can carry to "
         "a venue - it contains counters and events, never the API key.",
         diagnosticsBody);
    exportDiagnosticsButton_.onClick = [this] { exportPressed(); };
    styleButton(exportDiagnosticsButton_, diagnosticsBody);

    // ------------------------------------------------------------------ advanced
    advancedPage_ = new ui::ScrollablePage(
        [this](int width) { return layoutAdvancedPage(width); });
    auto& advancedBody = advancedPage_->body();
    caption(instructionsCaption_,
            "Translation instructions - unsupported by the current model (kept for a future one)",
            advancedBody);
    instructionsEditor_.setMultiLine(true);
    instructionsEditor_.setScrollbarsShown(true);
    instructionsEditor_.setReadOnly(true);   // An earlier review: no authoring of a dead setting
    advancedBody.addAndMakeVisible(instructionsEditor_);
    hint(instructionsHintLabel_,
         "The provider accepts no custom prompting (protocol docs section 12.1). This "
         "value is stored for a future model, is never sent today, and the backend logs "
         "plainly that it was ignored. Empty means \"nothing configured\".",
         advancedBody);
    toggle(developerToggle_, advancedBody);
    caption(devSourceCaption_, "Audio source (no SoundGrid needed for wav/tone)", advancedBody);
    devSourceChoice_.setColour(juce::ComboBox::backgroundColourId, sink::control);
    devSourceChoice_.setColour(juce::ComboBox::outlineColourId, sink::cardBorder);
    devSourceChoice_.setColour(juce::ComboBox::textColourId, sink::textPrimary);
    advancedBody.addAndMakeVisible(devSourceChoice_);
    caption(devWavInCaption_, "WAV input file (its rate must equal the sample-rate setting)", advancedBody);
    advancedBody.addAndMakeVisible(devWavInEditor_);
    caption(devWavOutCaption_, "Rehearsal recording WAV (empty = do not record)", advancedBody);
    advancedBody.addAndMakeVisible(devWavOutEditor_);
    toggle(mockToggle_, advancedBody);
    const auto [mockMin, mockMax] = config::mockLatencyRange();
    caption(mockLatencyCaption_, "Mock echo delay (ms)", advancedBody);
    slider(devMockLatencySlider_, mockMin, mockMax, 10, advancedBody);
    caption(devToneFreqCaption_, "Test tone frequency (Hz)", advancedBody);
    const auto [toneMin, toneMax] = config::toneFrequencyRange();
    slider(devToneFreqSlider_, toneMin, toneMax, 10, advancedBody);
    caption(devToneLevelCaption_, "Test tone level (dBFS)", advancedBody);
    const auto [levelMin, levelMax] = config::toneLevelRangeDb();
    slider(devToneLevelSlider_, levelMin, levelMax, 1, advancedBody);
    loopbackToggle_.onClick = [this] { loopbackVisuals(); };
    toggle(loopbackToggle_, advancedBody);
    hint(developerHintLabel_,
         "Mounted at application start: source, mock and loopback edits take effect on "
         "restart. Loopback plays the capture to the audience and leaves the translator "
         "unfed - never enable it with a live room.",
         advancedBody);

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
    // the export funnel, offered here as well as in the Diagnostics window
    // (the UI redesign Р’В§6): one call, and the note is the receipt either way.
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
    if (const int index = offBehaviorChoice_.getSelectedItemIndex();
        index >= 0 && static_cast<std::size_t> (index) < offBehaviorCache_.size())
    {
        candidate.translation.offOutputBehavior = offBehaviorCache_[static_cast<std::size_t> (index)].value;
    }
    candidate.translation.instructions = instructionsEditor_.getText().toStdString();
    candidate.translation.modelHint = modelHintEditor_.getText().toStdString();
    candidate.translation.reconnectEnabled = reconnectToggle_.getToggleState();
    candidate.translation.reconnectInitialBackoffMs = static_cast<int> (initialBackoffSlider_.getValue());
    candidate.translation.reconnectMaxBackoffMs = static_cast<int> (maxBackoffSlider_.getValue());
    candidate.translation.sessionMaxAgeSeconds = static_cast<int> (sessionAgeSlider_.getValue());
    candidate.translation.jitterBufferMs = static_cast<int> (jitterSlider_.getValue());

    // Subtitles tab: the whole draft read back from the widgets; the colour
    // pair lives in subtitleDraft_ (the RGB editor writes straight there).
    candidate.ndi.enabled = ndiToggle_.getToggleState();
    candidate.ndi.streamName = streamNameEditor_.getText().toStdString();
    candidate.ndi.subtitles = readSubtitleDraft();

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
    // the UI redesign Р’В§7: the safety sentence is red and explicit while loopback is
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
    syncBox(offBehaviorChoice_, offBehaviorCache_, panel.offOutputBehaviors,
            panel.selectedOffOutputBehavior);
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
    fillSubtitleWidgets(cfg.ndi.subtitles);

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

    // The preview only re-renders while its tab is actually shown - the render
    // is a real GDI+ paint, cheap but pointless off-screen. The draft is
    // pushed in, and refresh() compares before painting: widgets that did not
    // move cost nothing.
    if (subtitlePreview_ != nullptr && tabs_ != nullptr && tabs_->getCurrentTabIndex() == 2)
    {
        subtitlePreview_->setDraft(readSubtitleDraft());
        subtitlePreview_->refresh();
    }
}

// --------------------------------------------------------------------- subtitles draft

subtitles::SubtitleSettings SettingsContent::readSubtitleDraft() const
{
    // Committed first: a combo with no matching selection carries a
    // hand-edited config value outside the presets (it appears as a "custom"
    /// entry), and rewriting it to a preset the operator did not choose would
    /// be the settings file lying about the dialog.
    subtitles::SubtitleSettings draft = controller_.config().current().ndi.subtitles;

    if (const int index = ndiModeChoice_.getSelectedItemIndex(); index >= 0
        && static_cast<std::size_t> (index) < std::size(kSubtitleModes))
    {
        draft.mode = kSubtitleModes[static_cast<std::size_t> (index)].second;
    }

    if (const int index = ndiResolutionChoice_.getSelectedItemIndex(); index >= 0
        && static_cast<std::size_t> (index) < std::size(kSubtitleResolutions))
    {
        draft.video.width = kSubtitleResolutions[static_cast<std::size_t> (index)].second.first;
        draft.video.height = kSubtitleResolutions[static_cast<std::size_t> (index)].second.second;
    }

    if (const int index = ndiFpsChoice_.getSelectedItemIndex(); index >= 0
        && static_cast<std::size_t> (index) < std::size(kSubtitleFrameRates))
    {
        draft.video.fps = kSubtitleFrameRates[static_cast<std::size_t> (index)];
    }

    auto& style = draft.style;
    style.fontFamily = fontFamilyEditor_.getText().toStdString();
    style.fontSizePx = static_cast<int> (fontSizeSlider_.getValue());
    style.outlineWidthPx = static_cast<int> (outlineWidthSlider_.getValue());
    style.shadowEnabled = shadowToggle_.getToggleState();
    style.shadowOffsetPx = static_cast<int> (shadowOffsetSlider_.getValue());
    style.shadowOpacityPercent = static_cast<int> (shadowOpacitySlider_.getValue());
    style.backgroundEnabled = backgroundToggle_.getToggleState();
    style.edgeMarginPercent = static_cast<int> (marginSlider_.getValue());
    style.blockWidthPercent = static_cast<int> (blockWidthSlider_.getValue());
    style.maxLines = static_cast<int> (maxLinesSlider_.getValue());
    style.lineSpacingPercent = static_cast<int> (lineSpacingSlider_.getValue());

    // Colours: the R/G/B editor owns them and writes into subtitleDraft_, so
    // they travel by struct field, not by widget.
    style.textColorRgb = subtitleDraft_.style.textColorRgb;
    style.outlineColorRgb = subtitleDraft_.style.outlineColorRgb;

    if (const int index = hAlignChoice_.getSelectedItemIndex(); index >= 0 && index <= 2)
        style.horizontalAlign = static_cast<subtitles::HorizontalAlign> (index);
    if (const int index = vAnchorChoice_.getSelectedItemIndex(); index >= 0 && index <= 1)
        style.verticalAnchor = static_cast<subtitles::VerticalAnchor> (index);

    return draft;
}

void SettingsContent::fillSubtitleWidgets(const subtitles::SubtitleSettings& s)
{
    subtitleDraft_ = s;

    for (std::size_t i = 0; i < std::size(kSubtitleModes); ++i)
    {
        if (kSubtitleModes[i].second == s.mode)
        {
            ndiModeChoice_.setSelectedId(static_cast<int> (i) + 1,
                                         juce::NotificationType::dontSendNotification);
            break;
        }
    }

    // The geometry combos are rebuilt, not just reselected: a config value
    // outside the presets gets a visible "custom" entry instead of silently
    /// becoming a preset.
    ndiResolutionChoice_.clear(juce::NotificationType::dontSendNotification);
    int resolutionId = 0;
    for (std::size_t i = 0; i < std::size(kSubtitleResolutions); ++i)
    {
        ndiResolutionChoice_.addItem(juce::String(kSubtitleResolutions[i].first),
                                     static_cast<int> (i) + 1);
        if (kSubtitleResolutions[i].second.first == s.video.width
            && kSubtitleResolutions[i].second.second == s.video.height)
        {
            resolutionId = static_cast<int> (i) + 1;
        }
    }
    if (resolutionId == 0)
    {
        ndiResolutionChoice_.addItem(juce::String(s.video.width) + " x " + juce::String(s.video.height)
                                         + " (custom)",
                                     static_cast<int> (std::size(kSubtitleResolutions)) + 2);
        resolutionId = static_cast<int> (std::size(kSubtitleResolutions)) + 2;
    }
    ndiResolutionChoice_.setSelectedId(resolutionId, juce::NotificationType::dontSendNotification);

    ndiFpsChoice_.clear(juce::NotificationType::dontSendNotification);
    int fpsId = 0;
    for (std::size_t i = 0; i < std::size(kSubtitleFrameRates); ++i)
    {
        ndiFpsChoice_.addItem(juce::String(kSubtitleFrameRates[i]), static_cast<int> (i) + 1);
        if (kSubtitleFrameRates[i] == s.video.fps)
            fpsId = static_cast<int> (i) + 1;
    }
    if (fpsId == 0)
    {
        ndiFpsChoice_.addItem(juce::String(s.video.fps) + " (custom)",
                              static_cast<int> (std::size(kSubtitleFrameRates)) + 2);
        fpsId = static_cast<int> (std::size(kSubtitleFrameRates)) + 2;
    }
    ndiFpsChoice_.setSelectedId(fpsId, juce::NotificationType::dontSendNotification);

    fontFamilyEditor_.setText(juce::String(s.style.fontFamily),
                              juce::NotificationType::dontSendNotification);
    fontSizeSlider_.setValue(s.style.fontSizePx, juce::NotificationType::dontSendNotification);
    outlineWidthSlider_.setValue(s.style.outlineWidthPx, juce::NotificationType::dontSendNotification);
    shadowToggle_.setToggleState(s.style.shadowEnabled, juce::NotificationType::dontSendNotification);
    shadowOffsetSlider_.setValue(s.style.shadowOffsetPx, juce::NotificationType::dontSendNotification);
    shadowOpacitySlider_.setValue(s.style.shadowOpacityPercent,
                                  juce::NotificationType::dontSendNotification);
    backgroundToggle_.setToggleState(s.style.backgroundEnabled,
                                     juce::NotificationType::dontSendNotification);
    marginSlider_.setValue(s.style.edgeMarginPercent, juce::NotificationType::dontSendNotification);
    blockWidthSlider_.setValue(s.style.blockWidthPercent, juce::NotificationType::dontSendNotification);
    maxLinesSlider_.setValue(s.style.maxLines, juce::NotificationType::dontSendNotification);
    lineSpacingSlider_.setValue(s.style.lineSpacingPercent, juce::NotificationType::dontSendNotification);
    hAlignChoice_.setSelectedId(static_cast<int> (s.style.horizontalAlign) + 1,
                                juce::NotificationType::dontSendNotification);
    vAnchorChoice_.setSelectedId(static_cast<int> (s.style.verticalAnchor) + 1,
                                 juce::NotificationType::dontSendNotification);

    textColorButton_.setColour(juce::TextButton::buttonColourId, colourFromRgb(s.style.textColorRgb));
    outlineColorButton_.setColour(juce::TextButton::buttonColourId, colourFromRgb(s.style.outlineColorRgb));

    // The R/G/B editor shows the aimed colour (programmatic: no notification,
    // the operator has not picked anything yet).
    const std::uint32_t aimed = editingTextColour_ ? s.style.textColorRgb : s.style.outlineColorRgb;
    colourRSlider_.setValue((aimed >> 16) & 0xFFu, juce::NotificationType::dontSendNotification);
    colourGSlider_.setValue((aimed >> 8) & 0xFFu, juce::NotificationType::dontSendNotification);
    colourBSlider_.setValue(aimed & 0xFFu, juce::NotificationType::dontSendNotification);
    colourSwatch_.setColour(juce::Label::backgroundColourId, colourFromRgb(aimed));

    if (subtitlePreview_ != nullptr)
        subtitlePreview_->setDraft(s);
}

void SettingsContent::resetAppearancePressed()
{
    // The look returns to the schema defaults; the transport choices (mode,
    // resolution, fps, stream name, the switch) are the operator's other hand
    // and stay where they are. Still a draft until Apply, like everything else.
    subtitleDraft_.style = config::defaults().ndi.subtitles.style;
    fillSubtitleWidgets(subtitleDraft_);
    actionNote_ = "appearance reset to the defaults - press Apply settings to commit it";
    showNote();
}

void SettingsContent::applyColourEdit()
{
    if (updatingWidgets_)
        return;   // a programmatic rewrite is not the operator editing

    const std::uint32_t rgb = (static_cast<std::uint32_t> (colourRSlider_.getValue()) << 16)
                            | (static_cast<std::uint32_t> (colourGSlider_.getValue()) << 8)
                            | static_cast<std::uint32_t> (colourBSlider_.getValue());

    auto& style = subtitleDraft_.style;
    if (editingTextColour_)
    {
        style.textColorRgb = rgb;
        textColorButton_.setColour(juce::TextButton::buttonColourId, colourFromRgb(rgb));
    }
    else
    {
        style.outlineColorRgb = rgb;
        outlineColorButton_.setColour(juce::TextButton::buttonColourId, colourFromRgb(rgb));
    }

    colourSwatch_.setColour(juce::Label::backgroundColourId, colourFromRgb(rgb));

    if (subtitlePreview_ != nullptr)
    {
        subtitlePreview_->setDraft(readSubtitleDraft());
        subtitlePreview_->refresh();
    }
}

void SettingsContent::aimColourSelector(bool forText)
{
    editingTextColour_ = forText;

    const std::uint32_t rgb = forText ? subtitleDraft_.style.textColorRgb
                                      : subtitleDraft_.style.outlineColorRgb;
    updatingWidgets_ = true;   // the aim is not an edit - no draft write, no echo
    colourRSlider_.setValue((rgb >> 16) & 0xFFu, juce::NotificationType::dontSendNotification);
    colourGSlider_.setValue((rgb >> 8) & 0xFFu, juce::NotificationType::dontSendNotification);
    colourBSlider_.setValue(rgb & 0xFFu, juce::NotificationType::dontSendNotification);
    updatingWidgets_ = false;

    colourSwatch_.setColour(juce::Label::backgroundColourId, colourFromRgb(rgb));
}

// ---------------------------------------------------------------------------- layout

void SettingsContent::resized()
{
    auto bounds = getLocalBounds().reduced(kPad);

    // Breathing room between the tab content and the command row: the tabs
    // end, then the gap, then Apply - the first cut had them sharing an edge.
    // Apply and the note line sit OUTSIDE the tab pages on purpose: whichever
    // page is open and however far it is scrolled, the commit button never
    // hides behind the viewport (venue task 4 §3).
    auto bottom = bounds.removeFromBottom(kRowGap + kButtonH + kRowGap + 18);
    bottom.removeFromTop(kRowGap);
    applyButton_.setBounds(bottom.removeFromTop(kButtonH).withWidth(180));
    noteLabel_.setBounds(bottom.withHeight(18));

    tabs_->setBounds(bounds);

    // The pages lay themselves out: each ui::ScrollablePage measures and
    // positions its body inside its own resized() (driven here by the
    // TabbedComponent sizing the current page). Hidden pages measure when
    // they become current - their widgets wait in the body meanwhile.
}

int SettingsContent::layoutAudioPage(int width)
{
    auto b = juce::Rectangle<int>(0, 0, width, kMeasureHeight).reduced(kPad);

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
    audioRestartHintLabel_.setBounds(b.removeFromTop(28));

    // Everything the page needs sits above this line; the viewport hands the
    // rest to the scroll bar. kMeasureHeight is the measuring stick - the
    // answer is what the stack actually consumed, plus the bottom padding.
    return kMeasureHeight - b.getHeight() - kPad;
}

int SettingsContent::layoutTranslationPage(int width)
{
    auto b = juce::Rectangle<int>(0, 0, width, kMeasureHeight).reduced(kPad);

    auto cols = b.removeFromTop(kCaptionH + kControlH + 4);   // consumed from b too
    auto left = cols.removeFromLeft(cols.getWidth() / 2);
    auto right = cols;
    right.removeFromLeft(24);
    sourceCaption_.setBounds(left.removeFromTop(kCaptionH));
    sourceChoice_.setBounds(left);
    targetCaption_.setBounds(right.removeFromTop(kCaptionH));
    targetChoice_.setBounds(right);
    b.removeFromTop(kRowGap);

    sourceNoteLabel_.setBounds(b.removeFromTop(16));
    b.removeFromTop(4);

    pairWarningLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(kRowGap);

    offBehaviorCaption_.setBounds(b.removeFromTop(kCaptionH));
    offBehaviorChoice_.setBounds(b.removeFromTop(kControlH));
    b.removeFromTop(4);
    offBehaviorNoteLabel_.setBounds(b.removeFromTop(28));
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

    return kMeasureHeight - b.getHeight() - kPad;
}

int SettingsContent::layoutSubtitlesPage(int width)
{
    auto b = juce::Rectangle<int>(0, 0, width, kMeasureHeight).reduced(kPad);
    const int trackH = kMeasureHeight - 2 * kPad;   // both columns start this tall

    // Two columns: the transport (what NDI carries) left, the look (what the
    // audience reads) right, with the live preview under the look. Every row
    // is caption-over-control, the same rhythm as the other pages.
    auto right = b.removeFromRight((b.getWidth() * 52) / 100);
    b.removeFromRight(kPad);

    // ------------------------------------------------------------ left: transport
    ndiToggle_.setBounds(b.removeFromTop(24));
    b.removeFromTop(kRowGap);
    streamNameCaption_.setBounds(b.removeFromTop(kCaptionH));
    streamNameEditor_.setBounds(b.removeFromTop(kControlH));
    b.removeFromTop(kRowGap);
    ndiStateLabel_.setBounds(b.removeFromTop(20));
    b.removeFromTop(kRowGap);

    ndiModeCaption_.setBounds(b.removeFromTop(kCaptionH));
    ndiModeChoice_.setBounds(b.removeFromTop(kControlH));
    b.removeFromTop(kRowGap);

    ndiResolutionCaption_.setBounds(b.removeFromTop(kCaptionH));
    ndiResolutionChoice_.setBounds(b.removeFromTop(kControlH));
    b.removeFromTop(kRowGap);

    ndiFpsCaption_.setBounds(b.removeFromTop(kCaptionH));
    ndiFpsChoice_.setBounds(b.removeFromTop(kControlH));
    b.removeFromTop(kRowGap);

    ndiHintLabel_.setBounds(b.removeFromTop(80));

    // ------------------------------------------------------------- right: the look
    auto rb = right;
    fontFamilyCaption_.setBounds(rb.removeFromTop(kCaptionH));
    fontFamilyEditor_.setBounds(rb.removeFromTop(kControlH));
    rb.removeFromTop(kRowGap);

    fontSizeCaption_.setBounds(rb.removeFromTop(kCaptionH));
    fontSizeSlider_.setBounds(rb.removeFromTop(kControlH));
    rb.removeFromTop(kRowGap);

    {
        auto colours = rb.removeFromTop(kButtonH);
        const int half = colours.getWidth() / 2;
        textColorButton_.setBounds(colours.removeFromLeft(half - 4));
        colours.removeFromLeft(8);
        outlineColorButton_.setBounds(colours);
    }
    rb.removeFromTop(kRowGap);

    colourSwatch_.setBounds(rb.removeFromTop(22));
    colourEditorCaption_.setBounds(rb.removeFromTop(14));
    {
        auto rgb = rb.removeFromTop(24);
        const int third = rgb.getWidth() / 3;
        colourRSlider_.setBounds(rgb.removeFromLeft(third));
        colourGSlider_.setBounds(rgb.removeFromLeft(third));
        colourBSlider_.setBounds(rgb);
    }
    rb.removeFromTop(kRowGap);

    outlineWidthCaption_.setBounds(rb.removeFromTop(kCaptionH));
    outlineWidthSlider_.setBounds(rb.removeFromTop(24));
    shadowToggle_.setBounds(rb.removeFromTop(22));
    shadowOffsetCaption_.setBounds(rb.removeFromTop(kCaptionH));
    shadowOffsetSlider_.setBounds(rb.removeFromTop(24));
    shadowOpacityCaption_.setBounds(rb.removeFromTop(kCaptionH));
    shadowOpacitySlider_.setBounds(rb.removeFromTop(24));
    backgroundToggle_.setBounds(rb.removeFromTop(22));
    rb.removeFromTop(6);

    alignCaption_.setBounds(rb.removeFromTop(kCaptionH));
    {
        auto alignRow = rb.removeFromTop(kControlH);
        const int half = alignRow.getWidth() / 2;
        hAlignChoice_.setBounds(alignRow.removeFromLeft(half - 4));
        alignRow.removeFromLeft(8);
        vAnchorChoice_.setBounds(alignRow);
    }
    rb.removeFromTop(kRowGap);

    marginCaption_.setBounds(rb.removeFromTop(kCaptionH));
    marginSlider_.setBounds(rb.removeFromTop(24));
    blockWidthCaption_.setBounds(rb.removeFromTop(kCaptionH));
    blockWidthSlider_.setBounds(rb.removeFromTop(24));
    maxLinesCaption_.setBounds(rb.removeFromTop(kCaptionH));
    maxLinesSlider_.setBounds(rb.removeFromTop(24));
    lineSpacingCaption_.setBounds(rb.removeFromTop(kCaptionH));
    lineSpacingSlider_.setBounds(rb.removeFromTop(24));
    rb.removeFromTop(kRowGap);

    resetAppearanceButton_.setBounds(rb.removeFromTop(kButtonH).withWidth(240));
    rb.removeFromTop(kRowGap);

    // Preview: a fixed-height window. A measured page cannot hand the last
    // widget "the rest of the rectangle" - the viewport decides the height -
    // so the preview claims its own strip and the checkerboard scales to it.
    previewCaption_.setBounds(rb.removeFromTop(kCaptionH));
    subtitlePreview_->setBounds(rb.removeFromTop(kPreviewH));

    // The page is as tall as its TALLER column (the ui::ScrollablePage
    // monotone contract holds: nothing here depends on the leftover height).
    const int leftUsed = trackH - b.getHeight();
    const int rightUsed = trackH - rb.getHeight();
    return kPad + std::max(leftUsed, rightUsed);
}

int SettingsContent::layoutCredentialsPage(int width)
{
    auto b = juce::Rectangle<int>(0, 0, width, kMeasureHeight).reduced(kPad);

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

    return kMeasureHeight - b.getHeight() - kPad;
}

int SettingsContent::layoutDiagnosticsPage(int width)
{
    auto b = juce::Rectangle<int>(0, 0, width, kMeasureHeight).reduced(kPad);

    logLevelCaption_.setBounds(b.removeFromTop(kCaptionH));
    logLevelChoice_.setBounds(b.removeFromTop(kControlH).withWidth(200));
    b.removeFromTop(kRowGap);
    logFileToggle_.setBounds(b.removeFromTop(24));
    b.removeFromTop(kRowGap);
    exportDiagnosticsButton_.setBounds(b.removeFromTop(kButtonH).withWidth(200));
    b.removeFromTop(kRowGap);
    logHintLabel_.setBounds(b.removeFromTop(44));

    return kMeasureHeight - b.getHeight() - kPad;
}

int SettingsContent::layoutAdvancedPage(int width)
{
    auto b = juce::Rectangle<int>(0, 0, width, kMeasureHeight).reduced(kPad);

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
    developerHintLabel_.setBounds(right.removeFromTop(56));

    // Both columns live inside the fixed devBlockH - the block arithmetic is
    // the page's own, the return just closes the stack with the bottom pad.
    return kMeasureHeight - b.getHeight() - kPad;
}

// =========================================================================== SettingsWindow

SettingsWindow::SettingsWindow(ApplicationController& controller, juce::Component& ownerWindow)
    : juce::DocumentWindow("LingoFlow Settings",
                           juce::Colour(0xff1a1d20u),
                           juce::DocumentWindow::closeButton),
      ownerWindow_(ownerWindow)
{
    setUsingNativeTitleBar(true);
    // No delete-on-close: close hides, the owner window owns the lifetime
    //.

    auto* content = new SettingsContent(controller);
    content_ = content;
    setContentOwned(content, true);

    setResizable(true, true);
    // the UI redesign made the tabs; venue task 4 §3 made the pages
    // scrollable: the window no longer has to grow to the tallest page, it
    // shows one honest viewport and Apply outside the scroll. 820x620 keeps
    // every short page unscrolled on a venue laptop; taller ones scroll
    // instead of cutting their bottom rows off.
    setResizeLimits(640, 480, 4000, 4000);
    setSize(820, 620);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
    becomeModal();
}

void SettingsWindow::reopen()
{
    if (content_ != nullptr)
        content_->refreshFromSettings();   // a dialog reopened never shows a stale draft

    setVisible(true);
    toFront(true);   ///< the component-level bring-to-front; no focus tricks needed
    becomeModal();   // dismissal ended the modal state - a reopen begins it again
}

void SettingsWindow::becomeModal()
{
    // Venue task 4 §4. enterModalState returns immediately and runs NO nested
    // event loop (unlike runModalLoop): the message thread keeps dispatching,
    // so the audio engine, the network threads and the operator window's own
    // refresh clock are untouched, and a Stop or a quit behind this dialog can
    // never deadlock on it. Input to every other window is blocked by JUCE's
    // dispatch rule - the operator cannot drive the main screen mid-draft.
    //
    // Closing (closeButtonPressed) ends the modal state EXPLICITLY and the
    // dismissal callback hands control back to the operator window one loop-
    // pass later. The explicit call is deliberate: JUCE's hide-auto-cancel
    // needs a real showing->hidden transition, which only a peer-backed window
    // has - relying on it alone would make the guarantee untestable headless
    // and fragile to a close that arrives while the window is mid-show.
    // deleteWhenDismissed stays false: this dialog is hidden and reopened,
    // never destroyed per close (one settings object, one config funnel).
    if (isCurrentlyModal())
        return;   // a duplicate reopen never stacks modality on itself

    enterModalState(true,
                    juce::ModalCallbackFunction::create(
                        [this](int)
                        {
                            // Dismissed by any route (the X, a hide, the quit
                            // teardown): the main window gets the focus back.
                            // Guarded for shutdown, where the parent may be
                            // on its way out already.
                            if (auto* topLevel = ownerWindow_.getTopLevelComponent())
                                topLevel->toFront(true);
                        }),
                    false);
}

void SettingsWindow::closeButtonPressed()
{
    // End the modal state EXPLICITLY, then hide. (JUCE documents a hide as an
    // auto-cancel, but the unit probe on this very tree saw that watcher path
    // never fire without a peer - and the close button is not the only way a
    // dialog ends: the explicit call covers every path the same way. The
    // removal and the focus-return callback then land on the next message-loop
    // pass, one frame - the application loop never stops, so they always run.)
    if (isCurrentlyModal())
        exitModalState();

    setVisible(false);
}

} // namespace liveai
