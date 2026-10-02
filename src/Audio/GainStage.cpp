#include "Audio/GainStage.h"

#include <algorithm>
#include <cmath>

namespace liveai {
namespace audio {

namespace {

/// log2(10) / 20: the exponent that turns a dB value into a linear multiplier through
/// exp2. Written as a constant instead of calling powf(10, db/20) per block, because the
/// callback may not depend on a libm path it cannot see inside.
constexpr float kLog2TenOverTwenty = 0.166096404744368f;

/// Samples a glide takes at this rate. Zero means "jump", which is only reachable by
/// asking for a 0 ms ramp; it is documented and tested rather than special-cased away.
int rampSamplesFor(int sampleRate, int rampMs) noexcept
{
    const int rate = sampleRate > 0 ? sampleRate : 48000;
    return static_cast<int>(static_cast<std::int64_t>(rate) * rampMs / 1000);
}

} // namespace

float GainStage::dbToLinear(float gainDb) noexcept
{
    if (!std::isfinite(gainDb))
        return 1.0f;

    return std::exp2(gainDb * kLog2TenOverTwenty);
}

GainStage::GainStage() noexcept
{
    // The constructed stage is at unity with the default glide, so a test that never
    // calls configure() still behaves like a 48 kHz device and stays click-free.
    current_ = 1.0f;
    appliedLinear_.store(1.0f, std::memory_order_relaxed);
}

float GainStage::targetLinear() const noexcept
{
    if (muted_.load(std::memory_order_relaxed))
        return 0.0f;

    return dbToLinear(gainDb_.load(std::memory_order_relaxed));
}

void GainStage::configure(int sampleRate, int rampMs) noexcept
{
    sampleRate_.store(sampleRate > 0 ? sampleRate : 48000, std::memory_order_relaxed);

    const int ms = std::clamp(rampMs, 0, kMaxRampMs);
    rampMs_.store(ms, std::memory_order_relaxed);
    rampSamples_.store(rampSamplesFor(sampleRate_.load(std::memory_order_relaxed), ms), std::memory_order_relaxed);

    // Landing directly on the requested level is deliberate: the operator already sees
    // that number in the UI, and a glide at the very first block would make the audio
    // arrive at a level nobody set.
    reset();
}

void GainStage::setRampMs(int rampMs) noexcept
{
    const int ms = std::clamp(rampMs, 0, kMaxRampMs);
    rampMs_.store(ms, std::memory_order_relaxed);
    rampSamples_.store(rampSamplesFor(sampleRate_.load(std::memory_order_relaxed), ms), std::memory_order_relaxed);
}

void GainStage::setGainDb(float gainDb) noexcept
{
    if (!std::isfinite(gainDb))
    {
        // Refused, and counted. Keeping the previous value is the only safe reading of
        // "NaN dB": treating it as 0 dB would be an invented gain, treating it as
        // silence would be a fault nobody asked for.
        rejectedSets_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const float clamped = std::clamp(gainDb, kMinGainDb, kMaxGainDb);

    if (clamped != gainDb)
        clampedSets_.fetch_add(1, std::memory_order_relaxed);

    gainDb_.store(clamped, std::memory_order_relaxed);
}

void GainStage::setMuted(bool muted) noexcept
{
    muted_.store(muted, std::memory_order_relaxed);
}

void GainStage::reset() noexcept
{
    current_ = targetLinear();
    rampRemaining_ = 0;
    appliedLinear_.store(current_, std::memory_order_relaxed);
}

void GainStage::process(float* data, std::size_t frames) noexcept
{
    process(data, data, frames);
}

void GainStage::process(const float* input, float* output, std::size_t frames) noexcept
{
    if (input == nullptr || output == nullptr)
    {
        invalidCalls_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (frames == 0)
        return;

    const float target = targetLinear();

    std::uint64_t clippedIn = 0;
    std::uint64_t clippedOut = 0;
    std::uint64_t nonFinite = 0;

    // One sample, one coefficient. The magnitude tests are compares against full scale,
    // so "clipping" here means what it says in the class comment and nothing else.
    const auto applyOne = [this, &clippedIn, &clippedOut, &nonFinite](float sample, float coefficient) noexcept
    {
        float in = sample;

        if (!std::isfinite(in))
        {
            // Not audio. Replaced by silence, and counted: forwarding NaN into the
            // translation stream or to the audience is not a thing that may be invisible.
            in = 0.0f;
            ++nonFinite;
        }
        else if (in >= kFullScale || in <= -kFullScale)
        {
            ++clippedIn;
        }

        float out = in * coefficient;

        if (!(out > -kFullScale && out < kFullScale))
        {
            // Covers both "really loud" and "overflowed to infinity". The value stays as
            // it is unless it is not a number any more: an infinity that cannot be
            // reproduced by any converter becomes full scale instead, and either way the
            // count says it happened. This is a validity guard, not a limiter.
            ++clippedOut;

            if (!std::isfinite(out))
                out = out > 0.0f ? kFullScale : -kFullScale;
        }

        return out;
    };

    std::size_t index = 0;

    if (target == current_)
    {
        // Steady state: no ramp bookkeeping, and multiplying by exactly 1.0f leaves a
        // 0 dB pass-through bit-for-bit identical.
        rampRemaining_ = 0;
        const float coefficient = current_;

        for (; index < frames; ++index)
            output[index] = applyOne(input[index], coefficient);
    }
    else
    {
        while (index < frames)
        {
            if (rampRemaining_ == 0)
            {
                const int ramp = rampSamples_.load(std::memory_order_relaxed);
                rampRemaining_ = ramp > 0 ? static_cast<std::size_t>(ramp) : 0;

                if (rampRemaining_ == 0)
                {
                    // A 0 ms glide was asked for: jump to the target. Documented as the
                    // one configuration that can click, so the caller has to choose it.
                    current_ = target;

                    const float coefficient = current_;

                    for (; index < frames; ++index)
                        output[index] = applyOne(input[index], coefficient);

                    break;
                }
            }

            // Reaching the target in exactly rampRemaining_ more samples. The step is
            // recomputed per chunk, so a request that changes mid-glide turns around
            // smoothly instead of overshooting.
            const std::size_t chunk = std::min<std::size_t>(rampRemaining_, frames - index);
            const float step = (target - current_) / static_cast<float>(rampRemaining_);

            for (std::size_t taken = 0; taken < chunk; ++taken)
            {
                current_ += step;
                output[index] = applyOne(input[index], current_);
                ++index;
                --rampRemaining_;
            }

            if (rampRemaining_ == 0)
                current_ = target;   // land on it exactly; no accumulated float drift
        }
    }

    if (clippedIn != 0 || clippedOut != 0)
        clipIndicator_.store(true, std::memory_order_relaxed);

    clippedIn_.fetch_add(clippedIn, std::memory_order_relaxed);
    clippedOut_.fetch_add(clippedOut, std::memory_order_relaxed);
    nonFiniteIn_.fetch_add(nonFinite, std::memory_order_relaxed);
    blocks_.fetch_add(1, std::memory_order_relaxed);
    samples_.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);
    appliedLinear_.store(current_, std::memory_order_relaxed);
}

} // namespace audio
} // namespace liveai
