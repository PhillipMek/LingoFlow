#pragma once
//
// SubtitleStyle - the appearance and video settings of burned-in subtitles,
// as pure data.
//
// Why this lives in its own module: the SAME settings are read by the config
// schema (persistence + validation), drawn by the rasterizer (Subtitles),
// framed by the NDI transport (NDI), edited in the settings UI (App) and
// reported in diagnostics. Duplicating the struct in each of them is how a
// release train ships four definitions of "the operator's font size"; one
// definition here keeps them literally the same object. The module boundary
// audit registers Subtitles as its own node: it depends on nothing but the
// standard library, so anyone may depend on it, and the audio path never does.
//
// Coordinate system: every *Px field is specified against the reference frame
// of 1080 lines (the task's defaults are 1920x1080 numbers) and scales
// proportionally with the frame height at render time - so the same style
// settings describe 720p and 4K output identically. Percentage fields are
// already resolution-independent. Screen DPI never touches these numbers: the
// video buffer's pixels are the frame; the GUI preview is scaled by JUCE.
//
// Colors are stored as 0xRRGGBB integers, in the canonical "#RRGGBB" form the
// config file uses. Alpha is not part of the appearance colors: text, outline
// and box opacities are their own settings (a "#AARRGGBB" color would let one
// field smuggle two meanings, and the schema could not say which one changed).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace liveai {
namespace subtitles {

enum class OutputMode
{
    video = 0,    ///< burned-in subtitles as real NDI video (the OBS-facing default)
    metadata,     ///< TTML1 timed-text metadata frames (the delivered Mode A, kept)
    both          ///< one sender carrying video frames AND metadata frames
};

enum class HorizontalAlign
{
    left = 0,
    center,
    right
};

enum class VerticalAnchor
{
    bottom = 0,   ///< the live-subtitle convention: the block sits above the frame's bottom edge
    top
};

std::string_view nameOf(OutputMode mode) noexcept;
std::string_view nameOf(HorizontalAlign align) noexcept;
std::string_view nameOf(VerticalAnchor anchor) noexcept;

/// The config/UI parse side of the same names. Unknown spellings are a schema
/// problem, not a silent default: the callers report them.
std::optional<OutputMode> outputModeFromName(std::string_view name) noexcept;
std::optional<HorizontalAlign> horizontalAlignFromName(std::string_view name) noexcept;
std::optional<VerticalAnchor> verticalAnchorFromName(std::string_view name) noexcept;

/// The look of one caption. Defaults are the task's numbers for 1920x1080:
/// Segoe UI Semibold 48 px, white text, black 2 px outline, no shadow,
/// centered, 8% bottom margin, 85% block width, 2 lines, no background box.
/// Segoe UI is the deliberate choice: Microsoft ships it with Cyrillic, Latin,
/// Greek and Hebrew coverage, and the rasterizer falls back through it when an
/// operator types a family the machine does not have.
struct SubtitleStyle
{
    std::string fontFamily = "Segoe UI Semibold";
    int fontSizePx = 48;              ///< reference: 1920x1080
    std::uint32_t textColorRgb = 0xFFFFFF;
    std::uint32_t outlineColorRgb = 0x000000;
    int outlineWidthPx = 2;           ///< 0 draws glyphs bare (a choice, not a failure)
    bool shadowEnabled = false;       ///< optional by the task's design; off is the default
    int shadowOffsetPx = 3;
    int shadowOpacityPercent = 50;
    HorizontalAlign horizontalAlign = HorizontalAlign::center;
    VerticalAnchor verticalAnchor = VerticalAnchor::bottom;
    int edgeMarginPercent = 8;        ///< the task's "8% of the frame height from the bottom"
    int blockWidthPercent = 85;       ///< the text block never reads wider than this
    int maxLines = 2;
    int lineSpacingPercent = 25;      ///< leading between baselines, on top of the font height
    bool backgroundEnabled = false;
    int backgroundOpacityPercent = 50;///< the box is black at this alpha when enabled
    int backgroundPaddingPx = 12;

    friend constexpr bool operator==(const SubtitleStyle&, const SubtitleStyle&) = default;
};

/// Video geometry of the NDI stream. Widths must be even (the NDI frame-type
/// documentation: "image width values should be divisible by two" - recorded
/// with its access date in docs/ndi-protocol.md); the schema enforces it.
struct SubtitleVideoGeometry
{
    int width = 1920;
    int height = 1080;
    int fps = 25;   ///< whole-number rates by design (frame_rate_N=fps, frame_rate_D=1);
                    ///< 29.97-style fractions are not invented here

    friend constexpr bool operator==(const SubtitleVideoGeometry&, const SubtitleVideoGeometry&) = default;
};

/// Everything the subtitle transport needs to know beyond the stream name:
/// which media to emit, at what geometry, and how a caption looks.
struct SubtitleSettings
{
    OutputMode mode = OutputMode::video;
    SubtitleVideoGeometry video;
    SubtitleStyle style;

    friend constexpr bool operator==(const SubtitleSettings&, const SubtitleSettings&) = default;
};

} // namespace subtitles
} // namespace liveai
