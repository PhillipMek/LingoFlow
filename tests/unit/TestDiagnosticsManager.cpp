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
