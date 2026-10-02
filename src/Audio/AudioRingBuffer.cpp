#include "Audio/AudioRingBuffer.h"

#include <cstring>

namespace liveai {
namespace audio {
namespace {

/// Rounds up to a power of two so the index mask is a bitwise AND in the callback.
/// 0 and 1 both become 1; the largest sane audio block is far below the overflow
/// range of size_t on x64.
std::size_t roundUpToPowerOfTwo(std::size_t value) noexcept
{
    if (value <= 1)
        return 1;

    std::size_t power = 1;

    while (power < value)
        power <<= 1;

    return power;
}

} // namespace

AudioRingBuffer::AudioRingBuffer(std::size_t capacityFrames)
{
    if (capacityFrames == 0)
        return;

    capacity_ = roundUpToPowerOfTwo(capacityFrames);
    mask_ = capacity_ - 1;
    data_.assign(capacity_, 0.0f);
}

std::size_t AudioRingBuffer::readable() const noexcept
{
    const auto write = writePos_.load(std::memory_order_acquire);
    const auto read = readPos_.load(std::memory_order_acquire);
    return write - read;
}

std::size_t AudioRingBuffer::writable() const noexcept
{
    if (capacity_ == 0)
        return 0;

    return capacity_ - readable();
}

std::size_t AudioRingBuffer::write(const float* data, std::size_t frames) noexcept
{
    if (data == nullptr || frames == 0 || capacity_ == 0)
        return 0;

    // Producer: read position is observed with acquire so the consumer's releases
    // are visible; the write position itself is published with release.
    const auto read = readPos_.load(std::memory_order_acquire);
    const auto write = writePos_.load(std::memory_order_relaxed);

    std::size_t space = capacity_ - static_cast<std::size_t>(write - read);

    if (space == 0)
    {
        dropped_.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }

    const std::size_t accepted = frames < space ? frames : space;

    if (accepted != frames)
        dropped_.fetch_add(frames - accepted, std::memory_order_relaxed);

    const std::size_t firstIndex = write & mask_;
    const std::size_t firstLength = accepted < capacity_ - firstIndex ? accepted : capacity_ - firstIndex;

    std::memcpy(data_.data() + firstIndex, data, firstLength * sizeof(float));

    if (accepted != firstLength)
        std::memcpy(data_.data(), data + firstLength, (accepted - firstLength) * sizeof(float));

    writePos_.store(write + accepted, std::memory_order_release);
    return accepted;
}

std::size_t AudioRingBuffer::read(float* data, std::size_t frames) noexcept
{
    if (data == nullptr || frames == 0 || capacity_ == 0)
        return 0;

    const auto write = writePos_.load(std::memory_order_acquire);
    const auto read = readPos_.load(std::memory_order_relaxed);

    const std::size_t available = static_cast<std::size_t>(write - read);

    if (available == 0)
    {
        underrun_.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }

    const std::size_t taken = frames < available ? frames : available;
    const std::size_t firstIndex = read & mask_;
    const std::size_t firstLength = taken < capacity_ - firstIndex ? taken : capacity_ - firstIndex;

    std::memcpy(data, data_.data() + firstIndex, firstLength * sizeof(float));

    if (taken != firstLength)
        std::memcpy(data + firstLength, data_.data(), (taken - firstLength) * sizeof(float));

    readPos_.store(read + taken, std::memory_order_release);

    if (taken != frames)
        underrun_.fetch_add(frames - taken, std::memory_order_relaxed);

    return taken;
}

std::size_t AudioRingBuffer::readOrSilence(float* data, std::size_t frames) noexcept
{
    if (data == nullptr || frames == 0)
        return 0;

    const std::size_t taken = read(data, frames);

    if (taken != frames)
        std::memset(data + taken, 0, (frames - taken) * sizeof(float));

    return taken;
}

void AudioRingBuffer::reset() noexcept
{
    writePos_.store(0, std::memory_order_relaxed);
    readPos_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    underrun_.store(0, std::memory_order_relaxed);

    for (auto& sample : data_)
        sample = 0.0f;
}

} // namespace audio
} // namespace liveai
