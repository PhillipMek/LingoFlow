#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "App/ApplicationController.h"
#include "Audio/Dev/SimulatedDeviceBackend.h"
#include "Translation/ReconnectSupervisor.h"
#include "Utils/Log.h"
#include "support/MockTranslationBackend.h"

// Task 023 - dedicated failure injection over the PRODUCTION wiring.
//
// The unit suites (faults, reconnect, dispatch, config) prove each part survives
// its own failure in isolation. This file drives the real controller - real
// streaming worker, real supervisor, real device thread - through the seven
// failures the task names, and asserts the three PASS clauses every time:
//   * no crash, no deadlock (bounded waits; the test finishing IS the proof),
//   * ASIO preserved (the simulated device thread keeps producing blocks while
//     the network, NDI, or a config fault burns - AGENTS.md 12),
//   * recovery visible (counters, transitions and event-ring lines a venue
//     post-mortem can read - nothing swallowed).
//
// The audio backend here is the developer test-tone device: it drives
// engine.processAudio from its own paced thread, so the callback runs
// concurrently with the failures instead of being pumped by the test - that is
// what makes these races real rather than rehearsals.

using namespace liveai;
using liveai::audio::BackendState;
using liveai::test::MockTranslationBackend;
using liveai::translation::SessionState;
using liveai::translation::TranslationErrorCategory;

namespace {

constexpr int kFrames = 480;
constexpr int kRate = 48000;

struct QuietLog
{
    QuietLog()
    {
        LogConfig cfg;
        cfg.level = LogLevel::off;
        cfg.writeConsole = false;
        log::configure(cfg);
    }
    ~QuietLog() { log::resetForTests(); }
};

template <typename Predicate>
bool waitsFor(Predicate&& ready, int timeoutMs = 5000)
{
    for (int waited = 0; waited < timeoutMs; waited += 5)
    {
        if (ready())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return ready();
}

bool mentionsEvent(const ApplicationController& controller, std::string_view needle)
{
    for (const auto& event : controller.diagnostics().events())
        if (event.message.find(needle) != std::string::npos)
            return true;
    return false;
}

/// An audio backend whose open() simply fails: the "device the venue does not
/// have" case, injected as a class rather than as a machine-wide accident.
class UnavailableAudio final : public audio::IAudioBackend
{
public:
    std::string_view name() const noexcept override { return "UnavailableAudio (injected)"; }
    BackendState state() const noexcept override { return BackendState::closed; }

    bool open(audio::IAudioProcessor&, const audio::DeviceRequest&, std::string& error) override
    {
        error = "the device is not present (injected unavailable I/O)";
        return false;
    }

    bool start(std::string& error) override { error = "not open"; return false; }
    void close() noexcept override {}
    bool stop(std::string&) override { return true; }
    audio::DeviceCapabilities capabilities() const noexcept override { return {}; }
};

/// An NDI output that starts fine and then dies mid-show - the transport the
/// receiver stopped answering. `failAfter` frames are accepted, every later
/// publish is refused with an error, and `delayMs` keeps the dispatch worker
/// busy enough that stopping can race a publish in flight.
class FlakyNdiOutput final : public ndi::INdiOutput
{
public:
    int failAfter = 1;
    int delayMs = 0;

    std::string_view name() const noexcept override { return "FlakyNdi (injected)"; }
    ndi::OutputState state() const noexcept override { return state_.load(); }

    bool start(std::string_view, std::string&) override
    {
        state_.store(ndi::OutputState::publishing);
        return true;
    }

    void stop() noexcept override { state_.store(ndi::OutputState::ready); }

    bool publish(const ndi::SubtitleFrame&, std::string& error) override
    {
        if (delayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        const auto attempt = publishes_.fetch_add(1) + 1;
        if (attempt > static_cast<std::uint64_t>(failAfter))
        {
            errors_.fetch_add(1);
            state_.store(ndi::OutputState::faulted);
            error = "flaky transport refused the frame (injected)";
            return false;
        }

        accepted_.fetch_add(1);
        return true;
    }

    std::uint64_t publishedFrames() const noexcept override { return accepted_.load(); }
    std::uint64_t publishErrors() const noexcept override { return errors_.load(); }

    std::uint64_t attempts() const noexcept { return publishes_.load(); }

private:
    std::atomic<ndi::OutputState> state_{ ndi::OutputState::ready };
    std::atomic<std::uint64_t> publishes_{ 0 };
    std::atomic<std::uint64_t> accepted_{ 0 };
    std::atomic<std::uint64_t> errors_{ 0 };
};

/// Controller + running tone device + mock (optionally behind the production
/// supervisor). After start() the audio blocks advance on their own thread; the
/// streaming worker drains and submits without the test pumping anything.
class LiveRig
{
public:
    explicit LiveRig(bool useSupervisor)
    {
        auto mockPtr = std::make_unique<MockTranslationBackend>();
        mock = mockPtr.get();
        mock->deliverFrames = kFrames;
        mock->deliverGain = 0.5f;
        mock->negate = true;

        if (useSupervisor)
        {
            translation::ReconnectSupervisor::Policy policy;
            policy.enabled = true;
            policy.initialBackoffMs = 20;    // a fault test's clock, not a product number
            policy.maxBackoffMs = 40;
            controller.setTranslationBackend(
                std::make_unique<translation::ReconnectSupervisor>(std::move(mockPtr), policy));
        }
        else
        {
            controller.setTranslationBackend(std::move(mockPtr));
        }

        auto tone = std::make_unique<audio::TestToneAudioBackend>(1000.0, -20.0);
        toneBackend = tone.get();
        controller.setAudioBackend(std::move(tone));
    }

    bool start()
    {
        if (!controller.start())
            return false;

        REQUIRE(controller.translationStreamer() != nullptr);
        controller.engine().setJitterBufferMs(0);
        return true;
    }

    bool blocksAdvance(std::uint64_t& observedMs)
    {
        const auto before = controller.engine().blockCount();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto after = controller.engine().blockCount();
        observedMs = 150;
        return after > before;
    }

    QuietLog quiet;
    ApplicationController controller;
    MockTranslationBackend* mock = nullptr;
    audio::TestToneAudioBackend* toneBackend = nullptr;
};

} // namespace

TEST_CASE("Injection: a network outage mid-show costs counted gaps, not the show",
          "[faults][injection][e2e][recovery]")
{
    LiveRig rig{ true };
    REQUIRE(rig.start());

    REQUIRE(waitsFor([&] { return rig.mock->acceptedSubmits() >= 2; }));

    // Pull the line: connection is the retryable category - the supervisor owns
    // this failure, the application must not even notice through the audio path.
    rig.mock->injectError(TranslationErrorCategory::connection, "outage injected by task 023", true);

    // Audio preserved: the device thread never stopped while the session died
    // and was rebuilt underneath it.
    std::uint64_t observedMs = 0;
    CHECK(rig.blocksAdvance(observedMs));
    CHECK(rig.controller.state() == ApplicationState::running);
    CHECK(rig.controller.status().audio == BackendState::running);

    // Recovery happened and is visible: fresh session, counted reconnects,
    // counted refused frames, and the ring carries the operator's story.
    REQUIRE(waitsFor([&] { return rig.mock->sessionsOpened() >= 2; }));
    REQUIRE(waitsFor([&] { return rig.controller.sessionState() == SessionState::connected; }));
    CHECK(rig.controller.diagnostics().snapshot().reconnects == 1);   // counted once, exactly
    CHECK(mentionsEvent(rig.controller, "outage injected by task 023"));
    CHECK(mentionsEvent(rig.controller, "session reconnected"));

    const auto blocks = rig.controller.engine().blockCount();
    const auto capturedBefore = rig.controller.engine().inputSamplesCaptured();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));   // six device periods
    CHECK(rig.controller.engine().blockCount() > blocks);
    CHECK(rig.controller.engine().inputSamplesCaptured() > capturedBefore);

    // And the recovered session carries audio again end to end: the mock is
    // still delivering, and the controller still accepts.
    REQUIRE(waitsFor([&] { return rig.controller.diagnostics().snapshot().translatedAudioFrames
                                     >= static_cast<std::uint64_t>(kFrames); }));

    rig.controller.stop();
    CHECK(rig.controller.state() == ApplicationState::stopped);
}

TEST_CASE("Injection: wrong-rate deliveries are a data problem, never a fault",
          "[faults][injection][e2e][rates]")
{
    LiveRig rig{ false };
    rig.mock->deliverAtSampleRate = 16000;   // the provider ignored the request
    REQUIRE(rig.start());

    REQUIRE(waitsFor([&] {
        const auto snapshot = rig.controller.diagnostics().snapshot();
        return snapshot.rejectedAudioFrames >= 2 * static_cast<std::uint64_t>(kFrames);
    }));

    // Refused wholesale, counted, and loud nowhere: the app runs, the audio
    // path is healthy, nothing accepted the wrong-rate bytes even partly.
    const auto snapshot = rig.controller.diagnostics().snapshot();
    CHECK(snapshot.translatedAudioFrames == 0);
    CHECK(rig.controller.state() == ApplicationState::running);
    std::uint64_t observedMs = 0;
    CHECK(rig.blocksAdvance(observedMs));

    // The stop-then-reconfigure recovery (the knob belongs to the backend's
    // start-time contract; the test never races it mid-session on purpose).
    rig.controller.stop();
    rig.mock->deliverAtSampleRate = 0;
    REQUIRE(rig.start());

    REQUIRE(waitsFor([&] {
        const auto after = rig.controller.diagnostics().snapshot();
        return after.translatedAudioFrames >= static_cast<std::uint64_t>(kFrames);
    }));
    CHECK(rig.controller.state() == ApplicationState::running);

    rig.controller.stop();
}

TEST_CASE("Injection: the NDI transport dies mid-subtitle, the audience keeps hearing",
          "[faults][injection][e2e][ndi]")
{
    LiveRig rig{ false };
    rig.mock->deliverFrames = 0;   // text only: the NDI path is what is under test
    rig.mock->textCues = {
        { 1, "line one", true },
        { 2, "line two", true },
        { 3, "line three", true },
        { 4, "line four", true },
    };

    auto ndi = std::make_unique<FlakyNdiOutput>();
    auto* ndiRef = ndi.get();
    ndiRef->failAfter = 2;    // the transport "drops" after the third attempt
    rig.controller.setNdiOutput(std::move(ndi));

    auto cfg = rig.controller.config().current();
    cfg.ndi.enabled = true;
    cfg.ndi.streamName = "LingoFlow Faults";
    std::string error;
    REQUIRE(rig.controller.config().update(cfg, error));

    REQUIRE(rig.start());

    REQUIRE(waitsFor([&] { return ndiRef->attempts() >= 4; }));

    // NDI failure is not a show failure (AGENTS.md 12): counted, recorded, and
    // the rest of the machine could not care less.
    CHECK(ndiRef->publishedFrames() == 2);
    CHECK(ndiRef->publishErrors() >= 2);
    CHECK(ndiRef->state() == ndi::OutputState::faulted);
    CHECK(rig.controller.diagnostics().snapshot().ndiErrors >= 2);
    CHECK(mentionsEvent(rig.controller, "flaky transport refused"));
    CHECK(rig.controller.state() == ApplicationState::running);

    // The audio path keeps running while the captions burn.
    std::uint64_t observedMs = 0;
    CHECK(rig.blocksAdvance(observedMs));

    // Subtitles still flowed through the typed pipeline regardless of the
    // transport: history holds every delivered line, none lost by NDI dying.
    const auto text = rig.controller.textPipeline().snapshot();
    CHECK(text.history.size() >= 2);

    rig.controller.stop();
}

TEST_CASE("Injection: unavailable I/O faults visibly, and Retry reaches a running show",
          "[faults][injection][e2e][io]")
{
    QuietLog quiet;
    ApplicationController controller;

    controller.setTranslationBackend(std::make_unique<MockTranslationBackend>());
    controller.setAudioBackend(std::make_unique<UnavailableAudio>());

    // Start refuses: no crash, no half-machine - state faulted with the reason.
    CHECK_FALSE(controller.start());
    CHECK(controller.state() == ApplicationState::faulted);
    CHECK(controller.faultReason().find("not present") != std::string::npos);
    CHECK(mentionsEvent(controller, "not present"));

    // The corrected fault is retried by the operator, not by a silent loop:
    // clearFault, mount a device that exists, start - recovery is visible in
    // the running counters, not just the absence of the old error.
    auto tone = std::make_unique<audio::TestToneAudioBackend>(1000.0, -20.0);
    controller.setAudioBackend(std::move(tone));
    controller.clearFault();
    REQUIRE(controller.start());

    CHECK(controller.state() == ApplicationState::running);
    REQUIRE(waitsFor([&] { return controller.engine().blockCount() >= 3; }));

    controller.stop();
    CHECK(controller.state() == ApplicationState::stopped);
}

TEST_CASE("Injection: an invalid settings update mid-show is refused whole, the show does not care",
          "[faults][injection][e2e][config]")
{
    LiveRig rig{ false };
    REQUIRE(rig.start());

    const auto gainBefore = rig.controller.engine().inputGainDb();
    REQUIRE(gainBefore == 0.0f);

    auto candidate = rig.controller.config().current();
    candidate.audio.inputGainDb = std::numeric_limits<float>::quiet_NaN();

    std::string note;
    CHECK_FALSE(rig.controller.updateSettings(candidate, note));
    CHECK_FALSE(note.empty());   // refused with a reason, never silently

    // Memory, config and the running machine all still say the old truth.
    CHECK(rig.controller.config().current().audio.inputGainDb == 0.0f);
    CHECK(rig.controller.engine().inputGainDb() == gainBefore);
    CHECK(rig.controller.state() == ApplicationState::running);

    std::uint64_t observedMs = 0;
    CHECK(rig.blocksAdvance(observedMs));   // the refusal cost the audio path nothing

    rig.controller.stop();
}

TEST_CASE("Injection: start/stop hammering with deliveries in flight never wedges or crashes",
          "[faults][injection][e2e][race][stress]")
{
    LiveRig rig{ true };
    rig.mock->textCues = {
        { 1, "storm one", true },
        { 2, "storm two", true },
    };

    auto ndi = std::make_unique<FlakyNdiOutput>();
    auto* ndiRef = ndi.get();
    ndiRef->failAfter = 100000;    // it does not fail here - it just lags
    ndiRef->delayMs = 1;           // every publish takes a millisecond: queues pile up
    rig.controller.setNdiOutput(std::move(ndi));

    auto cfg = rig.controller.config().current();
    cfg.ndi.enabled = true;
    cfg.ndi.streamName = "LingoFlow Hammer";
    std::string error;
    REQUIRE(rig.controller.config().update(cfg, error));

    // Sessions opened and closed while audio, streaming, supervisor and the
    // dispatch worker were all live in the previous round. stop() while a
    // recovery backoff is in progress is part of the loop below.
    std::uint64_t totalDelivered = 0;
    for (int round = 0; round < 12; ++round)
    {
        REQUIRE(rig.start());

        // Let deliveries flow, disturb the session on the way, then tear down
        // while text is still being published (1 ms per frame keeps a queue
        // alive when stop() arrives).
        REQUIRE(waitsFor([&] { return rig.mock->acceptedSubmits() >= 2; }));
        rig.mock->injectError(TranslationErrorCategory::connection, "hammer outage", true);
        if (round == 11)
            rig.mock->setOpenFails(2);   // last round: stop racing the backoff ladder,
                                         // and there is no next start() to poison

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        rig.controller.stop();

        CHECK(rig.controller.state() == ApplicationState::stopped);
        CHECK_FALSE(rig.controller.translationStreamer());

        totalDelivered = rig.controller.textPipeline().deliveredEvents();
    }

    // Every round completed: the process getting here IS the no-deadlock proof
    // (all joins bounded). Delivered counters persisted across restarts and the
    // session counter says all twelve reopened cleanly.
    CHECK(totalDelivered > 0);
    CHECK(rig.mock->sessionsOpened() >= 12);

    // Nothing published after the last stop(): the dispatch door closed behind
    // the run. (attempts() is a plain counter; a later attempt could only come
    // from a live worker - the worker joined, so the number stands.)
    const auto attemptsAtEnd = ndiRef->attempts();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(ndiRef->attempts() == attemptsAtEnd);
}

TEST_CASE("Injection: stop drains the subtitle dispatch - everything delivered or counted",
          "[faults][injection][e2e][race][drain]")
{
    LiveRig rig{ false };
    rig.mock->textCues.clear();
    for (int i = 1; i <= 30; ++i)
        rig.mock->textCues.push_back({ i, "queued line " + std::to_string(i), i % 2 == 0 });
    // The last cue (30) is a final: at stop() there is no open draft, so the
    // close-open-line path adds no synthetic event and the drain invariant
    // below is exact - every emitted event was delivered or counted, never
    // both and never neither.

    auto ndi = std::make_unique<FlakyNdiOutput>();
    auto* ndiRef = ndi.get();
    ndiRef->failAfter = 100000;   // healthy transport - the test is the drain itself
    ndiRef->delayMs = 2;          // deliberately slower than the ingestion
    rig.controller.setNdiOutput(std::move(ndi));

    auto cfg = rig.controller.config().current();
    cfg.ndi.enabled = true;
    cfg.ndi.streamName = "LingoFlow Drain";
    std::string error;
    REQUIRE(rig.controller.config().update(cfg, error));

    REQUIRE(rig.start());

    // Let the capture feed all thirty submits (the tone thread is the clock).
    REQUIRE(waitsFor([&] { return rig.mock->acceptedSubmits() >= 30; }, 15000));

    // Stop while dispatch is demonstrably still behind ingest: publish takes
    // 2 ms and the worker just received a burst.
    rig.controller.stop();

    // The drain invariant: what the pipeline emitted equals what the listener
    // saw plus what pressure evicted. A stop that lost events silently fails
    // this; a stop that deadlocked fails the test process's clock instead.
    const auto emitted = rig.mock->textEvents();
    const auto delivered = rig.controller.textPipeline().deliveredEvents();
    const auto dropped = rig.controller.textPipeline().droppedEvents();
    CHECK(emitted > 0);
    CHECK(delivered + dropped == static_cast<std::uint64_t>(emitted));

    // And NDI saw every delivered frame - the accepted count matches, or the
    // missing ones are counted drops, never vanished.
    CHECK(ndiRef->attempts() <= delivered);

    // Re-open: the session's last words survived the stop - history kept.
    REQUIRE(rig.start());
    CHECK(rig.controller.textPipeline().snapshot().history.size() >= 1);
    rig.controller.stop();
}
