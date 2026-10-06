#pragma once
//
// AudioRingBuffer - single-producer / single-consumer float ring, lock-free and
// fully preallocated (spec "Audio Ring Buffer", the project rules).
//
// Shape of the product pipeline (spec "Audio Pipeline"):
//
//   ASIO Input -> [input gain] -> AudioRingBuffer -> translation worker/network
//   translated audio -> AudioJitterBuffer -> [output gain] -> ASIO Output
//
// The audio callback is always the producer of this buffer (input side) or the
// consumer of the jitter buffer (output side). Both roles here are realtime-safe:
// write() and read() touch preallocated memory, two atomic positions and relaxed
// counters. They never allocate, lock, log or block, and they never wait for the
// other side:
//
//   * producer overflow (consumer stalled): the incoming frames beyond the free
//     space are dropped and counted in droppedFrames();
//   * consumer underflow: the remainder of the output block is silence and the
//     event is counted in underrunFrames().
//
// One instance carries one channel. That matches the MVP (mono in, mono out) and
// keeps the realtime loop free of interleaving math; multi-channel use is N
// instances, not a new format.
//
// Capacity is rounded up to a power of two so the index mask is a bitwise AND
// instead of a modulo in the callback.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace liveai {
namespace audio {

class AudioRingBuffer final
{
public:
    /// Allocates here (non-realtime thread only). `capacityFrames` is rounded up to
    /// the next power of two; 0 leaves the buffer invalid and every call a no-op.
    explicit AudioRingBuffer(std::size_t capacityFrames);

    /// Fixed lock-free state: not copyable, not movable. The engine owns the buffers
    /// through std::unique_ptr, so nothing ever needs to move one.
    AudioRingBuffer(const AudioRingBuffer&) = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;
    AudioRingBuffer(AudioRingBuffer&&) = delete;
    AudioRingBuffer& operator=(AudioRingBuffer&&) = delete;

    /// False when the constructor was given 0 frames.
    bool valid() const noexcept { return !data_.empty(); }

    std::size_t capacity() const noexcept { return capacity_; }

    /// Frames currently stored (producer side: an upper bound, consumer side: exact
    /// at the moment of the call). Realtime-safe.
    std::size_t readable() const noexcept;

    /// Free slots for the producer. Realtime-safe.
    std::size_t writable() const noexcept;

    /// Producer role. Writes as many frames as fit; returns the number written and
    /// counts the rest in droppedFrames(). Never blocks, never allocates.
    std::size_t write(const float* data, std::size_t frames) noexcept;

    /// Consumer role. Reads what is available; returns the number read.
    std::size_t read(float* data, std::size_t frames) noexcept;

    /// Consumer role for an audio callback: reads what is available and zero-fills
    /// the remainder, so `data` is always a complete block. Returns the number of
    /// real frames read; 0 means the block was pure silence (an underrun).
    std::size_t readOrSilence(float* data, std::size_t frames) noexcept;

    /// Frames the producer had to drop because the consumer was too slow.
    std::uint64_t droppedFrames() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    /// Frames the consumer asked for while the buffer had none.
    std::uint64_t underrunFrames() const noexcept { return underrun_.load(std::memory_order_relaxed); }

    /// Clears the contents and the counters. Not thread safe: call only while both
    /// roles are stopped (used by reconfiguration and by tests).
    void reset() noexcept;

private:
    std::size_t nextPowerOfTwo(std::size_t value) noexcept;

    std::vector<float> data_;
    std::size_t capacity_ = 0;      ///< power of two, data_.size()
    std::size_t mask_ = 0;          ///< capacity_ - 1

    std::atomic<std::size_t> writePos_{ 0 };   ///< producer only
    std::atomic<std::size_t> readPos_{ 0 };    ///< consumer only

    std::atomic<std::uint64_t> dropped_{ 0 };
    std::atomic<std::uint64_t> underrun_{ 0 };
};

} // namespace audio
} // namespace liveai
