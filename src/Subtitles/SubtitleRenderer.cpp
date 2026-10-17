#include "Subtitles/SubtitleRenderer.h"

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

/// Places a line's outline so its bounding box sits at (x, y) with vertical
/// centering inside lineHeightPx. Returns false on GDI+ refusal.
bool placeLine(FontFamily& family, const std::string& line, float fontPx,
               float x, float y, float lineHeightPx, GraphicsPath& path, std::string& error)
{
    RectF bounds;
    if (!linePathAtOrigin(family, line, fontPx, path, bounds, error))
        return false;
    // Shift the (0,0)-built path to the target, centering the ink box vertically.
    // (The path's own handle is a GpPath: motion goes through a Matrix and the
    // documented GraphicsPath::Transform, not a path-level Translate.)
    Matrix shift;
    shift.Translate(x - bounds.X, y + (lineHeightPx - bounds.Height) / 2.0f - bounds.Y);
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

bool SubtitleRenderer::render(std::string_view captionText, RenderedSubtitleFrame& out, std::string& error)
{
    error.clear();
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
        return true;   // the fully transparent buffer above IS the answer: old text must not linger

    const LineMeasurer measure = [&](std::string_view line, float fontPx) {
        return measureRun(family, line, fontPx);
    };
    out.layout = layoutCaption(settings_.style, width, height, measure, captionText,
                               CaptionLayoutMode::singleLineTail);
    if (out.layout.lines.empty())
        return true;   // whitespace-only caption: transparent like the empty one

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

    const Color textColor(255,
                          static_cast<BYTE>((style.textColorRgb >> 16) & 0xFFu),
                          static_cast<BYTE>((style.textColorRgb >> 8) & 0xFFu),
                          static_cast<BYTE>(style.textColorRgb & 0xFFu));
    const Color outlineColor(255,
                             static_cast<BYTE>((style.outlineColorRgb >> 16) & 0xFFu),
                             static_cast<BYTE>((style.outlineColorRgb >> 8) & 0xFFu),
                             static_cast<BYTE>(style.outlineColorRgb & 0xFFu));
    const Color shadowColor(static_cast<BYTE>((style.shadowOpacityPercent * 255) / 100), 0, 0, 0);

    SolidBrush textBrush(textColor);
    Pen outlinePen(outlineColor, out.layout.outlineWidthPx > 0.0f ? out.layout.outlineWidthPx : 1.0f);
    outlinePen.SetLineJoin(LineJoinRound);

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

        GraphicsPath path;
        if (!placeLine(family, out.layout.lines[i], out.layout.fontPx, lineX, lineY,
                       out.layout.lineHeightPx, path, error))
            return false;

        if (style.shadowEnabled && out.layout.shadowOffsetPx > 0.0f)
        {
            // A separately placed path at the offset (GraphicsPath's copy ctor
            // is protected in the SDK headers; re-deriving the placed outline is
            // a few hundred microseconds on a thread that exists only to paint).
            GraphicsPath shadow;
            if (!placeLine(family, out.layout.lines[i], out.layout.fontPx,
                           lineX + out.layout.shadowOffsetPx, lineY + out.layout.shadowOffsetPx,
                           out.layout.lineHeightPx, shadow, error))
                return false;

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
            g.DrawPath(&outlinePen, &path);

        if (g.FillPath(&textBrush, &path) != Ok)
        {
            error = "GDI+ could not fill the caption glyphs";
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
