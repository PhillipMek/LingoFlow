#pragma once
//
// AudioEngine - the realtime stage between a device backend and the rest of the
// product (SPEC "Audio Pipeline", task 005).
//
//   ASIO Input -> [input gain, task 006] -> AudioRingBuffer -> translation worker
//   translated audio -> AudioJitterBuffer -> [output gain, task 006] -> ASIO Output
//
// The engine owns both buffers, both level meters and the underrun/overrun counters.
// Everything is allocated in activate(), on a non-realtime thread, before the device
// is started: the callback only ever touches preallocated memory, does fixed work and
// publishes relaxed atomics. It never allocates, locks, logs, sleeps or waits for the
// network (AGENTS.md 5).
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

    /// Current stored delay in the output jitter buffer, in frames (channel 0).
    std::size_t jitterFillFrames() const noexcept;

private:
    /// Allocates the pipeline from the opened device's geometry. Non-realtime.
    bool buildPipeline(int sampleRate, int blockFrames, int inputChannels, int outputChannels,
                       std::string& error);
    void releasePipeline() noexcept;

    /// Frames per channel that a ring/jitter must be able to hold without dropping.
    static std::size_t inputRingCapacity(int sampleRate, int blockFrames) noexcept;
    static std::size_t jitterCapacity(int sampleRate, int jitterMs, int blockFrames) noexcept;
    static std::size_t jitterTargetFrames(int sampleRate, int jitterMs) noexcept;

    audio::IAudioBackend* backend_ = nullptr;
    DiagnosticsManager* diagnostics_ = nullptr;

    std::vector<std::unique_ptr<audio::AudioRingBuffer>> inputRings_;
    std::vector<std::unique_ptr<audio::AudioJitterBuffer>> outputJitters_;
    std::vector<std::unique_ptr<audio::LevelMeter>> inputMeters_;
    std::vector<std::unique_ptr<audio::LevelMeter>> outputMeters_;

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
};

} // namespace liveai
