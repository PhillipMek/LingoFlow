#pragma once
//
// AudioLoopback - a worker thread that feeds the output jitter buffer straight from
// the input rings (SPEC "Audio Ring Buffer" + "Output Jitter Buffer", task 005).
//
// It exists for two reasons:
//
//   1. It exercises exactly the lock-free path that task 012 will use with OpenAI
//      audio in place of microphone audio: producer = ASIO callback, consumer = this
//      thread, producer again = this thread, consumer again = ASIO callback. If the
//      pipeline works through loopback, the transport is the only thing left to add.
//   2. It gives a SoundGrid machine a way to prove the physical path
//      (console -> ASIO in -> engine -> ASIO out -> monitor) before any translation
//      exists. That is the task 005 human check.
//
// Loopback is never on by default and never implicit: routing microphone audio to the
// audience is a deliberate operator action (developer mode, task 019, will expose it
// in the UI). An underrun in loopback still plays silence - this thread only writes
// what it actually read from the rings.
//
// Threading: this class is not realtime. It allocates its scratch buffer in start()
// and may sleep between polls; the ring reads and jitter writes it performs are the
// consumer/producer sides of the lock-free buffers and are themselves safe to call
// from a worker thread.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "Audio/AudioEngine.h"

namespace liveai {
namespace audio {

class AudioLoopback final
{
public:
    explicit AudioLoopback(AudioEngine& engine) noexcept;
    ~AudioLoopback();

    AudioLoopback(const AudioLoopback&) = delete;
    AudioLoopback& operator=(const AudioLoopback&) = delete;

    /// How long the worker sleeps when there is nothing to move. Lower values reduce
    /// added latency, higher values reduce wake-ups. Must be set before start().
    void setPollIntervalMs(int intervalMs) noexcept;
    int pollIntervalMs() const noexcept { return pollIntervalMs_.load(std::memory_order_relaxed); }

    /// Requires a live engine pipeline (AudioEngine::activate() succeeded). Attaches
    /// as the input consumer and spawns the worker thread.
    bool start(std::string& error);

    /// Signals the worker, joins it and detaches the input consumer. Idempotent.
    void stop() noexcept;

    bool running() const noexcept { return running_.load(std::memory_order_relaxed); }

    /// Audio channel pairs being looped (min of input and output channels).
    int channelPairs() const noexcept { return channelPairs_.load(std::memory_order_relaxed); }

    /// Frames moved from the input rings into the jitter buffers.
    std::uint64_t transferredFrames() const noexcept { return transferred_.load(std::memory_order_relaxed); }

private:
    void run() noexcept;

    AudioEngine& engine_;
    std::thread thread_;
    std::vector<float> scratch_;

    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> attached_{ false };
    std::atomic<int> pollIntervalMs_{ 2 };
    std::atomic<int> channelPairs_{ 0 };
    std::atomic<std::uint64_t> transferred_{ 0 };
};

} // namespace audio
} // namespace liveai
