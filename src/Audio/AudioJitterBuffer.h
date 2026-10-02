#pragma once
//
// AudioJitterBuffer - the output side of the pipeline (SPEC "Output Jitter Buffer"):
// translated audio arrives from the network at irregular times and the ASIO callback
// must leave with a full block every single period.
//
// Policy, all of it realtime-safe and all of it counted:
//
//   * pre-roll ("priming"): nothing is played until `targetFrames` are buffered.
//     That stores the operator's jitter allowance (translation.jitterBufferMs) as
//     delay, and it is what keeps a network hiccup from becoming an audible gap.
//   * once primed, the buffer is drained continuously. If it ever runs dry, the
//     remaining output is silence, one underrun event is counted, and the buffer
//     re-primes from scratch. Re-priming after a stall is deliberate: playing from
//     a nearly empty buffer just converts one hiccup into many.
//   * overflow (producer faster than the device consumes) drops the incoming
//     frames and counts them: the callback never waits for the network.
//
// Silence is always the fallback. Untranslated microphone audio can never reach
// this buffer by construction: only write() from the network/loopback side feeds it.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Audio/AudioRingBuffer.h"

namespace liveai {
namespace audio {

class AudioJitterBuffer final
{
public:
    /// Allocates here (non-realtime thread only). `capacityFrames` must be able to
    /// hold at least `targetFrames` plus two device blocks, otherwise the buffer can
    /// never prime. AudioEngine::activate sizes both from the opened device, so the
    /// caller normally does not choose these numbers by hand.
    AudioJitterBuffer(std::size_t capacityFrames, std::size_t targetFrames);

    AudioJitterBuffer(const AudioJitterBuffer&) = delete;
    AudioJitterBuffer& operator=(const AudioJitterBuffer&) = delete;
    AudioJitterBuffer(AudioJitterBuffer&&) = delete;
    AudioJitterBuffer& operator=(AudioJitterBuffer&&) = delete;

    bool valid() const noexcept { return ring_.valid(); }

    std::size_t capacity() const noexcept { return ring_.capacity(); }

    /// Frames of stored delay that must accumulate before playback starts.
    std::size_t targetFrames() const noexcept { return targetFrames_.load(std::memory_order_relaxed); }

    /// Non-realtime: change the pre-roll (operator moved the jitter slider). Raising
    /// it above the current fill re-primes, which is the correct, audible-latency
    /// consequence of asking for more protection.
    void setTargetFrames(std::size_t frames) noexcept;

    /// Producer (network/loopback thread). Never blocks. Also publishes readiness: as
    /// soon as the pre-roll is buffered, priming() reports false without requiring a
    /// read first.
    std::size_t write(const float* data, std::size_t frames) noexcept;

    /// Consumer (audio callback). Fills `data` completely; returns the number of real
    /// translated frames used, 0 when the block was silence.
    std::size_t readOrSilence(float* data, std::size_t frames) noexcept;

    /// Current stored frames = the delay this buffer is adding right now.
    std::size_t available() const noexcept { return ring_.readable(); }

    /// True while pre-rolling, i.e. waiting for targetFrames to accumulate.
    bool priming() const noexcept { return !primed_.load(std::memory_order_relaxed); }

    /// Underrun events (a block that had to be re-prime or partly silence), not frames.
    std::uint64_t underrunEvents() const noexcept { return underrunEvents_.load(std::memory_order_relaxed); }

    /// Frames dropped by the producer because the buffer was full.
    std::uint64_t droppedFrames() const noexcept { return ring_.droppedFrames(); }

    /// Frames played as silence while pre-rolling (startup and post-stall).
    std::uint64_t primingFrames() const noexcept { return primingFrames_.load(std::memory_order_relaxed); }

    /// Clears the contents and the state. Not thread safe: idle-time only.
    void reset() noexcept;

private:
    AudioRingBuffer ring_;
    std::atomic<std::size_t> targetFrames_{ 0 };
    std::atomic<bool> primed_{ false };
    std::atomic<std::uint64_t> underrunEvents_{ 0 };
    std::atomic<std::uint64_t> primingFrames_{ 0 };
};

} // namespace audio
} // namespace liveai
