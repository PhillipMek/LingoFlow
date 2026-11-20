#pragma once
//
// Theme - the single vocabulary of visual tokens and the one LookAndFeel of all
// three LingoFlow windows (the "style pass" the older window headers promised).
//
// Before this file each window carried its own palette (ink:: / sink:: / dink::)
// and its own copies of the font and control helpers. Colour means state and
// nothing else; that rule is now expressed ONCE here, so the operator screen,
// Settings and Diagnostics cannot drift apart. The values are the house values
// the windows already used - this is a consolidation, not a new colour scheme.
//
// Everything in this header is presentation only: it never sees the controller,
// a config value, an audio buffer or a credential. Every drawing call happens
// on the message thread; nothing here is reachable from an audio callback.
//
// juce_animation is used for short (<= 180 ms) decorative fades only. The
// shared updater is message-thread owned and never touches audio state.

#include <JuceHeader.h>

#include <functional>
#include <memory>

namespace liveai {
namespace ui {

// ------------------------------------------------------------------ typography
inline constexpr float kHintFontSize = 12.0f;

inline juce::Font uiFont(float height = 15.0f, bool bold = false)
{
    juce::Font font { juce::FontOptions().withHeight(height) };
    if (bold)
        font.setBold(true);
    return font;
}

inline juce::Font monoFont(float height = 13.0f)
{
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                       height, juce::Font::plain));
}

// --------------------------------------------------------------- colour tokens
/// One palette for the whole application. The names say WHAT a colour is for,
/// never which window owns it - so a token can be retuned in one place.
namespace colour {

inline const juce::Colour background    { 0xff1a1d20u };
inline const juce::Colour card          { 0xff22262bu };
inline const juce::Colour cardRaised    { 0xff272c32u };
inline const juce::Colour cardBorder    { 0xff2d333au };
inline const juce::Colour control       { 0xff30363du };
inline const juce::Colour controlHover  { 0xff3a424bu };
inline const juce::Colour controlDown   { 0xff262b31u };

inline const juce::Colour textPrimary   { 0xffe6edf3u };
inline const juce::Colour textSecondary { 0xff9aa4afu };
inline const juce::Colour textMuted     { 0xff8b949eu };
inline const juce::Colour header        { 0xffc9d1d9u };

/// State colours appear only on dots, words and the buttons that mean them.
inline const juce::Colour green { 0xff3fb950u };
inline const juce::Colour amber { 0xffd29922u };
inline const juce::Colour red   { 0xfff85149u };
inline const juce::Colour blue  { 0xff2f81f7u };
inline const juce::Colour focus { 0xff58a6ffu };

/// A hairline highlight at the top of a raised surface: the whole "elevation"
/// treatment stays flat and cheap - one extra rect, no shadows or blurs.
inline const juce::Colour highlight { 0xffffffffu };

} // namespace colour

// --------------------------------------------------------------- metric tokens
/// The 4 / 8 / 12 / 16 / 24 / 32 spacing scale, plus the radii and control
/// heights the windows share. Fixed numbers in layout code refer to these.
namespace metric {

inline constexpr int xs   = 4;
inline constexpr int sm   = 8;
inline constexpr int md   = 12;
inline constexpr int lg   = 16;
inline constexpr int xl   = 24;
inline constexpr int xxl  = 32;

inline constexpr float radiusSm   = 4.0f;
inline constexpr float radiusMd   = 7.0f;
inline constexpr float radiusLg   = 10.0f;

inline constexpr int controlH  = 30;
inline constexpr int commandH  = 46;

/// Decorative motion only. 120 ms for a colour change, 180 ms for a card
/// entrance - both far below the operator's reaction time and independent of
/// the 100 ms data refresh clock.
inline constexpr double motionFast     = 120.0;
inline constexpr double motionStandard = 180.0;

} // namespace metric

// ----------------------------------------------------------------------- icons
/// Small vector glyphs. They are declared as SVG path data and parsed once into
/// a juce::Path; the renderer is vector, so they stay sharp at any DPI scaling
/// on a venue monitor. No bitmap assets and no icon-font dependency.
enum class Icon
{
    settings,
    diagnostics,
    refresh,
    play,
    stop,
    mute,
    audio,
    translation,
    health,
    status,
};

juce::Path iconPath(Icon which);
void drawIcon(juce::Graphics& g, Icon which, juce::Rectangle<float> bounds,
              const juce::Colour& tint, float strokeWidth = 1.6f);

// ------------------------------------------------------------------ card paint
/// The shared card surface: rounded corners, a one-pixel border and a quiet
/// top highlight. `alpha` is used by the entrance fade; it never affects data.
void drawCard(juce::Graphics& g, juce::Rectangle<int> bounds, float alpha = 1.0f);

/// A thin rule under a card's section title, inset to the card padding so the
/// header reads as part of the card instead of floating above the rows.
void drawCardDivider(juce::Graphics& g, juce::Rectangle<int> bounds);

// ------------------------------------------------------------------- animation
/// The one animation clock. A message-thread timer (not a VBlank attachment,
/// which would require a live Component) drives a juce::AnimatorUpdater. It is
/// created on first use and drives only registered decorative animators.
class Motion final
{
public:
    static juce::AnimatorUpdater& updater();

    Motion() = delete;
};

/// Cross-fades a single colour and repaints its owner while it moves. Used for
/// the four state words on the operator screen, which used to snap between
/// green/amber/red. The animator is registered with the shared message-thread
/// clock; the owner's lifetime owns the fade.
class ColourFade final
{
public:
    /// `apply`, when given, receives every interpolated frame. Without it the
    /// faded colour is only exposed through current() and the owner repaints -
    /// useful for a component that reads current() inside its own paint().
    ColourFade(juce::Component& owner, juce::Colour initial,
               std::function<void(juce::Colour)> apply = {},
               double durationMs = metric::motionFast);
    ~ColourFade();

    void setTarget(juce::Colour target);
    juce::Colour current() const noexcept { return current_; }

    ColourFade(const ColourFade&) = delete;
    ColourFade& operator=(const ColourFade&) = delete;

private:
    juce::Component& owner_;
    std::function<void(juce::Colour)> apply_;
    juce::Colour from_;
    juce::Colour current_;
    juce::Colour to_;
    // Declared before the animator: its callbacks capture the shared flag, so
    // it must already be alive when the animator is constructed.
    std::shared_ptr<bool> alive_;
    juce::Animator animator_;
    bool registered_ = false;
};

/// Drives a 0 -> 1 entrance value for a decorative reveal. Like ColourFade it
/// is owned by the Component it repaints and registered with the shared clock.
class EntranceFade final
{
public:
    explicit EntranceFade(juce::Component& owner,
                          double durationMs = metric::motionStandard);
    ~EntranceFade();

    void play();
    float value() const noexcept { return value_; }

    EntranceFade(const EntranceFade&) = delete;
    EntranceFade& operator=(const EntranceFade&) = delete;

private:
    juce::Component& owner_;
    std::shared_ptr<bool> alive_;
    juce::Animator animator_;
    float value_ = 0.0f;
    bool registered_ = false;
};

// ------------------------------------------------------------------ look & feel
/// The application-wide skin. It deliberately derives from LookAndFeel_V4 so
/// every widget has a sane fallback; only the shapes LingoFlow actually uses
/// are redrawn. Control colours still come from each component's own colour
/// map (the windows set them from the tokens above), so this class can add
/// hover / pressed / focus / disabled treatment without changing values.
class LingoFlowLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LingoFlowLookAndFeel();

    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    void drawTickBox(juce::Graphics&, juce::Component&,
                     float x, float y, float w, float h,
                     bool ticked, bool isEnabled,
                     bool shouldDrawButtonAsHighlighted,
                     bool shouldDrawButtonAsDown) override;

    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;

    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;

    int getTabButtonBestWidth(juce::TabBarButton&, int tabDepth) override;
    void drawTabButton(juce::TabBarButton&, juce::Graphics&,
                       bool isMouseOver, bool isMouseDown) override;

    int getDefaultScrollbarWidth() override;
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                       bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                       bool isMouseOver, bool isMouseDown) override;

    void drawTooltip(juce::Graphics&, const juce::String& text, int width, int height) override;

    void fillTextEditorBackground(juce::Graphics&, int width, int height,
                                  juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height,
                               juce::TextEditor&) override;
};

} // namespace ui
} // namespace liveai
