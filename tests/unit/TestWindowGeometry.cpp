#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "App/WindowGeometry.h"

using namespace liveai;

namespace {

DisplayRect display(int x, int y, int width, int height, bool isMain = false)
{
    DisplayRect rect;
    rect.x = x;
    rect.y = y;
    rect.width = width;
    rect.height = height;
    rect.isMain = isMain;
    return rect;
}

WindowSettings stored(int x, int y, int width, int height)
{
    return WindowSettings{ x, y, width, height };
}

/// The venue baseline: one 1920x1040 work area (a 1080 panel minus the task
/// bar) at the origin, main. Every "what happens on this machine" answer in
/// these cases is decided against displays like this - synthesized, because a
/// vanished second monitor is exactly what an automated run cannot unplug.
std::vector<DisplayRect> singleMain()
{
    return { display(0, 0, 1920, 1040, true) };
}

} // namespace

TEST_CASE("WindowGeometry: an unset rectangle gets the application default", "[app][window]")
{
    // First run after the update (or a rectangle repaired as corrupt): width
    // or height zero means "never saved", and the shell keeps its centred
    // default - the pre-task-5 start, unchanged.
    const auto placement = resolveWindowPlacement(stored(0, 0, 0, 0), singleMain());

    CHECK_FALSE(placement.fromSavedGeometry);
    CHECK(placement.width == kMainWindowDefaultWidth);
    CHECK(placement.height == kMainWindowDefaultHeight);

    // A negative size is the same answer: the policy vouches only for
    // rectangles, never for the damage a schema problem already reported.
    CHECK_FALSE(resolveWindowPlacement(stored(100, 100, -5, 600), singleMain()).fromSavedGeometry);
    CHECK_FALSE(resolveWindowPlacement(stored(100, 100, 800, 0), singleMain()).fromSavedGeometry);

    // No displays at all (headless start): nothing can be vouched for, so
    // nothing is trusted, even a well-formed rectangle.
    CHECK_FALSE(resolveWindowPlacement(stored(100, 100, 800, 600), {}).fromSavedGeometry);
}

TEST_CASE("WindowGeometry: a stored rectangle on a visible display comes back unchanged",
          "[app][window]")
{
    const auto placement = resolveWindowPlacement(stored(350, 220, 1280, 800), singleMain());

    CHECK(placement.fromSavedGeometry);
    CHECK(placement.x == 350);
    CHECK(placement.y == 220);
    CHECK(placement.width == 1280);
    CHECK(placement.height == 800);
    CHECK_FALSE(placement.sizeClamped);
    CHECK_FALSE(placement.movedOnScreen);

    // The position may be exactly (0,0) and still be a SAVED one: zero is not
    // a sentinel for the position, only for the size pair.
    const auto atOrigin = resolveWindowPlacement(stored(0, 0, 980, 660), singleMain());
    CHECK(atOrigin.fromSavedGeometry);
    CHECK(atOrigin.x == 0);
    CHECK(atOrigin.y == 0);

    // A deliberate overhang the operator left behind is honoured, not
    // "fixed": the body hangs below the desktop while the title stays.
    const auto overhang = resolveWindowPlacement(stored(100, 700, 900, 600), singleMain());
    CHECK(overhang.fromSavedGeometry);
    CHECK_FALSE(overhang.movedOnScreen);
    CHECK(overhang.x == 100);
    CHECK(overhang.y == 700);
}

TEST_CASE("WindowGeometry: a rectangle on a REMOVED monitor comes back reachable",
          "[app][window][monitors]")
{
    // Yesterday: main plus a second display to its right; the window lived
    // there. The first two asserts are the yesterday case - the rectangle
    // must survive untouched while that display exists.
    const std::vector<DisplayRect> twoDisplays { display(0, 0, 1920, 1040, true),
                                                 display(1920, 0, 1920, 1040) };
    const auto withBoth = resolveWindowPlacement(stored(2100, 300, 1200, 800), twoDisplays);
    CHECK(withBoth.fromSavedGeometry);
    CHECK_FALSE(withBoth.movedOnScreen);
    CHECK(withBoth.x == 2100);

    // Today: the second display is gone. Same size, position pulled fully
    // onto the surviving desktop - the window must open VISIBLE, not on the
    // memory of a monitor nobody plugs in anymore. "Fully" includes the
    // bottom edge: the reconstructed position is the stored one CLAMPED so
    // the whole rectangle is inside the usable area, which is 60 px higher
    // here than where it hung over the old pair.
    const auto afterRemoval = resolveWindowPlacement(stored(2100, 300, 1200, 800), singleMain());
    CHECK(afterRemoval.fromSavedGeometry);
    CHECK(afterRemoval.movedOnScreen);
    CHECK(afterRemoval.width == 1200);          // the operator's size intent survives
    CHECK(afterRemoval.height == 800);
    CHECK(afterRemoval.x == 720);               // clamp(2100, 0, 1920 - 1200): flush right, as near the old spot as this desktop can be
    CHECK(afterRemoval.y == 240);               // clamp(300, 0, 1040 - 800): fully above the taskbar now

    // And the moved rectangle IS title-reachable on the only display: top and
    // left inside the usable area.
    CHECK(afterRemoval.x >= 0);
    CHECK(afterRemoval.y >= 0);

    // A window remembered on a monitor to the LEFT (negative coordinates)
    // comes back flush with the left edge of what survives.
    const std::vector<DisplayRect> leftSecond { display(-1920, 0, 1920, 1040),
                                                display(0, 0, 1920, 1040, true) };
    const auto fromLeft = resolveWindowPlacement(stored(-1700, 120, 1200, 800), leftSecond);
    CHECK(fromLeft.x == -1700);                 // still there: untouched
    CHECK_FALSE(fromLeft.movedOnScreen);

    const auto afterLeftRemoval = resolveWindowPlacement(stored(-1700, 120, 1200, 800), singleMain());
    CHECK(afterLeftRemoval.movedOnScreen);
    CHECK(afterLeftRemoval.x == 0);
    CHECK(afterLeftRemoval.y == 120);
}

TEST_CASE("WindowGeometry: sizes beyond the app contract return AT the contract",
          "[app][window]")
{
    // A 5000 px width may be legal config (a huge desktop once held it) but
    // 4000 is the frame's own maximum: it comes back at the maximum, and the
    // fact is REPORTED, not smoothed over.
    const auto tooWide = resolveWindowPlacement(stored(350, 220, 5000, 600), singleMain());
    CHECK(tooWide.fromSavedGeometry);
    CHECK(tooWide.sizeClamped);
    CHECK(tooWide.width == kMainWindowMaxWidth);
    CHECK(tooWide.height == 600);               // only the offending axis moves

    const auto tooSmall = resolveWindowPlacement(stored(100, 100, 300, 300), singleMain());
    CHECK(tooSmall.width == kMainWindowMinWidth);
    CHECK(tooSmall.height == kMainWindowMinHeight);
    CHECK(tooSmall.sizeClamped);

    // A rectangle wider than any current display keeps the app-legal size and
    // is guaranteed a reachable title: it is left-aligned on the nearest
    // display (it cannot fit) and its top is clamped so the title is visible.
    const std::vector<DisplayRect> smallDisplays { display(0, 0, 1024, 768, true) };
    const auto oversize = resolveWindowPlacement(stored(4000, 5000, 3000, 600), smallDisplays);
    CHECK(oversize.fromSavedGeometry);
    CHECK_FALSE(oversize.sizeClamped);          // 3000x600 is within the app limits
    CHECK(oversize.movedOnScreen);
    CHECK(oversize.x == 0);                     // wider than the display: left-aligned, title at the near edge
    CHECK(oversize.y == 168);                   // clamp(5000, 0, 768 - 600): the whole height fits above the bottom
}

TEST_CASE("WindowGeometry: the nearest display wins, the main display breaks ties",
          "[app][window][monitors]")
{
    // Main left, second right; the window is remembered far right. With both
    // displays alive nothing moves; with the RIGHT one gone the rectangle
    // lands on the survivor. With the LEFT one gone instead, the remembered
    // area is still visible and again nothing moves - the check is
    // reachability, not "always centre on main".
    const std::vector<DisplayRect> mainAndRight { display(0, 0, 1000, 800, true),
                                                  display(1000, 0, 1000, 800) };
    const WindowSettings farRight { 1600, 100, 800, 600 };

    CHECK_FALSE(resolveWindowPlacement(farRight, mainAndRight).movedOnScreen);

    const auto rightGone = resolveWindowPlacement(farRight, { display(0, 0, 1000, 800, true) });
    CHECK(rightGone.movedOnScreen);
    CHECK(rightGone.x == 200);                  // clamp(1600, 0, 1000 - 800)
    CHECK(rightGone.y == 100);

    const auto leftGone = resolveWindowPlacement(farRight, { display(1000, 0, 1000, 800) });
    CHECK_FALSE(leftGone.movedOnScreen);        // the window's own display is still there

    // Exact-distance tie between two displays: the main one is the documented
    // tie-break - an invisible window on "whichever std::min_element saw
    // first" is not an answer. The stored rectangle floats in the gap between
    // two stacked panels with its center exactly midway, so both distances
    // are equal and only isMain can decide; the size is app-legal so no clamp
    // distorts the geometry under test.
    const std::vector<DisplayRect> tie { display(0, 0, 1200, 800, true), display(0, 1600, 1200, 800) };
    const auto tieBreak = resolveWindowPlacement(stored(200, 900, 800, 600), tie);
    CHECK(tieBreak.fromSavedGeometry);
    CHECK(tieBreak.movedOnScreen);
    CHECK(tieBreak.x == 200);
    CHECK(tieBreak.y == 200);                   // onto the MAIN display, the upper one - the other choice lands at 1600
}

TEST_CASE("WindowGeometry: describe() names every deviation", "[app][window]")
{
    // The shell logs this line when the start is NOT the plain stored
    // rectangle: the operator (and the venue report) can see what happened.
    const auto plain = resolveWindowPlacement(stored(350, 220, 1280, 800), singleMain());
    CHECK(plain.describe().find("no usable saved") == std::string::npos);

    const auto fallback = resolveWindowPlacement(stored(0, 0, 0, 0), singleMain());
    CHECK(fallback.describe().find("default placement") != std::string::npos);

    const auto clamped = resolveWindowPlacement(stored(0, 0, 5000, 600), singleMain());
    CHECK(clamped.describe().find("clamped") != std::string::npos);

    const auto moved = resolveWindowPlacement(stored(4000, 4000, 800, 600), singleMain());
    CHECK(moved.describe().find("reachable display") != std::string::npos);
}
