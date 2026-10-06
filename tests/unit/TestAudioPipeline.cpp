#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
#include "Audio/GainStage.h"
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

    CHECK(engine.inputSamplesCaptured() == static_cast<std::uint64_t>(kFrames));
    CHECK(engine.inputSamplesForwarded() == 0);
    CHECK(engine.inputSamplesNotForwarded() == static_cast<std::uint64_t>(kFrames));
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

    CHECK(instrumented.inputSamplesCaptured() == static_cast<std::uint64_t>(kFrames));
    CHECK(instrumented.outputSilenceSamples() == static_cast<std::uint64_t>(kFrames));
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

    const std::uint64_t captured = engine.inputSamplesCaptured();
    const std::uint64_t forwarded = engine.inputSamplesForwarded();
    const std::uint64_t dropped = engine.inputRingDroppedSamples();

    REQUIRE(dropped > 0);                                   // the overflow did happen
    REQUIRE(captured == forwarded + dropped);               // and was accounted for

    // deactivate() destroys the buffers. The counters belong to the engine, so the
    // operator-facing numbers must not silently fall back to zero afterwards.
    engine.deactivate();

    CHECK(engine.inputSamplesCaptured() == captured);
    CHECK(engine.inputSamplesForwarded() == forwarded);
    CHECK(engine.inputRingDroppedSamples() == dropped);
    CHECK(engine.underrunEvents() > 0);
    CHECK(engine.overrunEvents() > 0);
    CHECK(engine.blockCount() == 400);
}

// ----------------------------------------------------------------------------- gain

namespace {

/// Mono driver for the engine: fills a constant input block, runs one callback, hands
/// back what the output side played.
struct MonoDrive
{
    std::vector<float> in;
    std::vector<float> out;

    explicit MonoDrive(int frames)
        : in(static_cast<std::size_t>(frames), 0.0f), out(static_cast<std::size_t>(frames), 0.0f)
    {
    }

    void run(AudioEngine& engine, float level, int frames)
    {
        std::fill(in.begin(), in.end(), level);
        std::fill(out.begin(), out.end(), -0.125f);

        const float* inputPointers[] = { in.data() };
        float* outputPointers[] = { out.data() };

        engine.processAudio(inputPointers, outputPointers, frames);
    }
};

float coefficientFor(float gainDb)
{
    return audio::GainStage::dbToLinear(gainDb);
}

} // namespace

TEST_CASE("AudioEngine: input gain is applied before the ring buffer", "[audio][engine][gain][pipeline]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    // The level is set before activate(): buildPipeline() lands the coefficient on the
    // request instead of gliding into it, so every frame in the ring is at the target and
    // the assertion is about WHERE the gain sits. The glide itself is tested in
    // TestGainStage.cpp, and below in "a gain change glides through the pipeline".
    engine.setInputGainDb(-6.0f);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    MonoDrive drive(kFrames);
    const float expectedLevel = 0.5f * coefficientFor(-6.0f);

    for (int block = 0; block < 3; ++block)
        drive.run(engine, 0.5f, kFrames);

    std::vector<float> received(static_cast<std::size_t>(kFrames), 0.0f);
    const std::size_t taken = engine.inputRing(0)->read(received.data(), received.size());

    // What the translator is handed is the operator's level, not the console's. SPEC
    // "Audio Ring Buffer" puts Input Gain between the callback and the ring.
    REQUIRE(taken > 0);

    for (std::size_t i = 1; i < taken; ++i)
        CHECK(std::fabs(received[i] - expectedLevel) < 1e-5f);

    engine.deactivate();
}

TEST_CASE("AudioEngine: the gain is in effect from the very first block", "[audio][engine][gain][startup]")
{
    // The operator set -12 dB in settings. The first callback after the device started
    // must already play it: a glide into the level at start-up would be audio that
    // nobody asked for, and it would land in the translator's stream.
    AudioEngine engine;
    NullAudioBackend backend;

    engine.setInputGainDb(-12.0f);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    MonoDrive drive(kFrames);
    drive.run(engine, 0.5f, kFrames);

    std::vector<float> received(static_cast<std::size_t>(kFrames), 0.0f);
    REQUIRE(engine.inputRing(0)->read(received.data(), received.size()) == received.size());

    const float expectedLevel = 0.5f * coefficientFor(-12.0f);

    CHECK(std::fabs(received.front() - expectedLevel) < 1e-5f);
    CHECK(std::fabs(received.back() - expectedLevel) < 1e-5f);

    engine.deactivate();
}

TEST_CASE("AudioEngine: the input meter follows the gain", "[audio][engine][gain][meters]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    MonoDrive drive(kFrames);

    drive.run(engine, 0.25f, kFrames);
    REQUIRE(engine.inputMeter(0) != nullptr);
    const float atUnity = engine.inputMeter(0)->peakLinear();
    CHECK(std::fabs(atUnity - 0.25f) < 1e-6f);

    engine.setInputGainDb(12.0f);

    for (int block = 0; block < 4; ++block)
        drive.run(engine, 0.25f, kFrames);

    // A knob that does not move the meter looks broken to an operator. The meter reads the
    // post-gain block for exactly that reason, and the stage separately reports what
    // arrived at full scale, so nothing is hidden by turning down.
    const float afterGainUp = engine.inputMeter(0)->peakLinear();
    CHECK(std::fabs(afterGainUp - 0.25f * coefficientFor(12.0f)) < 1e-5f);

    engine.setInputGainDb(-24.0f);

    for (int block = 0; block < 4; ++block)
        drive.run(engine, 0.25f, kFrames);

    const float afterGainDown = engine.inputMeter(0)->peakLinear();
    CHECK(afterGainDown < atUnity);
    CHECK(std::fabs(afterGainDown - 0.25f * coefficientFor(-24.0f)) < 1e-5f);

    engine.deactivate();
}

TEST_CASE("AudioEngine: attenuation cannot hide clipping that came from the device",
          "[audio][engine][gain][clipping][honesty]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    engine.setInputGainDb(-24.0f);   // the operator's reflex when something sounds loud

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    MonoDrive drive(kFrames);

    for (int block = 0; block < 4; ++block)
        drive.run(engine, 1.5f, kFrames);   // already over full scale on the way in

    // The count says the console fed clipped audio, and it stays true however much the
    // application turns its own gain down.
    CHECK(engine.inputClippedSamples() == 4 * static_cast<std::uint64_t>(kFrames));
    CHECK(engine.inputGainClippedSamples() == 0);   // ... and after -24 dB nothing is at full scale
    CHECK(engine.takeInputClipIndicator());        // the indicator still lights

    // The output side has its own number, because it answers a different question: what
    // is being sent to the audience.
    CHECK(engine.outputGainClippedSamples() == 0);

    engine.deactivate();
}

TEST_CASE("AudioEngine: output gain sits after the jitter buffer, on the way to the wire",
          "[audio][engine][gain][pipeline]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    engine.setJitterBufferMs(0);      // no pre-roll: this test is about the level, not the delay

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    engine.setOutputGainDb(6.0f);

    std::vector<float> translated(static_cast<std::size_t>(kFrames), 0.2f);

    MonoDrive drive(kFrames);

    for (int block = 0; block < 3; ++block)
    {
        // The translated stream arrives from the network side (loopback today, production later
        // later): the engine never lets the microphone take this path.
        engine.outputJitter(0)->write(translated.data(), translated.size());
        drive.run(engine, 0.0f, kFrames);
    }

    const float expected = 0.2f * coefficientFor(6.0f);

    for (const float sample : drive.out)
        CHECK(std::fabs(sample - expected) < 1e-5f);

    // The meter and the wire agree: what the operator sees is what the audience hears.
    CHECK(std::fabs(engine.outputMeter(0)->peakLinear() - expected) < 1e-5f);
    CHECK(engine.outputSilenceSamples() == 0);

    engine.deactivate();
}

TEST_CASE("AudioEngine: input mute and output mute are different actions",
          "[audio][engine][gain][safety]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    engine.setJitterBufferMs(0);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    MonoDrive drive(kFrames);
    std::vector<float> translated(static_cast<std::size_t>(kFrames), 0.2f);

    const auto pump = [&](int blocks)
    {
        for (int block = 0; block < blocks; ++block)
        {
            engine.outputJitter(0)->write(translated.data(), translated.size());
            drive.run(engine, 0.5f, kFrames);
        }
    };

    // Drains the whole ring and hands back the frames that were written LAST. The ring is
    // a FIFO: reading it right after a change would return audio recorded before the
    // change, and the 20 ms glide means the newest block is the first one fully at the
    // new level. That is the property worth asserting, so this takes the tail.
    const auto drainRingToTail = [&]()
    {
        std::vector<float> all;
        std::vector<float> piece(static_cast<std::size_t>(kFrames), 0.0f);

        for (;;)
        {
            const std::size_t taken = engine.inputRing(0)->read(piece.data(), piece.size());

            if (taken == 0)
                break;

            all.insert(all.end(), piece.begin(), piece.begin() + static_cast<std::ptrdiff_t>(taken));
        }

        REQUIRE(all.size() >= static_cast<std::size_t>(kFrames));

        return std::vector<float>(all.end() - static_cast<std::ptrdiff_t>(kFrames), all.end());
    };

    pump(4);

    // Mute the input: the translator must go quiet...
    engine.setInputMuted(true);
    pump(4);                                   // 1920 samples: the glide has landed

    for (const float sample : drainRingToTail())
        CHECK(sample == 0.0f);

    // ... while the audience keeps hearing the translation that is already in the buffer.
    // This is why one "mute" would be the wrong control to have.
    for (const float sample : drive.out)
        CHECK(std::fabs(sample - 0.2f) < 1e-5f);

    CHECK(engine.inputMuted());
    CHECK_FALSE(engine.outputMuted());

    // Now the other way round: unmute the input, mute the output.
    engine.setInputMuted(false);
    engine.setOutputMuted(true);
    pump(4);

    const std::vector<float> fed = drainRingToTail();

    for (const float sample : fed)
        CHECK(std::fabs(sample - 0.5f) < 1e-5f);      // the translator is fed again

    for (const float sample : drive.out)
        CHECK(sample == 0.0f);                        // the room is silent

    engine.deactivate();
}

TEST_CASE("AudioEngine: gain settings and clipping history survive a device restart",
          "[audio][engine][gain][restart]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    engine.setInputGainDb(-6.0f);
    engine.setOutputGainDb(3.0f);

    MonoDrive drive(kFrames);
    drive.run(engine, 1.2f, kFrames);          // over full scale: clipped on the way in

    engine.deactivate();

    // The numbers an operator set and the facts the pipeline measured are not device
    // geometry: a restart of the audio path must not reset the room's levels or forget
    // that it clipped.
    CHECK(engine.inputGainDb() == -6.0f);
    CHECK(engine.outputGainDb() == 3.0f);
    CHECK(engine.inputClippedSamples() == static_cast<std::uint64_t>(kFrames));

    NullAudioBackend another;
    REQUIRE(engine.activate(another, request(), error));
    engine.attachInputConsumer();

    CHECK(engine.inputGainDb() == -6.0f);

    std::vector<float> received(static_cast<std::size_t>(kFrames), 0.0f);
    drive.run(engine, 0.5f, kFrames);

    REQUIRE(engine.inputRing(0)->read(received.data(), received.size()) > 0);

    // ... and the new pipeline applies the level it remembered, from the first block.
    const float expected = 0.5f * coefficientFor(-6.0f);

    for (const float sample : received)
        CHECK(std::fabs(sample - expected) < 1e-5f);

    CHECK(engine.inputClippedSamples() == static_cast<std::uint64_t>(kFrames));   // not reset, not doubled
    CHECK(engine.oversizedCallbacks() == 0);

    engine.deactivate();
}

TEST_CASE("AudioEngine: a block bigger than one gain chunk is chunked, not overrun",
          "[audio][engine][gain][oversize]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    engine.setInputGainDb(-6.0f);          // before activate: no glide inside the assertion

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    // Deliberately larger than the preallocated chunk: the engine has to do it in pieces
    // and say that it happened, instead of writing past the scratch (the defect that a
    // badly sized buffer exposed earlier).
    const int frames = 5000;

    MonoDrive drive(frames);
    drive.run(engine, 0.5f, frames);

    CHECK(engine.oversizedCallbacks() == 1);
    CHECK(engine.inputSamplesCaptured() == static_cast<std::uint64_t>(frames));
    CHECK(engine.inputSamplesForwarded() == static_cast<std::uint64_t>(frames));
    CHECK(engine.inputRingDroppedSamples() == 0);
    CHECK(engine.malformedCallbacks() == 0);

    std::vector<float> received(static_cast<std::size_t>(frames), 0.0f);
    REQUIRE(engine.inputRing(0)->read(received.data(), received.size()) == static_cast<std::size_t>(frames));

    // Every sample was gained, in both chunks: a silent tail here would mean the second
    // chunk never happened.
    const float expected = 0.5f * coefficientFor(-6.0f);

    CHECK(std::fabs(received.front() - expected) < 1e-5f);
    CHECK(std::fabs(received[4095] - expected) < 1e-5f);
    CHECK(std::fabs(received[4096] - expected) < 1e-5f);
    CHECK(std::fabs(received.back() - expected) < 1e-5f);

    engine.deactivate();
}

TEST_CASE("AudioEngine: refused and clamped gain requests are counted, not swallowed",
          "[audio][engine][gain][policy]")
{
    AudioEngine engine;

    CHECK(engine.inputGainDb() == 0.0f);
    CHECK(engine.outputGainDb() == 0.0f);
    CHECK(engine.gainRampMs() == audio::GainStage::kDefaultRampMs);

    engine.setInputGainDb(96.0f);                       // beyond the window
    CHECK(engine.inputGainDb() == audio::GainStage::kMaxGainDb);
    CHECK(engine.gainRequestsClamped() == 1);
    CHECK(engine.gainRequestsRejected() == 0);

    engine.setOutputGainDb(std::nanf("1"));            // not a level at all
    CHECK(engine.outputGainDb() == 0.0f);              // previous value kept
    CHECK(engine.gainRequestsRejected() == 1);
    CHECK(engine.gainRequestsClamped() == 1);

    engine.setOutputGainDb(std::numeric_limits<float>::infinity());
    CHECK(engine.gainRequestsRejected() == 2);

    engine.setGainRampMs(100000);
    CHECK(engine.gainRampMs() == audio::GainStage::kMaxRampMs);

    // No device needed for the state, and the applied readout reports the request until a
    // pipeline exists to glide in the callback.
    CHECK(std::fabs(engine.appliedInputGainDb() - audio::GainStage::kMaxGainDb) < 0.02f);
    CHECK(engine.appliedOutputGainDb() == 0.0f);
}

TEST_CASE("AudioEngine: both trims compose along the pipeline in SPEC order",
          "[audio][engine][gain][pipeline]")
{
    // The transport is driven by hand here (ring -> jitter, exactly what AudioLoopback and
    // the production streaming worker do) so the assertion is about where the gain stages
    // sit in the signal chain, not about thread scheduling.
    AudioEngine engine;
    NullAudioBackend backend;

    engine.setJitterBufferMs(20);      // SPEC's minimum pre-roll; the level is what is under test

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    engine.setInputGainDb(-6.0f);
    engine.setOutputGainDb(6.0f);

    std::vector<float> transport(static_cast<std::size_t>(kFrames), 0.0f);
    MonoDrive drive(kFrames);

    for (int block = 0; block < 40; ++block)
    {
        drive.run(engine, 0.25f, kFrames);

        // What the consumer of the input ring would do: hand the (already gained) audio
        // to the translation side, and put the returned audio back for the output.
        const std::size_t available = engine.inputRing(0)->read(transport.data(), transport.size());

        if (available > 0)
            engine.outputJitter(0)->write(transport.data(), available);
    }

    // -6 dB in, +6 dB out: the trims cancel, and the number that leaves is the number that
    // arrived. That only works out this way because each stage is applied once, in order.
    for (const float sample : drive.out)
        CHECK(std::fabs(sample - 0.25f) < 1e-5f);

    CHECK(engine.inputSamplesForwarded() > 0);
    CHECK(engine.outputGainClippedSamples() == 0);
    CHECK(engine.inputGainClippedSamples() == 0);

    // The same run with the output trim pushed to the ceiling must clip on the way out and
    // say so, rather than deliver a quietly limited signal.
    engine.setOutputGainDb(audio::GainStage::kMaxGainDb);

    for (int block = 0; block < 12; ++block)
    {
        drive.run(engine, 0.25f, kFrames);

        const std::size_t available = engine.inputRing(0)->read(transport.data(), transport.size());

        if (available > 0)
            engine.outputJitter(0)->write(transport.data(), available);
    }

    // The input is still the tame -6 dB version, so the input side reports no clipping...
    CHECK(engine.inputGainClippedSamples() == 0);

    // ... while the wire is over full scale, and the only way it stays finite is that the
    // stage counted every one of those samples.
    CHECK(engine.outputGainClippedSamples() > 0);
    CHECK(engine.takeOutputClipIndicator());

    for (const float sample : drive.out)
        CHECK(std::isfinite(sample));

    engine.deactivate();
}

TEST_CASE("AudioEngine: meters and clipping readouts exist before the pipeline is built",
          "[audio][engine][gain][policy]")
{
    AudioEngine engine;

    // Nothing is running, so every readout has to answer rather than crash: the UI polls
    // these from a timer that does not know whether the device started.
    CHECK(engine.inputMeter(0) == nullptr);
    CHECK(engine.outputMeter(0) == nullptr);
    CHECK(engine.inputClippedSamples() == 0);
    CHECK(engine.inputGainClippedSamples() == 0);
    CHECK(engine.outputGainClippedSamples() == 0);
    CHECK(engine.nonFiniteInputSamples() == 0);
    CHECK_FALSE(engine.takeInputClipIndicator());
    CHECK_FALSE(engine.takeOutputClipIndicator());
    CHECK(engine.appliedInputGainDb() == 0.0f);
    CHECK(engine.appliedOutputGainDb() == 0.0f);
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
    CHECK(engine.inputSamplesForwarded() > 0);

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
    CHECK(engine.inputSamplesForwarded() == static_cast<std::uint64_t>(40 * kFrames));

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
    CHECK(engine.inputSamplesCaptured() == engine.inputSamplesForwarded() + engine.inputRingDroppedSamples());
    CHECK(engine.malformedCallbacks() == 0);
    CHECK(engine.inputSamplesForwarded() > 0);
    CHECK(heard > 0);
}
