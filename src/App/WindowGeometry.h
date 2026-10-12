#pragma once
//
// WindowGeometry - the decision "where does the main window start" (venue task 5).
//
// The saved rectangle answers "how did the operator leave it"; the displays
// answer "what can be shown today". Neither alone is enough, and this file is
// where the two meet: a stored geometry is honoured only while a desktop of
// THIS start can show it, otherwise the window comes back reachable - size
// kept, position moved onto the nearest display.
//
// The portable half, deliberately: the policy is integer arithmetic over a
// plain display list, so it is unit-testable with synthesized multi-monitor
// layouts (a vanished third monitor cannot be reproduced in an automated run
// against real displays, but it can be described as data - tests/unit/
// TestWindowGeometry.cpp). The JUCE shell only converts juce::Displays into
// DisplayRect values and applies the answer to the window before it is first
// shown, so there is no visible re-size jump after the frame appears.
//
// Coordinate space: the application's own logical pixels - exactly what
// Component::getBounds reports and Component::setBounds consumes. The numbers
// mean the same thing inside one process; across launches with a changed
// display scale factor they can shift, which is why the reachability check
// below is a position GUARANTEE, not a preference.

#include <string>
#include <vector>

#include "Config/AppConfig.h"

namespace liveai {

/// The main window's size contract, defined ONCE: OperatorWindow passes these
/// very numbers to setResizeLimits and the default size to setSize, and the
/// restore below clamps a stored rectangle against them - the frame and the
/// policy cannot drift apart (the schema deliberately does not know them:
/// Config stores raw coordinates, "reasonable here" is a displays question).
inline constexpr int kMainWindowMinWidth = 720;
inline constexpr int kMainWindowMinHeight = 540;
inline constexpr int kMainWindowMaxWidth = 4000;
inline constexpr int kMainWindowMaxHeight = 4000;
inline constexpr int kMainWindowDefaultWidth = 980;
inline constexpr int kMainWindowDefaultHeight = 660;

/// The usable area of one connected display in logical screen coordinates
/// (venue box: the desktop area minus the taskbar - a window whose title
/// hides under the taskbar is not "reachable"). The JUCE layer maps
/// juce::Displays::Display::userBounds; tests fabricate the layouts.
struct DisplayRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool isMain = false;   ///< the user's main display, the tie-break target
};

/// The decided rectangle to place the window at before it is first shown,
/// with honest flags: every deviation from the stored numbers is reportable,
/// nothing is quietly "fixed" (the no-swallowed-errors rule - the shell logs
/// describe() for exactly that report).
struct WindowPlacement
{
    int x = 0;
    int y = 0;
    int width = kMainWindowDefaultWidth;
    int height = kMainWindowDefaultHeight;

    /// False: no usable stored geometry at all (never saved, or repaired as
    /// corrupt) - the shell keeps its default size and centres itself, the
    /// pre-task-5 start. x/y carry no meaning then.
    bool fromSavedGeometry = false;

    /// The stored size left the app's resize limits and was raised or lowered
    /// to them; the position is the stored one unless movedOnScreen says more.
    bool sizeClamped = false;

    /// The stored position belongs to no reachable display of this start (the
    /// monitor it sat on is gone, the resolution moved): the rectangle was
    /// shifted onto the nearest display, its top-left inside the usable area.
    bool movedOnScreen = false;

    /// One honest line for the log - what the window will do and why.
    std::string describe() const;
};

/// The task-5 policy. Empty displays is the headless/early-start answer:
/// nothing can be vouched for, so nothing is trusted - the default placement.
WindowPlacement resolveWindowPlacement(const WindowSettings& saved,
                                       const std::vector<DisplayRect>& displays);

} // namespace liveai
