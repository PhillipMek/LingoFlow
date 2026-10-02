#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "App/ApplicationController.h"
#include "NDI/Null/NullNdiOutput.h"
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

/// One simulated device block: constant audio in, capture what the engine puts
/// on the wire, then hand the consumer's view of the input ring to the mock -
/// exactly the steps the task 012 streaming worker will take, run from a single
/// thread so the assertions are deterministic.
struct Pump
{
    std::vector<float> in = filled(kFrames, 0.4f);
    std::vector<float> out = filled(kFrames, -0.125f);
    std::vector<float> drained;

    const float* inPointers[1] = { in.data() };
    float* outPointers[1] = { out.data() };

    /// Runs one callback and forwards whatever the ring accepted. Returns the
    /// frames the callback played; on failure points the test at the submit.
    int step(AudioEngine& engine, MockTranslationBackend& backend, float level)
    {
        std::fill(in.begin(), in.end(), level);
        std::fill(out.begin(), out.end(), -0.125f);

        engine.processAudio(inPointers, outPointers, kFrames);

        drained.assign(static_cast<std::size_t>(kFrames), 0.0f);
        const std::size_t got = engine.inputRing(0)->read(drained.data(), drained.size());
        REQUIRE(got == static_cast<std::size_t>(kFrames));

        std::string error;
        const bool accepted = backend.submitAudio(drained.data(), static_cast<int>(got), error);
        REQUIRE(accepted);

        return kFrames;
    }

    /// A callback with silent input and no submit: plays whatever the jitter
    /// buffer holds.
    void play(AudioEngine& engine)
    {
        std::fill(in.begin(), in.end(), 0.0f);
        std::fill(out.begin(), out.end(), -0.125f);
        engine.processAudio(inPointers, outPointers, kFrames);
    }
};

/// A controller running the Null audio device and a mock the test keeps a
/// handle to, with playback latency removed.
struct Rig
{
    QuietLog quiet;
    ApplicationController controller;
    MockTranslationBackend* mock = nullptr;

    explicit Rig(MockTranslationBackend scriptBackend)
    {
        auto owner = std::make_unique<MockTranslationBackend>(std::move(scriptBackend));
        mock = owner.get();
        controller.setTranslationBackend(std::move(owner));
    }

    bool start()
    {
        if (!controller.start())
            return false;

        controller.engine().attachInputConsumer();
        controller.engine().setJitterBufferMs(0);   // the tests assert levels and routing,
                                                    // not the pre-roll (proven in task 005)
        return true;
    }
};

} // namespace

TEST_CASE("End to end through the mock: what came in, translated, goes out",
          "[translation][e2e][pipeline]")
{
    Rig rig{ MockTranslationBackend{} };
    rig.mock->deliverFrames = kFrames;
    rig.mock->deliverGain = 0.5f;
    rig.mock->negate = true;

    REQUIRE(rig.start());
    CHECK(rig.controller.status().session == SessionState::connected);
    CHECK(rig.mock->lastRequest().inputSampleRate == kRate);      // filled by the controller
    CHECK(rig.mock->lastRequest().outputSampleRate == kRate);

    Pump pump;
    AudioEngine& engine = rig.controller.engine();

    // Two blocks in: both are translated synchronously by the mock and land in
    // the jitter buffer through the controller's sink. The first of them was
    // already played inside the second step's callback - audio that arrives is
    // played by the NEXT block, which is precisely the jitter buffer's job.
    pump.step(engine, *rig.mock, 0.4f);
    pump.step(engine, *rig.mock, 0.4f);

    const auto before = rig.controller.diagnostics().snapshot();
    CHECK(before.translatedAudioFrames == 2 * static_cast<std::uint64_t>(kFrames));
    CHECK(before.rejectedAudioFrames == 0);
    CHECK(before.translatedAudioDroppedFrames == 0);

    // The next callback plays the block that arrived after it started: 0.4 in,
    // -0.2 out. No other value could come from this path - loopback is not
    // attached, and the jitter buffer is the one source of output (task 005's
    // safety rule).
    pump.play(engine);

    for (const float sample : pump.out)
        CHECK(sample == -0.2f);

    // A callback after that has nothing left to give: silence, counted, no
    // invention.
    pump.play(engine);
    CHECK(pump.out.front() == 0.0f);
    CHECK(rig.controller.engine().underrunEvents() > 0);

    rig.controller.stop();
    CHECK(rig.mock->state() == SessionState::closed);
}

TEST_CASE("End to end: a wrong-rate delivery is refused and counted, text goes on",
          "[translation][e2e][rates][independence]")
{
    Rig rig{ MockTranslationBackend{} };
    rig.mock->deliverFrames = kFrames;
    rig.mock->deliverGain = 0.5f;
    rig.mock->negate = true;
    rig.mock->deliverAtSampleRate = 16000;   // the provider ignored the request
    rig.mock->textCues = { { 1, "still here", false } };

    REQUIRE(rig.start());

    Pump pump;
    AudioEngine& engine = rig.controller.engine();

    pump.step(engine, *rig.mock, 0.4f);
    pump.step(engine, *rig.mock, 0.4f);

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
    Rig rig{ MockTranslationBackend{} };
    rig.mock->deliverFrames = kFrames;
    rig.mock->deliverGain = 0.5f;
    rig.mock->negate = true;
    rig.mock->errorCues = { { 2, TranslationErrorCategory::connection, "link dropped", true } };

    REQUIRE(rig.start());

    Pump pump;
    AudioEngine& engine = rig.controller.engine();

    pump.step(engine, *rig.mock, 0.4f);
    pump.step(engine, *rig.mock, 0.4f);   // the fatal cue fires here

    CHECK(rig.mock->state() == SessionState::faulted);
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

    // The callback keeps running afterwards; submissions fail cleanly and the
    // output degrades to counted silence.
    const auto blocksBefore = engine.blockCount();
    engine.processAudio(pump.inPointers, pump.outPointers, kFrames);
    CHECK(engine.blockCount() == blocksBefore + 1);

    std::string error;
    CHECK_FALSE(rig.mock->submitAudio(pump.drained.data(), kFrames, error));
    CHECK_FALSE(error.empty());

    rig.controller.stop();
    CHECK(rig.mock->state() == SessionState::closed);
}

TEST_CASE("End to end: mock text reaches NDI and diagnostics through the controller",
          "[translation][e2e][ndi][text]")
{
    Rig rig{ MockTranslationBackend{} };
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

    pump.step(engine, *rig.mock, 0.2f);
    pump.step(engine, *rig.mock, 0.2f);

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
    Rig rig{ MockTranslationBackend{} };

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
    // rejected again - counted, not swallowed. stop() closed the session first,
    // so a real backend could not be producing now; the controller's guard is
    // what keeps a programming error in task 010 from becoming a use-after-free.
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
    Rig rig{ MockTranslationBackend{} };
    rig.mock->deliverFrames = kFrames;
    rig.mock->deliverGain = 0.5f;
    rig.mock->negate = true;

    REQUIRE(rig.start());

    Pump pump;
    pump.step(rig.controller.engine(), *rig.mock, 0.4f);

    rig.controller.stop();
    CHECK(rig.mock->state() == SessionState::closed);

    REQUIRE(rig.start());
    CHECK(rig.mock->state() == SessionState::connected);
    CHECK(rig.mock->sessionsOpened() == 2);

    // Rule 6 of the contract: the new session behaves like a fresh one. The cue
    // positions are per session, and the first submit of the new session is the
    // first submit again.
    CHECK(rig.mock->sessionSubmits() == 0);

    pump.step(rig.controller.engine(), *rig.mock, 0.4f);
    CHECK(rig.mock->sessionSubmits() == 1);

    // Totals keep accumulating across sessions - the operator's counters do not
    // forget the first one.
    CHECK(rig.mock->acceptedSubmits() == 2);

    pump.play(rig.controller.engine());
    for (const float sample : pump.out)
        CHECK(sample == -0.2f);

    rig.controller.stop();
}
