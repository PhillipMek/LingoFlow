#pragma once
//
// LevelMeter - block peak/RMS metering for the realtime path (spec "Meters",
// the meter design). The gain that acts on these numbers is GainStage: the engine
// measures the post-gain block, so the knob visibly moves the meter, while what arrived
// at full scale before our trim is counted by the stage - attenuation cannot hide it.
//
// Contract:
//   * measure() runs on the audio callback. It only walks the block once, does fixed
//     math (abs, compare, accumulate, one sqrt) and publishes plain atomics. No
//     allocation, no lock, no logging.
//   * every getter is safe from any thread and returns the state of the most recent
//     measured block. The UI polls it; it must not expect a callback.
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

/// The shared floor every meter readout uses: linearToDb() clamps to it, silence
/// is a VALUE (-120 dBFS) and not minus infinity, and the display smoother treats
/// "no data" by gliding to it. Documented once here so no surface invents its own.
inline constexpr float kMeterFloorDb = -120.0f;

/// Time-aware attack/release smoothing for DISPLAYING a meter level (venue task
/// 3 §3: the bar jumped with every raw block value and read as nervous).
///
/// The separation is the point: LevelMeter keeps the real-time measurement -
/// the fact of the last block, published atomically from the callback - and
/// this class only filters what the UI already read. It never runs in the
/// callback, never allocates, never locks; the UI thread that owns a widget
/// calls update() with the milliseconds elapsed since its own previous call,
/// and the coefficient is DERIVED from that interval (first-order exponential:
/// 1 - e^(-dt/tau)). Consequences the tests pin:
///   * the display moves at a physical speed, not per-frame: ten 20 ms steps
///     and one 200 ms step converge to the same value, so a 30 fps machine and
///     a 144 fps machine show the same meter;
///   * attack (rising) is fast - 60 ms; release (falling) is slow - 350 ms:
///     a jump must look immediate to trust, a decay may look smooth;
///   * it never overshoots the raw target and never goes below the floor;
///   * "no new data" is the CALLER's fact: the widget feeds the floor when the
///     meter's blocksMeasured() stopped advancing, and the display glides to
///     silence instead of freezing at the last level (OperatorWindow does
///     exactly that).
class MeterSmoother final
{
public:
    struct Display
    {
        float peakDb;
        float rmsDb;
    };

    /// One glance at the raw dB values, dtMs = milliseconds since the previous
    /// update() call (<= 0 means "same instant": the values hold their ground).
    /// Non-finite raw values are treated as the floor - a poisoned measurement
    /// must not poison the display either.
    Display update(float rawPeakDb, float rawRmsDb, double elapsedMs) noexcept;

    /// Back to the floor. When a widget is shown again, the first fresh frame
    /// attacks up from silence - which is the honest story, not a frozen bar
    /// from a previous run.
    void reset() noexcept
    {
        peak_ = kMeterFloorDb;
        rms_ = kMeterFloorDb;
    }

    static constexpr double kAttackTauMs = 60.0;
    static constexpr double kReleaseTauMs = 350.0;

private:
    float peak_ = kMeterFloorDb;
    float rms_ = kMeterFloorDb;
};

} // namespace audio
} // namespace liveai
