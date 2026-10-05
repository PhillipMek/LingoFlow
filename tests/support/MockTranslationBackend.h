#pragma once
//
// MockTranslationBackend - the deterministic mock translation backend of task 007,
// hardened for the task 010 supervisor (which drives it from a second thread).
//
// It lives in the test tree, not in `src/`, on purpose: task 019 makes "mock
// behavior leaking into production" a FAIL criterion, and AGENTS.md 19 forbids
// anything in the product that could be mistaken for a working translation.
// The Null backend in the core stays a silent shell; this class is the one that
// behaves, and it exists so that the contract, the controller routing and the
// end-to-end pipeline can be tested without a network.
//
// Determinism rules (this is what makes the integration tests meaningful):
//   * no threads, no timers, no clock reads of its own: every callback into the
//     sink fires synchronously inside the method that was called, on the
//     caller's thread, in scripted order;
//   * the same script fed the same submits produces the same callback trace,
//     byte for byte - the contract tests assert exactly that;
//   * the "translated" audio is a scripted transform of the submitted audio
//     (gain, optional sign flip), so a test can predict the exact float values
//     without reading this class's code;
//   * per-session state (queued frames, cue positions) resets when a session
//     closes, so a reopened session behaves like a fresh one, and total
//     counters keep accumulating across sessions.
//
// Thread safety (task 010): the reconnect supervisor calls in from its own
// worker thread while tests inspect from the main thread, so every method
// takes the mock's lock for bookkeeping and every sink callback is issued
// AFTER the lock is released - a sink that reaches back into the mock (or into
// the supervisor, which may reach here) cannot deadlock it.
//
// The lifecycle rules of ITranslationBackend (sink first, one open session,
// reported transitions, refusal outside connected, idempotent close, no sink
// callbacks after closeSession() returns) are implemented here as the reference
// implementation task 009 can be checked against.

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "Translation/ITranslationBackend.h"

namespace liveai {
namespace test {

class MockTranslationBackend final : public translation::ITranslationBackend
{
public:
    /// One translated block per this many queued input frames. 0 delivers no
    /// audio at all - which is how the tests prove audio and text channels are
    /// independent (SPEC "Translation Provider Interface").
    int deliverFrames = 0;

    /// Amplitude of a delivered block, applied to the submitted samples.
    float deliverGain = 1.0f;

    /// Negate the delivered samples. Combined with a gain that is not 1.0, the
    /// mock's audio is unmistakably not the device's own loopback.
    bool negate = false;

    /// Sample rate stamped on every delivered block. 0 = "as the session
    /// requested" (the honest provider); a non-zero override exists to exercise
    /// the receiver's rejection path - task 008 decides which rates are real,
    /// and until then no test may pretend to know them.
    int deliverAtSampleRate = 0;

    /// openSession() refuses with this error and changes nothing.
    bool refuseOpen = false;
    std::string openError = "mock: refusing to open";

    /// Task 010: refuse this many further openSession() calls (then behave
    /// normally) - the way a test walks the supervisor's backoff ladder.
    /// Set it through setOpenFails() once a supervisor thread may be live.
    int openFailsRemaining = 0;

    void setOpenFails(int n)
    {
        std::lock_guard lock(mutex_);
        openFailsRemaining = n;
    }

    /// Text fires after the Nth accepted submitAudio() of the current session,
    /// in vector order, before the error cues of the same submit.
    struct TextCue
    {
        int afterSubmits = 1;
        std::string text;
        bool isFinal = false;
    };

    /// Errors fire after text cues of the same submit. A fatal error faults the
    /// session: the transition is reported and later submits are refused.
    struct ErrorCue
    {
        int afterSubmits = 1;
        translation::TranslationErrorCategory category = translation::TranslationErrorCategory::internal;
        std::string message = "mock: injected error";
        bool fatal = false;
        int retryAfterMs = 0;
    };

    std::vector<TextCue> textCues;
    std::vector<ErrorCue> errorCues;

    // ---------------------------------------------------------------- expiration
    /// The review-P1 knob: announce a server-side expiry. The accessor returns
    /// this remaining value verbatim while set (ms, -1 = "not announced"), so
    /// the mock keeps its determinism rule - it reads no clock of its own; the
    /// supervisor turns "remaining" into its own deadline exactly as it does
    /// for the real backend's steady-clock projection.
    void announceServerExpiry(long long remainingMs);

    bool serverSessionExpiryRemainingMs(long long& remainingMsOut) const noexcept override;

    // ------------------------------------------------------------------ contract
    std::string_view name() const noexcept override { return "Mock"; }

    translation::SessionState state() const noexcept override;

    void setSink(translation::ITranslationSink& sink) noexcept override;

    bool openSession(const translation::SessionRequest& request, std::string& error) override;
    bool submitAudio(const float* samples, int frameCount, std::string& error) override;
    void closeSession() noexcept override;

    // ------------------------------------------------------- injection (010 tests)
    /// Report an error on the sink right now, from the calling thread; a fatal
    /// one faults the session, exactly like the real backends do. This is how a
    /// recovery test pulls the trigger without scripting a submit count.
    void injectError(translation::TranslationErrorCategory category, const std::string& message,
                     bool fatal, int retryAfterMs = 0);

    // -------------------------------------------------------------- observations
    translation::SessionRequest lastRequest();
    std::vector<translation::SessionRequest> openRequests();

    int acceptedSubmits();
    int refusedSubmits();
    int deliveredBlocks();
    std::uint64_t deliveredFrames();
    int textEvents();
    int errorEvents();

    /// Successful openSession() calls so far - distinguishes "state reset per
    /// session" from "totals", which the tests assert separately.
    int sessionsOpened();

    /// Accepted submits of the current session (cue positions count these).
    int sessionSubmits();

private:
    /// Changes the state under the lock and returns the callback to run
    /// outside it (nullptr when nothing changed).
    void transition(translation::SessionState next,
                    std::vector<std::function<void()>>& emit);

    mutable std::mutex mutex_;

    // All fields below are guarded by mutex_.
    std::vector<float> pending_;      ///< queued input not yet enough to deliver
    translation::ITranslationSink* sink_ = nullptr;
    translation::SessionState state_ = translation::SessionState::closed;
    translation::SessionRequest lastRequest_;
    std::vector<translation::SessionRequest> openRequests_;

    int acceptedSubmits_ = 0;
    int refusedSubmits_ = 0;
    int deliveredBlocks_ = 0;
    std::uint64_t deliveredFrames_ = 0;
    int textEvents_ = 0;
    int errorEvents_ = 0;
    int sessionsOpened_ = 0;
    int sessionSubmits_ = 0;
    long long expiryRemainingMs_ = -1;   ///< -1: no announcement (see announceServerExpiry)
};

} // namespace test
} // namespace liveai
