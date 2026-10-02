#pragma once
//
// LevelMeter - block peak/RMS metering for the realtime path (SPEC "Meters",
// task 005). The gain that acts on these numbers is GainStage (task 006): the engine
// measures the post-gain block, so the knob visibly moves the meter, while what arrived
// at full scale before our trim is counted by the stage - attenuation cannot hide it.
//
// Contract:
//   * measure() runs on the audio callback. It only walks the block once, does fixed
//     math (abs, compare, accumulate, one sqrt) and publishes plain atomics. No
//     allocation, no lock, no logging.
//   * every getter is safe from any thread and returns the state of the most recent
//     measured block. The UI polls it (task 014); it must not expect a callback.
//   * clipFrames() counts samples at or above full scale. That is a fact about the
//     signal, not a threshold guess: |value| >= 1.0 is clipping by definition.
//
// signalPresent() uses one documented constant (kSignalPresentRms, -60 dBFS) so the
// UI can say "no signal" instead of showing a dead meter; it is a display threshold,
// not a measurement of the room.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace liveai {
namespace audio {

class LevelMeter final
{
public:
    LevelMeter() noexcept = default;

    /// Realtime entry point: call once per block per channel.
    void measure(const float* data, std::size_t frames) noexcept;

    /// Peak magnitude of the most recent block, 0..>1 (may exceed 1.0 when the device
    /// already delivers clipped data).
    float peakLinear() const noexcept { return peak_.load(std::memory_order_relaxed); }

    /// RMS of the most recent block.
    float rmsLinear() const noexcept { return rms_.load(std::memory_order_relaxed); }

    /// Blocks measured since construction or the last reset().
    std::uint64_t blocksMeasured() const noexcept { return blocks_.load(std::memory_order_relaxed); }

    /// Samples with |value| >= 1.0 since construction or the last reset().
    std::uint64_t clipFrames() const noexcept { return clipFrames_.load(std::memory_order_relaxed); }

    /// Blocks whose RMS was above the display threshold.
    std::uint64_t signalledBlocks() const noexcept { return signalledBlocks_.load(std::memory_order_relaxed); }

    /// True when the most recent block carried audio (see the class comment).
    bool signalPresent() const noexcept;

    /// Clears peak/rms and all counters. Idle-time only (tests, reconfiguration).
    void reset() noexcept;

    /// RMS level that counts as "there is signal", chosen as -60 dBFS. Display
    /// threshold, documented in README/device notes, not a measured room level.
    static constexpr float kSignalPresentRms = 0.001f;

private:
    std::atomic<float> peak_{ 0.0f };
    std::atomic<float> rms_{ 0.0f };
    std::atomic<std::uint64_t> blocks_{ 0 };
    std::atomic<std::uint64_t> clipFrames_{ 0 };
    std::atomic<std::uint64_t> signalledBlocks_{ 0 };
};

/// dBFS for a linear magnitude. Values at or below 0 return the floor (-120 dB) so a
/// silent channel reads as silence instead of -inf, which the UI cannot format.
float linearToDb(float linear) noexcept;

} // namespace audio
} // namespace liveai
