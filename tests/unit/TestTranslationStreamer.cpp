#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "App/TranslationStreamer.h"
#include "Audio/AudioEngine.h"
#include "Audio/Null/NullAudioBackend.h"
#include "Diagnostics/DiagnosticsManager.h"
#include "Utils/Log.h"

using namespace liveai;
using liveai::audio::DeviceRequest;
using liveai::audio::NullAudioBackend;

namespace {

constexpr int kRate = 48000;
constexpr int kFrames = 480;

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

DeviceRequest request()
{
    DeviceRequest r;
    r.sampleRate = kRate;
    r.bufferFrames = kFrames;
    return r;
}

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

/// A recording backend for the seam test: it keeps exactly what submitAudio()
/// handed it, in order. `accept` toggles the gap-policy path, `failAfter` makes
/// it throw like a broken backend would. Nothing here touches the network.
class RecorderBackend final : public translation::ITranslationBackend
{
public:
    std::atomic<bool> accept{ true };
    std::atomic<int> failAfter{ -1 };   ///< -1 never; N = throw at the Nth submit call

    std::string_view name() const noexcept override { return "Recorder"; }
    translation::SessionState state() const noexcept override { return translation::SessionState::connected; }
    void setSink(translation::ITranslationSink& sink) noexcept override { sink_ = &sink; }

    bool openSession(const translation::SessionRequest&, std::string& error) override
    {
        error.clear();
        return true;
    }

    bool submitAudio(const float* samples, int frameCount, std::string& error) override
    {
        std::lock_guard lock(mutex_);

        ++submitCalls_;

        if (failAfter.load() >= 0 && submitCalls_ > failAfter.load())
            throw std::runtime_error("recorder: injected backend defect");

        if (!accept.load())
        {
            error = "recorder: no session can accept this right now";
            return false;
        }

        captured_.insert(captured_.end(), samples, samples + frameCount);
        return true;
    }

    void closeSession() noexcept override {}

    int submitCalls()
    {
        std::lock_guard lock(mutex_);
        return submitCalls_;
    }

    std::vector<float> captured()
    {
        std::lock_guard lock(mutex_);
        return captured_;
    }

private:
    translation::ITranslationSink* sink_ = nullptr;
    std::mutex mutex_;
    std::vector<float> captured_;
    int submitCalls_ = 0;
};

/// The device side of a strictly increasing ramp, fed by direct callbacks the
/// way the pipeline tests drive them. A recorded ramp that is complete and in
/// order proves the worker neither loses, duplicates nor reorders frames.
class Ramp
{
public:
    void fill(float* data, std::size_t frames) noexcept
    {
        for (std::size_t i = 0; i < frames; ++i)
            data[i] = static_cast<float>(++counter_);
    }

    std::uint64_t produced() const noexcept { return counter_; }

private:
    std::uint64_t counter_ = 0;
};

struct Harness
{
    QuietLog quiet;
    DiagnosticsManager diagnostics;
    AudioEngine engine{ &diagnostics };
    NullAudioBackend device;
    RecorderBackend backend;

    void activate()
    {
        std::string error;
        REQUIRE(engine.activate(device, request(), error));
    }

    /// One device callback of ramp audio.
    void feed(AudioEngine& engine_, Ramp& ramp)
    {
        std::vector<float> in(static_cast<std::size_t>(kFrames));
        std::vector<float> out(static_cast<std::size_t>(kFrames), -1.0f);

        ramp.fill(in.data(), kFrames);

        const float* inPointers[1] = { in.data() };
        float* outPointers[1] = { out.data() };

        engine_.processAudio(inPointers, outPointers, kFrames);
    }
};

} // namespace

TEST_CASE("TranslationStreamer: capture reaches the backend complete and in order",
          "[translation][streamer]")
{
    Harness h;
    h.activate();

    TranslationStreamer streamer(h.engine, h.backend, &h.diagnostics);

    std::string error;
    REQUIRE(streamer.start(error));
    REQUIRE(h.engine.inputConsumerAttached());

    Ramp ramp;
    constexpr int kBlocks = 50;   // 50 x 480 frames, crossing ring wrap boundaries

    for (int i = 0; i < kBlocks; ++i)
        h.feed(h.engine, ramp);

    REQUIRE(waitsFor([&] { return static_cast<std::uint64_t>(h.backend.captured().size())
                                        == ramp.produced(); }));

    const auto captured = h.backend.captured();
    REQUIRE(captured.size() == ramp.produced());

    for (std::size_t i = 0; i < captured.size(); ++i)
        CHECK(captured[i] == static_cast<float>(i + 1));   // 1, 2, 3... nothing lost or reordered

    CHECK(streamer.submittedFrames() == captured.size());
    CHECK(streamer.gapRefusedFrames() == 0);
    CHECK_FALSE(streamer.crashed());

    // The same truth the recorder holds is in the operator's counters.
    const auto snapshot = h.diagnostics.snapshot();
    CHECK(snapshot.translationSubmittedFrames == captured.size());
    CHECK(snapshot.translationGapFrames == 0);

    streamer.stop();
    h.engine.deactivate();
}

TEST_CASE("TranslationStreamer: refused capture is drained and counted - the gap policy in action",
          "[translation][streamer][gap]")
{
    Harness h;
    h.activate();

    TranslationStreamer streamer(h.engine, h.backend, &h.diagnostics);

    std::string error;
    REQUIRE(streamer.start(error));

    h.backend.accept.store(false);

    Ramp ramp;
    for (int i = 0; i < 20; ++i)
        h.feed(h.engine, ramp);

    // The worker keeps consuming the ring even while everything is refused: that
    // is what "drop" means, and the counting is what keeps it honest. The ring
    // must never back up into overflow because the session is reconnecting.
    REQUIRE(waitsFor([&] { return streamer.gapRefusedFrames() == ramp.produced(); }));

    CHECK(streamer.submittedFrames() == 0);
    CHECK(streamer.running());
    CHECK_FALSE(streamer.crashed());
    CHECK(h.engine.inputRing(0)->readable() == 0);
    CHECK(h.engine.overrunEvents() == 0);
    CHECK(h.diagnostics.snapshot().translationGapFrames == ramp.produced());

    // Recovery of the session needs no restart of the worker: the next accepted
    // submit flows on the same thread.
    h.backend.accept.store(true);
    h.feed(h.engine, ramp);
    REQUIRE(waitsFor([&] { return streamer.submittedFrames() > 0; }));

    streamer.stop();
    h.engine.deactivate();
}

TEST_CASE("TranslationStreamer: stop joins the worker and detaches the consumer",
          "[translation][streamer][lifecycle]")
{
    Harness h;
    h.activate();

    TranslationStreamer streamer(h.engine, h.backend, &h.diagnostics);

    std::string error;
    REQUIRE(streamer.start(error));

    Ramp ramp;
    h.feed(h.engine, ramp);
    REQUIRE(waitsFor([&] { return h.backend.submitCalls() >= 1; }));

    streamer.stop();

    CHECK_FALSE(streamer.running());
    CHECK_FALSE(h.engine.inputConsumerAttached());

    // Frames produced after stop are back to "not forwarded" - nobody claimed
    // the consumer role anymore (the engine counts them, never fakes it).
    const auto before = h.engine.inputSamplesNotForwarded();
    h.feed(h.engine, ramp);
    CHECK(h.engine.inputSamplesNotForwarded() > before);

    // Idempotent, and start refuses without a pipeline or twice in a row.
    streamer.stop();

    std::string doubleStart;
    TranslationStreamer another(h.engine, h.backend, nullptr);
    REQUIRE(another.start(doubleStart));
    std::string secondStart;
    CHECK_FALSE(another.start(secondStart));
    CHECK_FALSE(secondStart.empty());
    another.stop();

    h.engine.deactivate();
    std::string noPipeline;
    CHECK_FALSE(another.start(noPipeline));
    CHECK(noPipeline.find("pipeline") != std::string::npos);
}

TEST_CASE("TranslationStreamer: a throwing backend takes down the worker, not the application",
          "[translation][streamer][errors]")
{
    Harness h;
    h.activate();

    TranslationStreamer streamer(h.engine, h.backend, &h.diagnostics);
    h.backend.failAfter.store(1);   // first submit is fine, the second throws

    std::string error;
    REQUIRE(streamer.start(error));

    Ramp ramp;
    h.feed(h.engine, ramp);
    REQUIRE(waitsFor([&] { return h.backend.submitCalls() >= 1; }));
    h.feed(h.engine, ramp);

    REQUIRE(waitsFor([&] { return streamer.crashed(); }));

    // The engine is untouched: it is not the callback's problem that a worker
    // died. Capture keeps being metered, and with nobody draining it the
    // overflow accounting says so - counted, not hidden.
    const auto blocksBefore = h.engine.blockCount();
    h.feed(h.engine, ramp);
    CHECK(h.engine.blockCount() == blocksBefore + 1);
    CHECK_FALSE(streamer.running());

    // stop() on a dead worker is a clean join, not a hang.
    streamer.stop();
    h.engine.deactivate();
}
