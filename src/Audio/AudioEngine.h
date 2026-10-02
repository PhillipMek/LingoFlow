#pragma once
//
// AudioEngine - realtime shell between a device backend and the rest of the
// product. At this stage it owns no DSP and no buffering: it verifies that the
// boundary is usable and that the callback does exactly the work a realtime
// thread is allowed to do.
//
// Will be extended by:
//   task 005 - lock-free input ring buffer, jitter buffer, underrun handling
//   task 006 - gain, metering, clipping
//   task 012 - feeding translated audio in
//
// Dependency direction (AGENTS.md 7): AudioEngine knows IAudioBackend only. It
// does not know about OpenAI, NDI or JUCE.

#include <atomic>
#include <cstdint>

#include "Audio/IAudioBackend.h"

namespace liveai {

class DiagnosticsManager;

class AudioEngine final : public audio::IAudioProcessor
{
public:
    /// `diagnostics` may be null; the engine then only keeps its own counters.
    explicit AudioEngine(DiagnosticsManager* diagnostics = nullptr) noexcept;

    audio::IAudioBackend* backend() const noexcept { return backend_; }

    /// Opens and starts the backend. Non-realtime thread only.
    bool activate(audio::IAudioBackend& backend, int sampleRate, int bufferFrames, std::string& error);

    /// Stops and closes the backend. Non-realtime thread only.
    void deactivate() noexcept;

    /// audio::IAudioProcessor - realtime thread.
    void processAudio(const float* const* input,
                      float* const* output,
                      int frameCount) noexcept override;

    void onAudioConfigurationChanged(int sampleRate, int bufferFrames) override;

    /// Monotonic counters, safe to read from the UI/worker threads.
    std::uint64_t blockCount() const noexcept { return blocks_.load(std::memory_order_relaxed); }
    std::uint64_t frameCount() const noexcept { return frames_.load(std::memory_order_relaxed); }
    int sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    int bufferFrames() const noexcept { return bufferFrames_.load(std::memory_order_relaxed); }

private:
    audio::IAudioBackend* backend_ = nullptr;
    DiagnosticsManager* diagnostics_ = nullptr;

    std::atomic<std::uint64_t> blocks_{ 0 };
    std::atomic<std::uint64_t> frames_{ 0 };
    std::atomic<int> sampleRate_{ 0 };
    std::atomic<int> bufferFrames_{ 0 };
};

} // namespace liveai
