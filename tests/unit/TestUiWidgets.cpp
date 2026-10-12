// Tests for the shared UI widgets (venue task 4): the application-wide slider
// wheel policy and the ui::ScrollablePage contract (measurement, width
// handling, the follow-at-bottom scrolling rule) plus the modal mechanism the
// auxiliary windows are built on. These are the widgets' own promises - what
// the windows DO with them (which sliders exist, which pages scroll) is the
// layout files' business and the venue's manual checks.
//
// Why these tests are a separate executable: they initialise the JUCE GUI
// library (the fixture below), while the portable core's test binary links no
// JUCE at all - the same isolation reason the realtime gate has its own
// process. The fixture makes the Catch2 thread the message thread, which is
// exactly how plugin hosts initialise juce_gui_basics without a
// JUCEApplication, so no widget ever meets a peer here and no window appears.

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "App/UiWidgets.h"

namespace ui = liveai::ui;

namespace {

struct GuiScope
{
    juce::ScopedJuceInitialiser_GUI initialiser;
};

} // namespace

TEST_CASE_METHOD(GuiScope, "the wheel rule: sliders ignore it, the gesture survives for the page",
                 "[ui][slider]")
{
    juce::Slider slider;

    // JUCE's default IS wheel-editing - the application rule is what turns it
    // off, so this first line pins the thing being overridden.
    CHECK(slider.isScrollWheelEnabled());

    ui::wheelSafeSlider(slider);
    CHECK_FALSE(slider.isScrollWheelEnabled());

    // The other half of the contract lives in the JUCE sources (9.0.3,
    // Slider::mouseWheelMove -> Component::mouseWheelMove -> nearest enabled
    // ancestor): a disabled slider does NOT swallow the wheel, it hands the
    // event up, so the viewport under the cursor scrolls. Drag, click, the
    // text box and keyboard of the Slider are untouched by this flag - and
    // the range setter below proves the value channel stays alive.
    slider.setRange(0.0, 10.0, 1.0);
    slider.setValue(4.0, juce::NotificationType::dontSendNotification);
    CHECK(slider.getValue() == 4.0);
}

TEST_CASE_METHOD(GuiScope, "ScrollablePage: the body gets the measured height at the offered width",
                 "[ui][scroll]")
{
    int calls = 0;
    int seenWidth = -1;
    ui::ScrollablePage page([&](int width)
    {
        ++calls;
        seenWidth = width;
        return 400;   // taller than the window: the vertical bar will be needed
    });

    page.setBounds({ 0, 0, 300, 100 });

    CHECK(calls >= 1);
    CHECK(page.body().getHeight() == 400);
    CHECK(seenWidth == page.body().getWidth());   // measured at exactly the width it placed for
    CHECK(seenWidth <= 300);
    CHECK(seenWidth > 300 - 20);                  // the bar costs a few pixels, nothing more
}

TEST_CASE_METHOD(GuiScope, "ScrollablePage: window resize re-measures at the new width",
                 "[ui][scroll]")
{
    int lastWidth = -1;
    ui::ScrollablePage page([&](int width)
    {
        lastWidth = width;
        return 100;   // shorter than any viewport here: never a bar
    });

    page.setBounds({ 0, 0, 300, 200 });
    CHECK(lastWidth == 300);
    CHECK(page.body().getWidth() == 300);
    CHECK(page.body().getHeight() == 100);

    page.setBounds({ 0, 0, 240, 200 });
    CHECK(lastWidth == 240);
    CHECK(page.body().getWidth() == 240);
    CHECK(page.body().getHeight() == 100);
}

// Test names keep to plain ASCII: ctest hands the name to the exe as a
// command-line argument, and a section sign survives that round trip in
// nobody's codepage ("No test cases matched"). The tags carry the grouping.
TEST_CASE_METHOD(GuiScope, "growing content is followed only from the bottom (task 4.5)",
                 "[ui][scroll][sticky]")
{
    int contentHeight = 400;
    ui::ScrollablePage page([&](int) { return contentHeight; });
    page.setFollowsNewContent(true);
    page.setBounds({ 0, 0, 300, 100 });

    auto& view = page.viewport();

    // First open: a page taller than the window shows its HEADLINE - the top
    // - not the tail of a log that grew into it. The first measurement never
    // follows (the venue rule reads history from the top; live tailing starts
    // once the reader is at the bottom).
    CHECK_FALSE(page.isAtBottom());
    CHECK(view.getViewPosition().y == 0);

    // The reader goes to the live tail; a new event grows the list: follow.
    page.scrollToBottom();
    CHECK(page.isAtBottom());
    contentHeight = 500;
    page.relayout();
    CHECK(page.isAtBottom());

    // The reader scrolls up into history - through the viewport's own
    // position, the synchronous truth (the ScrollBar's range is an async
    // projection of it; writing the bar here would only queue a move that the
    // next updateVisibleArea could recompute away before it lands).
    const int parkedY = jmax(0, page.body().getHeight() - view.getMaximumVisibleHeight() - 200);
    view.setViewPosition({ view.getViewPosition().x, parkedY });
    CHECK(view.getViewPosition().y == parkedY);

    // The next event must NOT move them.
    contentHeight = 600;
    page.relayout();
    CHECK_FALSE(page.isAtBottom());
    CHECK(view.getViewPosition().y == parkedY);   // exactly where they left it

    // Back at the bottom, following resumes.
    page.scrollToBottom();
    CHECK(page.isAtBottom());
    contentHeight = 700;
    page.relayout();
    CHECK(page.isAtBottom());
}

TEST_CASE_METHOD(GuiScope, "a modal component blocks its siblings until it is dismissed (task 4.4)",
                 "[ui][modal]")
{
    juce::Component main;      // stands in for the operator window
    juce::Component dialog;    // stands in for Settings/Diagnostics

    int dismissed = 0;
    dialog.enterModalState(true,
                           juce::ModalCallbackFunction::create(
                               [&](int) { ++dismissed; }),
                           false);

    CHECK(dialog.isCurrentlyModal());
    CHECK(main.isCurrentlyBlockedByAnotherModalComponent());   // input to the main window: blocked
    CHECK_FALSE(dialog.isCurrentlyBlockedByAnotherModalComponent());

    // Dismissal is JUCE-async: the manager queues an update and the callback
    // lands on the next pass of the message loop - in the running application
    // that loop never stops, so the operator's window is back one frame later.
    // Here we pump that frame. exitModalState is what the close paths call; the
    // hide itself is ALSO a documented auto-cancel for a peer-backed window, but
    // a bare juce::Component has no peer and isShowing() never turns true - a
    // headless test cannot observe the watcher the real DocumentWindow triggers
    // (this exact probe failed first and taught us that). The venue close-button
    // check therefore stays HUMAN; this case covers the same cancel, callback
    // and un-blocking machinery the frames depend on. No nested event loop
    // exists anywhere in the mechanism: the message thread keeps dispatching
    // while the dialog is up, which is why a Stop or a quit cannot deadlock on it.
    dialog.exitModalState();
    for (int i = 0; i < 30 && dismissed == 0; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);

    CHECK(dismissed == 1);
    CHECK_FALSE(dialog.isCurrentlyModal());
    CHECK_FALSE(main.isCurrentlyBlockedByAnotherModalComponent());

    // Reopen must be able to take modality again (the frames do exactly this).
    dialog.enterModalState(true, nullptr, false);
    CHECK(main.isCurrentlyBlockedByAnotherModalComponent());
    dialog.exitModalState();   // clean slate for the next case
    for (int i = 0; i < 30; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
}

TEST_CASE_METHOD(GuiScope, "destroying a modal component unblocks the application",
                 "[ui][modal]")
{
    juce::Component main;
    {
        juce::Component dialog;
        dialog.enterModalState(true, nullptr, false);
        CHECK(main.isCurrentlyBlockedByAnotherModalComponent());
        // Destruction while modal is the shutdown path: closing the application
        // with a dialog open tears the window tree down and the state unwinds -
        // this cannot hang the quit (there is no nested loop to be stuck in).
    }
    for (int i = 0; i < 30; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    CHECK_FALSE(main.isCurrentlyBlockedByAnotherModalComponent());
}
