#pragma once
//
// UiWidgets - the small shared vocabulary of the three LingoFlow windows
// (the UI redesign information architecture). Fonts, section captions, the one
// metric-list renderer the Diagnostics surface uses, the application-wide
// slider wheel policy and the scrolling page the auxiliary windows share.
// Widget helpers only: nothing here knows the application, the engine or the
// config - every value that reaches these helpers was built by App/UiModel,
// which is where the tested formatting lives.
//
// The two older windows keep their private copies of the font helpers for now
// (a style pass will fold them in later); new surfaces use these from the
// first pixel so the pattern has a home.

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace liveai {
namespace ui {

inline juce::Font uiFont(float height = 15.0f)
{
    return juce::Font(juce::FontOptions().withHeight(height));
}

inline juce::Font monoFont(float height = 13.0f)
{
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                       height, juce::Font::plain));
}

/// A section or field caption in the house grey. Registers with the parent.
/// Named distinctly (venue task 4 review): the windows' own anonymous-namespace
/// caption() helpers would otherwise meet this one through ADL as soon as the
/// parent argument is one of ui::'s component types - two vocabularies, one
/// name, no candidates MSVC can pick. If you are here wanting a caption, the
/// local file helper is the house style; this is the shared fallback.
inline juce::Label& fieldCaption(juce::Label& label, const juce::String& text,
                                 juce::Component& parent, float height = 12.0f)
{
    label.setFont(uiFont(height));
    label.setColour(juce::Label::textColourId, juce::Colour(0xff8b949eu));
    label.setText(text, juce::NotificationType::dontSendNotification);
    parent.addAndMakeVisible(label);
    return label;
}

/// A section header: brighter and larger than a field caption - the hierarchy
/// marker the UI redesign introduced (SYSTEM STATUS / AUDIO / TRANSLATION / LIVE HEALTH).
inline juce::Label& sectionHeader(juce::Label& label, const juce::String& text,
                                  juce::Component& parent)
{
    label.setFont(uiFont(13.0f));
    label.setColour(juce::Label::textColourId, juce::Colour(0xffc9d1d9u));
    label.setText(text, juce::NotificationType::dontSendNotification);
    parent.addAndMakeVisible(label);
    return label;
}

/// "label ......... value" rows, monospace, oldest format the operator windows
/// already used - so a row that moved from the main screen to Diagnostics
/// reads identically to the operator's screenshots from before the move.
inline std::string formatMetricRows(
    const std::vector<std::pair<std::string, std::string>>& rows, int labelWidth = 36)
{
    std::string out;
    for (const auto& [name, value] : rows)
    {
        out += std::format("{:<{}}{:>12}\n", name, labelWidth, value);
        if (name.size() > static_cast<std::size_t>(labelWidth))
            out += "\n";   // a long name wraps under itself, never over the column
    }
    return out;
}

/// The ONE mouse-wheel rule for every slider in the application (venue task
/// 4 §2): the wheel is a SCROLL gesture here, never a value gesture. The
/// mechanism is JUCE's own contract, checked against the vendored 9.0.3
/// sources: Slider::mouseWheelMove falls back to Component::mouseWheelMove
/// when the wheel is disabled, and the base class hands the event to the
/// nearest enabled ancestor - so a disabled slider does not swallow the
/// wheel, the page under the cursor scrolls instead of the value drifting.
/// Drag, click, the slider's own text box and keyboard editing stay intact.
/// Every slider is created through its window's factory, which calls this;
/// nothing may re-enable the wheel on a visible slider.
inline void wheelSafeSlider(juce::Slider& slider) noexcept
{
    slider.setScrollWheelEnabled(false);
}

/// A page whose content may be taller than the space the window gave it
/// (venue task 4 §3/§5): the component fills itself with a Viewport and
/// exposes a Body for the caller's widgets. The Body's WIDTH is what the
/// viewport offers (its holder width - minus the vertical bar once one is
/// shown); its HEIGHT is whatever `measure` asked for at that width.
///
/// `measure` runs on every resize and on every explicit relayout() (content
/// whose height depends on data calls one), and it positions the Body's
/// widgets while it measures: one pass does both, so a page cannot drift
/// between the height it reported and the widgets it placed. The callback
/// must be monotone - a narrower width may need more height, never less.
/// With that guarantee, at most one re-measure is needed when a scrollbar
/// appears or vanishes (the width change it causes), and this class caps
/// the correction at exactly that second pass - no oscillation loop.
class ScrollablePage final : public juce::Component
{
public:
    /// The scrolled surface: a plain component that hosts widgets and may
    /// paint. Paint is optional - a page without painting shows the parent's
    /// background through it.
    class Body : public juce::Component
    {
    public:
        void paint(juce::Graphics& g) override { paintContent(g, getLocalBounds()); }

        /// Bounds are body-local and equal to getLocalBounds(); passed in so
        /// subclasses never re-read the component inside their own paint.
        virtual void paintContent(juce::Graphics&, const juce::Rectangle<int>&) {}
    };

    using Measure = std::function<int(int /*width*/)>;

    explicit ScrollablePage(Measure measure)
        : ScrollablePage(std::make_unique<Body>(), std::move(measure))
    {
    }

    ScrollablePage(std::unique_ptr<Body> body, Measure measure)
        : measure_(std::move(measure))
    {
        viewport_ = std::make_unique<juce::Viewport>("uiScrollPage");
        viewport_->setScrollBarsShown(true, false);          // vertical only - width always fits
        body_ = body.get();
        viewport_->setViewedComponent(body.release(), true);  // the viewport deletes it
        addAndMakeVisible(*viewport_);
    }

    Body& body() noexcept { return *body_; }
    const Body& body() const noexcept { return *body_; }

    /// Escape hatch to the scrolling machinery - the sticky-bottom contract
    /// is asserted through it in tests, and a future frame that wants to
    /// reach a widget below the fold (autoscroll on selection, a "back to
    /// live" affordance) uses the same door instead of subclassing twice.
    juce::Viewport& viewport() noexcept { return *viewport_; }

    /// The growing-list rule (Diagnostics, venue task 4 §5): follow new
    /// content ONLY while the view already sits at the bottom. A reader who
    /// scrolled up into history stays exactly where they left it when the
    /// next event arrives; a reader watching the live tail never has to.
    /// The FIRST measurement is exempt: a fresh page opens at its headline
    /// (the top), and following applies to the growth after that.
    void setFollowsNewContent(bool follow) noexcept { followBottom_ = follow; }

    /// The bottom edge of the scroll, read from the VIEW POSITION - the one
    /// synchronous truth of JUCE's Viewport. (The ScrollBar's own range is an
    /// async projection of it: setCurrentRangeStart only queues a listener
    /// notification, and the next updateVisibleArea recomputes the bar from
    /// the content's position - a check against the bar can race a resize by
    /// one message-loop pass.)
    bool isAtBottom() const noexcept
    {
        return viewport_->getViewPosition().y + viewport_->getMaximumVisibleHeight()
               >= body_->getHeight() - 1;
    }

    /// Pin the view to the newest content (the follow step of the rule).
    /// Synchronous through setViewPosition, for the same reason: a follow
    /// that lands one loop-pass late can be overwritten by a resize in
    /// between - a position that lands now cannot.
    void scrollToBottom()
    {
        viewport_->setViewPosition(viewport_->getViewPosition().x,
                                   jmax(0, body_->getHeight()
                                             - viewport_->getMaximumVisibleHeight()));
    }

    void relayout()
    {
        viewport_->setBounds(getLocalBounds());

        // The first measurement never follows: a freshly opened page shows
        // its HEADLINE (the top), not the tail of a log that grew into it.
        // Following starts with the second growth and only from the bottom.
        const bool wasAtBottom = measured_ && isAtBottom();

        const int firstWidth = viewport_->getMaximumVisibleWidth();
        body_->setSize(firstWidth, measure_(firstWidth));

        // If the bar just appeared (or vanished) with the new height, the
        // holder width changed under us - re-measure once for that width.
        // A monotone measure converges here: a narrower width never needs
        // less height, so this second pass cannot toggle the bar back.
        const int secondWidth = viewport_->getMaximumVisibleWidth();
        if (secondWidth != firstWidth)
            body_->setSize(secondWidth, measure_(secondWidth));

        if (followBottom_ && wasAtBottom)
            scrollToBottom();

        measured_ = true;
    }

    void resized() override { relayout(); }

private:
    std::unique_ptr<juce::Viewport> viewport_;
    Body* body_ = nullptr;   ///< owned by the viewport
    Measure measure_;
    bool followBottom_ = false;
    bool measured_ = false;  ///< has relayout() ever sized the body? (first pass never follows)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScrollablePage)
};

} // namespace ui
} // namespace liveai
