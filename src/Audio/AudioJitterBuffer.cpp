#include "Audio/AudioJitterBuffer.h"

namespace liveai {
namespace audio {

AudioJitterBuffer::AudioJitterBuffer(std::size_t capacityFrames, std::size_t targetFrames)
    : ring_(capacityFrames)
{
    targetFrames_.store(targetFrames, std::memory_order_relaxed);
}

void AudioJitterBuffer::setTargetFrames(std::size_t frames) noexcept
{
    // Control thread publishes the requested pre-roll only. Deciding "primed" from
    // ring_.readable() here raced the callback's own priming state when the jitter
    // slider moved on a running device, so the decision moved into readOrSilence().
    requestedTarget_.store(frames, std::memory_order_release);
}

std::size_t AudioJitterBuffer::write(const float* data, std::size_t frames) noexcept
{
    const std::size_t written = ring_.write(data, frames);

    // Readiness is published by the producer as soon as the pre-roll is on hand, so a
    // caller asking priming() gets the truth without having to perform a read first.
    if (!primed_.load(std::memory_order_relaxed) && ring_.readable() >= targetFrames_.load(std::memory_order_relaxed))
        primed_.store(true, std::memory_order_relaxed);

    return written;
}

std::size_t AudioJitterBuffer::readOrSilence(float* data, std::size_t frames) noexcept
{
    if (data == nullptr || frames == 0)
        return 0;

    // Fixed-cost hand-off from the control thread: the callback owns both
    // targetFrames_ and primed_, so a live retarget cannot tear its state.
    const std::size_t requested = requestedTarget_.load(std::memory_order_acquire);
    if (requested != kNoPendingTarget && requested != targetFrames_.load(std::memory_order_relaxed))
    {
        targetFrames_.store(requested, std::memory_order_relaxed);
        primed_.store(ring_.readable() >= requested, std::memory_order_relaxed);
        requestedTarget_.store(kNoPendingTarget, std::memory_order_release);
    }

    const std::size_t target = targetFrames_.load(std::memory_order_relaxed);

    if (!primed_.load(std::memory_order_relaxed))
    {
        if (ring_.readable() < target)
        {
            // Still pre-rolling: silence out, nothing is consumed, and this is not an
            // error - it is the operator-requested latency being built up.
            for (std::size_t i = 0; i < frames; ++i)
                data[i] = 0.0f;

            primingFrames_.fetch_add(frames, std::memory_order_relaxed);
            return 0;
        }

        primed_.store(true, std::memory_order_relaxed);
    }

    const std::size_t taken = ring_.read(data, frames);

    if (taken != frames)
    {
        for (std::size_t i = taken; i < frames; ++i)
            data[i] = 0.0f;

        // The buffer ran dry in the middle of playback: count the event and go back
        // to pre-rolling instead of streaming from a nearly empty buffer.
        underrunEvents_.fetch_add(1, std::memory_order_relaxed);
        primed_.store(false, std::memory_order_relaxed);
    }

    return taken;
}

std::size_t AudioJitterBuffer::discard() noexcept
{
    // Consumer side of the SPSC pair (see AudioRingBuffer::discardAll): safe to
    // run from the callback; what the producer writes after this instant is
    // simply the next session's problem, and pre-rolling again means a resumed
    // pipeline waits for a real pre-roll instead of dribbling.
    const std::size_t dropped = ring_.discardAll();
    discarded_.fetch_add(dropped, std::memory_order_relaxed);
    primed_.store(false, std::memory_order_relaxed);
    return dropped;
}

void AudioJitterBuffer::reset() noexcept
{
    ring_.reset();
    primed_.store(false, std::memory_order_relaxed);
    underrunEvents_.store(0, std::memory_order_relaxed);
    primingFrames_.store(0, std::memory_order_relaxed);
    discarded_.store(0, std::memory_order_relaxed);
}

} // namespace audio
} // namespace liveai
