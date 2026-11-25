#include "App/Theme.h"

#include <utility>

namespace liveai {
namespace ui {
namespace {

// ------------------------------------------------------------------ icon paths
// Original, hand-authored SVG path data on a 24x24 grid. Parsed once into a
// Path; no files, no runtime SVG parsing per paint, no external asset licence.
juce::Path parsePath(const char* svg)
{
    return juce::Drawable::parseSVGPath(juce::String(svg));
}

const juce::Path& settingsIcon()
{
    static const juce::Path p = parsePath("M4 7h16 M4 12h16 M4 17h16 M9 5v4 M16 10v4 M7 15v4");
    return p;
}
const juce::Path& diagnosticsIcon()
{
    static const juce::Path p = parsePath("M3 12h4l2.5-6 4 12 2.5-6h5");
    return p;
}
const juce::Path& refreshIcon()
{
    static const juce::Path p = parsePath("M19 8a7 7 0 1 0 1 5 M19 4v5h-5");
    return p;
}
const juce::Path& playIcon()
{
    static const juce::Path p = parsePath("M8 5v14l11-7z");
    return p;
}
const juce::Path& stopIcon()
{
    static const juce::Path p = parsePath("M7 7h10v10H7z");
    return p;
}
const juce::Path& muteIcon()
{
    static const juce::Path p = parsePath("M4 9h4l5-4v14l-5-4H4z M17 9l4 6 M21 9l-4 6");
    return p;
}
const juce::Path& audioIcon()
{
    static const juce::Path p = parsePath("M3 12h2l2-6 3 12 3-9 2 3h6");
    return p;
}
const juce::Path& translationIcon()
{
    static const juce::Path p = parsePath("M3 19l5-12 5 12 M5 15h6 M14 19l3-7 3 7 M15 17h4");
    return p;
}
const juce::Path& healthIcon()
{
    static const juce::Path p = parsePath("M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z M9 12l2 2 4-4");
    return p;
}
const juce::Path& statusIcon()
{
    static const juce::Path p = parsePath("M12 4a8 8 0 1 0 0 16 8 8 0 0 0 0-16z M12 8v4l3 2");
    return p;
}

bool iconIsFilled(Icon which) noexcept
{
    return which == Icon::play || which == Icon::stop;
}

} // namespace

juce::Path iconPath(Icon which)
{
    switch (which)
    {
        case Icon::settings:    return settingsIcon();
        case Icon::diagnostics: return diagnosticsIcon();
        case Icon::refresh:     return refreshIcon();
        case Icon::play:        return playIcon();
        case Icon::stop:        return stopIcon();
        case Icon::mute:        return muteIcon();
        case Icon::audio:       return audioIcon();
        case Icon::translation: return translationIcon();
        case Icon::health:      return healthIcon();
        case Icon::status:      return statusIcon();
    }
    return {};
}

void drawIcon(juce::Graphics& g, Icon which, juce::Rectangle<float> bounds,
              const juce::Colour& tint, float strokeWidth)
{
    auto path = iconPath(which);
    if (path.isEmpty())
        return;

    const auto side = jmin(bounds.getWidth(), bounds.getHeight());
    const juce::Rectangle<float> square(bounds.getCentreX() - side * 0.5f,
                                        bounds.getCentreY() - side * 0.5f,
                                        side, side);
    path.scaleToFit(square.getX(), square.getY(), square.getWidth(), square.getHeight(), true);

    g.setColour(tint);
    if (iconIsFilled(which))
        g.fillPath(path);
    else
        g.strokePath(path, juce::PathStrokeType(strokeWidth,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
}

void drawCard(juce::Graphics& g, juce::Rectangle<int> bounds, float alpha)
{
    const auto a = juce::jlimit(0.0f, 1.0f, alpha);
    if (a <= 0.0f || bounds.isEmpty())
        return;

    const auto r = bounds.toFloat().reduced(0.5f);
    g.setColour(colour::card.withAlpha(a));
    g.fillRoundedRectangle(r, metric::radiusMd);

    // A quiet top edge reads as elevation without a shadow (shadows are
    // expensive, blurry at DPI scaling and out of place in a venue UI).
    g.setColour(colour::highlight.withAlpha(0.035f * a));
    g.fillRoundedRectangle(r.withHeight(1.5f), metric::radiusSm);

    g.setColour(colour::cardBorder.withAlpha(a));
    g.drawRoundedRectangle(r, metric::radiusMd, 1.0f);
}

void drawCardDivider(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    if (bounds.isEmpty())
        return;
    g.setColour(colour::cardBorder);
    g.drawHorizontalLine(bounds.getBottom() - 1,
                         static_cast<float>(bounds.getX()),
                         static_cast<float>(bounds.getRight() - 1));
}

// --------------------------------------------------------------------- Motion
class MotionUpdater final : private juce::Timer
{
public:
    MotionUpdater() { startTimerHz(60); }

    juce::AnimatorUpdater& updater() noexcept { return updater_; }

private:
    void timerCallback() override { updater_.update(); }

    juce::AnimatorUpdater updater_;
};

juce::AnimatorUpdater& Motion::updater()
{
    static MotionUpdater instance;
    return instance.updater();
}

// ------------------------------------------------------------------ ColourFade
ColourFade::ColourFade(juce::Component& owner, juce::Colour initial,
                       std::function<void(juce::Colour)> apply, double durationMs)
    : owner_(owner),
      apply_(std::move(apply)),
      from_(initial),
      current_(initial),
      to_(initial),
      alive_(std::make_shared<bool>(true)),
      animator_(juce::ValueAnimatorBuilder()
                    .withDurationMs(durationMs)
                    .withEasing(juce::Easings::createEaseOut())
                    .withOnStartReturningValueChangedCallback(
                        [this]
                        {
                            from_ = current_;
                            return [this, alive = alive_](float progress)
                            {
                                if (!*alive)
                                    return;
                                current_ = from_.interpolatedWith(to_, progress);
                                if (apply_)
                                    apply_(current_);
                                else
                                    owner_.repaint();
                            };
                        })
                    .build())
{
}

ColourFade::~ColourFade()
{
    *alive_ = false;
    if (registered_)
        Motion::updater().removeAnimator(animator_);
}

void ColourFade::setTarget(juce::Colour target)
{
    if (target == to_ && registered_)
        return;

    to_ = target;
    if (!registered_)
    {
        Motion::updater().addAnimator(animator_);
        registered_ = true;
    }
    animator_.start();
}

// ---------------------------------------------------------------- EntranceFade
EntranceFade::EntranceFade(juce::Component& owner, double durationMs)
    : owner_(owner),
      alive_(std::make_shared<bool>(true)),
      animator_(juce::ValueAnimatorBuilder()
                    .withDurationMs(durationMs)
                    .withEasing(juce::Easings::createEaseOut())
                    .withOnStartReturningValueChangedCallback(
                        [this]
                        {
                            value_ = 0.0f;
                            return [this, alive = alive_](float progress)
                            {
                                if (!*alive)
                                    return;
                                value_ = juce::jlimit(0.0f, 1.0f, progress);
                                owner_.repaint();
                            };
                        })
                    .build())
{
}

EntranceFade::~EntranceFade()
{
    *alive_ = false;
    if (registered_)
        Motion::updater().removeAnimator(animator_);
}

void EntranceFade::play()
{
    if (!registered_)
    {
        Motion::updater().addAnimator(animator_);
        registered_ = true;
    }
    animator_.start();
}

// -------------------------------------------------------------- Look and feel
LingoFlowLookAndFeel::LingoFlowLookAndFeel()
{
    setColour(juce::TextButton::buttonColourId, colour::control);
    setColour(juce::TextButton::buttonOnColourId, colour::blue);
    setColour(juce::TextButton::textColourOffId, colour::textPrimary);
    setColour(juce::TextButton::textColourOnId, colour::textPrimary);

    setColour(juce::Label::textColourId, colour::textPrimary);
    setColour(juce::ComboBox::backgroundColourId, colour::control);
    setColour(juce::ComboBox::outlineColourId, colour::cardBorder);
    setColour(juce::ComboBox::textColourId, colour::textPrimary);
    setColour(juce::ComboBox::arrowColourId, colour::textSecondary);
    setColour(juce::ComboBox::focusedOutlineColourId, colour::focus);

    setColour(juce::Slider::backgroundColourId, colour::card);
    setColour(juce::Slider::trackColourId, colour::blue.withAlpha(0.55f));
    setColour(juce::Slider::thumbColourId, colour::textPrimary);
    setColour(juce::Slider::textBoxTextColourId, colour::textPrimary);
    setColour(juce::Slider::textBoxBackgroundColourId, colour::control);
    setColour(juce::Slider::textBoxOutlineColourId, colour::cardBorder);

    setColour(juce::ToggleButton::textColourId, colour::textPrimary);
    setColour(juce::ToggleButton::tickColourId, colour::green);

    setColour(juce::TextEditor::backgroundColourId, colour::control);
    setColour(juce::TextEditor::textColourId, colour::textPrimary);
    setColour(juce::TextEditor::outlineColourId, colour::cardBorder);
    setColour(juce::TextEditor::focusedOutlineColourId, colour::focus);

    setColour(juce::PopupMenu::backgroundColourId, colour::control);
    setColour(juce::PopupMenu::textColourId, colour::textPrimary);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, colour::blue);
    setColour(juce::PopupMenu::highlightedTextColourId, colour::textPrimary);
}

juce::Font LingoFlowLookAndFeel::getTextButtonFont(juce::TextButton&, int)
{
    return uiFont(14.0f, true);
}

void LingoFlowLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                const juce::Colour& backgroundColour,
                                                bool highlighted, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    if (bounds.isEmpty())
        return;

    auto fill = backgroundColour;
    if (!button.isEnabled())
        fill = fill.withAlpha(0.45f);
    else if (down)
        fill = fill.darker(0.16f);
    else if (highlighted)
        fill = fill.brighter(0.10f);

    g.setColour(fill);
    g.fillRoundedRectangle(bounds, metric::radiusSm);

    const bool focused = button.hasKeyboardFocus(true) && button.isEnabled();
    g.setColour(focused ? colour::focus : colour::cardBorder);
    g.drawRoundedRectangle(bounds, metric::radiusSm, focused ? 1.6f : 1.0f);
}

void LingoFlowLookAndFeel::drawTickBox(juce::Graphics& g, juce::Component& component,
                                       float x, float y, float w, float h,
                                       bool ticked, bool isEnabled,
                                       bool highlighted, bool)
{
    juce::ignoreUnused(component);
    const juce::Rectangle<float> box(x, y, w, h);
    g.setColour(isEnabled ? colour::control : colour::control.withAlpha(0.45f));
    g.fillRoundedRectangle(box, metric::radiusSm);

    g.setColour(highlighted && isEnabled ? colour::focus : colour::cardBorder);
    g.drawRoundedRectangle(box, metric::radiusSm, 1.0f);

    if (ticked)
    {
        juce::Path tick;
        tick.startNewSubPath(box.getX() + w * 0.22f, box.getCentreY());
        tick.lineTo(box.getX() + w * 0.44f, box.getY() + h * 0.72f);
        tick.lineTo(box.getX() + w * 0.80f, box.getY() + h * 0.28f);
        g.setColour(isEnabled ? colour::green : colour::green.withAlpha(0.45f));
        g.strokePath(tick, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }
}

void LingoFlowLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height,
                                        bool isButtonDown, int buttonX, int, int buttonW, int,
                                        juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<int>(0, 0, width, height).toFloat().reduced(0.5f);
    const bool focused = box.hasKeyboardFocus(true);

    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle(bounds, metric::radiusSm);
    g.setColour(focused ? colour::focus
                        : box.findColour(juce::ComboBox::backgroundColourId).isTransparent()
                              ? colour::cardBorder
                              : box.findColour(juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle(bounds, metric::radiusSm, focused ? 1.6f : 1.0f);

    const juce::Rectangle<float> arrow(buttonX + buttonW * 0.25f, height * 0.38f,
                                       buttonW * 0.5f, height * 0.24f);
    juce::Path chevron;
    chevron.startNewSubPath(arrow.getX(), arrow.getY());
    chevron.lineTo(arrow.getCentreX(), arrow.getBottom());
    chevron.lineTo(arrow.getRight(), arrow.getY());
    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.strokePath(chevron, juce::PathStrokeType(isButtonDown ? 2.0f : 1.5f,
                                               juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
}

juce::Font LingoFlowLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return uiFont(14.0f);
}

void LingoFlowLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float, float,
                                            juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, 0, 0, style, slider);
        return;
    }

    const bool horizontal = style == juce::Slider::LinearHorizontal;
    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float track = horizontal ? jmin(6.0f, height * 0.3f)
                                   : jmin(6.0f, width * 0.3f);
    const auto centre = horizontal ? area.getCentreY() : area.getCentreX();
    const auto trackBounds = horizontal
        ? juce::Rectangle<float>(area.getX(), centre - track * 0.5f, area.getWidth(), track)
        : juce::Rectangle<float>(centre - track * 0.5f, area.getY(), track, area.getHeight());

    const float alpha = slider.isEnabled() ? 1.0f : 0.45f;
    g.setColour(slider.findColour(juce::Slider::backgroundColourId).withAlpha(alpha));
    g.fillRoundedRectangle(trackBounds, track * 0.5f);

    const auto filled = horizontal
        ? trackBounds.withRight(jmax(trackBounds.getX(), sliderPos))
        : trackBounds.withTop(jmin(trackBounds.getBottom(), sliderPos));
    g.setColour(slider.findColour(juce::Slider::trackColourId).withAlpha(alpha));
    g.fillRoundedRectangle(filled, track * 0.5f);

    const auto thumbRadius = jmin(9.0f, (horizontal ? height : width) * 0.4f);
    const auto thumbCentre = horizontal
        ? juce::Point<float>(sliderPos, centre)
        : juce::Point<float>(centre, sliderPos);
    const auto thumb = juce::Rectangle<float>(thumbRadius * 2.0f, thumbRadius * 2.0f)
                           .withCentre(thumbCentre);

    g.setColour(slider.findColour(juce::Slider::thumbColourId).withAlpha(alpha));
    g.fillEllipse(thumb);
    g.setColour(colour::cardBorder.withAlpha(alpha));
    g.drawEllipse(thumb.reduced(0.5f), 1.0f);

    if (slider.hasKeyboardFocus(true))
    {
        g.setColour(colour::focus);
        g.drawEllipse(thumb.expanded(2.0f), 1.4f);
    }
}

int LingoFlowLookAndFeel::getTabButtonBestWidth(juce::TabBarButton& button, int tabDepth)
{
    // V4 measures the text tightly, so six Settings tabs ran together
    // ("TranslationSubtitles"). A 16 px gutter gives each label room to breathe.
    return LookAndFeel_V4::getTabButtonBestWidth(button, tabDepth) + metric::lg;
}

void LingoFlowLookAndFeel::drawTabButton(juce::TabBarButton& button, juce::Graphics& g,
                                         bool isMouseOver, bool isMouseDown)
{
    const auto bounds = button.getLocalBounds().toFloat();
    if (bounds.isEmpty())
        return;

    if (button.isFrontTab())
    {
        // The active tab is the same surface as the page below it, with a
        // squared-off bottom edge: it reads as one panel with the content,
        // not as a separate pill floating above a rectangle.
        g.setColour(colour::card);
        g.fillRoundedRectangle(bounds, metric::radiusSm);
        g.fillRect(bounds.withTop(bounds.getCentreY()));
        g.setColour(colour::blue.withAlpha(0.9f));
        g.fillRect(bounds.withTop(bounds.getBottom() - 2.0f));
    }
    else if (isMouseDown || isMouseOver)
    {
        g.setColour(isMouseDown ? colour::controlDown : colour::controlHover.withAlpha(0.65f));
        g.fillRoundedRectangle(bounds.reduced(1.0f), metric::radiusSm);
    }
    // An idle tab carries no chip at all: the strip stays quiet and the only
    // "selected" signal is the active tab joined to the page.

    LookAndFeel_V4::drawTabButtonText(button, g, isMouseOver, isMouseDown);
}

int LingoFlowLookAndFeel::getDefaultScrollbarWidth()
{
    return 10;
}

void LingoFlowLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar,
                                         int x, int y, int width, int height,
                                         bool isScrollbarVertical, int thumbStartPosition,
                                         int thumbSize, bool isMouseOver, bool isMouseDown)
{
    juce::ignoreUnused(scrollbar);
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
    const auto thumb = isScrollbarVertical
        ? juce::Rectangle<float>(bounds.getX() + 2.0f, static_cast<float>(thumbStartPosition),
                                 bounds.getWidth() - 4.0f, static_cast<float>(thumbSize))
        : juce::Rectangle<float>(static_cast<float>(thumbStartPosition), bounds.getY() + 2.0f,
                                 static_cast<float>(thumbSize), bounds.getHeight() - 4.0f);

    if (thumb.getWidth() <= 0.0f || thumb.getHeight() <= 0.0f)
        return;

    g.setColour((isMouseDown ? colour::controlHover
                             : isMouseOver ? colour::control : colour::control.withAlpha(0.65f)));
    g.fillRoundedRectangle(thumb, 3.0f);
}

void LingoFlowLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text,
                                       int width, int height)
{
    const auto bounds = juce::Rectangle<int>(0, 0, width, height).toFloat().reduced(0.5f);
    g.setColour(colour::cardRaised);
    g.fillRoundedRectangle(bounds, metric::radiusSm);
    g.setColour(colour::cardBorder);
    g.drawRoundedRectangle(bounds, metric::radiusSm, 1.0f);

    g.setFont(uiFont(12.0f));
    g.setColour(colour::textPrimary);
    g.drawFittedText(text, bounds.toNearestInt().reduced(metric::sm),
                     juce::Justification::centredLeft, 2);
}

void LingoFlowLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                                    juce::TextEditor& editor)
{
    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(juce::Rectangle<int>(0, 0, width, height).toFloat(), metric::radiusSm);
}

void LingoFlowLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                                 juce::TextEditor& editor)
{
    const bool focused = editor.hasKeyboardFocus(true) && editor.isEnabled();
    g.setColour(focused ? colour::focus : editor.findColour(juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle(juce::Rectangle<int>(0, 0, width, height).toFloat().reduced(0.5f),
                           metric::radiusSm, focused ? 1.6f : 1.0f);
}

} // namespace ui
} // namespace liveai
