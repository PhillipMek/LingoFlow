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
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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

    /// Translated audio (task 007). Three numbers because the operator
    /// distinguishes three questions: did translation deliver, did we refuse
    /// what it delivered (wrong rate, bad block), and did our own buffer drop
    /// what we accepted because playback could not keep up.
    void countTranslatedAudioFrames(std::uint64_t frames) noexcept
    {
        translatedAudioFrames_.fetch_add(frames, std::memory_order_relaxed);
    }

    void countRejectedAudioFrames(std::uint64_t frames) noexcept
    {
        rejectedAudioFrames_.fetch_add(frames, std::memory_order_relaxed);
    }

    void countTranslatedAudioDroppedFrames(std::uint64_t frames) noexcept
    {
        translatedAudioDroppedFrames_.fetch_add(frames, std::memory_order_relaxed);
    }

    /// Capture throughput (task 012). Two numbers because they answer two
    /// different questions: did the translator get fed, and how much live speech
    /// did the gap policy drop while the session was reconnecting.
    void countTranslationSubmittedFrames(std::uint64_t frames) noexcept
    {
        translationSubmittedFrames_.fetch_add(frames, std::memory_order_relaxed);
    }

    void countTranslationGapFrames(std::uint64_t frames) noexcept
    {
        translationGapFrames_.fetch_add(frames, std::memory_order_relaxed);
    }

    /// Translation failures are counted, never swallowed (AGENTS.md 12); fatal
    /// ones are counted separately because they end a session.
    void countTranslationError(bool fatal) noexcept
    {
        translationErrors_.fetch_add(1, std::memory_order_relaxed);

        if (fatal)
            translationFatalErrors_.fetch_add(1, std::memory_order_relaxed);
    }

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

    /// Non-realtime (allocates): last human-readable error per subsystem, and -
    /// since task 017 - one entry in the event ring below. Errors are the events
    /// an operator most needs replayed, so the two roles share one call site.
    void noteError(std::string_view subsystem, std::string_view message);

    // ---------------------------------------------------------------- events (017)
    /// One dated entry in the ring-buffered event log: the replay of "what
    /// happened this run" that survives neither the screen (a glance is a
    /// glance) nor a scrollable log file's 10-thousandth line.
    struct DiagnosticEvent
    {
        std::uint64_t sequence = 0;   ///< monotone, never reused
        std::string timestamp;        ///< log::timestampNow() - the log's own format
        std::string subsystem;        ///< app | audio | translation | ndi | security | ...
        std::string message;
    };

    /// Ring capacity: enough for a whole show's incidents, small enough that an
    /// export is readable. Oldest entries are evicted, and the eviction is said
    /// out loud in evictedEvents() - a truncated history is never presented as
    /// the complete one.
    static constexpr std::size_t kMaxEvents = 256;

    /// Non-realtime (allocates a dated copy; the mutex is a UI/worker hand, and
    /// no audio-thread path calls this - the engine's callback keeps to the
    /// atomic count*() methods above, which is exactly why incidents from the
    /// callback arrive as counters and this ring receives the narrated ones).
    void noteEvent(std::string_view subsystem, std::string_view message);

    /// Oldest-first copy of the ring. Non-realtime (copies strings; UI/export only).
    std::vector<DiagnosticEvent> events() const;

    /// How many events were evicted since startup. With a nonzero value the
    /// ring is a WINDOW, not the full history - the export labels itself with
    /// this number for that reason.
    std::uint64_t evictedEvents() const noexcept { return evictedEvents_.load(std::memory_order_relaxed); }

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
        std::uint64_t translatedAudioFrames = 0;
        std::uint64_t rejectedAudioFrames = 0;
        std::uint64_t translatedAudioDroppedFrames = 0;
        std::uint64_t translationSubmittedFrames = 0;
        std::uint64_t translationGapFrames = 0;
        std::uint64_t translationErrors = 0;
        std::uint64_t translationFatalErrors = 0;
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

    /// Test helper: zeroes every counter, clears all recorded strings and the
    /// event ring.
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
    std::atomic<std::uint64_t> translatedAudioFrames_{ 0 };
    std::atomic<std::uint64_t> rejectedAudioFrames_{ 0 };
    std::atomic<std::uint64_t> translatedAudioDroppedFrames_{ 0 };
    std::atomic<std::uint64_t> translationSubmittedFrames_{ 0 };
    std::atomic<std::uint64_t> translationGapFrames_{ 0 };
    std::atomic<std::uint64_t> translationErrors_{ 0 };
    std::atomic<std::uint64_t> translationFatalErrors_{ 0 };
    std::atomic<int> sampleRate_{ 0 };
    std::atomic<int> bufferFrames_{ 0 };

    // Guarded strings + the event ring: non-realtime only.
    mutable std::mutex textMutex_;
    std::string audioBackend_;
    std::string lastErrorSubsystem_;
    std::string lastErrorMessage_;
    std::deque<DiagnosticEvent> events_;      ///< oldest first, capped at kMaxEvents
    std::uint64_t eventSequence_ = 0;         ///< guarded by textMutex_
    std::atomic<std::uint64_t> evictedEvents_{ 0 };

    /// Caller holds textMutex_.
    void appendEventLocked(std::string_view subsystem, std::string_view message);
};

} // namespace liveai
