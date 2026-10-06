#pragma once
//
// GainStage - the digital gain block of spec "Input Gain" / "Output Gain".
//
// One class serves both positions of spec "Audio Pipeline", because the requirement is
// the same in each: a level the operator sets, that must never click.
//
//   ASIO Input -> [GainStage: input]  -> Ring Buffer  -> translation
//   Jitter Buffer -> [GainStage: output] -> ASIO Output
//
// Contract (spec "Input Gain DSP Requirements", the project rules):
//   * process() runs on the audio callback. It touches no allocation, no lock, no log,
//     no filesystem, no network, no UI. Work per sample: one finite check, one multiply,
//     two magnitude compares.
//   * setGainDb()/setMuted()/setRampMs() are safe from any thread, including the UI
//     thread while the device is running: they publish relaxed atomics only, and the
//     callback picks them up at the start of the next block.
//   * configure() and reset() touch the ramp state and belong on a control thread while
//     the callback is not running - the same rule the pipeline buffers follow.
//   * a gain change is glided, not jumped: the coefficient moves toward the requested
//     value in a fixed number of samples (default kDefaultRampMs), which is what keeps a
//     knob move from producing the click that an instant coefficient change always does
//     at a waveform's nonzero sample.
//
// What this block deliberately does NOT do: it does not limit. Samples above full scale
// stay above full scale so the meter and the operator see the truth (SPEC lists a
// limiter under "Future architecture"). The only exception is a sample that is not a
// finite number: NaN or Inf is not audio, it is replaced by silence or full scale, and
// every such replacement is counted, never swallowed.
//
// Clipping is counted twice on purpose, because the two numbers mean different things:
//   clippedInFrames()  - full-scale samples arriving from the device. Turning the input
//                        gain down cannot undo that, and a meter placed after the gain
//                        would otherwise hide a damaged console feed.
//   clippedOutFrames() - full-scale samples this stage produced. That is our own trim
//                        creating the problem, which is what the operator needs to hear
//                        about when they push +24 dB.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace liveai {
namespace audio {

class GainStage final
{
public:
    /// spec "Input Gain" suggests -24..+24 dB and keeps the range configurable. The
    /// stage accepts the SPEC ceiling exactly and goes to -96 dB below the SPEC floor, so
    /// a setting the configuration layer validated is never quietly contradicted here.
    static constexpr float kMinGainDb = -96.0f;
    static constexpr float kMaxGainDb = 24.0f;

    /// Glide time for a gain or mute change. 20 ms is two blocks at 48 kHz/480 frames:
    /// long enough that the coefficient step per sample stays far below the slew rate of
    /// speech, short enough that the knob feels immediate. A documented design constant,
    /// not a measured value.
    static constexpr int kDefaultRampMs = 20;

    /// Upper bound for the glide, so a hand-edited setting cannot ask for ten seconds.
    static constexpr int kMaxRampMs = 500;

    /// |value| at or above this is clipping, by definition of full scale.
    static constexpr float kFullScale = 1.0f;

    GainStage() noexcept;

    /// Control thread, device not running: sets the sample rate the glide is computed
    /// against and lands the coefficient on the current request (no ramp at start-up -
    /// the first block must already play the level the UI shows).
    void configure(int sampleRate, int rampMs) noexcept;

    /// Control thread, device not running: jumps the coefficient to the current request
    /// and forgets any glide in progress. Counters are kept - this re-aligns the state,
    /// it does not reset the history.
    void reset() noexcept;

    /// Any thread, including while running: changes the glide length. Only affects the
    /// next glide, so it cannot click.
    void setRampMs(int rampMs) noexcept;

    /// Any thread. Out-of-window values are clamped and counted; non-finite values are
    /// refused and counted. Either way the request is never silently accepted.
    void setGainDb(float gainDb) noexcept;
    void setMuted(bool muted) noexcept;

    /// Realtime. Applies the ramped coefficient to `frames` samples.
    /// @{ The in-place form matches the spec "Input Gain DSP Requirements" signature;
    /// the two-buffer form is for a read-only device pointer, and passing the same
    /// pointer twice is allowed.
    void process(float* data, std::size_t frames) noexcept;
    void process(const float* input, float* output, std::size_t frames) noexcept;
    /// @}

    // ------------------------------------------------------------------- readbacks
    /// The requested value, after clamping (what the UI should show).
    float gainDb() const noexcept { return gainDb_.load(std::memory_order_relaxed); }
    bool muted() const noexcept { return muted_.load(std::memory_order_relaxed); }
    int rampMs() const noexcept { return rampMs_.load(std::memory_order_relaxed); }
    int sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }

    /// Coefficient the callback last applied, linear. Use audio::linearToDb() for a
    /// dB readout: it lags gainDb() while a glide is in progress, which is the truth.
    float appliedLinear() const noexcept { return appliedLinear_.load(std::memory_order_relaxed); }

    // -------------------------------------------------------------------- counters
    /// Full-scale samples seen before gain (a fact about the device and the console).
    std::uint64_t clippedInFrames() const noexcept { return clippedIn_.load(std::memory_order_relaxed); }

    /// Full-scale or overflowing samples this stage produced.
    std::uint64_t clippedOutFrames() const noexcept { return clippedOut_.load(std::memory_order_relaxed); }

    /// Non-finite input samples replaced by silence.
    std::uint64_t nonFiniteInFrames() const noexcept { return nonFiniteIn_.load(std::memory_order_relaxed); }

    /// setGainDb() calls that were out of window and therefore clamped.
    std::uint64_t clampedSets() const noexcept { return clampedSets_.load(std::memory_order_relaxed); }

    /// setGainDb() calls with NaN/Inf that were refused, leaving the previous value.
    std::uint64_t rejectedSets() const noexcept { return rejectedSets_.load(std::memory_order_relaxed); }

    /// process() calls with a null buffer or null destination: a caller contract
    /// violation, counted instead of silently doing nothing.
    std::uint64_t invalidCalls() const noexcept { return invalidCalls_.load(std::memory_order_relaxed); }

    std::uint64_t blocksProcessed() const noexcept { return blocks_.load(std::memory_order_relaxed); }
    std::uint64_t samplesProcessed() const noexcept { return samples_.load(std::memory_order_relaxed); }

    /// True once any full-scale sample was seen at either side since the last call.
    /// The UI uses this for the clipping indicator (spec "clipping indication") and does
    /// not have to remember counters to render it. Consumes the latch.
    bool takeClipIndicator() noexcept { return clipIndicator_.exchange(false, std::memory_order_relaxed); }

    /// Linear multiplier for a dB value. exp2 rather than powf: fixed cost, no libm
    /// path that could allocate, and exactly what a 20*log10() readout inverts.
    static float dbToLinear(float gainDb) noexcept;

private:
    /// The coefficient the glide is heading toward right now (mute wins over the value).
    float targetLinear() const noexcept;

    std::atomic<float> gainDb_{ 0.0f };
    std::atomic<float> appliedLinear_{ 1.0f };
    std::atomic<bool> muted_{ false };
    std::atomic<int> sampleRate_{ 48000 };
    std::atomic<int> rampMs_{ kDefaultRampMs };
    std::atomic<int> rampSamples_{ 960 };

    std::atomic<std::uint64_t> clippedIn_{ 0 };
    std::atomic<std::uint64_t> clippedOut_{ 0 };
    std::atomic<std::uint64_t> nonFiniteIn_{ 0 };
    std::atomic<std::uint64_t> clampedSets_{ 0 };
    std::atomic<std::uint64_t> rejectedSets_{ 0 };
    std::atomic<std::uint64_t> invalidCalls_{ 0 };
    std::atomic<std::uint64_t> blocks_{ 0 };
    std::atomic<std::uint64_t> samples_{ 0 };
    std::atomic<bool> clipIndicator_{ false };

    /// Callback-thread private: the coefficient in progress and the samples left to glide.
    float current_ = 1.0f;
    std::size_t rampRemaining_ = 0;
};

} // namespace audio
} // namespace liveai
