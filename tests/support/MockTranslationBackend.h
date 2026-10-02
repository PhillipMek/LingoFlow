#pragma once
//
// MockTranslationBackend - the deterministic mock translation backend of task 007.
//
// It lives in the test tree, not in `src/`, on purpose: task 019 makes "mock
// behavior leaking into production" a FAIL criterion, and AGENTS.md 19 forbids
// anything in the product that could be mistaken for a working translation.
// The Null backend in the core stays a silent shell; this class is the one that
// behaves, and it exists so that the contract, the controller routing and the
// end-to-end pipeline can be tested without a network.
//
// Determinism rules (this is what makes the integration tests meaningful):
//   * no threads, no timers, no clock reads: every callback into the sink fires
//     synchronously inside the method that was called, on the caller's thread;
//   * the same script fed the same submits produces the same callback trace,
//     byte for byte - the contract tests assert exactly that;
//   * the "translated" audio is a scripted transform of the submitted audio
//     (gain, optional sign flip), so a test can predict the exact float values
//     without reading this class's code;
//   * per-session state (queued frames, cue positions) resets when a session
//     closes, so a reopened session behaves like a fresh one, and total
//     counters keep accumulating across sessions.
//
// The lifecycle rules of ITranslationBackend (sink first, one open session,
// reported transitions, refusal outside connected, idempotent close, no sink
// callbacks after closeSession() returns) are implemented here as the reference
// implementation task 009 can be checked against.

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
    };

    std::vector<TextCue> textCues;
    std::vector<ErrorCue> errorCues;

    // ------------------------------------------------------------------ contract
    std::string_view name() const noexcept override { return "Mock"; }

    /// Read-only snapshot. Single-threaded by construction: tests drive
    /// everything from one thread, which is also what makes the determinism
    /// assertion possible.
    translation::SessionState state() const noexcept override { return state_; }

    void setSink(translation::ITranslationSink& sink) noexcept override { sink_ = &sink; }

    bool openSession(const translation::SessionRequest& request, std::string& error) override;
    bool submitAudio(const float* samples, int frameCount, std::string& error) override;
    void closeSession() noexcept override;

    // -------------------------------------------------------------- observations
    const translation::SessionRequest& lastRequest() const noexcept { return lastRequest_; }

    int acceptedSubmits() const noexcept { return acceptedSubmits_; }
    int refusedSubmits() const noexcept { return refusedSubmits_; }
    int deliveredBlocks() const noexcept { return deliveredBlocks_; }
    std::uint64_t deliveredFrames() const noexcept { return deliveredFrames_; }
    int textEvents() const noexcept { return textEvents_; }
    int errorEvents() const noexcept { return errorEvents_; }

    /// Successful openSession() calls so far - distinguishes "state reset per
    /// session" from "totals", which the tests assert separately.
    int sessionsOpened() const noexcept { return sessionsOpened_; }

    /// Accepted submits of the current session (cue positions count these).
    int sessionSubmits() const noexcept { return sessionSubmits_; }

private:
    /// Changes the state and reports it, once, only if it actually changed.
    void transition(translation::SessionState next);

    /// Queued input not yet enough to deliver; cleared per session.
    std::vector<float> pending_;

    translation::ITranslationSink* sink_ = nullptr;
    translation::SessionState state_ = translation::SessionState::closed;
    translation::SessionRequest lastRequest_;

    int acceptedSubmits_ = 0;
    int refusedSubmits_ = 0;
    int deliveredBlocks_ = 0;
    std::uint64_t deliveredFrames_ = 0;
    int textEvents_ = 0;
    int errorEvents_ = 0;
    int sessionsOpened_ = 0;
    int sessionSubmits_ = 0;
};

} // namespace test
} // namespace liveai
