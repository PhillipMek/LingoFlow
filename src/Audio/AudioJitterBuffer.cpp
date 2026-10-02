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
    targetFrames_.store(frames, std::memory_order_relaxed);

    // Raising the target above what is buffered means pre-rolling again; lowering it
    // to what is already buffered makes the buffer ready at once. Both are decided
    // from the data actually present, never from a timer.
    primed_.store(ring_.readable() >= frames, std::memory_order_relaxed);
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

void AudioJitterBuffer::reset() noexcept
{
    ring_.reset();
    primed_.store(false, std::memory_order_relaxed);
    underrunEvents_.store(0, std::memory_order_relaxed);
    primingFrames_.store(0, std::memory_order_relaxed);
}

} // namespace audio
} // namespace liveai
