#include "Audio/HardGate.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Audio/GainStage.h"

namespace liveai {
namespace audio {

namespace {

/// Samples a glide takes at this rate, never fewer than one: the ramp lengths are
/// fixed design constants (there is no 0 ms configuration for the gate, a jump
/// is exactly the click the whole class exists to avoid), so even an absurd
/// sample rate keeps the division in the glide loop well-defined.
std::size_t rampSamplesFor(int sampleRate, int rampMs) noexcept
{
    const int rate = sampleRate > 0 ? sampleRate : 48000;
    const std::size_t samples = static_cast<std::size_t>(
        static_cast<std::int64_t>(rate) * rampMs / 1000);

    return samples > 1 ? samples : 1;
}

} // namespace

HardGate::HardGate() noexcept
{
    // The constructed gate is bypassed and wide open, so a test that never calls
    // configure() still behaves like a 48 kHz device and passes audio through.
    appliedLinear_.store(1.0f, std::memory_order_relaxed);
}

void HardGate::configure(int sampleRate) noexcept
{
    const int rate = sampleRate > 0 ? sampleRate : 48000;
    sampleRate_.store(rate, std::memory_order_relaxed);
    openRampSamples_.store(rampSamplesFor(rate, kOpenRampMs), std::memory_order_relaxed);
    closeRampSamples_.store(rampSamplesFor(rate, kCloseRampMs), std::memory_order_relaxed);

    // Land on open: the first block of a fresh run is never muted before its
    // own RMS has been measured, the same "the first block is already right"
    // promise GainStage makes for the level.
    reset();
}

void HardGate::reset() noexcept
{
    current_ = 1.0f;
    rampRemaining_ = 0;
    lastTargetOpen_ = true;
    appliedLinear_.store(1.0f, std::memory_order_relaxed);
}

void HardGate::setEnabled(bool enabled) noexcept
{
    enabled_.store(enabled, std::memory_order_relaxed);

    if (!enabled)
    {
        // "No decision is in effect" while bypassed: the callback stops
        // publishing open_ entirely, so a stale closed flag from the last
        // measured block cannot keep the readout showing a gate nobody enabled.
        open_.store(false, std::memory_order_relaxed);
    }
}

void HardGate::setThresholdDb(float thresholdDb) noexcept
{
    if (!std::isfinite(thresholdDb))
    {
        // Refused, and counted. Keeping the previous threshold is the only safe
        // reading of a NaN dB request: inventing a cutoff that passes nothing
        // (silence) or one that passes everything (floor) would be a gate
        // nobody set.
        rejectedSets_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const float clamped = std::clamp(thresholdDb, kMinThresholdDb, kMaxThresholdDb);

    if (clamped != thresholdDb)
        clampedSets_.fetch_add(1, std::memory_order_relaxed);

    thresholdDb_.store(clamped, std::memory_order_relaxed);
}

void HardGate::process(float* data, std::size_t frames) noexcept
{
    if (data == nullptr)
    {
        invalidCalls_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (frames == 0)
        return;

    blocks_.fetch_add(1, std::memory_order_relaxed);
    samples_.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);

    // ---------------------------------------------------------------- decision
    // Measured from the block about to be acted on: no lookahead, no delay,
    // one pass over the samples. The threshold is converted once per chunk
    // through the same audited exp2 path the gain stage uses - fixed cost,
    // nothing that can allocate. While disabled there is no decision at all:
    // the counters below measure the GATE's behaviour, and a bypass is neither
    // open nor closed, it is absent.
    bool decidedOpen = true;

    if (enabled_.load(std::memory_order_relaxed))
    {
        const double threshold = static_cast<double>(
            GainStage::dbToLinear(thresholdDb_.load(std::memory_order_relaxed))) * kThresholdMargin;

        double sumSquares = 0.0;
        std::size_t nonFinite = 0;

        for (std::size_t index = 0; index < frames; ++index)
        {
            const float sample = data[index];

            if (std::isfinite(sample))
                sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
            else
                ++nonFinite;   // contributes silence to the measurement, and is counted
        }

        if (nonFinite != 0)
            nonFiniteSeen_.fetch_add(static_cast<std::uint64_t>(nonFinite), std::memory_order_relaxed);

        // A block of all non-finite samples measures as silence (below any
        // threshold), which is the honest reading: a stream of NaN is not a
        // signal the translator should be handed.
        const double rms = std::sqrt(sumSquares / static_cast<double>(frames));
        decidedOpen = rms >= threshold;   // "at or above the threshold passes"
        open_.store(decidedOpen, std::memory_order_relaxed);

        if (decidedOpen != lastTargetOpen_)
        {
            lastTargetOpen_ = decidedOpen;
            transitions_.fetch_add(1, std::memory_order_relaxed);
        }

        if (decidedOpen)
            openBlocks_.fetch_add(1, std::memory_order_relaxed);
        else
        {
            closedBlocks_.fetch_add(1, std::memory_order_relaxed);
            closedSamples_.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);
        }
    }

    const float target = decidedOpen ? 1.0f : 0.0f;

    // ---------------------------------------------------------------- application
    if (target == current_)
    {
        // Steady state.
        rampRemaining_ = 0;

        if (target < 1.0f)
        {
            // Closed: exact zeros, not a very small gain. The mute is complete,
            // and a poisoned sample cannot survive a memset.
            std::memset(data, 0, frames * sizeof(float));
        }
        // Open (and the settled disabled bypass): the buffer is left exactly
        // as the gain stage produced it - bit-for-bit the input stream.
    }
    else
    {
        // Gliding. Same bookkeeping as GainStage: the step is recomputed per
        // segment against the samples left, so a decision that flips mid-glide
        // turns around from wherever the coefficient is instead of overshooting,
        // and the ramp lands exactly on 1.0 / 0.0 with no accumulated drift.
        std::size_t index = 0;

        while (index < frames)
        {
            if (rampRemaining_ == 0)
            {
                rampRemaining_ = target > current_
                                     ? openRampSamples_.load(std::memory_order_relaxed)
                                     : closeRampSamples_.load(std::memory_order_relaxed);
            }

            const std::size_t chunk = std::min<std::size_t>(rampRemaining_, frames - index);
            const float step = (target - current_) / static_cast<float>(rampRemaining_);

            if (target == 0.0f)
            {
                // A block fading to mute ends muted even when a poisoned sample rode
                // in on the fade: NaN * 0 is NaN, so the plain multiply would keep the
                // poison alive inside the "silence". A non-finite sample takes the value
                // the block is converging to - exact zero - and was already counted by
                // the RMS pass above.
                for (std::size_t taken = 0; taken < chunk; ++taken)
                {
                    current_ += step;

                    const float sample = data[index];
                    data[index] = std::isfinite(sample) ? sample * current_ : 0.0f;

                    ++index;
                    --rampRemaining_;
                }
            }
            else
            {
                for (std::size_t taken = 0; taken < chunk; ++taken)
                {
                    current_ += step;
                    data[index] *= current_;
                    ++index;
                    --rampRemaining_;
                }
            }

            if (rampRemaining_ == 0)
                current_ = target;
        }
    }

    appliedLinear_.store(current_, std::memory_order_relaxed);
}

} // namespace audio
} // namespace liveai
