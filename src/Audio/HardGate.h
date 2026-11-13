#pragma once
//
// HardGate - the optional hard noise gate of the INPUT path only.
//
//   ASIO Input -> GainStage (input) -> [HardGate] -> Ring Buffer -> translation
//
// It sits between the input trim and everything downstream that consumes the
// captured stream (the ring buffer, and through it the loopback / streaming
// worker). It is never placed on the output side: translated audio, playback
// and the deliberate bypass are the audience's stream, and gating the audience
// is not what this block was asked for.
//
// Behaviour (the gate task's contract):
//   * the block RMS is measured from the post-gain signal at the start of every
//     chunk, in the same pass that applies the gate - no separate monitor thread,
//     no queue, no lookahead. The decision acts on the block it was measured
//     from, so the gate adds ZERO latency;
//   * RMS >= threshold  -> the gate is OPEN: the block passes through
//     bit-for-bit untouched (no multiply, same steady-state rule GainStage uses
//     for 0 dB, which is what makes "disabled = original stream" provable);
//     the "at or above" boundary carries a documented 1e-6 relative measurement
//     margin (kThresholdMargin) so a signal exactly at the threshold opens
//     deterministically instead of flipping on float rounding;
//   * RMS <  threshold  -> the gate is CLOSED: the block is muted completely.
//     The steady closed state is a memset to exact zeros, so "silence" is a
//     value, not a very small number, and no poisoned sample can leak;
//   * switching between the two states is a short glide, not a jump: dropping
//     the coefficient at a nonzero sample is the click that an instant gate
//     always produces at the waveform's phase. The ramp lengths are internal
//     design constants (fast to open, slower to close) - deliberately NOT
//     operator settings, the task says so. Landing exactly on 1.0 / 0.0 keeps
//     the two steady states pure.
//
// Contract (the same shape GainStage established, and the project rules):
//   * process() runs on the audio callback. No allocation, no lock, no log, no
//     filesystem, no network, no UI; relaxed atomics and fixed-cost math only
//     (tests/RealtimeSafetyAudit.cmake scans the body).
//   * setEnabled()/setThresholdDb() are safe from any thread, including the UI
//     thread while the device runs: they publish relaxed atomics and the
//     callback picks them up at the start of the next chunk. Enabling or
//     disabling never interrupts the stream - the device and the rings are
//     untouched, only the coefficient target moves.
//   * configure() and reset() touch the ramp state and belong on a control
//     thread while the callback is not running - the same rule every other
//     stage follows.
//   * out-of-window thresholds are clamped and counted; non-finite values keep
//     the previous threshold and are counted. Nothing is refused silently.
//   * input is expected to have passed the GainStage already (finite samples).
//     The RMS pass treats a non-finite sample as contributing silence and
//     counts it (nonFiniteSeenFrames()) - replacing samples is the gain
//     stage's documented job, not this stage's; in the closed state everything
//     is zeroed regardless.
//
// Deliberate simplifications: no hysteresis, no hold, no sidechain. A signal
// hovering around the threshold modulates the coefficient between 1 and 0 at
// block rate - bounded by the glides, never a step discontinuity. If a venue
// ever hears that as nervous, the fix is a different threshold, not four new
// knobs on the operator's panel.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace liveai {
namespace audio {

class HardGate final
{
public:
    /// The stage accepts the same floor the meter readouts use (kMeterFloorDb) and a
    /// ceiling of full scale: a threshold of 0 dBFS RMS can only be crossed by a
    /// signal that never leaves the building. The CONFIG window is narrower
    /// (ConfigSchema's gateThresholdRange); the stage window bounds what the
    /// callback will execute, the same split as GainStage.
    static constexpr float kMinThresholdDb = -120.0f;
    static constexpr float kMaxThresholdDb = 0.0f;

    /// The documented default (-40 dBFS): above the -60 dBFS "signal present"
    /// floor the meters already publish (LevelMeter::kSignalPresentRms), below
    /// conversational level at the pipeline's unity default. A setting, not a
    /// measurement - the venue picks its own number against its own room tone.
    static constexpr float kDefaultThresholdDb = -40.0f;

    /// The two directions' steady targets are exactly 1.0 (pass) and 0.0 (mute);
    /// the boundary wording of the task is "at or above the threshold passes".
    /// A constant-amplitude block measured at exactly the threshold can land one
    /// float ulp below it because of RMS accumulation rounding, so the decision
    /// carries a 1e-6 relative margin (0.0000087 dB): an audible signal cannot
    /// tell it, and the test for "exactly at threshold -> open" stops being a
    /// coin flip against the rounding mode. The margin is internal, not a knob.
    static constexpr double kThresholdMargin = 0.999999;

    /// Internal glide lengths for the two directions. Opening is fast (a
    /// transients is only waiting 5 ms) and closing is slower (40 ms): a slow
    /// fade-out is what keeps a dropped syllable tail from clicking and what
    /// turns borderline chatter into a smooth tremolo instead of a step.
    /// These are design constants; the operator sees neither of them.
    static constexpr int kOpenRampMs = 5;
    static constexpr int kCloseRampMs = 40;

    HardGate() noexcept;

    /// Control thread, device not running: sets the sample rate the glides are
    /// computed against and lands the coefficient on open, so the first block of
    /// a fresh run is never muted before its own RMS has been measured.
    void configure(int sampleRate) noexcept;

    /// Control thread, device not running: coefficient back to open, any glide
    /// in progress forgotten. Counters are kept - this re-aligns state, it does
    /// not reset history.
    void reset() noexcept;

    /// Any thread, including while running. The gate is bypassed entirely while
    /// disabled: process() leaves the buffer untouched (once the opening glide
    /// from a closed state has finished), so the ring receives exactly what the
    /// gain stage produced - the unmodified input stream.
    void setEnabled(bool enabled) noexcept;

    /// Any thread. Out-of-window values are clamped and counted; non-finite
    /// values are refused and counted, keeping the previous threshold.
    void setThresholdDb(float thresholdDb) noexcept;

    /// Realtime. Measures the block, decides, and applies the gate coefficient
    /// in place. One call per gain chunk; the decision is per chunk, uniform
    /// across it (a chunk cannot be half open and half closed).
    void process(float* data, std::size_t frames) noexcept;

    // ------------------------------------------------------------------- readbacks
    /// The requested state after clamping (what the UI should show).
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    float thresholdDb() const noexcept { return thresholdDb_.load(std::memory_order_relaxed); }

    /// The decision the callback made for the most recent measured chunk. False
    /// before the first chunk with the gate enabled - no decision has happened
    /// yet, and "not open" is the honest reading of "not decided".
    bool open() const noexcept { return open_.load(std::memory_order_relaxed); }

    /// The coefficient the callback is actually applying. During a glide it lags
    /// the decision; 1.0 with the gate disabled is the settled bypass.
    float appliedLinear() const noexcept { return appliedLinear_.load(std::memory_order_relaxed); }

    // -------------------------------------------------------------------- counters
    /// Chunk-mutes: blocks decided closed, counted with their full frames
    /// (samples silenced by the closed gate, the ramp-out included - the block
    /// did not reach the translator as it was). While the gate is DISABLED
    /// neither this nor the open/closed block counters move: a bypass is
    /// absence of the gate, not a gate decision.
    std::uint64_t closedSamples() const noexcept { return closedSamples_.load(std::memory_order_relaxed); }
    std::uint64_t closedBlocks() const noexcept { return closedBlocks_.load(std::memory_order_relaxed); }

    /// Blocks decided open while the gate was enabled.
    std::uint64_t openBlocks() const noexcept { return openBlocks_.load(std::memory_order_relaxed); }

    /// Coefficient transitions between the two steady states, both directions:
    /// the operator-visible "how nervous is the room" number.
    std::uint64_t transitions() const noexcept { return transitions_.load(std::memory_order_relaxed); }

    /// Non-finite samples seen by the RMS pass (counted, never swallowed; the
    /// gain stage upstream normally removes them - seeing one here means the
    /// stage was driven outside the engine).
    std::uint64_t nonFiniteSeenFrames() const noexcept { return nonFiniteSeen_.load(std::memory_order_relaxed); }

    /// setThresholdDb() calls that were out of window and therefore clamped.
    std::uint64_t clampedSets() const noexcept { return clampedSets_.load(std::memory_order_relaxed); }

    /// setThresholdDb() calls with NaN/Inf that were refused, leaving the previous.
    std::uint64_t rejectedSets() const noexcept { return rejectedSets_.load(std::memory_order_relaxed); }

    /// process() calls with a null buffer: a caller contract violation, counted
    /// instead of silently doing nothing.
    std::uint64_t invalidCalls() const noexcept { return invalidCalls_.load(std::memory_order_relaxed); }

    std::uint64_t blocksProcessed() const noexcept { return blocks_.load(std::memory_order_relaxed); }
    std::uint64_t samplesProcessed() const noexcept { return samples_.load(std::memory_order_relaxed); }

    /// Zero the SIGNAL facts (block/sample totals, decisions, transitions, the
    /// non-finite sightings, contract violations) but NOT the settings - the
    /// gate's enable flag and threshold belong to the operator across a device
    /// restart, exactly like the gain levels. Called between runs from
    /// AudioEngine::resetRunCounters(), never while the callback uses the stage.
    void resetSignalCounters() noexcept
    {
        closedSamples_.store(0, std::memory_order_relaxed);
        closedBlocks_.store(0, std::memory_order_relaxed);
        openBlocks_.store(0, std::memory_order_relaxed);
        transitions_.store(0, std::memory_order_relaxed);
        nonFiniteSeen_.store(0, std::memory_order_relaxed);
        invalidCalls_.store(0, std::memory_order_relaxed);
        blocks_.store(0, std::memory_order_relaxed);
        samples_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<bool> enabled_{ false };
    std::atomic<float> thresholdDb_{ kDefaultThresholdDb };
    std::atomic<bool> open_{ false };
    std::atomic<float> appliedLinear_{ 1.0f };
    std::atomic<int> sampleRate_{ 48000 };

    /// Glide lengths in samples, derived from the ms constants by configure().
    std::atomic<std::size_t> openRampSamples_{ 240 };    ///< 5 ms at 48 kHz
    std::atomic<std::size_t> closeRampSamples_{ 1920 };  ///< 40 ms at 48 kHz

    std::atomic<std::uint64_t> closedSamples_{ 0 };
    std::atomic<std::uint64_t> closedBlocks_{ 0 };
    std::atomic<std::uint64_t> openBlocks_{ 0 };
    std::atomic<std::uint64_t> transitions_{ 0 };
    std::atomic<std::uint64_t> nonFiniteSeen_{ 0 };
    std::atomic<std::uint64_t> clampedSets_{ 0 };
    std::atomic<std::uint64_t> rejectedSets_{ 0 };
    std::atomic<std::uint64_t> invalidCalls_{ 0 };
    std::atomic<std::uint64_t> blocks_{ 0 };
    std::atomic<std::uint64_t> samples_{ 0 };

    /// Callback-thread private (one thread ever touches them, the same rule as
    /// GainStage): the coefficient in progress and the samples left to glide.
    float current_ = 1.0f;
    std::size_t rampRemaining_ = 0;

    /// The steady state the last chunk headed for. Callback-thread private like
    /// the two above; a change against it is what the transitions counter counts
    /// ("the gate switched", once per run of blocks in either direction).
    bool lastTargetOpen_ = true;
};

} // namespace audio
} // namespace liveai
