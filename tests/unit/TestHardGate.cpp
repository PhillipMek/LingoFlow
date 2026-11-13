#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "Audio/AudioEngine.h"
#include "Audio/AudioJitterBuffer.h"
#include "Audio/AudioRingBuffer.h"
#include "Audio/GainStage.h"
#include "Audio/HardGate.h"
#include "Audio/LevelMeter.h"
#include "Audio/Null/NullAudioBackend.h"
#include "Config/AppConfig.h"
#include "Config/ConfigSchema.h"

using namespace liveai;
using liveai::audio::DeviceRequest;
using liveai::audio::GainStage;
using liveai::audio::HardGate;
using liveai::audio::NullAudioBackend;

namespace {

constexpr int kRate = 48000;
constexpr std::size_t kFrames = 480;   ///< one 10 ms block

/// The window arithmetic the tests quote with: the close ramp lasts
/// kCloseRampMs, the open ramp kOpenRampMs, in samples at 48 kHz.
constexpr std::size_t kCloseRampSamples = static_cast<std::size_t>(kRate) * HardGate::kCloseRampMs / 1000;   // 1920
constexpr std::size_t kOpenRampSamples = static_cast<std::size_t>(kRate) * HardGate::kOpenRampMs / 1000;    // 240

float coefficientFor(float db)
{
    return GainStage::dbToLinear(db);
}

/// A deterministic, varied finite signal: mixed magnitudes and signs so a
/// bit-for-bit pass-through claim tests more than one constant.
std::vector<float> makePattern(std::size_t frames)
{
    std::vector<float> data(frames);

    for (std::size_t i = 0; i < frames; ++i)
    {
        const float magnitude = static_cast<float>((i * 37 % 100) + 1) / 1000.0f;   // 0.001 .. 0.1
        data[i] = (i % 2 == 0) ? magnitude : -magnitude;
    }

    return data;
}

/// Feed one constant block of `frames` samples through one gate.
void processConstant(HardGate& gate, float value, std::size_t frames = kFrames)
{
    std::vector<float> block(frames, value);
    gate.process(block.data(), frames);
}

std::vector<float> runConstant(HardGate& gate, float value, std::size_t frames = kFrames)
{
    std::vector<float> block(frames, value);
    gate.process(block.data(), frames);
    return block;
}

} // namespace

// ------------------------------------------------------------------ stage level

TEST_CASE("HardGate: disabled, every block passes bit-for-bit and nothing is counted",
          "[audio][gate]")
{
    HardGate gate;
    gate.configure(kRate);

    // The default is OFF: an installation that never heard of the gate runs the
    // pipeline it ran before it existed, sample for sample.
    CHECK_FALSE(gate.enabled());

    std::vector<float> original = makePattern(kFrames * 4);
    std::vector<float> passed = original;

    for (std::size_t block = 0; block < 4; ++block)
    {
        gate.process(passed.data() + block * kFrames, kFrames);
    }

    CHECK(std::memcmp(original.data(), passed.data(), original.size() * sizeof(float)) == 0);

    // Bypass is absence, not a decision: no open/closed bookkeeping moves, the
    // readouts say "not gating".
    CHECK(gate.openBlocks() == 0);
    CHECK(gate.closedBlocks() == 0);
    CHECK(gate.closedSamples() == 0);
    CHECK(gate.transitions() == 0);
    CHECK_FALSE(gate.open());
    CHECK(gate.appliedLinear() == 1.0f);
    CHECK(gate.blocksProcessed() == 4);          // the fact that it WAS called still counts
    CHECK(gate.samplesProcessed() == kFrames * 4);
}

TEST_CASE("HardGate: an open gate leaves the block untouched", "[audio][gate]")
{
    HardGate gate;
    gate.configure(kRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-40.0f);

    // makePattern peaks at 0.1 linear (-20 dBFS) - its RMS is a hair above the
    // default-free threshold of -40, so the steady state here is open.
    std::vector<float> original = makePattern(kFrames);
    std::vector<float> passed = original;
    gate.process(passed.data(), kFrames);

    CHECK(gate.open());
    CHECK(gate.appliedLinear() == 1.0f);
    CHECK(std::memcmp(original.data(), passed.data(), original.size() * sizeof(float)) == 0);

    // Steady open is not "multiplied by one", it is not touched at all: the same
    // block a second time measures, decides, and stays bit-for-bit.
    gate.process(passed.data(), kFrames);
    CHECK(std::memcmp(original.data(), passed.data(), original.size() * sizeof(float)) == 0);
    CHECK(gate.openBlocks() == 2);
    CHECK(gate.closedBlocks() == 0);
}

TEST_CASE("HardGate: exactly at the threshold opens; a hair below closes", "[audio][gate]")
{
    // "Reaches or exceeds the threshold" is a >= decision. The margin constant is
    // what keeps the boundary deterministic for a constant-amplitude block whose
    // measured RMS lands an ulp below the float threshold; half a percent below
    // still closes, so the margin is measurement relief, not a behaviour change.
    HardGate gate;
    gate.configure(kRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-40.0f);

    const float at = coefficientFor(-40.0f);
    processConstant(gate, at);
    CHECK(gate.open());

    processConstant(gate, at * 0.995f);   // -0.04 dB below: clearly past the margin
    CHECK_FALSE(gate.open());
}

TEST_CASE("HardGate: closing glides to exact zero over the internal close ramp", "[audio][gate]")
{
    HardGate gate;
    gate.configure(kRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-40.0f);

    // First block above the threshold: fully open, untouched.
    const std::vector<float> above = runConstant(gate, 0.5f);
    CHECK(above.front() == 0.5f);
    CHECK(above.back() == 0.5f);

    // Then below it. The decision is taken from the block it acts on - no lookahead,
    // no delay - but the coefficient travels, it never jumps: 1920 samples (four
    // 480-frame blocks) of glide, landing EXACTLY on 0.
    const double expectedEnd = 1.0 - static_cast<double>(kFrames) / static_cast<double>(kCloseRampSamples);

    const std::vector<float> closing1 = runConstant(gate, 0.001f);
    CHECK(Catch::Approx(closing1.back()).epsilon(1e-3) == 0.001f * static_cast<float>(expectedEnd));

    runConstant(gate, 0.001f);
    runConstant(gate, 0.001f);
    const std::vector<float> settled = runConstant(gate, 0.001f);   // ramp done within this block

    // The fourth below-threshold block crosses the end of the ramp: the coefficient
    // LANDS exactly on 0.0 at the block's last sample (appliedLinear is the landed
    // value), so the glide tail is bounded by the float drift of the ramp itself -
    // not by the signal any more.
    CHECK(gate.appliedLinear() == 0.0f);
    CHECK(std::fabs(settled.back()) < 1e-6f);

    // Every subsequent block starts and ends closed: the steady closed state is the
    // memset, so the WHOLE block is exact zeros - "completely muted" is a value, not
    // an approximation. The counters call every closed block with its full frames.
    const std::vector<float> muted = runConstant(gate, 0.001f);
    for (const float sample : muted)
        CHECK(sample == 0.0f);

    CHECK(gate.closedBlocks() == 5);       // four gliding + one settled
    CHECK(gate.transitions() == 1);        // one run of closing is one transition
    CHECK(gate.closedSamples() == kFrames * 5);
}

TEST_CASE("HardGate: a mute never steps - the click the task forbids", "[audio][gate]")
{
    HardGate gate;
    gate.configure(kRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-40.0f);

    // One continuous stream: a DC 0.001 block run. The gate starts open (configure
    // lands it there), so the first samples after the decision fade from 0.001 * 1.0
    // toward silence. The input has no slew of its own (constant), so EVERY
    // sample-to-sample step in the output is the gate's.
    std::vector<float> stream;

    for (std::size_t block = 0; block < 6; ++block)
    {
        const std::vector<float> out = runConstant(gate, 0.001f);
        stream.insert(stream.end(), out.begin(), out.end());
    }

    float maxStep = 0.0f;

    for (std::size_t i = 1; i < stream.size(); ++i)
        maxStep = std::max(maxStep, std::fabs(stream[i] - stream[i - 1]));

    // An instant mute would step by the full 0.001; the glide bounds it by
    // 0.001 / close-ramp-samples plus float noise.
    INFO("max step: " << maxStep);
    CHECK(maxStep < 0.001f);
    CHECK(stream.back() == 0.0f);

    // And the same in the opening direction: an abrupt 0 -> level jump is a click
    // at the start of speech, so re-opening is bounded by its own (fast but finite)
    // 5 ms ramp instead of a step.
    std::vector<float> reopening = runConstant(gate, 0.5f);
    float openStep = 0.0f;

    for (std::size_t i = 1; i < reopening.size(); ++i)
        openStep = std::max(openStep, std::fabs(reopening[i] - reopening[i - 1]));

    INFO("max opening step: " << openStep);
    CHECK(openStep < 0.5f);   // a step TO 0.5 from 0 would be ~0.5 or more per sample at the seam
    CHECK(reopening.back() == 0.5f);   // the 240-sample open ramp is done inside the 480-frame block
}

TEST_CASE("HardGate: the threshold can be moved while the stream runs", "[audio][gate]")
{
    HardGate gate;
    gate.configure(kRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-90.0f);

    // -90 dBFS: everything with any level passes.
    const std::vector<float> loud = runConstant(gate, 0.001f);   // -60 dBFS
    CHECK(gate.open());
    CHECK(loud.back() == 0.001f);

    // Push the threshold above the live level: the gate closes, completely.
    gate.setThresholdDb(-40.0f);

    for (int block = 0; block < 6; ++block)
    {
        const std::vector<float> muted = runConstant(gate, 0.001f);
        (void)muted;
    }

    CHECK_FALSE(gate.open());
    CHECK(gate.appliedLinear() == 0.0f);

    // Drop it back: the SAME signal is whole again, bit-for-bit, once the open
    // ramp has crossed. One block glides, the next is the untouched stream.
    gate.setThresholdDb(-90.0f);
    runConstant(gate, 0.001f);

    std::vector<float> original(kFrames, 0.001f);
    const std::vector<float> passed = runConstant(gate, 0.001f);
    CHECK(std::memcmp(original.data(), passed.data(), original.size() * sizeof(float)) == 0);
    CHECK(gate.transitions() == 2);
}

TEST_CASE("HardGate: threshold requests are clamped or refused, never swallowed", "[audio][gate]")
{
    HardGate gate;
    CHECK(gate.thresholdDb() == HardGate::kDefaultThresholdDb);

    gate.setThresholdDb(std::nanf("1"));
    CHECK(gate.thresholdDb() == HardGate::kDefaultThresholdDb);   // previous kept
    CHECK(gate.rejectedSets() == 1);
    CHECK(gate.clampedSets() == 0);

    gate.setThresholdDb(1e30f);                                       // huge, but a finite level
    CHECK(gate.thresholdDb() == HardGate::kMaxThresholdDb);           // clamped to the window
    CHECK(gate.clampedSets() == 1);

    gate.setThresholdDb(-1000.0f);
    CHECK(gate.thresholdDb() == HardGate::kMinThresholdDb);
    CHECK(gate.clampedSets() == 2);

    gate.setThresholdDb(std::numeric_limits<float>::infinity());      // not finite: refused again
    CHECK(gate.thresholdDb() == HardGate::kMinThresholdDb);           // previous kept
    CHECK(gate.rejectedSets() == 2);   // the NaN and the infinity, nothing else
}

TEST_CASE("HardGate: disabling while closed glides back to the untouched stream", "[audio][gate]")
{
    HardGate gate;
    gate.configure(kRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-40.0f);

    for (int block = 0; block < 6; ++block)
        runConstant(gate, 0.001f);   // fully closed

    CHECK(gate.appliedLinear() == 0.0f);

    // The operator takes the gate away mid-show. The coefficient is at zero and the
    // stream is live at -12 dBFS: a jump to "bypass" would be the loudest click
    // available. The stage has to walk back up first - then, and only then, is the
    // answer the original input stream, bit-for-bit.
    gate.setEnabled(false);

    std::vector<float> seam;

    for (std::size_t block = 0; block < 2; ++block)
    {
        const std::vector<float> out = runConstant(gate, 0.25f);
        seam.insert(seam.end(), out.begin(), out.end());
    }

    float maxStep = 0.0f;

    for (std::size_t i = 1; i < seam.size(); ++i)
        maxStep = std::max(maxStep, std::fabs(seam[i] - seam[i - 1]));

    INFO("max step at disable: " << maxStep);
    CHECK(maxStep < 0.25f);   // an instant bypass would step from 0.0 to 0.25 within the block

    std::vector<float> original(kFrames, 0.25f);
    const std::vector<float> passed = runConstant(gate, 0.25f);
    CHECK(std::memcmp(original.data(), passed.data(), original.size() * sizeof(float)) == 0);
    CHECK(gate.appliedLinear() == 1.0f);
}

TEST_CASE("HardGate: a closed block is zero even from poisoned samples", "[audio][gate]")
{
    HardGate gate;
    gate.configure(kRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-40.0f);

    std::vector<float> block(kFrames, std::numeric_limits<float>::quiet_NaN());
    gate.process(block.data(), kFrames);   // non-finite contributes silence to the RMS

    for (const float sample : block)
        CHECK(sample == 0.0f);   // the memset closes the door on whatever tried to walk through

    CHECK(gate.nonFiniteSeenFrames() == kFrames);
    CHECK(gate.closedBlocks() == 1);
}

TEST_CASE("HardGate: configure lands the stage open for a fresh run", "[audio][gate]")
{
    HardGate gate;
    gate.configure(kRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-40.0f);

    for (int block = 0; block < 6; ++block)
        runConstant(gate, 0.001f);
    CHECK(gate.appliedLinear() == 0.0f);

    // A device restart re-configures (it does not recreate) the stage. The first
    // block of the new run must not be muted out of the gate's memory: the gate
    // starts as open as it was before the old stream ever arrived, and decides
    // from its own measured blocks again.
    gate.configure(kRate);
    CHECK(gate.appliedLinear() == 1.0f);

    const std::vector<float> first = runConstant(gate, 0.5f);
    CHECK(first.front() == 0.5f);   // no half-faded lead-in
}

TEST_CASE("HardGate: a null buffer is counted, a silent call is neither work nor fault", "[audio][gate]")
{
    HardGate gate;
    gate.configure(kRate);

    gate.process(nullptr, kFrames);
    CHECK(gate.invalidCalls() == 1);

    std::vector<float> block = makePattern(4);
    gate.process(block.data(), 0);
    CHECK(gate.invalidCalls() == 1);   // zero frames is not a contract violation
}

TEST_CASE("HardGate: resetSignalCounters clears the show's facts, not the operator's settings", "[audio][gate]")
{
    HardGate gate;
    gate.configure(kRate);
    gate.setEnabled(true);
    gate.setThresholdDb(-40.0f);

    runConstant(gate, 0.5f);     // open
    for (int block = 0; block < 6; ++block)
        runConstant(gate, 0.001f);
    runConstant(gate, 0.5f);     // open again
    CHECK(gate.closedBlocks() > 0);

    gate.resetSignalCounters();

    CHECK(gate.openBlocks() == 0);
    CHECK(gate.closedBlocks() == 0);
    CHECK(gate.closedSamples() == 0);
    CHECK(gate.transitions() == 0);
    CHECK(gate.blocksProcessed() == 0);
    CHECK(gate.enabled());                  // settings survive exactly like the gain levels
    CHECK(gate.thresholdDb() == -40.0f);
    CHECK(gate.clampedSets() == 0);         // the knob-quality totals survive too (console facts)
    CHECK(gate.rejectedSets() == 0);
}

// ------------------------------------------------------------------- engine level

namespace {

/// Mono driver for the engine: fills a constant input block, runs one callback,
/// hands back what the output side played.
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

DeviceRequest request()
{
    DeviceRequest request;
    request.sampleRate = kRate;
    request.bufferFrames = static_cast<int>(kFrames);
    return request;
}

/// Drain everything the engine forwarded into the input ring since last time.
std::vector<float> drainRing(AudioEngine& engine)
{
    std::vector<float> all;
    auto* ring = engine.inputRing(0);
    REQUIRE(ring != nullptr);

    std::vector<float> sink(kFrames);

    while (ring->readable() > 0)
    {
        const std::size_t taken = ring->read(sink.data(), kFrames);
        all.insert(all.end(), sink.begin(), sink.begin() + static_cast<std::ptrdiff_t>(taken));
    }

    return all;
}

} // namespace

TEST_CASE("AudioEngine: the gate is off by default and the engine default equals the settings default",
          "[audio][engine][gate]")
{
    AudioEngine engine;

    // Criterion 1: disabled by default, and the default threshold the UI shows is the
    // same number the config schema carries (two modules, one promise, pinned by test
    // because Config may not include Audio).
    CHECK_FALSE(engine.inputGateEnabled());
    CHECK(engine.inputGateThresholdDb() == config::defaults().audio.gateThresholdDb);
    CHECK(engine.inputGateThresholdDb() == HardGate::kDefaultThresholdDb);
    CHECK_FALSE(engine.inputGateOpen());
    CHECK(engine.inputGateClosedSamples() == 0);
}

TEST_CASE("AudioEngine: with the gate disabled the ring receives the gained stream unchanged",
          "[audio][engine][gate]")
{
    AudioEngine engine;
    NullAudioBackend backend;
    engine.setInputGainDb(-6.0f);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    MonoDrive drive(static_cast<int>(kFrames));
    drive.run(engine, 0.5f, static_cast<int>(kFrames));
    drive.run(engine, 0.5f, static_cast<int>(kFrames));

    const std::vector<float> received = drainRing(engine);
    REQUIRE(received.size() >= kFrames);

    const float expected = 0.5f * coefficientFor(-6.0f);

    for (const float sample : received)
        CHECK(std::fabs(sample - expected) < 1e-5f);

    CHECK(engine.inputGateClosedSamples() == 0);
    CHECK(engine.oversizedCallbacks() == 0);
    CHECK(engine.malformedCallbacks() == 0);

    engine.deactivate();
}

TEST_CASE("AudioEngine: an enabled gate mutes the ring, while the input meter keeps showing the room",
          "[audio][engine][gate]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    engine.setInputGateEnabled(true);
    engine.setInputGateThresholdDb(-40.0f);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    MonoDrive drive(static_cast<int>(kFrames));

    // 0.001 (-60 dBFS) is under the threshold from the very first block. Pump past
    // the close ramp; every later block reaches the ring as exact zeros.
    for (int block = 0; block < 8; ++block)
        drive.run(engine, 0.001f, static_cast<int>(kFrames));

    const std::vector<float> received = drainRing(engine);
    REQUIRE(received.size() == kFrames * 8);

    for (std::size_t i = kFrames * 5; i < received.size(); ++i)   // ramp over by block 5
        CHECK(received[i] == 0.0f);

    // The capture kept counting and forwarding the whole time - the stream was
    // muted, never interrupted.
    CHECK(engine.inputSamplesCaptured() == kFrames * 8);
    CHECK(engine.inputSamplesForwarded() == kFrames * 8);
    CHECK(engine.inputRingDroppedSamples() == 0);

    // And the meter reads the PRE-gate block (the class comment promises the
    // operator the level a threshold is chosen from, not the gate's own answer).
    const auto* meter = engine.inputMeter(0);
    REQUIRE(meter != nullptr);
    CHECK(meter->rmsLinear() == Catch::Approx(0.001f).epsilon(0.01));
    CHECK(meter->rmsLinear() > 0.0f);   // the pre-gate truth, while the ring above read exact zeros

    CHECK(engine.inputGateOpen() == false);
    CHECK(engine.inputGateClosedSamples() > 0);

    // Above the threshold the ring gets the stream again (0 dB unity gain here).
    for (int block = 0; block < 3; ++block)
        drive.run(engine, 0.5f, static_cast<int>(kFrames));

    const std::vector<float> reopened = drainRing(engine);
    REQUIRE(!reopened.empty());

    for (std::size_t i = kFrames; i < reopened.size(); ++i)   // skip the open-ramp block
        CHECK(std::fabs(reopened[i] - 0.5f) < 1e-6f);

    CHECK(engine.inputGateOpen());

    engine.deactivate();
}

TEST_CASE("AudioEngine: the output side never gates - translation and bypass both play", "[audio][engine][gate]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    engine.setJitterBufferMs(20);   // the SPEC minimum pre-roll; the point is the routing, not the delay
    engine.setInputGateEnabled(true);
    engine.setInputGateThresholdDb(-20.0f);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    // A quiet room: the input gate is shut.
    MonoDrive drive(static_cast<int>(kFrames));
    std::vector<float> translationAudio(kFrames, 0.2f);

    for (int block = 0; block < 8; ++block)
    {
        engine.outputJitter(0)->write(translationAudio.data(), kFrames);
        drive.run(engine, 0.0001f, static_cast<int>(kFrames));
        drainRing(engine);   // keep the ring from overflowing; the input claim is not what this case is about
    }

    // Criterion 9: the audience still hears the translation, at full level, while
    // the gate holds the input at exact zero.
    CHECK(engine.inputGateClosedSamples() > 0);

    for (const float sample : drive.out)
        CHECK(std::fabs(sample - 0.2f) < 1e-5f);

    CHECK(engine.outputMeter(0) != nullptr);
    CHECK(engine.outputMeter(0)->rmsLinear() == Catch::Approx(0.2f).epsilon(0.01));

    // And the deliberate Off/bypass route: the capture copied straight to the output.
    // The gate lives BEFORE the ring on the input side; this route never enters the
    // gated stage of the pipeline, so the operator hears the room as the driver
    // produced it - which is exactly what "do not gate the output" says.
    engine.setOutputRouting(OutputRouting::bypassOriginal);
    drive.run(engine, 0.0001f, static_cast<int>(kFrames));

    for (const float sample : drive.out)
        CHECK(std::fabs(sample - 0.0001f) < 1e-7f);

    engine.deactivate();
}

TEST_CASE("AudioEngine: toggling and re-thresholding the gate while running never interrupts capture",
          "[audio][engine][gate]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    const std::uint64_t blocksBefore = engine.blockCount();
    MonoDrive drive(static_cast<int>(kFrames));
    std::vector<float> sink(kFrames);

    for (int block = 0; block < 400; ++block)
    {
        engine.setInputGateEnabled(block % 2 == 0);
        engine.setInputGateThresholdDb(static_cast<float>(-24 - block % 40));   // moves live, mid-stream
        drive.run(engine, block % 3 == 0 ? 0.25f : 0.0005f, static_cast<int>(kFrames));

        while (engine.inputRing(0)->readable() > 0)
            engine.inputRing(0)->read(sink.data(), kFrames);
    }

    // The device ran the whole time: one callback, one block, no gap, no error path.
    CHECK(engine.blockCount() == blocksBefore + 400);
    CHECK(engine.malformedCallbacks() == 0);
    CHECK(engine.inputSamplesCaptured() == kFrames * 400);
    CHECK(engine.inputSamplesCaptured() == engine.inputSamplesForwarded() + engine.inputRingDroppedSamples());
    CHECK(engine.inputRingDroppedSamples() == 0);
    CHECK(engine.inputGateClosedSamples() > 0);   // it really did gate, not just run beside

    engine.deactivate();
}

TEST_CASE("AudioEngine: gate settings and history survive a restart; the run reset clears only the show",
          "[audio][engine][gate]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    engine.setInputGateEnabled(true);
    engine.setInputGateThresholdDb(-50.0f);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    MonoDrive drive(static_cast<int>(kFrames));

    for (int block = 0; block < 8; ++block)
    {
        drive.run(engine, 0.001f, static_cast<int>(kFrames));
        drainRing(engine);
    }

    CHECK(engine.inputGateClosedSamples() > 0);
    engine.deactivate();

    // The operator's decision is not device geometry: a restart must not forget it,
    // same promise the gain stages make with the room's levels.
    CHECK(engine.inputGateEnabled());
    CHECK(engine.inputGateThresholdDb() == -50.0f);
    CHECK(engine.inputGateClosedSamples() > 0);

    NullAudioBackend another;
    REQUIRE(engine.activate(another, request(), error));
    engine.attachInputConsumer();

    CHECK(engine.inputGateEnabled());
    CHECK(engine.inputGateThresholdDb() == -50.0f);

    // ... and the re-created pipeline keeps gating: the next run's quiet blocks are
    // muted by the remembered threshold, from its own first measured blocks.
    for (int block = 0; block < 6; ++block)
    {
        drive.run(engine, 0.001f, static_cast<int>(kFrames));
        drainRing(engine);
    }

    // Start a new show: the signal facts are the session's ledger, the settings are not.
    engine.resetRunCounters();
    CHECK(engine.inputGateClosedSamples() == 0);
    CHECK(engine.inputGateEnabled());
    CHECK(engine.inputGateThresholdDb() == -50.0f);

    engine.deactivate();
}

TEST_CASE("AudioEngine: clamped and refused threshold requests are counted, not swallowed", "[audio][engine][gate]")
{
    AudioEngine engine;

    CHECK(engine.inputGateThresholdDb() == HardGate::kDefaultThresholdDb);

    engine.setInputGateThresholdDb(-1000.0f);   // below the window
    CHECK(engine.inputGateThresholdDb() == HardGate::kMinThresholdDb);
    CHECK(engine.gateRequestsClamped() == 1);
    CHECK(engine.gateRequestsRejected() == 0);

    engine.setInputGateThresholdDb(std::nanf("1"));   // not a level at all
    CHECK(engine.inputGateThresholdDb() == HardGate::kMinThresholdDb);   // previous kept
    CHECK(engine.gateRequestsRejected() == 1);

    engine.setInputGateThresholdDb(1e30f);   // huge but finite: clamped to the ceiling
    CHECK(engine.inputGateThresholdDb() == HardGate::kMaxThresholdDb);
    CHECK(engine.gateRequestsClamped() == 2);

    engine.setInputGateThresholdDb(std::numeric_limits<float>::infinity());   // non-finite: refused again
    CHECK(engine.inputGateThresholdDb() == HardGate::kMaxThresholdDb);        // previous kept
    CHECK(engine.gateRequestsRejected() == 2);
}

TEST_CASE("AudioEngine: a block larger than one chunk is gated in every chunk", "[audio][engine][gate]")
{
    AudioEngine engine;
    NullAudioBackend backend;

    engine.setInputGateEnabled(true);
    engine.setInputGateThresholdDb(-40.0f);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    const int frames = 5000;   // > kGainChunkFrames: the chunked path, with the gate in it
    MonoDrive drive(frames);

    for (int block = 0; block < 3; ++block)
        drive.run(engine, 0.001f, frames);

    CHECK(engine.oversizedCallbacks() == 3);
    CHECK(engine.inputSamplesCaptured() == static_cast<std::uint64_t>(frames) * 3);

    const std::vector<float> received = drainRing(engine);
    REQUIRE(received.size() == static_cast<std::size_t>(frames) * 3);

    // The tail of the oversized block is gated too - chunking must not hand the
    // translator the second half unmuted.
    for (std::size_t i = static_cast<std::size_t>(frames); i < received.size(); ++i)
        CHECK(received[i] == 0.0f);

    engine.deactivate();
}

TEST_CASE("AudioEngine: each input channel is gated on its own level", "[audio][engine][gate]")
{
    struct StereoDrive
    {
        std::vector<float> left = std::vector<float>(kFrames, 0.5f);
        std::vector<float> right = std::vector<float>(kFrames, 0.001f);
        std::vector<float> outA = std::vector<float>(kFrames, 0.0f);
        std::vector<float> outB = std::vector<float>(kFrames, 0.0f);

        void run(AudioEngine& engine)
        {
            const float* inputPointers[] = { left.data(), right.data() };
            float* outputPointers[] = { outA.data(), outB.data() };
            engine.processAudio(inputPointers, outputPointers, static_cast<int>(kFrames));
        }
    };

    AudioEngine engine;
    NullAudioBackend backend(audio::DeviceCapabilities{
        .sampleRate = kRate,
        .preferredBufferFrames = static_cast<int>(kFrames),
        .inputChannels = 2,
        .outputChannels = 2 });

    engine.setInputGateEnabled(true);
    engine.setInputGateThresholdDb(-40.0f);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    engine.attachInputConsumer();

    StereoDrive drive;

    for (int block = 0; block < 6; ++block)
        drive.run(engine);

    REQUIRE(engine.inputRing(0) != nullptr);
    REQUIRE(engine.inputRing(1) != nullptr);

    const std::vector<float> loud = [&]
    {
        std::vector<float> all;
        std::vector<float> sink(kFrames);
        while (engine.inputRing(0)->readable() > 0)
        {
            const std::size_t taken = engine.inputRing(0)->read(sink.data(), kFrames);
            all.insert(all.end(), sink.begin(), sink.begin() + static_cast<std::ptrdiff_t>(taken));
        }
        return all;
    }();

    const std::vector<float> quiet = [&]
    {
        std::vector<float> all;
        std::vector<float> sink(kFrames);
        while (engine.inputRing(1)->readable() > 0)
        {
            const std::size_t taken = engine.inputRing(1)->read(sink.data(), kFrames);
            all.insert(all.end(), sink.begin(), sink.begin() + static_cast<std::ptrdiff_t>(taken));
        }
        return all;
    }();

    REQUIRE(loud.size() == kFrames * 6);
    REQUIRE(quiet.size() == kFrames * 6);

    // The talkative channel flows; the quiet one is muted. A shared decision would
    // either gate the speech away or let the noise floor through.
    for (std::size_t i = kFrames; i < loud.size(); ++i)
        CHECK(std::fabs(loud[i] - 0.5f) < 1e-6f);

    for (std::size_t i = kFrames * 4; i < quiet.size(); ++i)
        CHECK(quiet[i] == 0.0f);

    engine.deactivate();
}
