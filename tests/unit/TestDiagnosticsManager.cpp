#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

#include "Diagnostics/DiagnosticsManager.h"

using liveai::DiagnosticsManager;

TEST_CASE("DiagnosticsManager: counters start at zero", "[diagnostics]")
{
    DiagnosticsManager diagnostics;

    auto snapshot = diagnostics.snapshot();
    CHECK(snapshot.audioBlocks == 0);
    CHECK(snapshot.audioFrames == 0);
    CHECK(snapshot.underruns == 0);
    CHECK(snapshot.audioBackend.empty());
    CHECK(snapshot.lastErrorSubsystem.empty());
}

TEST_CASE("DiagnosticsManager: audio counters accumulate", "[diagnostics]")
{
    DiagnosticsManager diagnostics;

    diagnostics.countAudioBlock(480);
    diagnostics.countAudioBlock(480);
    diagnostics.countAudioBlock(0);      // degenerate block: counted, adds no frames
    diagnostics.countUnderrun();
    diagnostics.countOverrun();

    const auto c = diagnostics.counters();
    CHECK(c.audioBlocks == 3);
    CHECK(c.audioFrames == 960);
    CHECK(c.underruns == 1);
    CHECK(c.overruns == 1);
}

TEST_CASE("DiagnosticsManager: text and transport counters", "[diagnostics]")
{
    DiagnosticsManager diagnostics;

    diagnostics.countPartialTextEvent();
    diagnostics.countPartialTextEvent();
    diagnostics.countFinalTextEvent();
    diagnostics.countReconnect();
    diagnostics.countNdiError();

    const auto s = diagnostics.snapshot();
    CHECK(s.partialTextEvents == 2);
    CHECK(s.finalTextEvents == 1);
    CHECK(s.reconnects == 1);
    CHECK(s.ndiErrors == 1);
}

TEST_CASE("DiagnosticsManager: translation delivery counters keep the three questions apart",
          "[diagnostics][translation]")
{
    // Delivered, rejected (wrong rate or shape), and dropped by our own full
    // buffer are three different facts: an operator reading these
    // must be able to tell whose problem the frames were.
    DiagnosticsManager diagnostics;

    diagnostics.countTranslatedAudioFrames(480);
    diagnostics.countTranslatedAudioFrames(480);
    diagnostics.countRejectedAudioFrames(240);
    diagnostics.countTranslatedAudioDroppedFrames(16);
    diagnostics.countTranslationError(false);
    diagnostics.countTranslationError(true);

    const auto s = diagnostics.snapshot();
    CHECK(s.translatedAudioFrames == 960);
    CHECK(s.rejectedAudioFrames == 240);
    CHECK(s.translatedAudioDroppedFrames == 16);
    CHECK(s.translationErrors == 2);
    CHECK(s.translationFatalErrors == 1);

    diagnostics.resetForTests();

    const auto zero = diagnostics.snapshot();
    CHECK(zero.translatedAudioFrames == 0);
    CHECK(zero.rejectedAudioFrames == 0);
    CHECK(zero.translationErrors == 0);
}

TEST_CASE("DiagnosticsManager: geometry and backend label are recorded", "[diagnostics]")
{
    DiagnosticsManager diagnostics;

    diagnostics.noteAudioGeometry(48000, 480);
    diagnostics.noteAudioBackend("Waves SoundGrid ASIO");

    auto s = diagnostics.snapshot();
    CHECK(s.sampleRate == 48000);
    CHECK(s.bufferFrames == 480);
    CHECK(s.audioBackend == "Waves SoundGrid ASIO");

    diagnostics.noteAudioBackendStopped();
    s = diagnostics.snapshot();
    CHECK(s.audioBackend.empty());
}

TEST_CASE("DiagnosticsManager: error record keeps subsystem and message", "[diagnostics]")
{
    DiagnosticsManager diagnostics;

    diagnostics.noteError("translation", "session closed by peer");

    const auto s = diagnostics.snapshot();
    CHECK(s.lastErrorSubsystem == "translation");
    CHECK(s.lastErrorMessage == "session closed by peer");

    diagnostics.resetForTests();
    CHECK(diagnostics.snapshot().lastErrorSubsystem.empty());
}

TEST_CASE("DiagnosticsManager: concurrent counting from several threads", "[diagnostics][concurrency]")
{
    DiagnosticsManager diagnostics;

    // Two producer threads stand in for the audio thread and a worker thread.
    // Only relaxed atomic counters are touched, which is what the realtime rules
    // allow; snapshot() is read while they run.
    std::vector<std::thread> producers;
    producers.emplace_back([&] {
        for (int i = 0; i < 10000; ++i)
            diagnostics.countAudioBlock(480);
    });
    producers.emplace_back([&] {
        for (int i = 0; i < 10000; ++i)
            diagnostics.countUnderrun();
    });

    const auto mid = diagnostics.snapshot();
    (void)mid;   // must not block the producers or crash

    for (auto& t : producers)
        t.join();

    const auto s = diagnostics.snapshot();
    CHECK(s.audioBlocks == 10000);
    CHECK(s.audioFrames == 10000ULL * 480ULL);
    CHECK(s.underruns == 10000);
}

TEST_CASE("DiagnosticsManager: beginSession scopes the ledger to the new explicit session",
          "[diagnostics][session]")
{
    // Venue task 3 �2: what Start begins, Start also draws a line under. Every
    // per-run number is cleared, the last-error pair is cleared, and the
    // session gets a number and a timestamp. What is PERMANENT survives: the
    // event ring (history across runs), the eviction truth, and the device
    // geometry/backend label, which describe the machine, not the show.
    DiagnosticsManager diagnostics;

    for (int i = 0; i < 40; ++i)
        diagnostics.countAudioBlock(480);
    diagnostics.countUnderrun();
    diagnostics.countReconnect();
    diagnostics.countLateAudioFrames(1234);
    diagnostics.countTranslationError(false);
    diagnostics.countPartialTextEvent();
    diagnostics.noteAudioGeometry(48000, 480);
    diagnostics.noteAudioBackend("Waves SoundGrid ASIO");
    diagnostics.noteError("translation", "socket died mid-show");
    diagnostics.noteEvent("app", "the show itself begins");

    const auto before = diagnostics.snapshot();
    REQUIRE(before.audioBlocks == 40);
    REQUIRE(before.lateAudioFrames == 1234);

    diagnostics.beginSession();

    const auto after = diagnostics.snapshot();
    CHECK(after.audioBlocks == 0);
    CHECK(after.audioFrames == 0);
    CHECK(after.underruns == 0);
    CHECK(after.reconnects == 0);
    CHECK(after.lateAudioFrames == 0);
    CHECK(after.translationErrors == 0);
    CHECK(after.partialTextEvents == 0);
    CHECK(after.lastErrorSubsystem.empty());        // the old error must stop haunting
    CHECK(after.lastErrorMessage.empty());          // the ring still remembers it below

    // Permanent side, intact:
    CHECK(after.sampleRate == 48000);               // the machine's facts survive
    CHECK(after.audioBackend == "Waves SoundGrid ASIO");
    const auto events = diagnostics.events();
    REQUIRE(events.size() >= 3);                    // app event + error event + boundary
    bool sawOldHistory = false;
    bool sawBoundary = false;
    for (const auto& event : events)
    {
        if (event.message == "the show itself begins")
            sawOldHistory = true;
        if (event.subsystem == "diagnostics" && event.message.find("session 1 started") != std::string::npos)
            sawBoundary = true;
    }
    CHECK(sawOldHistory);
    CHECK(sawBoundary);

    // Session scope made visible:
    CHECK(after.sessionGeneration == 1);
    CHECK_FALSE(after.sessionStarted.empty());
}

TEST_CASE("DiagnosticsManager: the boundary event archives the previous session's headline numbers",
          "[diagnostics][session]")
{
    // The numbers the post-run questions need - "was the audio sick, did the
    // provider drop us, did translation fail, did old audio leak" - are written
    // into the history BEFORE they are zeroed, so an export taken three runs
    // later can still answer them for run one.
    DiagnosticsManager diagnostics;

    diagnostics.countUnderrun();
    diagnostics.countUnderrun();
    diagnostics.countUnderrun();
    diagnostics.countReconnect();
    diagnostics.countReconnect();
    diagnostics.countTranslationError(true);
    diagnostics.countLateAudioFrames(7);

    diagnostics.beginSession();

    const auto events = diagnostics.events();
    REQUIRE_FALSE(events.empty());
    const auto& boundary = events.back();
    CHECK(boundary.subsystem == "diagnostics");
    CHECK(boundary.message.find("session 1 started") != std::string::npos);
    CHECK(boundary.message.find("3 underruns") != std::string::npos);
    CHECK(boundary.message.find("2 reconnects") != std::string::npos);
    CHECK(boundary.message.find("1 translation errors") != std::string::npos);
    CHECK(boundary.message.find("7 late audio frames") != std::string::npos);

    // And the next boundary describes the NEXT archive, not the first one.
    diagnostics.beginSession();
    const auto afterSecond = diagnostics.events();
    CHECK(afterSecond.back().message.find("session 2 started") != std::string::npos);
    CHECK(afterSecond.back().message.find("0 underruns") != std::string::npos);
    CHECK(diagnostics.sessionGeneration() == 2);
}

TEST_CASE("DiagnosticsManager: beginSession is called where the lifecycle has no old writers",
          "[diagnostics][session][concurrency]")
{
    // The late-event rule of task 3 is guaranteed by CONSTRUCTION (the Stop's
    // abandoning close joins every session thread before the next Start runs
    // this method), and this test pins the mechanical half: a counter
    // incremented after beginSession belongs to the new session by definition -
    // the ledger has exactly one clear point, and the generation says which
    // session's numbers the snapshot now holds.
    DiagnosticsManager diagnostics;

    diagnostics.countLateAudioFrames(999);          // the previous session's last fact
    diagnostics.beginSession();
    diagnostics.countLateAudioFrames(1);            // a new-session fact

    const auto after = diagnostics.snapshot();
    CHECK(after.lateAudioFrames == 1);              // 999 is history (archived), not this session
    CHECK(after.sessionGeneration == 1);
    const auto events = diagnostics.events();       // the copy first: back() into a temporary
    REQUIRE_FALSE(events.empty());
    const auto& boundary = events.back();
    CHECK(boundary.message.find("999 late audio frames") != std::string::npos);
}
