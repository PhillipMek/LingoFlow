#pragma once
//
// SubtitleLayout - where the caption's lines go, as a pure computation.
//
// The task splits rendering into "what is on screen where" (this file) and
// "how pixels appear" (SubtitleRaster). The split pays for itself twice:
//   * the layout rules - wrapping, the two-line limit, the fit-shrink, the
//     ellipsis last resort, margins, alignment, proportional scaling - are
//     the interesting logic, and they are tested here without a font engine,
//     a display, or an NDI runtime;
//   * the rasterizer arrives with the only thing the layout cannot know: how
//     wide a real glyph run is. Measurement is INJECTED (a LineMeasurer), so
//     tests drive the same code with a fake font whose widths are arithmetic.
//
// The rules, in the order they act on a caption:
//   1. all pixel-sized style numbers scale with the frame height against the
//      1080-line reference (the same style describes 720p and 4K identically);
//   2. the text wraps greedily at spaces to the block-width limit; a word
//      that alone exceeds the limit is split at UTF-8 code-point boundaries -
//      never inside a multibyte character, never assuming a language's
//      hyphenation - so a long Russian word survives intact as pieces;
//   3. if the wrapped caption needs more than maxLines, the font SHRINKS in
//      steps toward the floor (the task's "не обрезай текст" is served first
//      by making room, not by cutting);
//   4. only if the caption still exceeds maxLines at the floor font is the
//      tail replaced by an ellipsis, measured so the last line cannot leave
//      the block - a caption long enough for that is information the audience
//      is reading elsewhere (the operator screen), and the ellipsized flag
//      says so loudly in diagnostics; the text is never silently cropped;
//   5. the block is positioned by the anchors: the edge margin is a frame
//      percentage, the horizontal placement centers/lefts/rights the block
//      inside the frame's safe area (a fixed percentage inset - subtitles
//      must never touch the very edge of a broadcast frame).
//
// The NDI video rule set (LayoutMode::singleLineTrimHead) is a SEPARATE mode
// the same entry point selects, because burned-in NDI captions must stay
// visually stable no matter how much text has accumulated: ONE row at the
// configured font size. Rules 2-4 above are replaced by a single rule: the
// caption joins its words into one row (any ASCII whitespace becomes a
// single space - no newline can reach the rasterizer), the font NEVER
// adapts to the text (no shrink, ever), and when the row measures wider
// than the block, the OLDEST characters leave its head - the longest
// suffix whose MEASURED width fits is shown, so the newest words on the
// right are always on screen. The cut walks whole displayed clusters: a
// multibyte UTF-8 character is never split, a combining mark never parts
// from its base, ZWJ emoji sequences and regional-indicator flags ride as
// single units. This is a bounded approximation of UAX #29 (no Hangul jamo
// composition, no Indic spacing-mark rules) - recorded honestly in the
// implementation, and every cut it makes is safe. The geometry rules (1
// and 5) stay in force: line height, anchors and alignment do not move.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "Subtitles/SubtitleStyle.h"

namespace liveai {
namespace subtitles {

/// Width of one text run at one font size, in frame pixels. The rasterizer
/// supplies a real font-engine implementation; tests supply arithmetic.
using LineMeasurer = std::function<float(std::string_view line, float fontPx)>;

/// Which rule set lays out the caption. The mode is chosen by the CALLER,
/// never by a global default: the delivered fit-to-block rules stay what an
/// unmarked call does, and the NDI burned-in video renderer asks for the
/// single-line trim explicitly (the operator screen and every other consumer
/// keeps the rules it has always had).
enum class LayoutMode
{
    fitToBlock = 0,      ///< wrap, bounded shrink, ellipsis: the delivered rules 1-5
    singleLineTrimHead   ///< one fixed-size row; characters leave the head to fit
};

/// The outcome of laying out one caption. All values are in frame pixels.
struct LaidOutCaption
{
    std::vector<std::string> lines;   ///< UTF-8, never a partial code point
    float fontPx = 0.0f;              ///< the (possibly shrunk) font size used
    float lineHeightPx = 0.0f;
    float blockX = 0.0f, blockY = 0.0f;       ///< the block's top-left corner
    float blockWidth = 0.0f, blockHeight = 0.0f;
    float maxLineWidth = 0.0f;        ///< the widest line actually laid out
    std::vector<float> lineWidths;    ///< per-line measured widths (same order as lines)

    float outlineWidthPx = 0.0f;      ///< the style's pixel fields, already scaled
    float shadowOffsetPx = 0.0f;
    float backgroundPaddingPx = 0.0f;
    float boxX = 0.0f, boxY = 0.0f, boxWidth = 0.0f, boxHeight = 0.0f; ///< block + padding

    bool shrunk = false;      ///< rule 3 acted (fitToBlock only; the single-line mode never shrinks)
    bool ellipsized = false;  ///< rule 4 acted - the caption did not fully fit (fitToBlock only)
    bool headTrimmed = false; ///< single-line mode only: characters left the head so the row fits
    bool clipped = false;     ///< frame too small for even the floor layout: the block was clamped

    friend constexpr bool operator==(const LaidOutCaption&, const LaidOutCaption&) = default;
};

/// Reference frame height for the style's pixel numbers (1080).
constexpr float kReferenceFrameHeight = 1080.0f;

/// The fixed safe-area inset, as a percentage of the frame height; margins
/// are measured from the safe edge, so no glyph, outline stroke or box can
/// sit in the outer band of the picture.
constexpr float kSafeAreaPercent = 5.0f;

/// The floor of the fit-shrink loop: the font may shrink to 60% of the
/// configured size before the ellipsis fallback is allowed to act.
constexpr float kMinFontScale = 0.6f;

/// Each shrink step is 90% of the previous size (five steps reach the floor).
constexpr float kShrinkStep = 0.9f;

/// Lays out `text` for a frame of width x height under `mode`'s rules. An
/// empty text yields an empty result (zero lines, zero block) - the caller
/// renders the frame's transparent background and nothing else.
LaidOutCaption layoutCaption(const SubtitleStyle& style,
                             int frameWidth,
                             int frameHeight,
                             const LineMeasurer& measure,
                             std::string_view text,
                             LayoutMode mode = LayoutMode::fitToBlock);

} // namespace subtitles
} // namespace liveai
