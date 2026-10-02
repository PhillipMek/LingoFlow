#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "Audio/GainStage.h"
#include "Audio/LevelMeter.h"

using namespace liveai;
using liveai::audio::GainStage;

namespace {

constexpr int kRate = 48000;
constexpr std::size_t kFrames = 480;      // 10 ms at 48 kHz, the MVP device block

constexpr double kTwoPi = 3.14159265358979323846 * 2.0;

/// The reference is computed with std::pow, not with the stage's own exp2, so a mistake
/// in the stage's constant cannot agree with itself.
double expectedLinear(double gainDb)
{
    return std::pow(10.0, gainDb / 20.0);
}

std::vector<float> sine(std::size_t frames, double frequencyHz, double amplitude = 0.25)
{
    std::vector<float> out(frames);

    for (std::size_t i = 0; i < frames; ++i)
        out[i] = static_cast<float>(amplitude * std::sin(kTwoPi * frequencyHz * static_cast<double>(i) / kRate));

    return out;
}

std::vector<float> constant(std::size_t frames, float value)
{
    return std::vector<float>(frames, value);
}

/// Largest move between two neighbouring samples: the shape an operator hears as a click.
double maxNeighbourJump(const std::vector<float>& samples)
{
    double worst = 0.0;

    for (std::size_t i = 1; i < samples.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(samples[i]) - samples[i - 1]));

    return worst;
}

/// The steepest move a clean sine can make between two samples. Any jump larger than this
/// came from the coefficient, not from the music.
double slewBound(double amplitude, double frequencyHz)
{
    return amplitude * kTwoPi * frequencyHz / kRate;
}

} // namespace

TEST_CASE("GainStage: 0 dB passes the block through bit for bit", "[audio][gain][dsp]")
{
    GainStage stage;
    stage.configure(kRate, 0);

    const std::vector<float> reference = sine(kFrames, 1000.0);
    std::vector<float> out = reference;

    stage.process(out.data(), out.size());

    // A unity coefficient has to be an exact 1.0f multiply: any drift here would mean
    // every pass-through block is slowly re-quantised.
    CHECK(std::memcmp(out.data(), reference.data(), reference.size() * sizeof(float)) == 0);
    CHECK(stage.blocksProcessed() == 1);
    CHECK(stage.samplesProcessed() == reference.size());
}

TEST_CASE("GainStage: dB values map to the linear factors SPEC implies", "[audio][gain][dsp]")
{
    CHECK(GainStage::dbToLinear(0.0f) == 1.0f);

    const auto checkOne = [](float db)
    {
        const double expected = expectedLinear(db);
        const float actual = GainStage::dbToLinear(db);

        INFO("dB=" << db << " expected=" << expected << " actual=" << actual);
        CHECK(std::fabs(actual - expected) < expected * 1e-5);
    };

    checkOne(6.0f);
    checkOne(-6.0f);
    checkOne(24.0f);
    checkOne(-24.0f);
    checkOne(GainStage::kMinGainDb);
    checkOne(0.5f);

    // +6 dB is very close to a doubling and -20 dB is a factor of ten: the two numbers an
    // operator reads off a meter. -24 dB, the SPEC ceiling and floor, is 10^(-1.2).
    CHECK(std::fabs(GainStage::dbToLinear(6.0f) - 1.9953f) < 1e-3f);
    CHECK(std::fabs(GainStage::dbToLinear(-20.0f) - 0.1f) < 1e-4f);
    CHECK(std::fabs(GainStage::dbToLinear(-24.0f) - 0.0631f) < 1e-4f);

    // Round trip against the meter's conversion, so the UI and the DSP agree on dB.
    for (const float db : { 0.0f, -12.0f, 12.0f, 24.0f })
        CHECK(std::fabs(audio::linearToDb(GainStage::dbToLinear(db)) - db) < 0.01f);

    // A non-finite request is refused with unity, never with a plausible-looking number.
    CHECK(GainStage::dbToLinear(std::nanf("")) == 1.0f);
    CHECK(GainStage::dbToLinear(-std::numeric_limits<float>::infinity()) == 1.0f);
}

TEST_CASE("GainStage: the gain is applied to every sample of the block", "[audio][gain][dsp]")
{
    GainStage stage;
    stage.configure(kRate, 0);            // jump: exactness tests; the glide has its own
    stage.setGainDb(-6.0f);

    std::vector<float> out = constant(kFrames, 0.5f);
    stage.process(out.data(), out.size());

    const float expected = static_cast<float>(0.5 * expectedLinear(-6.0));

    for (const float sample : out)
        CHECK(std::fabs(sample - expected) < 1e-5f);

    // appliedLinear() is the coefficient, not the sample: 0.5 was multiplied by it.
    CHECK(std::fabs(stage.appliedLinear() - static_cast<float>(expectedLinear(-6.0))) < 1e-5f);
    CHECK(std::fabs(audio::linearToDb(stage.appliedLinear()) + 6.0f) < 0.01f);
}

TEST_CASE("GainStage: a gain change glides and lands exactly on the target", "[audio][gain][no-click]")
{
    GainStage stage;
    stage.configure(kRate, GainStage::kDefaultRampMs);   // 20 ms = 960 samples at 48 kHz

    CHECK(stage.appliedLinear() == 1.0f);                // configure() landed on 0 dB

    stage.setGainDb(-6.0f);

    const std::vector<float> in = constant(kFrames * 3, 1.0f);
    std::vector<float> out;

    for (int block = 0; block < 3; ++block)
    {
        std::vector<float> chunk(in.begin() + block * static_cast<std::ptrdiff_t>(kFrames),
                                 in.begin() + (block + 1) * static_cast<std::ptrdiff_t>(kFrames));
        stage.process(chunk.data(), chunk.size());
        out.insert(out.end(), chunk.begin(), chunk.end());
    }

    const float target = static_cast<float>(expectedLinear(-6.0));

    // Mid-glide: the block is neither where it started nor where it is going, which is the
    // point - a change that finished in one sample would be a click.
    CHECK(out.front() < 1.0f);
    CHECK(out.front() > target);
    CHECK(out[kFrames - 1] > target);

    // 960 samples of glide, so three blocks of 480 have finished it, and the last samples
    // are exactly the requested level: no accumulated float drift left behind.
    CHECK(std::fabs(out.back() - target) < 1e-5f);
    CHECK(std::fabs(stage.appliedLinear() - target) < 1e-5f);

    // Each step is at most the whole change spread over the glide, and the sequence only
    // ever decreases - with one documented exception. The coefficient accumulates by
    // repeated float addition, so it can arrive a little short of the target, and the
    // landing that makes the final value exact moves it back up by exactly that much.
    // Allowing one bounded increase is what "glide, then land" means.
    const double perSample = (1.0 - target) / static_cast<double>(GainStage::kDefaultRampMs * kRate / 1000);
    const double drift = (1.0 - target) * 1e-4;

    int increases = 0;

    for (std::size_t i = 1; i < out.size(); ++i)
    {
        CHECK(std::fabs(out[i] - out[i - 1]) <= perSample * 1.001);

        if (out[i] > out[i - 1])
        {
            ++increases;
            CHECK(out[i] - out[i - 1] <= drift);
        }
    }

    CHECK(increases <= 1);
}

TEST_CASE("GainStage: gliding removes the discontinuity an instant change creates",
          "[audio][gain][no-click]")
{
    // A gain move hurts most at a waveform peak: the sample either side of the change is
    // otherwise the same, so the whole step is the coefficient. Cutting exactly at a peak
    // keeps the test independent of luck with the phase.
    constexpr double kFrequency = 500.0;
    constexpr double kAmplitude = 0.5;

    const std::size_t period = static_cast<std::size_t>(kRate / kFrequency);   // 96 samples
    const std::size_t split = period * 5 + period / 4;                          // 504, the peak

    const std::vector<float> reference = sine(kFrames * 2, kFrequency, kAmplitude);
    REQUIRE(split + 1 < reference.size());
    CHECK(reference[split] > kAmplitude * 0.99);

    const auto run = [&](int rampMs)
    {
        GainStage stage;
        stage.configure(kRate, rampMs);

        std::vector<float> out;

        std::vector<float> first(reference.begin(), reference.begin() + static_cast<std::ptrdiff_t>(split));
        stage.process(first.data(), first.size());
        out.insert(out.end(), first.begin(), first.end());

        stage.setGainDb(-12.0f);                        // a real operator move, mid-stream

        std::vector<float> second(reference.begin() + static_cast<std::ptrdiff_t>(split), reference.end());
        stage.process(second.data(), second.size());
        out.insert(out.end(), second.begin(), second.end());

        return out;
    };

    const std::vector<float> jumped = run(0);
    const std::vector<float> glided = run(GainStage::kDefaultRampMs);

    const double jumpAtBoundary = std::fabs(jumped[split] - jumped[split - 1]);
    REQUIRE(jumpAtBoundary > 0.3);                      // the click, for reference

    const double glideWorst = maxNeighbourJump(glided);
    const double jumpWorst = maxNeighbourJump(jumped);

    INFO("glide worst=" << glideWorst << " jump worst=" << jumpWorst << " boundary=" << jumpAtBoundary);

    // With the glide the steepest move anywhere in the file is the sine's own slew.
    CHECK(glideWorst < slewBound(kAmplitude, kFrequency) * 1.2);
    CHECK(glideWorst < jumpWorst / 5.0);
    CHECK(glided.size() == jumped.size());
}

TEST_CASE("GainStage: mute glides to silence and unmute returns to the same level",
          "[audio][gain][no-click][policy]")
{
    GainStage stage;
    stage.configure(kRate, GainStage::kDefaultRampMs);
    stage.setGainDb(-6.0f);

    std::vector<float> out = constant(kFrames, 0.5f);
    stage.process(out.data(), out.size());

    const float levelBeforeMute = out.front();
    REQUIRE(levelBeforeMute > 0.1f);

    stage.setMuted(true);
    CHECK(stage.muted());

    // One block is not enough to go silent, and the request does not lose the level.
    out = constant(kFrames, 0.5f);
    stage.process(out.data(), out.size());
    CHECK(out.front() > 0.0f);
    CHECK(out.back() < out.front());
    CHECK(stage.gainDb() == -6.0f);

    for (int block = 0; block < 3; ++block)
    {
        out = constant(kFrames, 0.5f);
        stage.process(out.data(), out.size());
    }

    CHECK(out.front() == 0.0f);
    CHECK(out.back() == 0.0f);
    CHECK(stage.appliedLinear() == 0.0f);

    stage.setMuted(false);

    for (int block = 0; block < 4; ++block)
    {
        out = constant(kFrames, 0.5f);
        stage.process(out.data(), out.size());
    }

    // SPEC "Input Gain" has mute and "reset to 0 dB" as separate controls, so unmuting has
    // to come back to -6 dB rather than to unity.
    const float expected = static_cast<float>(0.5 * expectedLinear(-6.0));
    CHECK(std::fabs(out.back() - expected) < 1e-5f);
    CHECK(std::fabs(stage.appliedLinear() - static_cast<float>(expectedLinear(-6.0))) < 1e-5f);
}

TEST_CASE("GainStage: out-of-window gains are clamped and counted, never accepted", "[audio][gain][policy]")
{
    GainStage stage;
    stage.configure(kRate, GainStage::kDefaultRampMs);

    stage.setGainDb(40.0f);                        // above the SPEC ceiling
    CHECK(stage.gainDb() == GainStage::kMaxGainDb);
    CHECK(stage.clampedSets() == 1);

    stage.setGainDb(-200.0f);                      // below the stage floor
    CHECK(stage.gainDb() == GainStage::kMinGainDb);
    CHECK(stage.clampedSets() == 2);

    stage.setGainDb(24.0f);                        // exactly on the ceiling is not a clamp
    CHECK(stage.gainDb() == 24.0f);
    CHECK(stage.clampedSets() == 2);

    // The whole accepted window has to be reachable: SPEC's suggested -24..+24 sits inside
    // it, and so does the -60 that the configuration layer allows as a console trim.
    stage.setGainDb(-60.0f);
    CHECK(stage.gainDb() == -60.0f);
    CHECK(stage.clampedSets() == 2);
}

TEST_CASE("GainStage: a non-finite gain request is refused and the level stays put", "[audio][gain][policy]")
{
    GainStage stage;
    stage.configure(kRate, 0);
    stage.setGainDb(-12.0f);

    stage.setGainDb(std::nanf(""));
    stage.setGainDb(std::numeric_limits<float>::infinity());

    CHECK(stage.gainDb() == -12.0f);               // not silently 0 dB, not silently silence
    CHECK(stage.rejectedSets() == 2);
    CHECK(stage.clampedSets() == 0);

    std::vector<float> out = constant(kFrames, 0.5f);
    stage.process(out.data(), out.size());

    CHECK(std::fabs(out.front() - static_cast<float>(0.5 * expectedLinear(-12.0))) < 1e-5f);
}

TEST_CASE("GainStage: non-finite samples become silence and are counted", "[audio][gain][honesty]")
{
    GainStage stage;
    stage.configure(kRate, 0);
    stage.setGainDb(-6.0f);

    std::vector<float> in = constant(kFrames, 0.5f);
    in[7] = std::nanf("1");
    in[11] = std::numeric_limits<float>::infinity();
    in[400] = -std::numeric_limits<float>::infinity();

    std::vector<float> out = in;
    stage.process(out.data(), out.size());

    CHECK(out[7] == 0.0f);
    CHECK(out[11] == 0.0f);
    CHECK(out[400] == 0.0f);
    CHECK(std::isfinite(out[8]));

    // The neighbours of a poisoned sample are untouched: one NaN is one silenced sample,
    // not a block of them, and it never reaches the translator or the audience.
    CHECK(std::fabs(out[8] - static_cast<float>(0.5 * expectedLinear(-6.0))) < 1e-5f);
    CHECK(stage.nonFiniteInFrames() == 3);
    CHECK(stage.blocksProcessed() == 1);
}

TEST_CASE("GainStage: attenuation cannot hide clipping that came from the device",
          "[audio][gain][clipping][honesty]")
{
    GainStage stage;
    stage.configure(kRate, 0);
    stage.setGainDb(-24.0f);                       // turn it down: the operator's reflex

    std::vector<float> in = constant(kFrames, 0.5f);
    in[0] = 1.0f;                                  // exactly full scale
    in[5] = -1.5f;                                 // already over
    in[9] = 0.999f;                                // just under, must not count

    std::vector<float> out = in;
    stage.process(out.data(), out.size());

    // Two samples arrived at or beyond full scale. That is a fact about the console and the
    // driver, and it stays true however far the application turns its own level down.
    CHECK(stage.clippedInFrames() == 2);

    // After -24 dB nothing is at full scale any more, so the post-gain count is correctly
    // zero. The two numbers answer two different questions.
    CHECK(stage.clippedOutFrames() == 0);
    CHECK(std::fabs(out[0] - static_cast<float>(expectedLinear(-24.0))) < 1e-5f);
    CHECK(std::fabs(out[5] - static_cast<float>(-1.5 * expectedLinear(-24.0))) < 1e-5f);

    CHECK(stage.takeClipIndicator());              // the device clipping still lights the LED
    CHECK_FALSE(stage.takeClipIndicator());        // ... and reading it consumes it
}

TEST_CASE("GainStage: the clipping indicator latches and clears when read", "[audio][gain][clipping]")
{
    GainStage stage;
    stage.configure(kRate, 0);

    std::vector<float> quiet = constant(kFrames, 0.25f);
    stage.process(quiet.data(), quiet.size());
    CHECK_FALSE(stage.takeClipIndicator());

    stage.setGainDb(24.0f);                        // 15.85x: 0.25 becomes 3.96

    std::vector<float> loud = constant(kFrames, 0.25f);
    stage.process(loud.data(), loud.size());

    CHECK(stage.takeClipIndicator());
    CHECK_FALSE(stage.takeClipIndicator());

    stage.setGainDb(0.0f);
    stage.process(loud.data(), loud.size());       // loud[] already holds the over-scale values
    CHECK(stage.takeClipIndicator());              // they are still over full scale
    CHECK_FALSE(stage.takeClipIndicator());
}

TEST_CASE("GainStage: a product that overflows is reported and turned into full scale",
          "[audio][gain][clipping][honesty]")
{
    GainStage stage;
    stage.configure(kRate, 0);
    stage.setGainDb(GainStage::kMaxGainDb);        // the highest gain the window allows

    std::vector<float> in = constant(kFrames, 1.0e38f);   // finite, far above full scale
    std::vector<float> out = in;
    stage.process(out.data(), out.size());

    // 1e38 * 15.85 is not representable. The stage must not put Inf on the wire, and must
    // not pretend nothing happened either.
    for (const float sample : out)
    {
        CHECK(std::isfinite(sample));
        CHECK(sample == GainStage::kFullScale);
    }

    CHECK(stage.clippedInFrames() == kFrames);
    CHECK(stage.clippedOutFrames() == kFrames);
    CHECK(stage.nonFiniteInFrames() == 0);         // the inputs were finite; only we overflowed
}

TEST_CASE("GainStage: this block does not limit, it only reports", "[audio][gain][clipping][policy]")
{
    // SPEC lists a limiter under "Future architecture", so a value above full scale has to
    // survive this stage untouched. If this test ever fails, a limiter has quietly been
    // added and the operator's clipping indication has become meaningless.
    GainStage stage;
    stage.configure(kRate, 0);

    std::vector<float> in = constant(kFrames, 2.0f);
    std::vector<float> out = in;
    stage.process(out.data(), out.size());

    CHECK(out.front() == 2.0f);
    CHECK(stage.clippedInFrames() == kFrames);
    CHECK(stage.clippedOutFrames() == kFrames);
}

TEST_CASE("GainStage: a null buffer is a counted contract violation", "[audio][gain][policy]")
{
    GainStage stage;
    stage.configure(kRate, 0);

    std::vector<float> out = constant(4, 1.0f);

    stage.process(nullptr, out.data(), out.size());
    stage.process(out.data(), nullptr, out.size());
    stage.process(out.data(), 0);                  // a 0-frame block is not work and not a fault

    CHECK(stage.invalidCalls() == 2);              // null buffers only: 0 frames is not work
    CHECK(stage.blocksProcessed() == 0);
    CHECK(stage.samplesProcessed() == 0);
    CHECK(out == constant(4, 1.0f));               // nothing was written anywhere
}

TEST_CASE("GainStage: the two-buffer form reads the source and never writes it", "[audio][gain][dsp]")
{
    // The ASIO callback hands the engine read-only input pointers; the scratch form is what
    // the input side uses, so the source must survive untouched.
    GainStage stage;
    stage.configure(kRate, 0);
    stage.setGainDb(-6.0f);

    const std::vector<float> source = sine(kFrames, 1000.0);
    const std::vector<float> original = source;
    std::vector<float> destination(kFrames, -0.125f);

    stage.process(source.data(), destination.data(), destination.size());

    CHECK(std::memcmp(source.data(), original.data(), source.size() * sizeof(float)) == 0);
    CHECK(destination.size() == source.size());

    const float coefficient = static_cast<float>(expectedLinear(-6.0));

    for (std::size_t i = 0; i < destination.size(); ++i)
        CHECK(std::fabs(destination[i] - source[i] * coefficient) < 1e-6f);
}

TEST_CASE("GainStage: configure lands on the level instead of gliding into it", "[audio][gain][startup]")
{
    // The operator has already set -6 dB. The first block after a device start has to play
    // -6 dB, not glide up to it from unity for 20 ms.
    GainStage stage;
    stage.setGainDb(-6.0f);
    stage.configure(kRate, GainStage::kDefaultRampMs);

    std::vector<float> out = constant(kFrames, 0.5f);
    stage.process(out.data(), out.size());

    const float expected = static_cast<float>(0.5 * expectedLinear(-6.0));
    CHECK(std::fabs(out.front() - expected) < 1e-5f);
    CHECK(std::fabs(out.back() - expected) < 1e-5f);
}

TEST_CASE("GainStage: the glide length can be changed while running", "[audio][gain][policy]")
{
    GainStage stage;
    stage.configure(kRate, 200);

    stage.setRampMs(0);
    CHECK(stage.rampMs() == 0);

    stage.setGainDb(-24.0f);

    std::vector<float> out = constant(kFrames, 0.5f);
    stage.process(out.data(), out.size());

    // A 0 ms glide is documented as the one setting that jumps. It is reachable, but it has
    // to be asked for.
    CHECK(std::fabs(out.front() - static_cast<float>(0.5 * expectedLinear(-24.0))) < 1e-5f);

    stage.setRampMs(100000);                       // absurd: clamped to the window
    CHECK(stage.rampMs() == GainStage::kMaxRampMs);

    stage.setRampMs(-5);                           // nonsense: clamped to 0
    CHECK(stage.rampMs() == 0);
}

TEST_CASE("GainStage: 20 000 blocks with a UI thread sweeping the level stay sane",
          "[audio][gain][stress][threading]")
{
    GainStage stage;
    stage.configure(kRate, GainStage::kDefaultRampMs);

    constexpr double kFrequency = 500.0;
    constexpr double kAmplitude = 0.05;

    std::thread ui([&]
    {
        for (float db = -24.0f; db <= 24.0f; db += 0.5f)
        {
            stage.setGainDb(db);
            stage.setMuted(static_cast<int>(db * 2.0f) % 7 == 0);
        }

        stage.setGainDb(std::nanf(""));
        stage.setGainDb(0.0f);
        stage.setMuted(false);
    });

    const std::vector<float> in = sine(kFrames, kFrequency, kAmplitude);
    std::vector<float> out(kFrames);

    double worstJump = 0.0;
    float last = 0.0f;

    for (int block = 0; block < 20000; ++block)
    {
        out = in;
        stage.process(out.data(), out.size());

        for (const float sample : out)
        {
            REQUIRE(std::isfinite(sample));        // a glide must never produce Inf
            worstJump = std::max(worstJump, std::fabs(static_cast<double>(sample) - last));
            last = sample;
        }
    }

    ui.join();

    CHECK(stage.blocksProcessed() == 20000);
    CHECK(stage.samplesProcessed() == 20000 * kFrames);
    CHECK(stage.rejectedSets() == 1);              // the NaN request from the UI thread
    CHECK(stage.gainDb() == 0.0f);
    CHECK_FALSE(stage.muted());

    // An instant 0 -> +24 dB step at this amplitude would move a sample by
    // 0.05 * 15.85 = 0.79. With the glide the steepest move ever seen is the sine's own
    // slew at full gain - which is what "no clicks" means in numbers.
    const double instantStep = kAmplitude * expectedLinear(GainStage::kMaxGainDb);
    const double glidingBound = slewBound(kAmplitude * static_cast<double>(expectedLinear(GainStage::kMaxGainDb)),
                                          kFrequency) * 2.0;

    INFO("worstJump=" << worstJump << " instant-step=" << instantStep << " bound=" << glidingBound);
    CHECK(worstJump < glidingBound);
    CHECK(worstJump < instantStep / 5.0);
}

TEST_CASE("GainStage: accounting closes across a long run", "[audio][gain][stress]")
{
    GainStage stage;
    stage.configure(kRate, GainStage::kDefaultRampMs);

    std::vector<float> in = constant(kFrames * 8, 1.5f);   // clipped at the input
    std::vector<float> out = in;

    for (int block = 0; block < 8; ++block)
        stage.process(out.data() + static_cast<std::size_t>(block) * kFrames, kFrames);

    CHECK(stage.blocksProcessed() == 8);
    CHECK(stage.samplesProcessed() == 8 * kFrames);
    CHECK(stage.clippedInFrames() == 8 * kFrames);
    CHECK(stage.invalidCalls() == 0);
    CHECK(stage.nonFiniteInFrames() == 0);
}
