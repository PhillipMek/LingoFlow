#pragma once
//
// AudioEngine - the realtime stage between a device backend and the rest of the
// product (SPEC "Audio Pipeline", task 005).
//
//   ASIO Input -> GainStage (input) -> AudioRingBuffer -> translation worker
//   translated audio -> AudioJitterBuffer -> GainStage (output) -> ASIO Output
//
// The engine owns both buffers, both level meters, both gain stages and the
// underrun/overrun counters. Everything is allocated in activate(), on a non-realtime
// thread, before the device is started: the callback only ever touches preallocated
// memory, does fixed work and publishes relaxed atomics. It never allocates, locks,
// logs, sleeps or waits for the network (AGENTS.md 5).
//
// Gain stages are the exception to "released with the pipeline": they carry the
// operator's settings and the clipping history rather than device geometry, so they
// survive deactivate() and are re-configured (not recreated) by the next activate().
// A device restart must not silently reset the room's levels.
//
// Safety rule that the buffer design enforces: the only source of audio on the output
// side is the jitter buffer, and only the network/loopback side writes into it. Input
// audio can therefore never leak to the audience - an underrun plays silence, not the
// microphone.
//
// The input rings are drained by whoever is consuming translation: task 005 ships the
// loopback worker, task 012 the OpenAI streaming worker. While nobody is attached, the
// engine still measures the input but does not write it into the rings, and counts
// those frames separately (inputFramesNotForwarded) instead of reporting a fake
// overrun.
//
// Dependency direction (AGENTS.md 7): AudioEngine knows IAudioBackend only. It does
// not know about OpenAI, NDI or JUCE.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "Audio/AudioJitterBuffer.h"
#include "Audio/AudioRingBuffer.h"
#include "Audio/GainStage.h"
#include "Audio/IAudioBackend.h"
#include "Audio/LevelMeter.h"

namespace liveai {

class DiagnosticsManager;

class AudioEngine final : public audio::IAudioProcessor
{
public:
    /// `diagnostics` may be null; the engine then only keeps its own counters.
    explicit AudioEngine(DiagnosticsManager* diagnostics = nullptr) noexcept;

    audio::IAudioBackend* backend() const noexcept { return backend_; }

    /// Opens and starts the backend. Non-realtime thread only. `request` carries the
    /// sample rate, block size and the one-based channel indices from settings.
    /// The pipeline (buffers, meters, counters) is allocated here, before the device
    /// runs. On failure the backend is left closed and `error` says why.
    bool activate(audio::IAudioBackend& backend, const audio::DeviceRequest& request, std::string& error);

    /// Stops and closes the backend, then releases the pipeline. Non-realtime thread
    /// only. Any consumer of inputRing()/outputJitter() must be stopped first - the
    /// pointers handed out there are owned by this engine.
    void deactivate() noexcept;

    /// audio::IAudioProcessor - realtime thread.
    void processAudio(const float* const* input,
                      float* const* output,
                      int frameCount) noexcept override;

    void onAudioConfigurationChanged(int sampleRate, int bufferFrames) override;

    // --------------------------------------------------------------- pipeline state
    bool pipelineReady() const noexcept { return pipelineReady_.load(std::memory_order_relaxed); }
    int inputChannels() const noexcept { return inputChannels_.load(std::memory_order_relaxed); }
    int outputChannels() const noexcept { return outputChannels_.load(std::memory_order_relaxed); }

    /// Frames of device block currently configured (0 before activate()).
    int configuredBufferFrames() const noexcept { return bufferFrames_.load(std::memory_order_relaxed); }

    /// Non-realtime: change the output pre-roll while running. Raising it above the
    /// current fill re-primes the jitter buffer, i.e. adds audible delay on purpose.
    void setJitterBufferMs(int jitterBufferMs) noexcept;
    int jitterBufferMs() const noexcept { return jitterMs_.load(std::memory_order_relaxed); }

    /// Owner of the input rings: the loopback worker (005) or the streaming worker
    /// (012) attaches here. Non-realtime call, cheap atomic.
    void attachInputConsumer() noexcept { consumerAttached_.store(true, std::memory_order_relaxed); }
    void detachInputConsumer() noexcept { consumerAttached_.store(false, std::memory_order_relaxed); }
    bool inputConsumerAttached() const noexcept { return consumerAttached_.load(std::memory_order_relaxed); }

    // ------------------------------------------------------------------ gain (006)
    /// SPEC "Input Gain" and "Output Gain": two independent digital trims, settable at
    /// any time, including from the UI while the device runs. Both glide to the new
    /// coefficient instead of jumping, so turning a knob cannot click.
    ///
    /// Out-of-window values are clamped to GainStage::kMinGainDb..kMaxGainDb and counted
    /// (gainRequestsClamped()); non-finite values keep the previous level and are counted
    /// (gainRequestsRejected()). Nothing is accepted silently.
    void setInputGainDb(float gainDb) noexcept;
    void setOutputGainDb(float gainDb) noexcept;

    /// The level in effect for new requests - what the UI number box should show.
    float inputGainDb() const noexcept { return inputGainDb_.load(std::memory_order_relaxed); }
    float outputGainDb() const noexcept { return outputGainDb_.load(std::memory_order_relaxed); }

    /// SPEC "Input Gain" mute control. Input mute stops audio reaching the translator;
    /// output mute stops audio reaching the audience. They are different actions, so the
    /// engine keeps them apart instead of having one "mute" mean whichever the operator
    /// needed.
    void setInputMuted(bool muted) noexcept;
    void setOutputMuted(bool muted) noexcept;
    bool inputMuted() const noexcept { return inputMuted_.load(std::memory_order_relaxed); }
    bool outputMuted() const noexcept { return outputMuted_.load(std::memory_order_relaxed); }

    /// Glide length applied to every stage. 0 ms is accepted and documented as the one
    /// setting that can click; the default is GainStage::kDefaultRampMs.
    void setGainRampMs(int rampMs) noexcept;
    int gainRampMs() const noexcept { return gainRampMs_.load(std::memory_order_relaxed); }

    /// The coefficient the callback is actually applying, in dB. During a glide it lags
    /// the requested value, which is the honest readout for a "settling" indicator.
    /// All channels of a side take the same operator level, so channel 0 is the readout.
    float appliedInputGainDb() const noexcept;
    float appliedOutputGainDb() const noexcept;

    /// Clipping, as three separate facts, because they need three different reactions:
    ///  inputClippedFrames()      - full scale arriving from the device. Turning our own
    ///                              gain down cannot undo it, and a post-gain meter alone
    ///                              would hide a damaged console feed.
    ///  inputGainClippedFrames()  - full scale produced by the input trim, i.e. what the
    ///                              translator is being handed.
    ///  outputGainClippedFrames() - full scale on the way to the audience.
    std::uint64_t inputClippedFrames() const noexcept;
    std::uint64_t inputGainClippedFrames() const noexcept;
    std::uint64_t outputGainClippedFrames() const noexcept;

    /// Non-finite samples the input stage replaced by silence (counted, never swallowed).
    std::uint64_t nonFiniteInputFrames() const noexcept;

    /// Requests the engine had to clamp or refuse, summed over both sides.
    std::uint64_t gainRequestsClamped() const noexcept { return gainClamped_.load(std::memory_order_relaxed); }
    std::uint64_t gainRequestsRejected() const noexcept { return gainRejected_.load(std::memory_order_relaxed); }

    /// Latching indicators for the operator UI (SPEC "clipping indication"): true when
    /// anything reached full scale since this was last called.
    bool takeInputClipIndicator() noexcept;
    bool takeOutputClipIndicator() noexcept;

    /// Buffer endpoints for the non-realtime side. Valid from activate() until
    /// deactivate(); nullptr when the channel index is out of range or the pipeline
    /// is not built. The engine keeps ownership.
    audio::AudioRingBuffer* inputRing(int channel) noexcept;
    audio::AudioJitterBuffer* outputJitter(int channel) noexcept;

    const audio::LevelMeter* inputMeter(int channel) const noexcept;
    const audio::LevelMeter* outputMeter(int channel) const noexcept;

    // ---------------------------------------------------------------- live readouts
    std::uint64_t blockCount() const noexcept { return blocks_.load(std::memory_order_relaxed); }
    std::uint64_t frameCount() const noexcept { return frames_.load(std::memory_order_relaxed); }
    int sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    int bufferFrames() const noexcept { return bufferFrames_.load(std::memory_order_relaxed); }

    /// Device frames seen in the callback.
    std::uint64_t inputFramesCaptured() const noexcept { return inputCaptured_.load(std::memory_order_relaxed); }

    /// Frames written into the input rings for the consumer.
    std::uint64_t inputFramesForwarded() const noexcept { return inputForwarded_.load(std::memory_order_relaxed); }

    /// Frames not forwarded because no consumer was attached.
    std::uint64_t inputFramesNotForwarded() const noexcept { return inputDropped_.load(std::memory_order_relaxed); }

    /// Frames the rings lost because the consumer stalled (ring overflow). Kept as an
    /// engine-level total, not read out of the buffers: deactivate() releases the
    /// buffers, and a counter that silently turns into 0 after shutdown would be worse
    /// than useless for the operator's diagnostics.
    std::uint64_t inputRingDroppedFrames() const noexcept { return ringDropped_.load(std::memory_order_relaxed); }

    /// Frames played as silence (jitter underflow or pre-roll).
    std::uint64_t outputSilenceFrames() const noexcept { return outputSilence_.load(std::memory_order_relaxed); }

    /// Blocks that had to be partly or fully silence.
    std::uint64_t underrunEvents() const noexcept { return underruns_.load(std::memory_order_relaxed); }

    /// Blocks in which the input ring overflowed.
    std::uint64_t overrunEvents() const noexcept { return overruns_.load(std::memory_order_relaxed); }

    /// Callbacks that did not match the IAudioProcessor contract (null channel
    /// arrays). A real device should never produce these: if this grows, the backend
    /// is broken and no audio can be trusted. Counted instead of silently absorbed.
    std::uint64_t malformedCallbacks() const noexcept { return malformedCallbacks_.load(std::memory_order_relaxed); }

    /// Callbacks that delivered more frames than one gain chunk can hold
    /// (kGainChunkFrames). They are still processed correctly - in chunks - and the
    /// counter exists because such a block is outside the geometry the pipeline was
    /// built for, so the operator must be able to see that it happened.
    std::uint64_t oversizedCallbacks() const noexcept { return oversized_.load(std::memory_order_relaxed); }

    /// Current stored delay in the output jitter buffer, in frames (channel 0).
    std::size_t jitterFillFrames() const noexcept;

private:
    /// Allocates the pipeline from the opened device's geometry. Non-realtime.
    bool buildPipeline(int sampleRate, int blockFrames, int inputChannels, int outputChannels,
                       std::string& error);
    void releasePipeline() noexcept;

    /// Frames of preallocated scratch that one input gain stage consumes at a time.
    /// Fixed rather than taken from the device, so a block bigger than the configured
    /// one is gain-processed in chunks instead of overrunning anything. 4096 frames is
    /// 85 ms at 48 kHz and above any ASIO block size in use.
    static constexpr std::size_t kGainChunkFrames = 4096;

    /// Makes sure each side has one stage per channel and the input scratch is big
    /// enough. Non-realtime: may allocate, so it runs inside buildPipeline's try.
    void growGainStages(int inputChannels, int outputChannels);

    /// Pushes the operator's gain, mute and glide settings into every stage and configures
    /// the sample rate they convert ms into samples with. Non-realtime.
    void applyGainToStages(int sampleRate);

    /// Sum of a per-stage counter across one side. Non-realtime read path.
    std::uint64_t sumInputStages(std::uint64_t (audio::GainStage::*counter)() const noexcept) const noexcept;
    std::uint64_t sumOutputStages(std::uint64_t (audio::GainStage::*counter)() const noexcept) const noexcept;

    audio::IAudioBackend* backend_ = nullptr;
    DiagnosticsManager* diagnostics_ = nullptr;

    std::vector<std::unique_ptr<audio::AudioRingBuffer>> inputRings_;
    std::vector<std::unique_ptr<audio::AudioJitterBuffer>> outputJitters_;
    std::vector<std::unique_ptr<audio::LevelMeter>> inputMeters_;
    std::vector<std::unique_ptr<audio::LevelMeter>> outputMeters_;

    /// Gain stages are per channel but belong to the operator, not to the device:
    /// releasePipeline() deliberately keeps them, so their settings and their clipping
    /// history outlive a restart of the audio path.
    std::vector<std::unique_ptr<audio::GainStage>> inputStages_;
    std::vector<std::unique_ptr<audio::GainStage>> outputStages_;

    /// kGainChunkFrames floats per input channel. The callback copies the device block
    /// here because the device hands it over read-only and gain has to be applied before
    /// the ring write (SPEC "Audio Ring Buffer").
    std::vector<float> gainScratch_;

    /// Frames per channel that a ring/jitter must be able to hold without dropping.
    static std::size_t inputRingCapacity(int sampleRate, int blockFrames) noexcept;
    static std::size_t jitterCapacity(int sampleRate, int jitterMs, int blockFrames) noexcept;
    static std::size_t jitterTargetFrames(int sampleRate, int jitterMs) noexcept;

    std::atomic<bool> pipelineReady_{ false };
    std::atomic<bool> consumerAttached_{ false };
    std::atomic<int> inputChannels_{ 0 };
    std::atomic<int> outputChannels_{ 0 };
    std::atomic<int> jitterMs_{ 120 };

    std::atomic<std::uint64_t> blocks_{ 0 };
    std::atomic<std::uint64_t> frames_{ 0 };
    std::atomic<int> sampleRate_{ 0 };
    std::atomic<int> bufferFrames_{ 0 };

    std::atomic<std::uint64_t> inputCaptured_{ 0 };
    std::atomic<std::uint64_t> inputForwarded_{ 0 };
    std::atomic<std::uint64_t> inputDropped_{ 0 };
    std::atomic<std::uint64_t> outputSilence_{ 0 };
    std::atomic<std::uint64_t> underruns_{ 0 };
    std::atomic<std::uint64_t> overruns_{ 0 };
    std::atomic<std::uint64_t> ringDropped_{ 0 };
    std::atomic<std::uint64_t> malformedCallbacks_{ 0 };
    std::atomic<std::uint64_t> oversized_{ 0 };

    /// The operator's gain state, kept here rather than only in the stages so that a
    /// level can be set before the device exists and survives a restart of the pipeline.
    std::atomic<float> inputGainDb_{ 0.0f };
    std::atomic<float> outputGainDb_{ 0.0f };
    std::atomic<bool> inputMuted_{ false };
    std::atomic<bool> outputMuted_{ false };
    std::atomic<int> gainRampMs_{ audio::GainStage::kDefaultRampMs };
    std::atomic<std::uint64_t> gainClamped_{ 0 };
    std::atomic<std::uint64_t> gainRejected_{ 0 };
};

} // namespace liveai
