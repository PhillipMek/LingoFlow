#pragma once
//
// ReconnectSupervisor - session recovery without touching anything else (task 010).
//
// It sits between the application and any ITranslationBackend: to the application
// it is a backend, to the backend it is the sink. When a session dies for a
// reason a new session can fix, it closes, waits and replays the stored
// SessionRequest (protocol doc section 10: reconnection for translation means a
// fresh session, there is nothing to resume), and reports the composite state to
// the application. The audio device is not part of this file and cannot be
// affected through it (SPEC "Reliability": a network failure must not close
// ASIO) - the engine keeps running, the jitter buffer drains its tail and
// underruns into counted silence while recovery runs.
//
// Policy decided here, from the protocol doc section 9 table (task 010 owns the
// "when"; the docs deliberately do not choose for the product):
//   * retryable: `connection` (transport death, refused upgrade with 429
//     slow_down/rate/500/503) and `protocol` (the event stream broke its shape -
//     a new session is the cure, and the docs' stance is that a broken stream
//     says nothing about the request being valid).
//   * terminal: `rejectedRequest` (a bad pair, a bad key, billing - "retrying
//     won't restore API access"), `audioFormat` (a contract we ourselves got
//     wrong; replaying the same request cannot help) and `internal` (unknown:
//     never mask an implementation defect inside a forever-loop). These map to
//     the operator-actionable `faulted` state instead of hammering.
//   * retries continue forever while the application runs - a venue network
//     blip must not end the event (owner decision, 2026-10-02); the only stops
//     are operator close() and the terminal categories above.
//   * gap policy: audio submitted while not connected is refused and counted
//     (contract rule 4 makes refusal an operating state, not a fault); after a
//     recovery the session continues from "now". Nothing is buffered and
//     replayed - that would let the translation drift behind the room by the
//     length of every outage (owner decision, 2026-10-02; docs section 10
//     deliberately left the choice to us).
//   * proactive reopen: sessionMaxAgeMs before the one-hour provider ceiling
//     measured live (docs section 15) so a long event never hits the expiry at
//     all; the reopen is an ordinary close→open cycle, its gap is counted.
//
// Threading: one worker thread owns the retry timer and the reopen attempts.
// All methods are callable from non-realtime threads; nothing here runs on or
// blocks an audio callback (AGENTS.md 5). The supervisor holds its own lock only
// for state bookkeeping - never across a call into the wrapped backend or the
// application sink, so neither can observe it mid-update.
//
// Contract notes inherited by construction: rule 5 (no sink callbacks after
// closeSession() returns) is honoured by waiting out an in-flight attempt, whose
// own duration is bounded by the backend's openSession/closeSession bounds;
// rule 6 (fresh session per open) is exactly the replay mechanic.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Translation/ITranslationBackend.h"

namespace liveai {
namespace translation {

class ReconnectSupervisor final : public ITranslationBackend, public ITranslationSink
{
public:
    struct Policy
    {
        bool enabled = true;             ///< false = pure pass-through (no recovery thread work)
        int initialBackoffMs = 1000;     ///< first retry delay
        int maxBackoffMs = 15000;        ///< exponential backoff cap (doubling)
        int sessionMaxAgeMs = 3300000;   ///< proactive reopen before the provider ceiling; 0 = off
    };

    /// Recovery metrics (SPEC "Diagnostics": reconnect count, session
    /// duration). A snapshot for the UI (task 014) and the diagnostics export
    /// (task 017); counters keep accumulating across operator stop/start runs.
    struct Stats
    {
        bool recovering = false;        ///< recovery is in progress right now
        std::uint64_t attempts = 0;     ///< reopen attempts made (error- and age-triggered)
        std::uint64_t recoveries = 0;   ///< sessions brought back after an error
        std::uint64_t proactiveReopens = 0; ///< recoveries started by the age deadline
        std::uint64_t terminalFaults = 0;   ///< runs ended as faulted-by-policy
        std::uint64_t gapRefusedFrames = 0; ///< audio refused while not connected
        std::uint64_t recoveringMsTotal = 0;///< time spent in reconnecting state
        std::uint64_t sessionAgeMs = 0;     ///< current live session's age, else 0
        std::uint64_t lastSessionMs = 0;    ///< duration of the last ended session
    };

    /// Takes ownership of the real backend; the supervisor must outlive the
    /// application's use of it. The wrapped backend's sink is replaced with the
    /// supervisor's - setSink() on the wrapped one afterwards is not supported.
    ReconnectSupervisor(std::unique_ptr<ITranslationBackend> backend, Policy policy);
    ~ReconnectSupervisor() override;

    ReconnectSupervisor(const ReconnectSupervisor&) = delete;
    ReconnectSupervisor& operator=(const ReconnectSupervisor&) = delete;

    // ------------------------------------------------------- ITranslationBackend
    std::string_view name() const noexcept override;
    SessionState state() const noexcept override;
    void setSink(ITranslationSink& sink) noexcept override;
    bool openSession(const SessionRequest& request, std::string& error) override;
    bool submitAudio(const float* samples, int frameCount, std::string& error) override;
    void closeSession() noexcept override;

    // ------------------------------------------- ITranslationSink (wrapped side)
    void onTranslatedAudio(const float* samples, int frameCount, int sampleRate) override;
    void onPartialText(std::string_view text) override;
    void onFinalText(std::string_view text) override;
    void onSessionStateChanged(SessionState state) override;
    void onTranslationError(const TranslationError& error) override;

    // ------------------------------------------------------------------ extras
    Stats stats() const noexcept;

    /// Whether a category is one a fresh session can fix (docs section 9).
    /// Public for tests and the task 017 export; the policy above explains why.
    static bool isRetryable(TranslationErrorCategory category) noexcept;

private:
    using clock = std::chrono::steady_clock;

    enum class Mode
    {
        idle = 0,   ///< no operator intent to run; nothing is attempted
        live,       ///< a session is (or is being) opened normally
        recovering, ///< an error or the age deadline started a close→wait→replay cycle
        unavailable ///< a terminal error ended recovery; only operator action restarts it
    };

    void workerLoop();
    /// One close→open replay of the stored request. Runs with the lock released.
    void runAttempt();
    /// Enter the recovering mode and schedule the first attempt. Lock must be held.
    void enterRecoveringLocked(bool proactive);
    /// Book a session leaving the live state (its duration; recovery clock start).
    void leaveLiveLocked();
    /// Tell the worker that something it computes its wake-up from has
    /// changed. Lock must be held; waking early is harmless, missing a wake is not.
    void touchLocked();

    std::unique_ptr<ITranslationBackend> backend_;
    const Policy policy_;
    std::thread worker_;                ///< the retry/age timer thread (only when enabled)

    mutable std::mutex mutex_;
    std::condition_variable wakeCv_;      ///< wakes the worker for shutdown/reschedule
    std::condition_variable attemptIdleCv_; ///< worker signals "no attempt in flight"

    ITranslationSink* app_ = nullptr;     ///< guarded by mutex_
    Mode mode_ = Mode::idle;              ///< guarded by mutex_
    bool shutdown_ = false;               ///< guarded by mutex_
    bool attemptRunning_ = false;         ///< guarded by mutex_
    bool attemptDue_ = false;             ///< guarded by mutex_
    std::uint64_t wakeSeq_ = 0;           ///< guarded by mutex_; bump on any wake-worthy change

    /// Recovery book-keeping, all guarded by mutex_.
    clock::time_point nextAttemptAt_ {};
    clock::time_point recoverStarted_ {};
    clock::time_point connectedSince_ {};
    clock::time_point ageDeadline_ {};    ///< valid while live && policy_.sessionMaxAgeMs > 0
    int nextBackoffMs_ = 0;               ///< doubles per failed attempt, capped
    int pendingRetryAfterMs_ = 0;         ///< service hint consumed by the next wait
    bool recoveryWasProactive_ = false;   ///< classifies the next successful recovery

    SessionRequest request_;              ///< stored for replay; guarded by mutex_

    Stats stats_;                         ///< accumulators (without the two derived fields)
};

} // namespace translation
} // namespace liveai
