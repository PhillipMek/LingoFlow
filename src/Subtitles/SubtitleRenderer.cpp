#include "Subtitles/SubtitleRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#define NOMINMAX 1
#include <windows.h>
// Lean-and-mean cuts the COM headers; gdiplus.h then fails on IStream/IImageBytes.
// objidl.h is the documented companion (the SDK's own samples include it the same
// way). GDI+'s C++ wrapper is written to be used with `using namespace Gdiplus;`
// (it is not fully namespace-qualified in the modern scoped-enum sense), so the
// whole file below lives under that using - a deliberate, local convention.
#include <objidl.h>
#include <gdiplus.h>
#endif

namespace liveai {
namespace subtitles {
namespace {

#ifdef _WIN32
using namespace Gdiplus;
#endif

std::mutex rasterMutex;   ///< serializes every GDI+ entry point (see the header's thread model)

#ifdef _WIN32

/// The fade ramp for one word: 0 at its arrival, 1 once kWordFadeInMs have
/// passed, linear between. Pure arithmetic on monotonic-clock durations - the
/// same answer tests and the render thread get from the same (word, time).
float fadeRamp(std::chrono::steady_clock::duration age)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(age).count();
    if (ms <= 0)
        return 0.0f;
    if (ms >= SubtitleRenderer::kWordFadeInMs)
        return 1.0f;
    return static_cast<float>(ms) / static_cast<float>(SubtitleRenderer::kWordFadeInMs);
}

/// GDI+ startup, once per process and never shut down: the same deliberate
/// lifetime choice as the NDI runtime module - tearing a raster library down
/// while a thread might still be inside a paint is a shutdown-crash story,
/// and a held token costs a few bytes.
bool gdiplusStarted()
{
    static const bool started = []
    {
        GdiplusStartupInput input;
        auto* token = new ULONG_PTR(0);
        return GdiplusStartup(token, &input, nullptr) == Ok;
    }();
    return started;
}

std::wstring toWide(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                          nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

/// The fallback chain, in order: the operator's family, then the two Windows
/// families the task's language coverage demands (Segoe UI: Latin, Cyrillic,
/// Greek, Hebrew; Arial: the long-standing universal). The first family the
/// system can open wins, and the caller is told which one it was - a fallback
/// that stays invisible is how "my subtitles are serif now" becomes a mystery.
/// Returns the name actually usable, empty when none opened. The NAME travels,
/// not the handle: GDI+ FontFamily objects own a native pointer and are not
/// copy-assignable, so they never leave the function that made them.
std::string pickFamilyName(const SubtitleStyle& style)
{
    const std::vector<std::string> candidates{ style.fontFamily, "Segoe UI", "Arial" };
    for (const std::string& name : candidates)
    {
        if (name.empty())
            continue;
        const std::wstring wide = toWide(name);
        if (wide.empty())
            continue;
        FontFamily attempt(wide.c_str());
        if (attempt.GetLastStatus() == Ok && attempt.IsStyleAvailable(FontStyleRegular))
            return name;
    }
    return {};
}

/// Builds the outline path for one line at the origin (0,0), returning its
/// tight ink bounds. The single-line PointF-origin overload is used on purpose:
/// the layout already decided the line breaks, and a 0-size layout rectangle
/// with the null format means "no wrapping, no trailing half-advance".
bool linePathAtOrigin(FontFamily& family, const std::string& line, float fontPx,
                      GraphicsPath& path, RectF& bounds, std::string& error)
{
    const std::wstring wide = toWide(line);
    if (wide.empty())
    {
        error = "caption text did not convert to UTF-16 (invalid encoding?)";
        return false;
    }
    path.Reset();
    if (path.AddString(wide.c_str(), static_cast<int>(wide.size()), &family,
                       FontStyleRegular, fontPx, PointF(0.0f, 0.0f), nullptr) != Ok)
    {
        error = "GDI+ could not turn the caption into glyph outlines";
        return false;
    }
    if (path.GetBounds(&bounds) != Ok)
    {
        error = "GDI+ could not measure the caption outline";
        return false;
    }
    return true;
}

/// Ink width of one run at one em size, in frame pixels; 0 on any failure (the
/// degenerate case degrades to "everything fits" at layout, and the paint pass
/// re-checks the same calls for its own errors).
float measureRun(FontFamily& family, std::string_view text, float fontPx)
{
    if (text.empty())
        return 0.0f;
    GraphicsPath path;
    RectF bounds;
    std::string ignored;
    if (!linePathAtOrigin(family, std::string(text), fontPx, path, bounds, ignored))
        return 0.0f;
    return bounds.Width;   // origin is near x=0, so the width IS the ink width
}

/// Places a line's outline so its FONT LAYOUT BOX sits at (x, y) with vertical
/// centering inside lineHeightPx. Returns false on GDI+ refusal.
///
/// The vertical answer is metric, never ink: AddString puts the text at its
/// origin the way DrawString would put it into a layout rectangle - the
/// baseline lies a constant distance (cell ascent + half the internal leading)
/// BELOW the origin, determined by the font and the em size alone. Centering
/// the tight INK bounds instead - the previous behavior - re-decided the row's
/// vertical position every time the caption's letters changed their ascenders
/// or descenders, which is exactly the baseline jitter an arriving word used
/// to cause. Placing the layout box keeps the baseline pinned no matter which
/// glyphs are currently on screen. (The horizontal anchor stays ink-based on
/// purpose: with the left-aligned default, blockX is the fixed safe-area edge,
/// so the row's first ink is welded to one column as words join at the tail.)
bool placeLine(FontFamily& family, const std::string& line, float fontPx,
               float x, float y, float lineHeightPx, GraphicsPath& path, std::string& error)
{
    RectF bounds;
    if (!linePathAtOrigin(family, line, fontPx, path, bounds, error))
        return false;
    // Shift the (0,0)-built path to the target. The x shift anchors the ink's
    // left edge; the y shift places the layout box - whose top IS the origin
    // the path was built at - centered in the line, using the font's own line
    // spacing so the baseline is a per-(font,size) constant.
    const UINT emHeight = family.GetEmHeight(FontStyleRegular);
    const UINT lineSpacing = family.GetLineSpacing(FontStyleRegular);
    const float layoutBoxPx = emHeight > 0
                                  ? fontPx * static_cast<float>(lineSpacing) / static_cast<float>(emHeight)
                                  : 0.0f;
    Matrix shift;
    if (layoutBoxPx > 0.0f)
    {
        shift.Translate(x - bounds.X, y + (lineHeightPx - layoutBoxPx) / 2.0f);
    }
    else
    {
        // Metrics the font refused to give (a degenerate family): fall back to
        // the old ink centering rather than failing the frame - visibly
        // imperfect, never blank.
        shift.Translate(x - bounds.X, y + (lineHeightPx - bounds.Height) / 2.0f - bounds.Y);
    }
    // (The path's own handle is a GpPath: motion goes through a Matrix and the
    // documented GraphicsPath::Transform, not a path-level Translate.)
    return path.Transform(&shift) == Ok;
}

#endif // _WIN32

} // namespace

bool SubtitleRenderer::available()
{
#ifdef _WIN32
    return gdiplusStarted();
#else
    return false;   // there is no GDI+ off Windows; the honest answer, not a stub that pretends
#endif
}

void SubtitleRenderer::setWordFadeEnabled(bool enabled)
{
    wordFadeEnabled_ = enabled;
    if (!enabled)
        resetFade();   // the next enable starts the timeline from that frame on
}

void SubtitleRenderer::resetFade()
{
    animationPending_ = false;
    fadeLine_.clear();
    fadeWords_.clear();
    fadeSeen_.clear();
}

bool SubtitleRenderer::render(std::string_view captionText, RenderedSubtitleFrame& out,
                              std::string& error, std::optional<SteadyTime> atTime,
                              bool lineStart)
{
    error.clear();
    const SteadyTime now = atTime.value_or(std::chrono::steady_clock::now());
    const int width = settings_.video.width;
    const int height = settings_.video.height;

    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
    {
        error = "subtitle video geometry is unusable (" + std::to_string(width) + "x"
                + std::to_string(height) + ")";
        return false;
    }

    out.width = width;
    out.height = height;
    out.bgra.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0u);
    out.familyUsed.clear();
    out.layout = LaidOutCaption{};

#ifdef _WIN32
    std::lock_guard lock(rasterMutex);

    if (!gdiplusStarted())
    {
        error = "GDI+ (the subtitle rasterizer) could not start on this machine";
        return false;
    }

    out.familyUsed = pickFamilyName(settings_.style);
    if (out.familyUsed.empty())
    {
        error = "no usable font family: '" + settings_.style.fontFamily
                + "', 'Segoe UI' and 'Arial' are all unavailable";
        return false;
    }
    const std::wstring familyWide = toWide(out.familyUsed);
    FontFamily family(familyWide.c_str());
    if (family.GetLastStatus() != Ok)
    {
        error = "GDI+ could not open the font family '" + out.familyUsed + "'";
        out.familyUsed.clear();
        return false;
    }

    if (captionText.empty())
    {
        resetFade();   // the sentence ended: the word timeline ends with it
        return true;   // the fully transparent buffer above IS the answer: old text must not linger
    }

    const LineMeasurer measure = [&](std::string_view line, float fontPx) {
        return measureRun(family, line, fontPx);
    };
    // Burned-in NDI captions are laid out in the single-line mode: the
    // configured font size never shrinks, the caption never wraps, and when
    // the row outgrows the block the OLDEST characters leave its left edge
    // (the newest words are what the audience cannot get back). The trim is
    // driven by the same GDI+ measurement the paint below uses, so "fits"
    // is measured truth, not a character estimate.
    out.layout = layoutCaption(settings_.style, width, height, measure, captionText,
                               LayoutMode::singleLineTrimHead);
    if (out.layout.lines.empty())
    {
        resetFade();   // whitespace-only caption: transparent like the empty one, timeline too
        return true;
    }

    // The buffer is fully transparent until drawn on. GDI+ 32bppARGB holds
    // straight (non-premultiplied) alpha, matching fourcc_BGRA - so its drawn
    // pixels are already the wire bytes (docs/ndi-protocol.md; the test suite
    // asserts the alpha convention on real pixels, not on this comment).
    Bitmap bitmap(width, height, width * 4, PixelFormat32bppARGB, out.bgra.data());
    if (bitmap.GetLastStatus() != Ok)
    {
        error = "GDI+ refused the subtitle frame buffer";
        return false;
    }

    Graphics g(&bitmap);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    const SubtitleStyle& style = settings_.style;

    if (style.backgroundEnabled)
    {
        const Color box(static_cast<BYTE>((style.backgroundOpacityPercent * 255) / 100), 0, 0, 0);
        SolidBrush brush(box);
        if (g.FillRectangle(&brush, out.layout.boxX, out.layout.boxY, out.layout.boxWidth,
                            out.layout.boxHeight) != Ok)
        {
            error = "GDI+ could not paint the subtitle background box";
            return false;
        }
    }

    const BYTE shadowBaseAlpha = static_cast<BYTE>((style.shadowOpacityPercent * 255) / 100);
    const float runOutlineWidth = out.layout.outlineWidthPx > 0.0f ? out.layout.outlineWidthPx : 1.0f;

    // Paints one contiguous run of glyphs at (x, y) with every layer's opacity
    // scaled by `factor`. The fade-less render uses it once per line at
    // factor 1.0 and paints exactly what this file painted before the fade
    // existed; the fade path batches the fully-ramped head of the row into one
    // such run and gives every still-animating word its own lower factor.
    auto paintRun = [&](const std::string& run, float x, float y, float factor) -> bool
    {
        if (run.empty() || factor <= 0.0f)
            return true;   // nothing here yet (a word at ramp 0 is not on screen)

        const BYTE alpha = factor >= 1.0f
                               ? static_cast<BYTE>(255)
                               : static_cast<BYTE>(factor * 255.0f + 0.5f);

        GraphicsPath path;
        if (!placeLine(family, run, out.layout.fontPx, x, y, out.layout.lineHeightPx, path, error))
            return false;

        if (style.shadowEnabled && out.layout.shadowOffsetPx > 0.0f)
        {
            // A separately placed path at the offset (GraphicsPath's copy ctor
            // is protected in the SDK headers; re-deriving the placed outline is
            // a few hundred microseconds on a thread that exists only to paint).
            GraphicsPath shadow;
            if (!placeLine(family, run, out.layout.fontPx,
                           x + out.layout.shadowOffsetPx, y + out.layout.shadowOffsetPx,
                           out.layout.lineHeightPx, shadow, error))
                return false;

            const Color shadowColor(static_cast<BYTE>((shadowBaseAlpha * alpha) / 255), 0, 0, 0);
            SolidBrush shadowBrush(shadowColor);
            if (out.layout.outlineWidthPx > 0.0f)
            {
                Pen shadowPen(shadowColor, out.layout.outlineWidthPx);
                shadowPen.SetLineJoin(LineJoinRound);
                g.DrawPath(&shadowPen, &shadow);
            }
            g.FillPath(&shadowBrush, &shadow);
        }

        if (out.layout.outlineWidthPx > 0.0f)
        {
            const Color outlineColor(alpha,
                                     static_cast<BYTE>((style.outlineColorRgb >> 16) & 0xFFu),
                                     static_cast<BYTE>((style.outlineColorRgb >> 8) & 0xFFu),
                                     static_cast<BYTE>(style.outlineColorRgb & 0xFFu));
            Pen outlinePen(outlineColor, runOutlineWidth);
            outlinePen.SetLineJoin(LineJoinRound);
            g.DrawPath(&outlinePen, &path);
        }

        const Color textColor(alpha,
                              static_cast<BYTE>((style.textColorRgb >> 16) & 0xFFu),
                              static_cast<BYTE>((style.textColorRgb >> 8) & 0xFFu),
                              static_cast<BYTE>(style.textColorRgb & 0xFFu));
        SolidBrush textBrush(textColor);
        if (g.FillPath(&textBrush, &path) != Ok)
        {
            error = "GDI+ could not fill the caption glyphs";
            return false;
        }
        return true;
    };

    // ---------------------------------------------------------------- word timeline
    // The fade animates WORDS of the canonical caption row (the same join the
    // single-line layout trims, shared through canonicalCaption, so the row on
    // screen is always a byte-suffix of the timeline's line). Word identity is
    // re-verified against the previous render word by word, and it is
    // STREAM-TOLERANT on purpose: a provider delivers a caption as fragments
    // that split words and carry their own spacing ("Hel" -> "Hello we" ->
    // "Hello world"), so a word whose text merely grew at the tail is the
    // SAME word continuing the ramp it started, not a new one. A word is
    // new - and fades exactly once - when its text is entirely absent,
    // replaced rather than extended, or when the transport says this
    // snapshot opens a new line (lineStart: the caption before it was
    // final). Growing captions stamp only their new tail, a replaced caption
    // is all-new, and a re-render of the same caption moves no stamp - only
    // the clock. The head trim removes nothing here: identities run from
    // the row's head, the screen only shows its suffix.
    animationPending_ = false;
    if (wordFadeEnabled_)
    {
        const std::string canonical = canonicalCaption(captionText);

        std::vector<std::pair<std::uint32_t, std::uint32_t>> words;
        for (std::size_t pos = 0; pos < canonical.size();)
        {
            if (canonical[pos] == ' ')
            {
                ++pos;
                continue;
            }
            const std::size_t start = pos;
            while (pos < canonical.size() && canonical[pos] != ' ')
                ++pos;
            words.emplace_back(static_cast<std::uint32_t>(start),
                               static_cast<std::uint32_t>(pos - start));
        }

        std::size_t keep = 0;
        // lineStart closes the caption before it: nothing continues into the
        // new line, every word of it fades exactly once. The exception is a
        // re-render of the very same snapshot (the line boundary arrived
        // between two paints and the text happens to be identical): on
        // screen nothing changed, so restarting the ramp there would BE the
        // blink this file exists to prevent.
        if (!lineStart || canonical == fadeLine_)
        {
            while (keep < words.size() && keep < fadeWords_.size())
            {
                const std::string_view fresh{canonical.data() + words[keep].first,
                                             words[keep].second};
                const std::string_view previous{fadeLine_.data() + fadeWords_[keep].first,
                                                fadeWords_[keep].second};
                // Same text, or the same text grown at the tail: the word
                // continues. Anything else is a different word at this
                // position and the row is new from here on.
                if (fresh == previous
                    || (fresh.size() > previous.size()
                        && fresh.substr(0, previous.size()) == previous))
                    ++keep;
                else
                    break;
            }
        }

        std::vector<SteadyTime> seen(words.size(), now);   // new/changed words: arriving now
        for (std::size_t i = 0; i < keep; ++i)
            seen[i] = fadeSeen_[i];                        // continuing words keep their history

        fadeLine_ = std::move(canonical);
        fadeWords_ = std::move(words);
        fadeSeen_ = std::move(seen);

        // Arrival times are non-decreasing by word index (a continued word
        // was stamped at an earlier render, a new one now), so the newest
        // word finishing its ramp is exactly the moment the animation is
        // over.
        animationPending_ = !fadeSeen_.empty() && fadeRamp(now - fadeSeen_.back()) < 1.0f;
    }

    for (std::size_t i = 0; i < out.layout.lines.size(); ++i)
    {
        float lineX = out.layout.blockX;
        switch (style.horizontalAlign)
        {
            case HorizontalAlign::left:  lineX = out.layout.blockX; break;
            case HorizontalAlign::right: lineX = out.layout.blockX + out.layout.blockWidth - out.layout.lineWidths[i]; break;
            case HorizontalAlign::center:
                lineX = out.layout.blockX + (out.layout.blockWidth - out.layout.lineWidths[i]) / 2.0f;
                break;
        }
        const float lineY = out.layout.blockY + static_cast<float>(i) * out.layout.lineHeightPx;

        if (!wordFadeEnabled_ || out.layout.lines.size() != 1 || fadeWords_.empty())
        {
            if (!paintRun(out.layout.lines[i], lineX, lineY, 1.0f))
                return false;
            continue;
        }

        // The fade path: this row IS a suffix of the timeline's line (the trim
        // only ever cuts its head; if the suffix property failed, the world
        // broke elsewhere - paint the whole row rather than guess placements).
        const std::string& shown = out.layout.lines[i];
        if (fadeLine_.size() < shown.size()
            || fadeLine_.compare(fadeLine_.size() - shown.size(), shown.size(), shown) != 0)
        {
            if (!paintRun(shown, lineX, lineY, 1.0f))
                return false;
            continue;
        }
        const std::size_t head = fadeLine_.size() - shown.size();

        std::size_t first = 0;   // the first word still (partly) on screen
        while (first < fadeWords_.size()
               && static_cast<std::size_t>(fadeWords_[first].first)
                    + static_cast<std::size_t>(fadeWords_[first].second) <= head)
            ++first;
        std::size_t rampedEnd = first;   // words in [first, rampedEnd) are fully opaque
        while (rampedEnd < fadeWords_.size() && fadeRamp(now - fadeSeen_[rampedEnd]) >= 1.0f)
            ++rampedEnd;

        if (rampedEnd > first)
        {
            // One run for everything that already reached opacity 1: the same
            // string, the same single path, the same pixels the pre-fade
            // renderer painted - settled words are not re-laid-out word by word.
            const std::size_t batchEnd =
                static_cast<std::size_t>(fadeWords_[rampedEnd - 1].first)
              + static_cast<std::size_t>(fadeWords_[rampedEnd - 1].second);
            if (!paintRun(fadeLine_.substr(head, batchEnd - head), lineX, lineY, 1.0f))
                return false;
        }

        for (std::size_t w = rampedEnd; w < fadeWords_.size(); ++w)
        {
            const float factor = fadeRamp(now - fadeSeen_[w]);
            if (factor <= 0.0f)
                continue;   // arriving on the next frames, not on this one

            const std::size_t wordEnd = static_cast<std::size_t>(fadeWords_[w].first)
                                      + static_cast<std::size_t>(fadeWords_[w].second);
            const std::size_t visStart = (std::max)(static_cast<std::size_t>(fadeWords_[w].first), head);
            const std::string word = fadeLine_.substr(visStart, wordEnd - visStart);
            // The word's left edge, measured: the row prefix ending at this
            // word minus the word's own ink is its place IN THE REAL ROW (its
            // spaces and kerning come from the same string the batch draws),
            // so a fading word sits exactly where its opaque self will sit -
            // nothing slides sideways when the ramp completes.
            const std::string prefix = fadeLine_.substr(head, wordEnd - head);
            const float x = lineX + measureRun(family, prefix, out.layout.fontPx)
                                  - measureRun(family, word, out.layout.fontPx);
            if (!paintRun(word, x, lineY, factor))
                return false;
        }
    }

    return true;
#else
    static_cast<void>(captionText);
    static_cast<void>(out);
    error = "the subtitle rasterizer needs Windows (GDI+); this build has no renderer";
    return false;
#endif
}

} // namespace subtitles
} // namespace liveai
