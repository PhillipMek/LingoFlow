#pragma once
//
// DiagnosticsManager - counters and state snapshots for the status UI (task 014)
// and the diagnostics exporter (task 017).
//
// Design rules:
//   * The audio thread may only call count*() and the noexcept note*() methods:
//     relaxed atomics, bounded work, no allocation, no locks, no logging.
//   * Anything that allocates (strings, snapshots with text) is documented as
//     non-realtime and guarded by a mutex. It must never be called from the
//     audio callback.
//   * No secrets in here (AGENTS.md 10): an API key must never reach any field
//     of this class or the exported snapshot.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace liveai {

class DiagnosticsManager
{
public:
    DiagnosticsManager() = default;

    // ----------------------------------------------------------------- counters
    // Realtime-safe: relaxed atomic increments only.

    void countAudioBlock(int frameCount) noexcept
    {
        audioBlocks_.fetch_add(1, std::memory_order_relaxed);
        audioFrames_.fetch_add(static_cast<std::uint64_t>(frameCount > 0 ? frameCount : 0),
                               std::memory_order_relaxed);
    }

    void countUnderrun() noexcept { underruns_.fetch_add(1, std::memory_order_relaxed); }
    void countOverrun() noexcept { overruns_.fetch_add(1, std::memory_order_relaxed); }
    void countPartialTextEvent() noexcept { partialTextEvents_.fetch_add(1, std::memory_order_relaxed); }
    void countFinalTextEvent() noexcept { finalTextEvents_.fetch_add(1, std::memory_order_relaxed); }
    void countReconnect() noexcept { reconnects_.fetch_add(1, std::memory_order_relaxed); }
    void countNdiError() noexcept { ndiErrors_.fetch_add(1, std::memory_order_relaxed); }

    // -------------------------------------------------------------- configuration
    /// Realtime-safe (relaxed stores). Called after the device is opened.
    void noteAudioGeometry(int sampleRate, int bufferFrames) noexcept
    {
        sampleRate_.store(sampleRate, std::memory_order_relaxed);
        bufferFrames_.store(bufferFrames, std::memory_order_relaxed);
    }

    /// Non-realtime (allocates a copy of the name): device/backend label.
    void noteAudioBackend(std::string_view backendName);

    /// Non-realtime: clears the recorded backend label.
    void noteAudioBackendStopped();

    /// Non-realtime (allocates): last human-readable error per subsystem.
    /// Not used as an event log; task 017 adds the ring-buffered event log.
    void noteError(std::string_view subsystem, std::string_view message);

    // ------------------------------------------------------------------ readout
    struct Snapshot
    {
        std::uint64_t audioBlocks = 0;
        std::uint64_t audioFrames = 0;
        std::uint64_t underruns = 0;
        std::uint64_t overruns = 0;
        std::uint64_t partialTextEvents = 0;
        std::uint64_t finalTextEvents = 0;
        std::uint64_t reconnects = 0;
        std::uint64_t ndiErrors = 0;
        int sampleRate = 0;
        int bufferFrames = 0;
        std::string audioBackend;                 ///< empty while no backend is running
        std::string lastErrorSubsystem;           ///< empty when no error was recorded
        std::string lastErrorMessage;
    };

    /// Aggregation point for the UI/worker threads. Never call from the audio thread.
    Snapshot snapshot() const;

    /// Numeric counters only, safe to call from any thread.
    struct Counters
    {
        std::uint64_t audioBlocks = 0;
        std::uint64_t audioFrames = 0;
        std::uint64_t underruns = 0;
        std::uint64_t overruns = 0;
    };

    Counters counters() const noexcept
    {
        return Counters{ audioBlocks_.load(std::memory_order_relaxed),
                         audioFrames_.load(std::memory_order_relaxed),
                         underruns_.load(std::memory_order_relaxed),
                         overruns_.load(std::memory_order_relaxed) };
    }

    /// Test helper: zeroes every counter and clears all recorded strings.
    void resetForTests() noexcept;

private:
    std::atomic<std::uint64_t> audioBlocks_{ 0 };
    std::atomic<std::uint64_t> audioFrames_{ 0 };
    std::atomic<std::uint64_t> underruns_{ 0 };
    std::atomic<std::uint64_t> overruns_{ 0 };
    std::atomic<std::uint64_t> partialTextEvents_{ 0 };
    std::atomic<std::uint64_t> finalTextEvents_{ 0 };
    std::atomic<std::uint64_t> reconnects_{ 0 };
    std::atomic<std::uint64_t> ndiErrors_{ 0 };
    std::atomic<int> sampleRate_{ 0 };
    std::atomic<int> bufferFrames_{ 0 };

    // Guarded strings: non-realtime only.
    mutable std::mutex textMutex_;
    std::string audioBackend_;
    std::string lastErrorSubsystem_;
    std::string lastErrorMessage_;
};

} // namespace liveai
