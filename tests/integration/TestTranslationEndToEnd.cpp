#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "App/ApplicationController.h"
#include "NDI/Null/NullNdiOutput.h"
#include "Translation/ReconnectSupervisor.h"
#include "Utils/Log.h"
#include "support/MockTranslationBackend.h"

using namespace liveai;
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

std::vector<float> filled(int frames, float value)
{
    return std::vector<float>(static_cast<std::size_t>(frames), value);
}

/// Poll the observable until it is true. The point of task 012 is that capture
/// moves on its own worker thread, so "the frames arrived at the backend" is an
/// event to wait for, not a step to perform. Timeouts fail the test loudly - a
/// hang is exactly what these checks exist to catch.
template <typename Predicate>
bool waitsFor(Predicate&& ready, int timeoutMs = 4000)
{
    for (int waited = 0; waited < timeoutMs; waited += 5)
    {
        if (ready())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return ready();
}

/// One simulated device callback of constant audio. It does NOT touch the ring
/// or the backend: from task 012 the streaming worker is the only thing that
/// drains the input ring, and the tests assert exactly that.
struct Pump
{
    std::vector<float> in = filled(kFrames, 0.4f);
    std::vector<float> out = filled(kFrames, -0.125f);

    const float* inPointers[1] = { in.data() };
    float* outPointers[1] = { out.data() };

    /// Run one callback with `level` on the wire in.
    void feed(AudioEngine& engine, float level)
    {
        std::fill(in.begin(), in.end(), level);
        std::fill(out.begin(), out.end(), -0.125f);   // poison: only the engine may rewrite it
        engine.processAudio(inPointers, outPointers, kFrames);
    }

    /// A callback with silent input: plays whatever the jitter buffer holds.
    void play(AudioEngine& engine)
    {
        feed(engine, 0.0f);
    }

    /// Feed one block and wait until the streaming worker has handed it to the
    /// backend as a single submit. The wait between feeds is what keeps the
    /// mock's per-submit cue counters meaningful: one callback, one submit.
    void feedOneSubmit(AudioEngine& engine, MockTranslationBackend& backend, float level)
    {
        const int before = backend.acceptedSubmits();
        feed(engine, level);
        REQUIRE(waitsFor([&] { return backend.acceptedSubmits() >= before + 1; }));
    }
};

/// A controller running the Null audio device and a mock the test keeps a
/// handle to, with playback latency removed. From task 012 the controller also
/// runs the production streaming worker itself between session open and close.
struct Rig
{
    QuietLog quiet;
    ApplicationController controller;
    MockTranslationBackend* mock = nullptr;

    explicit Rig(std::unique_ptr<MockTranslationBackend> scripted)
    {
        mock = scripted.get();
        controller.setTranslationBackend(std::move(scripted));
    }

    bool start()
    {
        if (!controller.start())
            return false;

        // The streaming worker is created by startSession(); this is task 012's
        // production shape, not a test scaffold: capture moves on its own thread.
        REQUIRE(controller.translationStreamer() != nullptr);

        controller.engine().setJitterBufferMs(0);   // the tests assert levels and routing,
                                                    // not the pre-roll (proven in task 005)
        return true;
    }
};

} // namespace

TEST_CASE("End to end through the mock: what came in, translated, goes out",
          "[translation][e2e][pipeline]")
{
    Rig rig{ std::make_unique<MockTranslationBackend>() };
    rig.mock->deliverFrames = kFrames;
    rig.mock->deliverGain = 0.5f;
    rig.mock->negate = true;

    REQUIRE(rig.start());
    CHECK(rig.controller.status().session == SessionState::connected);
    CHECK(rig.mock->lastRequest().inputSampleRate == kRate);      // filled by the controller
    CHECK(rig.mock->lastRequest().outputSampleRate == kRate);

    Pump pump;
    AudioEngine& engine = rig.controller.engine();

    // One block in. The streaming worker takes it out of the ring on its own
    // thread and the mock translates it synchronously; by the time the delivery
    // is observable the whole block sits in the jitter buffer.
    pump.feed(engine, 0.4f);
    REQUIRE(waitsFor([&] { return rig.mock->deliveredBlocks() >= 1; }));
    REQUIRE(waitsFor([&] { return rig.controller.diagnostics().snapshot().translatedAudioFrames
                                        >= static_cast<std::uint64_t>(kFrames); }));

    const auto before = rig.controller.diagnostics().snapshot();
    CHECK(before.translatedAudioFrames == static_cast<std::uint64_t>(kFrames));
    CHECK(before.translationSubmittedFrames == static_cast<std::uint64_t>(kFrames));
    CHECK(before.translationGapFrames == 0);
    CHECK(before.rejectedAudioFrames == 0);
    CHECK(before.translatedAudioDroppedFrames == 0);

    // The next callback plays what arrived: 0.4 in, -0.2 out. No other value
    // could come from this path - loopback is not attached, and the jitter
    // buffer is the one source of output (task 005's safety rule).
    pump.play(engine);

    for (const float sample : pump.out)
        CHECK(sample == -0.2f);

    // A callback after that has nothing left to give: silence, counted, no
    // invention.
    pump.play(engine);
    CHECK(pump.out.front() == 0.0f);
    CHECK(engine.underrunEvents() > 0);

    rig.controller.stop();
    CHECK(rig.mock->state() == SessionState::closed);
    CHECK(rig.controller.translationStreamer() == nullptr);
}

TEST_CASE("End to end: a wrong-rate delivery is refused and counted, text goes on",
          "[translation][e2e][rates][independence]")
{
    Rig rig{ std::make_unique<MockTranslationBackend>() };
    rig.mock->deliverFrames = kFrames;
    rig.mock->deliverGain = 0.5f;
    rig.mock->negate = true;
    rig.mock->deliverAtSampleRate = 16000;   // the provider ignored the request
    rig.mock->textCues = { { 1, "still here", false } };

    REQUIRE(rig.start());

    Pump pump;
    AudioEngine& engine = rig.controller.engine();

    pump.feedOneSubmit(engine, *rig.mock, 0.4f);
    pump.feedOneSubmit(engine, *rig.mock, 0.4f);

    REQUIRE(waitsFor([&] { return rig.controller.diagnostics().snapshot().rejectedAudioFrames
                                        >= 2 * static_cast<std::uint64_t>(kFrames); }));

    const auto snapshot = rig.controller.diagnostics().snapshot();
    CHECK(snapshot.rejectedAudioFrames == 2 * static_cast<std::uint64_t>(kFrames));
    CHECK(snapshot.translatedAudioFrames == 0);   // nothing was accepted, not even partly

    // Nothing plays: refusing audio that would sound wrong beats playing it
    // fast and chipmunked.
    pump.play(engine);
    CHECK(std::all_of(pump.out.begin(), pump.out.end(), [](float s) { return s == 0.0f; }));

    // And the text channel reached the sink through the same broken delivery:
    // SPEC "the audio callback and the text callback must be independent" means
    // the failure of one must not silence the other.
    CHECK(snapshot.partialTextEvents == 1);

    // The application treats a rejected block as a data problem, not a fault.
    CHECK(rig.controller.state() == ApplicationState::running);
    CHECK(engine.blockCount() > 0);

    rig.controller.stop();
}

TEST_CASE("End to end: a fatal translation error leaves audio running and tells the operator",
          "[translation][e2e][errors]")
{
    Rig rig{ std::make_unique<MockTranslationBackend>() };
    rig.mock->deliverFrames = kFrames;
    rig.mock->deliverGain = 0.5f;
    rig.mock->negate = true;
    rig.mock->errorCues = { { 2, TranslationErrorCategory::connection, "link dropped", true } };

    REQUIRE(rig.start());

    Pump pump;
    AudioEngine& engine = rig.controller.engine();

    // One submit per feed: the cue fires on the second accepted submit.
    pump.feedOneSubmit(engine, *rig.mock, 0.4f);
    pump.feedOneSubmit(engine, *rig.mock, 0.4f);   // the fatal cue fires here
    REQUIRE(waitsFor([&] { return rig.mock->state() == SessionState::faulted; }));

    CHECK(rig.controller.status().session == SessionState::faulted);

    // The application is NOT faulted and the device is NOT closed: AGENTS.md 12
    // says a translation failure must not take the audio path down.
    CHECK(rig.controller.state() == ApplicationState::running);
    CHECK(rig.controller.status().audio == audio::BackendState::running);

    // Counted, recorded, and visible in the status detail for the operator.
    const auto snapshot = rig.controller.diagnostics().snapshot();
    CHECK(snapshot.translationErrors == 1);
    CHECK(snapshot.translationFatalErrors == 1);
    CHECK(rig.controller.status().detail.find("fatal connection: link dropped") != std::string::npos);

    // The translated audio buffered before the fault is still played: an error
    // is not allowed to revoke data the audience is owed.
    pump.play(engine);
    for (const float sample : pump.out)
        CHECK(sample == -0.2f);

    // The callback keeps running afterwards; the streaming worker's submits are
    // now refused, counted as gap, and the output degrades to counted silence.
    const auto blocksBefore = engine.blockCount();
    pump.feed(engine, 0.3f);
    CHECK(engine.blockCount() == blocksBefore + 1);
    REQUIRE(waitsFor([&] { return rig.mock->refusedSubmits() >= 1; }));
    CHECK(rig.controller.translationStreamer()->running());

    rig.controller.stop();
    CHECK(rig.mock->state() == SessionState::closed);
}

TEST_CASE("End to end: mock text reaches NDI and diagnostics through the controller",
          "[translation][e2e][ndi][text]")
{
    Rig rig{ std::make_unique<MockTranslationBackend>() };
    rig.mock->deliverFrames = 0;   // text only: nothing for the audio channel to do
    rig.mock->textCues = {
        { 1, "good evening", false },
        { 2, "good evening", false },
        { 2, "good evening, welcome", true },
    };

    auto cfg = rig.controller.config().current();
    cfg.ndi.enabled = true;
    cfg.ndi.streamName = "LingoFlow E2E";
    std::string error;
    REQUIRE(rig.controller.config().update(cfg, error));

    auto ndi = std::make_unique<ndi::NullNdiOutput>();
    auto* ndiRef = ndi.get();
    rig.controller.setNdiOutput(std::move(ndi));

    REQUIRE(rig.start());

    Pump pump;
    AudioEngine& engine = rig.controller.engine();

    pump.feedOneSubmit(engine, *rig.mock, 0.2f);
    pump.feedOneSubmit(engine, *rig.mock, 0.2f);
    REQUIRE(waitsFor([&] { return ndiRef->publishedFrames() >= 3; }));

    CHECK(ndiRef->publishedFrames() == 3);
    CHECK(ndiRef->state() == ndi::OutputState::publishing);

    const auto snapshot = rig.controller.diagnostics().snapshot();
    CHECK(snapshot.partialTextEvents == 2);
    CHECK(snapshot.finalTextEvents == 1);

    // Text events never needed an audio path to be attached: the mock delivered
    // no audio at all in this script.
    CHECK(rig.mock->deliveredBlocks() == 0);
    CHECK(snapshot.translatedAudioFrames == 0);

    rig.controller.stop();
}

TEST_CASE("End to end: the session request carries settings, model hint and live rates",
          "[translation][e2e][config]")
{
    Rig rig{ std::make_unique<MockTranslationBackend>() };

    auto cfg = rig.controller.config().current();
    cfg.translation.inputLanguage = "en";
    cfg.translation.outputLanguage = "ru";
    cfg.translation.instructions = "translate the operator's words, keep names";
    cfg.translation.modelHint = "settings-provided-value";
    std::string error;
    REQUIRE(rig.controller.config().update(cfg, error));

    REQUIRE(rig.start());

    const auto& request = rig.mock->lastRequest();
    CHECK(request.pair.input == "en");
    CHECK(request.pair.output == "ru");
    CHECK(request.instructions == "translate the operator's words, keep names");

    // The hint travels as a string the operator or the manifest chose; the
    // product itself does not invent identifiers (AGENTS.md 8 - which values are
    // legal is task 008's documentation to establish).
    CHECK(request.model == "settings-provided-value");

    // The rates come from the live engine, not from a guess at settings.
    CHECK(request.inputSampleRate == rig.controller.engine().sampleRate());
    CHECK(request.outputSampleRate == rig.controller.engine().sampleRate());
    CHECK(rig.controller.engine().sampleRate() == kRate);

    rig.controller.stop();
}

TEST_CASE("End to end: deliveries before start and after stop are refused, counted, harmless",
          "[translation][e2e][shutdown]")
{
    // A backend callback that races with the audio path being down must not
    // crash, must not queue memory waiting for a device, and must not pretend
    // the frames went somewhere.
    QuietLog quiet;
    ApplicationController controller;

    std::vector<float> audio = filled(kFrames, 0.5f);

    // The sink itself, with no pipeline: nullptr jitter is the honest answer.
    controller.onTranslatedAudio(audio.data(), kFrames, kRate);
    CHECK(controller.diagnostics().snapshot().rejectedAudioFrames == static_cast<std::uint64_t>(kFrames));

    REQUIRE(controller.start());
    controller.onTranslatedAudio(audio.data(), kFrames, kRate);
    CHECK(controller.diagnostics().snapshot().translatedAudioFrames == static_cast<std::uint64_t>(kFrames));

    controller.stop();

    // After stop(): the engine has released its buffers, and a late delivery is
    // rejected again - counted, not swallowed. stop() stopped the streaming
    // worker, then closed the session, then deactivated the engine, so a real
    // backend could not be producing now; the controller's guard is what keeps
    // a programming error in a future task from becoming a use-after-free.
    CHECK_FALSE(controller.translationStreamer());
    controller.onTranslatedAudio(audio.data(), kFrames, kRate);
    CHECK(controller.diagnostics().snapshot().rejectedAudioFrames == static_cast<std::uint64_t>(2 * kFrames));
    CHECK(controller.diagnostics().snapshot().translatedAudioFrames == static_cast<std::uint64_t>(kFrames));

    // Malformed deliveries are counted as rejected too, separately from rates.
    controller.onTranslatedAudio(nullptr, 480, kRate);
    controller.onTranslatedAudio(audio.data(), 0, kRate);
    CHECK(controller.diagnostics().snapshot().rejectedAudioFrames == static_cast<std::uint64_t>(2 * kFrames + 480));
}

TEST_CASE("End to end: restart brings a fresh session with the same wiring",
          "[translation][e2e][restart]")
{
    Rig rig{ std::make_unique<MockTranslationBackend>() };
    rig.mock->deliverFrames = kFrames;
    rig.mock->deliverGain = 0.5f;
    rig.mock->negate = true;

    REQUIRE(rig.start());

    Pump pump;
    pump.feedOneSubmit(rig.controller.engine(), *rig.mock, 0.4f);

    rig.controller.stop();
    CHECK(rig.mock->state() == SessionState::closed);
    CHECK(rig.controller.translationStreamer() == nullptr);

    REQUIRE(rig.start());
    CHECK(rig.mock->state() == SessionState::connected);
    CHECK(rig.mock->sessionsOpened() == 2);

    // Rule 6 of the contract: the new session behaves like a fresh one. The cue
    // positions are per session, and the streaming worker of the new session
    // starts from zero submits again.
    CHECK(rig.mock->sessionSubmits() == 0);

    pump.feedOneSubmit(rig.controller.engine(), *rig.mock, 0.4f);
    CHECK(rig.mock->sessionSubmits() == 1);

    // Totals keep accumulating across sessions - the operator's counters do not
    // forget the first one.
    CHECK(rig.mock->acceptedSubmits() == 2);

    pump.play(rig.controller.engine());
    for (const float sample : pump.out)
        CHECK(sample == -0.2f);

    rig.controller.stop();
}

TEST_CASE("End to end with the supervisor mounted: an outage costs a counted gap, not the show",
          "[translation][e2e][recovery][supervisor]")
{
    // Task 012's production shape: the controller talks to ReconnectSupervisor,
    // the supervisor owns the backend, and the streaming worker keeps draining
    // the capture through both. This is the wiring Main.cpp installs.
    QuietLog quiet;
    ApplicationController controller;

    auto mock = std::make_unique<MockTranslationBackend>();
    MockTranslationBackend& backend = *mock;
    backend.deliverFrames = kFrames;
    backend.deliverGain = 0.5f;
    backend.negate = true;

    translation::ReconnectSupervisor::Policy policy;
    policy.enabled = true;
    policy.initialBackoffMs = 20;      // a recovery test's clock, not a product number
    policy.maxBackoffMs = 40;

    controller.setTranslationBackend(
        std::make_unique<translation::ReconnectSupervisor>(std::move(mock), policy));

    REQUIRE(controller.start());
    REQUIRE(controller.translationStreamer() != nullptr);
    controller.engine().setJitterBufferMs(0);

    CHECK(controller.sessionState() == SessionState::connected);

    Pump pump;
    AudioEngine& engine = controller.engine();
    pump.feedOneSubmit(engine, backend, 0.4f);

    // Pull the line on the wrapped backend: the supervisor - not the
    // application - sees the outage first, and connection is the retryable
    // category (task 010), so it closes, waits and replays the stored request.
    backend.injectError(TranslationErrorCategory::connection, "outage injected by the test", true);

    REQUIRE(waitsFor([&] { return backend.sessionsOpened() >= 2; }));
    REQUIRE(waitsFor([&] { return controller.sessionState() == SessionState::connected; }));

    // The application never left running and the device never stopped: the cost
    // of the outage is counted audio in the gap, not the show.
    CHECK(controller.state() == ApplicationState::running);
    CHECK(controller.status().audio == audio::BackendState::running);
    CHECK(controller.translationStreamer()->running());
    CHECK(controller.translationStreamer()->submittedFrames() > 0);

    // The new session is fed again exactly like the first one was.
    pump.feedOneSubmit(engine, backend, 0.4f);
    REQUIRE(waitsFor([&] { return backend.deliveredBlocks() >= 2; }));

    // And the recovered session plays on: the block delivered above is in the
    // jitter buffer, the audience gets it.
    pump.play(engine);
    CHECK(std::any_of(pump.out.begin(), pump.out.end(), [](float s) { return s == -0.2f; }));

    controller.stop();
    CHECK(controller.translationStreamer() == nullptr);
}
