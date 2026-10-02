#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "Audio/AudioEngine.h"
#include "Audio/AudioJitterBuffer.h"
#include "Audio/AudioLoopback.h"
#include "Audio/AudioRingBuffer.h"
#include "Audio/LevelMeter.h"
#include "Audio/Null/NullAudioBackend.h"
#include "Diagnostics/DiagnosticsManager.h"

using namespace liveai;
using liveai::audio::AudioJitterBuffer;
using liveai::audio::AudioLoopback;
using liveai::audio::AudioRingBuffer;
using liveai::audio::DeviceRequest;
using liveai::audio::LevelMeter;
using liveai::audio::linearToDb;
using liveai::audio::NullAudioBackend;

namespace {

constexpr int kRate = 48000;
constexpr int kFrames = 480;

DeviceRequest request(std::string deviceId = {})
{
    DeviceRequest request;
    request.deviceId = std::move(deviceId);
    request.sampleRate = kRate;
    request.bufferFrames = kFrames;
    return request;
}

/// Fills `data` with a strictly increasing counter ramp: sample i of the whole stream
/// keeps its index, so a reader can prove order and completeness across block
/// boundaries and wraps. Floats are exact up to 2^24, so a few million samples stay
/// distinguishable - no modulo, because a wrapping ramp could not prove ordering.
class Ramp
{
public:
    void fill(float* data, std::size_t frames) noexcept
    {
        for (std::size_t i = 0; i < frames; ++i)
            data[i] = static_cast<float>(++counter_);      // 1, 2, 3... never 0 (silence)
    }

    /// Samples produced so far.
    std::uint64_t produced() const noexcept { return counter_; }

private:
    std::uint64_t counter_ = 0;
};

} // namespace

// --------------------------------------------------------------------- ring buffer

TEST_CASE("AudioRingBuffer: capacity is rounded to a power of two and is lock-free",
          "[audio][ring][realtime]")
{
    AudioRingBuffer ring(1000);

    CHECK(ring.valid());
    CHECK(ring.capacity() == 1024);                 // rounded up, mask is capacity-1
    CHECK(ring.writable() == ring.capacity());
    CHECK(ring.readable() == 0);

    // The realtime path depends on these being atomic machine ops, not locks.
    CHECK(std::atomic<std::size_t>::is_always_lock_free);
    CHECK(std::atomic<std::uint64_t>::is_always_lock_free);

    AudioRingBuffer empty(0);
    CHECK_FALSE(empty.valid());
    CHECK(empty.capacity() == 0);
    CHECK(empty.write(std::array<float, 4>{ 1, 2, 3, 4 }.data(), 4) == 0);
}

TEST_CASE("AudioRingBuffer: FIFO order survives wrap-around", "[audio][ring]")
{
    AudioRingBuffer ring(8);
    REQUIRE(ring.capacity() == 8);

    std::array<float, 8> block{};
    for (std::size_t i = 0; i < block.size(); ++i)
        block[i] = static_cast<float>(i);

    // Push past the end so the data wraps, then read it all back in order.
    std::array<float, 5> first{};
    for (std::size_t i = 0; i < first.size(); ++i)
        first[i] = static_cast<float>(100 + i);

    REQUIRE(ring.write(first.data(), first.size()) == 5);
    REQUIRE(ring.read(first.data(), 3) == 3);              // free 3 slots
    CHECK(first[0] == 100.0f);
    CHECK(first[2] == 102.0f);

    std::array<float, 4> second{ 200.0f, 201.0f, 202.0f, 203.0f };
    REQUIRE(ring.write(second.data(), second.size()) == 4);   // wraps

    std::array<float, 6> out{};
    REQUIRE(ring.read(out.data(), out.size()) == 6);
    CHECK(out[0] == 103.0f);
    CHECK(out[1] == 104.0f);
    CHECK(out[2] == 200.0f);
    CHECK(out[5] == 203.0f);
    CHECK(ring.droppedFrames() == 0);
}

TEST_CASE("AudioRingBuffer: a full ring drops the newest frames and counts them",
          "[audio][ring][honesty]")
{
    AudioRingBuffer ring(4);
    REQUIRE(ring.capacity() == 4);

    std::array<float, 4> full{ 1.0f, 2.0f, 3.0f, 4.0f };
    CHECK(ring.write(full.data(), full.size()) == 4);

    std::array<float, 3> extra{ 5.0f, 6.0f, 7.0f };
    CHECK(ring.write(extra.data(), extra.size()) == 0);      // never blocks
    CHECK(ring.droppedFrames() == 3);

    // The producer kept the data it could fit; nothing of what is stored changed.
    std::array<float, 4> out{};
    CHECK(ring.read(out.data(), out.size()) == 4);
    CHECK(out[0] == 1.0f);
    CHECK(out[3] == 4.0f);
}

TEST_CASE("AudioRingBuffer: readOrSilence pads with silence and counts the shortfall",
          "[audio][ring][honesty]")
{
    AudioRingBuffer ring(16);

    std::array<float, 4> two{ 8.0f, 9.0f, 0.0f, 0.0f };
    CHECK(ring.write(two.data(), 2) == 2);

    std::array<float, 8> out{};
    out.fill(-1.0f);

    CHECK(ring.readOrSilence(out.data(), out.size()) == 2);
    CHECK(out[0] == 8.0f);
    CHECK(out[1] == 9.0f);

    for (std::size_t i = 2; i < out.size(); ++i)
        CHECK(out[i] == 0.0f);                                // silence, never garbage

    CHECK(ring.underrunFrames() == 6);                        // frames that were missing

    std::array<float, 4> empty{};
    CHECK(ring.readOrSilence(empty.data(), empty.size()) == 0);
    CHECK(ring.underrunFrames() == 10);
}

TEST_CASE("AudioRingBuffer: reset returns an idle buffer to empty", "[audio][ring]")
{
    AudioRingBuffer ring(8);
    std::array<float, 8> data{};
    data.fill(1.0f);

    REQUIRE(ring.write(data.data(), 8) == 8);
    ring.reset();

    CHECK(ring.readable() == 0);
    CHECK(ring.writable() == 8);
    CHECK(ring.droppedFrames() == 0);
    CHECK(ring.underrunFrames() == 0);
}

// ------------------------------------------------------------------- jitter buffer

TEST_CASE("AudioJitterBuffer: nothing plays before the pre-roll is filled",
          "[audio][jitter][spec]")
{
    AudioJitterBuffer jitter(2048, 960);      // 20 ms at 48 kHz

    CHECK(jitter.priming());

    std::array<float, 480> block{};
    block.fill(0.5f);

    std::array<float, 480> out{};
    out.fill(-1.0f);

    // One block of a 960-frame target: still pre-rolling, so silence out.
    REQUIRE(jitter.write(block.data(), block.size()) == block.size());
    CHECK(jitter.readOrSilence(out.data(), out.size()) == 0);
    CHECK(out[0] == 0.0f);
    CHECK(jitter.priming());
    CHECK(jitter.primingFrames() == 480);
    CHECK(jitter.underrunEvents() == 0);      // pre-roll is not an error

    // Second block reaches the target: playback starts with the oldest data.
    REQUIRE(jitter.write(block.data(), block.size()) == block.size());
    CHECK_FALSE(jitter.priming());
    REQUIRE(jitter.readOrSilence(out.data(), out.size()) == out.size());
    CHECK(out[0] == 0.5f);
}

TEST_CASE("AudioJitterBuffer: running dry is one underrun and re-primes",
          "[audio][jitter][honesty]")
{
    AudioJitterBuffer jitter(1024, 480);

    std::array<float, 480> block{};
    block.fill(0.25f);

    REQUIRE(jitter.write(block.data(), block.size()) == block.size());
    REQUIRE(jitter.readOrSilence(block.data(), block.size()) == block.size());
    CHECK_FALSE(jitter.priming());

    // The network fell behind: silence, one counted event, and pre-roll again.
    std::array<float, 480> out{};
    out.fill(3.0f);
    CHECK(jitter.readOrSilence(out.data(), out.size()) == 0);
    CHECK(out[0] == 0.0f);
    CHECK(jitter.underrunEvents() == 1);
    CHECK(jitter.priming());

    // Feeding it again re-primes rather than playing a trickle.
    REQUIRE(jitter.write(out.data(), out.size()) == out.size());
    CHECK_FALSE(jitter.priming());
    CHECK(jitter.readOrSilence(out.data(), out.size()) == out.size());
    CHECK(jitter.underrunEvents() == 1);
}

TEST_CASE("AudioJitterBuffer: a full buffer drops incoming frames, never blocks",
          "[audio][jitter][honesty]")
{
    AudioJitterBuffer jitter(1024, 480);

    std::array<float, 480> block{};
    block.fill(0.1f);

    for (int attempt = 0; attempt < 4; ++attempt)
        jitter.write(block.data(), block.size());

    CHECK(jitter.available() <= jitter.capacity());
    CHECK(jitter.droppedFrames() > 0);          // the producer was faster than playback
}

TEST_CASE("AudioJitterBuffer: raising the target re-primes, lowering it accepts what is buffered",
          "[audio][jitter]")
{
    AudioJitterBuffer jitter(4096, 480);

    std::array<float, 480> block{};
    block.fill(0.2f);
    std::array<float, 480> out{};

    // Prime once and drain, so the buffer starts empty for the target changes below.
    REQUIRE(jitter.write(block.data(), block.size()) == block.size());
    CHECK_FALSE(jitter.priming());
    REQUIRE(jitter.readOrSilence(out.data(), out.size()) == block.size());

    // Asking for more protection than exists means pre-rolling again: the operator
    // chose latency, and silence is the honest output while it is being built up.
    jitter.setTargetFrames(2400);
    CHECK(jitter.priming());
    CHECK(jitter.readOrSilence(out.data(), out.size()) == 0);

    REQUIRE(jitter.write(block.data(), block.size()) == block.size());
    REQUIRE(jitter.write(block.data(), block.size()) == block.size());
    CHECK(jitter.priming());                     // 960 of 2400 buffered: still waiting

    // Lowering the target to what is already stored must not stall the callback for
    // no reason: readiness comes from the content, not from a timer.
    jitter.setTargetFrames(480);
    CHECK_FALSE(jitter.priming());
    CHECK(jitter.readOrSilence(out.data(), out.size()) == block.size());
}

// --------------------------------------------------------------------------- meters

TEST_CASE("LevelMeter: peak, rms and clipping for a known signal", "[audio][meter]")
{
    LevelMeter meter;

    std::array<float, 100> sine{};

    for (std::size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.5f * std::sin(static_cast<float>(i) * 0.25f);

    meter.measure(sine.data(), sine.size());

    const float peak = meter.peakLinear();
    CHECK(peak > 0.45f);
    CHECK(peak <= 0.5f);

    // A sine's RMS is peak/sqrt(2) over a full number of cycles; this window is close.
    const float rms = meter.rmsLinear();
    CHECK(rms > 0.2f);
    CHECK(rms < 0.5f);

    CHECK(meter.blocksMeasured() == 1);
    CHECK(meter.clipFrames() == 0);
    CHECK(meter.signalPresent());
}

TEST_CASE("LevelMeter: silence is not signal, clipping is counted", "[audio][meter]")
{
    LevelMeter meter;

    std::array<float, 64> silence{};
    silence.fill(0.0f);
    meter.measure(silence.data(), silence.size());
    CHECK_FALSE(meter.signalPresent());
    CHECK(meter.peakLinear() == 0.0f);

    std::array<float, 64> loud{};
    loud.fill(1.25f);                            // already over full scale when it arrives
    meter.measure(loud.data(), loud.size());
    CHECK(meter.clipFrames() == 64);
    CHECK(meter.signalPresent());
}

TEST_CASE("LevelMeter: non-finite samples do not poison the meter", "[audio][meter][robustness]")
{
    LevelMeter meter;

    std::array<float, 8> mixed{ 0.1f, std::nanf("1"), 0.2f, std::numeric_limits<float>::infinity(),
                                0.3f, 0.0f, 0.0f, 0.0f };

    meter.measure(mixed.data(), mixed.size());

    CHECK(std::isfinite(meter.peakLinear()));
    CHECK(std::isfinite(meter.rmsLinear()));
    CHECK(meter.clipFrames() == 2);              // the two non-finite samples
    CHECK(meter.blocksMeasured() == 1);
}

TEST_CASE("linearToDb: full scale is 0 dB and silence is the floor", "[audio][meter]")
{
    CHECK(linearToDb(1.0f) == Catch::Approx(0.0f));
    CHECK(linearToDb(0.5f) == Catch::Approx(-6.0206f).margin(0.001));
    CHECK(linearToDb(0.001f) == Catch::Approx(-60.0f).margin(0.001));
    CHECK(linearToDb(0.0f) == -120.0f);
    CHECK(linearToDb(std::nanf("")) == -120.0f);
}

// --------------------------------------------------------------------------- engine

TEST_CASE("AudioEngine: activation builds the pipeline from the device geometry",
          "[audio][engine][pipeline]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    CHECK(engine.pipelineReady());
    CHECK(engine.inputChannels() == backend.capabilities().inputChannels);
    CHECK(engine.outputChannels() == backend.capabilities().outputChannels);
    CHECK(engine.inputRing(0) != nullptr);
    CHECK(engine.outputJitter(0) != nullptr);
    CHECK(engine.inputMeter(0) != nullptr);
    CHECK(engine.outputMeter(0) != nullptr);

    // Out-of-range channels are nullptr, not undefined behaviour.
    CHECK(engine.inputRing(-1) == nullptr);
    CHECK(engine.inputRing(engine.inputChannels()) == nullptr);

    // Sized for the device, not guessed: two seconds at 48 kHz, rounded to a power
    // of two, and the jitter buffer must hold its pre-roll with slack.
    CHECK(engine.inputRing(0)->capacity() >= 48000 * 2);
    CHECK(engine.outputJitter(0)->capacity() >= engine.outputJitter(0)->targetFrames());
    CHECK(engine.outputJitter(0)->targetFrames() == 48000 * 120 / 1000);   // config default

    engine.deactivate();
    CHECK_FALSE(engine.pipelineReady());
    CHECK(engine.inputRing(0) == nullptr);
}

TEST_CASE("AudioEngine: without a consumer the input is measured but not forwarded",
          "[audio][engine][pipeline][honesty]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    std::array<float, static_cast<std::size_t>(kFrames)> in{};
    in.fill(0.75f);
    std::array<float, static_cast<std::size_t>(kFrames)> out{};
    out.fill(-1.0f);

    const float* inPointers[] = { in.data() };
    float* outPointers[] = { out.data() };

    engine.processAudio(inPointers, outPointers, kFrames);

    CHECK(engine.inputFramesCaptured() == static_cast<std::uint64_t>(kFrames));
    CHECK(engine.inputFramesForwarded() == 0);
    CHECK(engine.inputFramesNotForwarded() == static_cast<std::uint64_t>(kFrames));
    CHECK(engine.inputRing(0)->readable() == 0);

    // Nothing was dropped by a full buffer, so the overrun counter must stay clean.
    CHECK(engine.overrunEvents() == 0);

    // Microphone audio must never appear on the output.
    for (const float sample : out)
        CHECK(sample == 0.0f);

    CHECK(engine.outputMeter(0)->peakLinear() == 0.0f);
    CHECK(engine.inputMeter(0)->peakLinear() == 0.75f);

    engine.deactivate();
}

TEST_CASE("AudioEngine: input frames are measured and underruns are reported to diagnostics",
          "[audio][engine][pipeline]")
{
    NullAudioBackend backend;
    DiagnosticsManager diagnostics;

    AudioEngine instrumented(&diagnostics);
    std::string error;
    REQUIRE(instrumented.activate(backend, request(), error));
    REQUIRE(backend.renderOneBlock());

    CHECK(instrumented.inputFramesCaptured() == static_cast<std::uint64_t>(kFrames));
    CHECK(instrumented.outputSilenceFrames() == static_cast<std::uint64_t>(kFrames));
    CHECK(instrumented.underrunEvents() >= 1);
    CHECK(diagnostics.counters().underruns >= 1);
    CHECK(diagnostics.counters().overruns == 0);

    instrumented.deactivate();
}

TEST_CASE("AudioEngine: setJitterBufferMs retargets the live buffer", "[audio][engine][pipeline]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    engine.setJitterBufferMs(20);
    CHECK(engine.jitterBufferMs() == 20);
    CHECK(engine.outputJitter(0)->targetFrames() == 48000 * 20 / 1000);

    // SPEC suggests 20..500 ms; a hand-edited 5000 is clamped, not honoured.
    engine.setJitterBufferMs(5000);
    CHECK(engine.jitterBufferMs() == 500);

    engine.deactivate();
}

TEST_CASE("AudioEngine: pipeline counters survive deactivation", "[audio][engine][pipeline][diagnostics]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();          // no one drains: the ring will overflow

    std::array<float, static_cast<std::size_t>(kFrames)> in{};
    std::array<float, static_cast<std::size_t>(kFrames)> out{};
    in.fill(0.5f);

    const float* inPointers[] = { in.data() };
    float* outPointers[] = { out.data() };

    for (int block = 0; block < 400; ++block)
        engine.processAudio(inPointers, outPointers, kFrames);

    const std::uint64_t captured = engine.inputFramesCaptured();
    const std::uint64_t forwarded = engine.inputFramesForwarded();
    const std::uint64_t dropped = engine.inputRingDroppedFrames();

    REQUIRE(dropped > 0);                                   // the overflow did happen
    REQUIRE(captured == forwarded + dropped);               // and was accounted for

    // deactivate() destroys the buffers. The counters belong to the engine, so the
    // operator-facing numbers must not silently fall back to zero afterwards.
    engine.deactivate();

    CHECK(engine.inputFramesCaptured() == captured);
    CHECK(engine.inputFramesForwarded() == forwarded);
    CHECK(engine.inputRingDroppedFrames() == dropped);
    CHECK(engine.underrunEvents() > 0);
    CHECK(engine.overrunEvents() > 0);
    CHECK(engine.blockCount() == 400);
}

// -------------------------------------------------------------------------- loopback

TEST_CASE("AudioLoopback: refuses to start without a running pipeline", "[audio][loopback]")
{
    AudioEngine engine;
    AudioLoopback loopback(engine);
    std::string error;

    CHECK_FALSE(loopback.start(error));
    CHECK(error.find("running audio pipeline") != std::string::npos);
    CHECK_FALSE(loopback.running());
}

TEST_CASE("AudioLoopback: a ramp written at the input comes back at the output",
          "[audio][loopback][pipeline]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    AudioLoopback loopback(engine);
    loopback.setPollIntervalMs(1);
    REQUIRE(loopback.start(error));
    REQUIRE(engine.inputConsumerAttached());

    // Feed a counter ramp through the callback path and collect whatever the output
    // side ends up playing. The pump is driven in wall-clock time on purpose: the
    // worker thread has to actually run, and a tight loop that finishes in tens of
    // microseconds would make this test depend on the scheduler rather than on the
    // pipeline (it did: it passed in Debug and failed in Release).
    Ramp ramp;
    std::vector<float> played;
    std::array<float, static_cast<std::size_t>(kFrames)> in{};
    std::array<float, static_cast<std::size_t>(kFrames)> out{};
    const float* inPointers[] = { in.data() };
    float* outPointers[] = { out.data() };

    constexpr int kPumpBlocks = 240;                 // 2.4 s of audio

    for (int block = 0; block < kPumpBlocks; ++block)
    {
        ramp.fill(in.data(), in.size());
        engine.processAudio(inPointers, outPointers, kFrames);

        for (std::size_t i = 0; i < out.size(); ++i)
            if (out[i] != 0.0f)
                played.push_back(out[i]);

        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        if (played.size() > static_cast<std::size_t>(kFrames))
            break;                                   // already proved the round trip
    }

    loopback.stop();
    CHECK_FALSE(loopback.running());
    CHECK_FALSE(engine.inputConsumerAttached());

    // The pre-roll (120 ms by default) means the first blocks are silence; what
    // matters is that the input reappeared at the output, in order and unmodified.
    REQUIRE_FALSE(played.empty());
    CHECK(loopback.transferredFrames() > 0);
    CHECK(engine.inputFramesForwarded() > 0);

    for (std::size_t i = 1; i < played.size(); ++i)
        CHECK(played[i] >= played[i - 1]);           // monotone: nothing reordered

    // Every sample that reached the output came from the input, unchanged: the ramp is
    // strictly increasing, so a value here must be one the Ramp produced.
    CHECK(played.front() >= 1.0f);
    CHECK(played.back() <= static_cast<float>(ramp.produced()));
}

TEST_CASE("AudioLoopback: stopping it leaves the output on silence, never stale audio",
          "[audio][loopback][safety]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    {
        AudioLoopback loopback(engine);
        REQUIRE(loopback.start(error));

        std::array<float, static_cast<std::size_t>(kFrames)> in{};
        in.fill(0.9f);
        std::array<float, static_cast<std::size_t>(kFrames)> out{};
        const float* inPointers[] = { in.data() };
        float* outPointers[] = { out.data() };

        for (int block = 0; block < 40; ++block)
            engine.processAudio(inPointers, outPointers, kFrames);
    }

    // Consumer gone: the rings stop being written, the jitter drains, then silence.
    std::array<float, static_cast<std::size_t>(kFrames)> in{};
    in.fill(0.9f);
    std::array<float, static_cast<std::size_t>(kFrames)> out{};
    const float* inPointers[] = { in.data() };
    float* outPointers[] = { out.data() };

    bool heardSilence = false;

    for (int block = 0; block < 200 && !heardSilence; ++block)
    {
        out.fill(-1.0f);
        engine.processAudio(inPointers, outPointers, kFrames);

        heardSilence = true;
        for (const float sample : out)
            if (sample != 0.0f)
                heardSilence = false;
    }

    CHECK(heardSilence);
    CHECK(engine.inputFramesForwarded() == static_cast<std::uint64_t>(40 * kFrames));

    engine.deactivate();
}

TEST_CASE("AudioEngine: stress - device callback and loopback thread against the same rings",
          "[audio][engine][stress][realtime]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    AudioLoopback loopback(engine);
    loopback.setPollIntervalMs(1);
    REQUIRE(loopback.start(error));

    constexpr int kBlocks = 5000;                  // 50 s of 10 ms blocks of bookkeeping
    Ramp ramp;

    std::array<float, static_cast<std::size_t>(kFrames)> in{};
    std::array<float, static_cast<std::size_t>(kFrames)> out{};
    const float* inPointers[] = { in.data() };
    float* outPointers[] = { out.data() };

    std::uint64_t heard = 0;
    float previous = -1.0f;
    bool ordered = true;

    for (int block = 0; block < kBlocks; ++block)
    {
        ramp.fill(in.data(), in.size());
        engine.processAudio(inPointers, outPointers, kFrames);

        for (std::size_t i = 0; i < out.size(); ++i)
        {
            if (out[i] == 0.0f)
                continue;

            if (out[i] < previous)
                ordered = false;

            previous = out[i];
            ++heard;
        }
    }

    loopback.stop();
    engine.deactivate();

    CHECK(ordered);                                 // whatever arrived stayed in order

    // The accounting must close, in every case: frames the callback saw are either
    // forwarded into the rings or explicitly dropped. A synthetic burst of 5000 blocks
    // is far faster than a real device, so the ring may legitimately overflow - what
    // must never happen is audio that disappeared without being counted.
    CHECK(engine.inputFramesCaptured() == engine.inputFramesForwarded() + engine.inputRingDroppedFrames());
    CHECK(engine.malformedCallbacks() == 0);
    CHECK(engine.inputFramesForwarded() > 0);
    CHECK(heard > 0);
}
