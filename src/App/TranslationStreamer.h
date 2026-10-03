#pragma once
//
// TranslationStreamer - the input-side transport worker of task 012.
//
// It drains the engine's input ring and hands the capture to an
// ITranslationBackend:
//
//   ASIO callback -> input gain -> AudioRingBuffer -> [this thread] -> submitAudio
//   backend -> sink (ApplicationController) -> AudioJitterBuffer -> output gain -> ASIO
//
// This is the shape task 005's AudioLoopback proved on the lock-free path, with the
// loopback worker replaced by the translation seam: producer = audio callback,
// consumer = this thread. Like the loopback it is not realtime: scratch is allocated
// in start(), it may sleep between polls; everything the callback touches stays in
// the engine (AGENTS.md 5).
//
// The gap policy (task 010, docs/openai-realtime-protocol.md section 10) is executed
// HERE on the capture side: ring frames are consumed before submitAudio() knows
// whether the session can accept them, so while the supervisor is reconnecting the
// capture is refused and counted, never buffered for late replay. Replaying it would
// let the translation drift behind the room by the length of every outage - the
// owner's decision of 2026-10-02 forbids exactly that. Refusal is an operating state
// (contract rule 4), not a fault: the worker keeps draining, keeps counting, and
// says it out loud once per episode.
//
// Mono by contract: SPEC "Audio" fixes mono for translation, and the controller
// delivers to outputJitter(0); this worker drains inputRing(0). A multichannel
// product would add instances, not interleaving math (AGENTS.md 16 extension point).
//
// Lifetime: owned by ApplicationController between openSession() success and the stop
// of the session. The engine must be activated before start(); stop() must run
// before the engine deactivates (the controller's stop order guarantees it: session
// first, device after - the same ordering contract rule 5 was built for).

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Audio/AudioEngine.h"
#include "Translation/ITranslationBackend.h"

namespace liveai {

class DiagnosticsManager;

class TranslationStreamer final
{
public:
    TranslationStreamer(AudioEngine& engine,
                        translation::ITranslationBackend& backend,
                        DiagnosticsManager* diagnostics) noexcept;
    ~TranslationStreamer();

    TranslationStreamer(const TranslationStreamer&) = delete;
    TranslationStreamer& operator=(const TranslationStreamer&) = delete;

    /// Idle sleep when the ring has nothing. Set before start().
    void setPollIntervalMs(int intervalMs) noexcept;
    int pollIntervalMs() const noexcept { return pollIntervalMs_.load(std::memory_order_relaxed); }

    /// Requires a live engine pipeline. Allocates the scratch, attaches as the
    /// input consumer and spawns the worker.
    bool start(std::string& error);

    /// Signals the worker, joins it, detaches the input consumer. Idempotent.
    /// After it returns the backend sees no further submitAudio() from this
    /// streamer, which is what lets the controller close the session cleanly.
    void stop() noexcept;

    bool running() const noexcept { return running_.load(std::memory_order_relaxed); }

    /// Capture frames handed to the backend in accepted submits.
    std::uint64_t submittedFrames() const noexcept { return submittedFrames_.load(std::memory_order_relaxed); }

    /// Capture frames consumed and refused because the session could not accept
    /// them (the counted half of the gap policy).
    std::uint64_t gapRefusedFrames() const noexcept { return gapFrames_.load(std::memory_order_relaxed); }

    /// Worker died on a backend exception. The log says it; this is for status.
    bool crashed() const noexcept { return crashed_.load(std::memory_order_relaxed); }

private:
    void run() noexcept;

    AudioEngine& engine_;
    translation::ITranslationBackend& backend_;
    DiagnosticsManager* diagnostics_; ///< may be null

    std::thread thread_;
    std::vector<float> scratch_;

    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> attached_{ false };
    std::atomic<bool> crashed_{ false };
    std::atomic<bool> refusalLogged_{ false }; ///< one warning per refusal episode
    std::atomic<int> pollIntervalMs_{ 2 };
    std::atomic<std::uint64_t> submittedFrames_{ 0 };
    std::atomic<std::uint64_t> gapFrames_{ 0 };
};

} // namespace liveai
