#include "App/WindowGeometry.h"

#include <algorithm>
#include <cstdint>
#include <format>

namespace liveai {
namespace {

int clampInt(int value, int low, int high) noexcept
{
    return std::max(low, std::min(value, high));
}

// What "reachable" means: the operator can grab this window with the mouse.
// The native title bar is the only part Windows offers for dragging, so a
// stored position is honoured when the TOP BAND of the window - title bar
// height with a small margin, logical pixels - is visible on some display by
// at least this strip of width. Everything below the band may hang off the
// desktop: that is the position the operator chose, and the shell is not
// allowed to "fix" a deliberate partial overhang.
constexpr int kTitleBandPx = 48;
constexpr int kMinReachWidthPx = 64;

bool titleReachableOn(const DisplayRect& display, int x, int y, int width) noexcept
{
    const int overlapWidth = std::min(x + width, display.x + display.width) - std::max(x, display.x);

    if (overlapWidth < kMinReachWidthPx)
        return false;

    // The band [y, y + kTitleBandPx) must intersect the display's usable
    // vertical range - a title fully above the top, below the bottom or
    // beside the edges is not grabbable and gets the window repositioned.
    return std::max(y, display.y) < std::min(y + kTitleBandPx, display.y + display.height);
}

/// Squared center-to-center distance in int64: no floats, no overflow at any
/// desktop magnitude the schema lets through.
long long centerDistanceSquared(const DisplayRect& display, int x, int y, int width, int height) noexcept
{
    const long long dx = (static_cast<long long>(display.x) + display.width / 2)
                       - (static_cast<long long>(x) + width / 2);
    const long long dy = (static_cast<long long>(display.y) + display.height / 2)
                       - (static_cast<long long>(y) + height / 2);
    return dx * dx + dy * dy;
}

} // namespace

WindowPlacement resolveWindowPlacement(const WindowSettings& saved,
                                       const std::vector<DisplayRect>& displays)
{
    WindowPlacement placement;   // the default answer: the app's default size

    // Nothing stored (first run, or a rectangle repaired as corrupt), or
    // nothing to check against (headless start): the stored numbers are not
    // vouched for and the shell keeps its own default centred placement.
    if (displays.empty() || !saved.isSet())
        return placement;

    placement.fromSavedGeometry = true;

    // 1. The size contract first: a stored window beyond the app's resize
    //    limits comes back AT the limit, not discarded - the operator's
    //    width or height intent survives as far as the frame can honour it.
    placement.width = clampInt(saved.width, kMainWindowMinWidth, kMainWindowMaxWidth);
    placement.height = clampInt(saved.height, kMainWindowMinHeight, kMainWindowMaxHeight);
    placement.sizeClamped = placement.width != saved.width || placement.height != saved.height;

    // 2. A stored position stays while the title bar is reachable on any
    //    display of THIS start - the same monitor layout is the normal case
    //    and must change nothing.
    for (const auto& display : displays)
    {
        if (titleReachableOn(display, saved.x, saved.y, placement.width))
        {
            placement.x = saved.x;
            placement.y = saved.y;
            return placement;
        }
    }

    // 3. Otherwise the window sat on a display that is gone, or the desktop
    //    moved under it. It comes back at the SAME size, clamped fully onto
    //    the NEAREST display (center-to-center - the one the operator's spot
    //    was closest to; the main display wins only an exact tie), so the
    //    multi-monitor layout is remembered as well as today's desktop can
    //    express it. The top-left lands inside the usable area, which also
    //    guarantees the title bar is visible.
    const auto nearest = std::min_element(displays.begin(), displays.end(),
                                          [&](const DisplayRect& a, const DisplayRect& b)
                                          {
                                              const long long da =
                                                  centerDistanceSquared(a, saved.x, saved.y, placement.width, placement.height);
                                              const long long db =
                                                  centerDistanceSquared(b, saved.x, saved.y, placement.width, placement.height);
                                              if (da != db)
                                                  return da < db;
                                              return a.isMain && !b.isMain;
                                          });

    placement.x = nearest->width >= placement.width
                      ? clampInt(saved.x, nearest->x, nearest->x + nearest->width - placement.width)
                      : nearest->x;
    placement.y = nearest->height >= placement.height
                      ? clampInt(saved.y, nearest->y, nearest->y + nearest->height - placement.height)
                      : nearest->y;
    placement.movedOnScreen = true;
    return placement;
}

std::string WindowPlacement::describe() const
{
    if (!fromSavedGeometry)
        return std::format("default placement {}x{} (no usable saved window geometry)", width, height);

    std::string text = std::format("saved geometry -> {}x{} at ({}, {})", width, height, x, y);

    if (sizeClamped)
        text += ", size clamped to the app limits";
    if (movedOnScreen)
        text += ", position moved onto a reachable display";

    return text;
}

} // namespace liveai
