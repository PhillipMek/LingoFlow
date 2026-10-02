#include "Translation/ReconnectSupervisor.h"

#include <algorithm>

#include "Utils/Log.h"

namespace liveai {
namespace translation {
namespace {

constexpr const char* kLogComponent = "translation.reconnect";

std::chrono::milliseconds ms(int value) { return std::chrono::milliseconds(value > 0 ? value : 0); }

std::uint64_t elapsedMs(std::chrono::steady_clock::time_point from,
                        std::chrono::steady_clock::time_point to)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          to - from)
                                          .count());
}

} // namespace

// ------------------------------------------------------------------ construction

ReconnectSupervisor::ReconnectSupervisor(std::unique_ptr<ITranslationBackend> backend,
                                         Policy policy)
    : backend_(std::move(backend))
    , policy_(policy)
    , nextBackoffMs_(policy.initialBackoffMs)
{
    backend_->setSink(*this);
    if (policy_.enabled)
        worker_ = std::thread([this] { workerLoop(); });
}

ReconnectSupervisor::~ReconnectSupervisor()
{
    {
        std::lock_guard lock(mutex_);
        shutdown_ = true;
        touchLocked();
    }
    attemptIdleCv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    closeSession();
}

// ------------------------------------------------------------- policy vocabulary

bool ReconnectSupervisor::isRetryable(TranslationErrorCategory category) noexcept
{
    // docs/openai-realtime-protocol.md section 9, decided by task 010:
    // a new session fixes transport deaths and a broken event stream; it fixes
    // nothing that was refused because of what the request (or our own contract
    // handling) is, and unknown failures must stay visible, not looped.
    return category == TranslationErrorCategory::connection
        || category == TranslationErrorCategory::protocol;
}

// ---------------------------------------------------------------- ITranslationBackend

std::string_view ReconnectSupervisor::name() const noexcept
{
    return backend_->name(); // transparent: the application identifies the real backend
}

SessionState ReconnectSupervisor::state() const noexcept
{
    Mode mode;
    {
        std::lock_guard lock(mutex_);
        mode = mode_;
    }
    if (mode == Mode::recovering)
        return SessionState::reconnecting;
    if (mode == Mode::unavailable)
        return SessionState::faulted;
    // Deliberately read outside the lock: the wrapped backend may report
    // callbacks synchronously from its methods, and our hooks take this lock -
    // holding it here could invert that order against a third thread.
    return backend_->state();
}

void ReconnectSupervisor::setSink(ITranslationSink& sink) noexcept
{
    std::lock_guard lock(mutex_);
    app_ = &sink;
}

bool ReconnectSupervisor::openSession(const SessionRequest& request, std::string& error)
{
    {
        std::unique_lock lock(mutex_);
        if (app_ == nullptr)
        {
            // Contract rule 1, enforced here too: without a sink the recovered
            // session would be deaf to the application.
            error = "reconnect supervisor has no sink";
            return false;
        }
        if (attemptRunning_)
            attemptIdleCv_.wait(lock, [this] { return !attemptRunning_; });

        // Operator intent resets the recovery machine: a run that went
        // terminal (bad key, billing) may run again now that the request may
        // have changed, and the backoff starts fresh.
        leaveLiveLocked();
        request_ = request;
        mode_ = Mode::live;
        nextBackoffMs_ = policy_.initialBackoffMs;
        pendingRetryAfterMs_ = 0;
        attemptDue_ = false;
        connectedSince_ = {};
        ageDeadline_ = {};
        touchLocked();
    }

    // Rule 2 says callers close first, deliberately: the wrapped session may
    // be faulted from before, and closeSession() is idempotent.
    backend_->closeSession();
    return backend_->openSession(request, error);
}

bool ReconnectSupervisor::submitAudio(const float* samples, int frameCount, std::string& error)
{
    {
        std::lock_guard lock(mutex_);
        if (attemptRunning_ || mode_ != Mode::live)
        {
            // The gap policy (task 010, owner decision): audio arriving while
            // there is no session is refused and counted, never buffered for a
            // replay that would push the translation behind the room. Contract
            // rule 4: refusal is an operating state, not a fault - the caller
            // keeps running and the engine stays open.
            stats_.gapRefusedFrames += static_cast<std::uint64_t>(frameCount > 0 ? frameCount : 0);
            error = mode_ == Mode::unavailable
                ? "translation session faulted (operator action required)"
                : mode_ == Mode::recovering
                    ? "translation session reconnecting"
                    : "no translation session";
            return false;
        }
    }
    return backend_->submitAudio(samples, frameCount, error);
}

void ReconnectSupervisor::closeSession() noexcept
{
    // The wrapped backend reports `closed` only when it actually had a
    // session; if recovery had already closed it, the application would miss
    // the transition. Remember the truth so the state can be synthesized.
    const bool alreadyClosed = backend_->state() == SessionState::closed;

    bool wasRunning = false;
    {
        std::unique_lock lock(mutex_);
        wasRunning = mode_ != Mode::idle;
        if (mode_ == Mode::recovering)
        {
            stats_.recoveringMsTotal += elapsedMs(recoverStarted_, clock::now());
            recoveryWasProactive_ = false;
        }
        leaveLiveLocked();
        mode_ = Mode::idle;
        attemptDue_ = false;
        pendingRetryAfterMs_ = 0;
        touchLocked();

        // Rule 5 for this wrapper: no callback of ours may follow the return,
        // so an in-flight attempt (whose own duration is bounded by the
        // backend's open/close bounds) is joined here, not fought.
        attemptIdleCv_.wait(lock, [this] { return !attemptRunning_; });
    }
    backend_->closeSession();

    if (wasRunning && alreadyClosed)
    {
        ITranslationSink* app;
        {
            std::lock_guard lock(mutex_);
            app = app_;
        }
        if (app != nullptr)
            app->onSessionStateChanged(SessionState::closed);
    }
}

ReconnectSupervisor::Stats ReconnectSupervisor::stats() const noexcept
{
    Stats out;
    std::lock_guard lock(mutex_);
    out = stats_;
    out.recovering = mode_ == Mode::recovering;
    if (mode_ == Mode::live && connectedSince_ != clock::time_point {})
        out.sessionAgeMs = elapsedMs(connectedSince_, clock::now());
    if (mode_ == Mode::recovering)
        out.sessionAgeMs = 0;
    return out;
}

// --------------------------------------------------- ITranslationSink (wrapped side)

void ReconnectSupervisor::onTranslatedAudio(const float* samples, int frameCount, int sampleRate)
{
    ITranslationSink* app;
    {
        std::lock_guard lock(mutex_);
        app = app_;
    }
    if (app != nullptr)
        app->onTranslatedAudio(samples, frameCount, sampleRate);
}

void ReconnectSupervisor::onPartialText(std::string_view text)
{
    ITranslationSink* app;
    {
        std::lock_guard lock(mutex_);
        app = app_;
    }
    if (app != nullptr)
        app->onPartialText(text);
}

void ReconnectSupervisor::onFinalText(std::string_view text)
{
    ITranslationSink* app;
    {
        std::lock_guard lock(mutex_);
        app = app_;
    }
    if (app != nullptr)
        app->onFinalText(text);
}

void ReconnectSupervisor::onSessionStateChanged(SessionState state)
{
    ITranslationSink* app;
    bool forward = true;
    {
        std::lock_guard lock(mutex_);
        app = app_;
        const auto now = clock::now();

        if (state == SessionState::connected)
        {
            if (mode_ == Mode::recovering)
            {
                // The recovery succeeded (the wrapped backend reports connected
                // from its worker or from the attempt's openSession call).
                stats_.recoveringMsTotal += elapsedMs(recoverStarted_, now);
                if (recoveryWasProactive_)
                    ++stats_.proactiveReopens;
                else
                    ++stats_.recoveries;
                recoveryWasProactive_ = false;
                mode_ = Mode::live;
                attemptDue_ = false;
                nextBackoffMs_ = policy_.initialBackoffMs;
                pendingRetryAfterMs_ = 0;
                connectedSince_ = now;
                if (policy_.sessionMaxAgeMs > 0)
                    ageDeadline_ = connectedSince_ + ms(policy_.sessionMaxAgeMs);
                log::info(kLogComponent, "translation session recovered");
                touchLocked();
            }
            else if (mode_ == Mode::live)
            {
                connectedSince_ = now;
                if (policy_.sessionMaxAgeMs > 0)
                    ageDeadline_ = connectedSince_ + ms(policy_.sessionMaxAgeMs);
                touchLocked();
            }
            forward = true; // connected always reaches the application
        }
        else if (state == SessionState::faulted || state == SessionState::closed)
        {
            // During recovery the machine's composite state is `reconnecting`;
            // the wrapped session's own faulted/closed flapping is internal
            // noise. In `unavailable` the faulted transition was synthesized
            // when the terminal error arrived, so the wrapped report is the
            // duplicate.
            forward = mode_ != Mode::recovering && mode_ != Mode::unavailable;
        }
        else if (state == SessionState::connecting)
        {
            forward = mode_ != Mode::recovering; // hide attempt churn
        }
        else // reconnecting from a backend: this state is ours to synthesize
        {
            forward = false;
        }
    }

    if (app != nullptr && forward)
        app->onSessionStateChanged(state);
}

void ReconnectSupervisor::onTranslationError(const TranslationError& error)
{
    ITranslationSink* app;
    bool forwardError = true;
    bool emitReconnecting = false;
    bool emitFaulted = false;
    {
        std::lock_guard lock(mutex_);
        app = app_;

        if (!error.fatal || !policy_.enabled)
        {
            forwardError = true; // events and pass-through mode travel unchanged
        }
        else if (mode_ == Mode::live)
        {
            if (isRetryable(error.category))
            {
                if (error.retryAfterMs > 0)
                    pendingRetryAfterMs_ = error.retryAfterMs;
                enterRecoveringLocked(/*proactive=*/false);
                emitReconnecting = true;
            }
            else
            {
                leaveLiveLocked();
                mode_ = Mode::unavailable;
                ++stats_.terminalFaults;
                emitFaulted = true;
                touchLocked();
                log::warning(kLogComponent,
                             std::string("translation session unrecoverable (")
                                 + std::string(nameOf(error.category))
                                 + "); operator action required");
            }
        }
        else if (mode_ == Mode::recovering)
        {
            if (isRetryable(error.category))
            {
                // Failure noise from the attempts themselves: counted through
                // the attempt loop, not forwarded. A service hint still steers
                // the next wait.
                forwardError = false;
                if (error.retryAfterMs > pendingRetryAfterMs_)
                    pendingRetryAfterMs_ = error.retryAfterMs;
            }
            else
            {
                stats_.recoveringMsTotal += elapsedMs(recoverStarted_, clock::now());
                recoveryWasProactive_ = false;
                mode_ = Mode::unavailable;
                attemptDue_ = false;
                ++stats_.terminalFaults;
                emitFaulted = true;
                touchLocked();
                log::warning(kLogComponent,
                             std::string("recovery ended: a new session cannot fix a ")
                                 + std::string(nameOf(error.category)) + " failure");
            }
        }
        // idle / unavailable: reported as they come.
    }

    if (app == nullptr)
        return;
    if (forwardError)
        app->onTranslationError(error);
    if (emitReconnecting)
        app->onSessionStateChanged(SessionState::reconnecting);
    if (emitFaulted)
        app->onSessionStateChanged(SessionState::faulted);
}

// ------------------------------------------------------------------------- internals

void ReconnectSupervisor::touchLocked()
{
    ++wakeSeq_;
    wakeCv_.notify_all();
}

void ReconnectSupervisor::enterRecoveringLocked(bool proactive)
{
    leaveLiveLocked();
    mode_ = Mode::recovering;
    recoverStarted_ = clock::now();
    recoveryWasProactive_ = proactive;

    // The very first wait must already honour a service hint: "wait at least
    // as long as Retry-After specifies" (docs section 9) does not become
    // optional just because the error that carried it started this recovery.
    int waitMs = nextBackoffMs_;
    if (pendingRetryAfterMs_ > waitMs)
        waitMs = pendingRetryAfterMs_;
    pendingRetryAfterMs_ = 0;

    nextAttemptAt_ = proactive ? clock::now() : clock::now() + ms(waitMs);
    nextBackoffMs_ = (std::min)(policy_.maxBackoffMs, nextBackoffMs_ * 2);
    attemptDue_ = true;
    touchLocked();

    log::info(kLogComponent,
              proactive ? "reopening session before the provider ceiling (proactive)"
                        : "translation session lost, reopening with backoff");
}

void ReconnectSupervisor::leaveLiveLocked()
{
    if (mode_ == Mode::live && connectedSince_ != clock::time_point {})
    {
        stats_.lastSessionMs = elapsedMs(connectedSince_, clock::now());
        connectedSince_ = {};
    }
}

void ReconnectSupervisor::runAttempt()
{
    SessionRequest request;
    {
        std::lock_guard lock(mutex_);
        request = request_;
    }

    // Close the dead session first (idempotent, bounded) so openSession meets
    // contract rule 2 the way the wrapped backend expects, then replay the
    // stored request verbatim: a recovered session translates the same pair at
    // the same rates (docs section 10).
    backend_->closeSession();
    std::string error;
    const bool ok = backend_->openSession(request, error);

    std::lock_guard lock(mutex_);
    if (shutdown_ || mode_ == Mode::idle)
        return; // the operator closed while we were trying; closeSession() waits for us
    ++stats_.attempts;
    if (ok && mode_ == Mode::live)
        return; // the connected hook already booked the success
    if (mode_ != Mode::recovering)
    {
        attemptDue_ = false; // terminal: a hook decided while the call was running
        touchLocked();
        return;
    }

    int waitMs = nextBackoffMs_;
    if (pendingRetryAfterMs_ > waitMs)
        waitMs = pendingRetryAfterMs_; // "wait at least as long as Retry-After" (docs 9)
    pendingRetryAfterMs_ = 0;
    nextBackoffMs_ = (std::min)(policy_.maxBackoffMs, nextBackoffMs_ * 2);
    nextAttemptAt_ = clock::now() + ms(waitMs);
    attemptDue_ = true;
    touchLocked();
}

void ReconnectSupervisor::workerLoop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    std::uint64_t seenSeq = wakeSeq_;
    for (;;)
    {
        if (shutdown_)
            break;

        ITranslationSink* app = app_;
        const auto now = clock::now();

        bool doAttempt = false;
        if (mode_ == Mode::recovering && attemptDue_ && now >= nextAttemptAt_)
        {
            // The backoff/Retry-After wait has actually passed; only now may
            // the attempt run. (Checking the deadline is the whole point:
            // attemptDue_ alone means "a recovery is scheduled", not "run".)
            doAttempt = true;
        }
        else if (mode_ == Mode::live && policy_.sessionMaxAgeMs > 0
                 && connectedSince_ != clock::time_point {} && now >= ageDeadline_)
        {
            enterRecoveringLocked(/*proactive=*/true);
            doAttempt = true;
            if (app != nullptr)
            {
                lock.unlock();
                app->onSessionStateChanged(SessionState::reconnecting);
                lock.lock();
            }
        }

        if (doAttempt)
        {
            attemptRunning_ = true;
            attemptDue_ = false;
            lock.unlock();
            runAttempt(); // every backend call happens without our lock held
            lock.lock();
            attemptRunning_ = false;
            seenSeq = wakeSeq_; // hooks during the attempt already bumped it
            attemptIdleCv_.notify_all();
            continue;
        }

        // Sleep until the next real event: a due attempt, the age deadline, or
        // any state change that makes the computed wake-up stale (touchLocked).
        bool hasWake = false;
        clock::time_point wakeAt {};
        if (mode_ == Mode::recovering && attemptDue_)
        {
            hasWake = true;
            wakeAt = nextAttemptAt_;
        }
        else if (mode_ == Mode::live && policy_.sessionMaxAgeMs > 0
                 && connectedSince_ != clock::time_point {})
        {
            hasWake = true;
            wakeAt = ageDeadline_;
        }

        if (hasWake && clock::now() >= wakeAt)
            continue; // due right now, loop to act

        seenSeq = wakeSeq_;
        if (hasWake)
            wakeCv_.wait_until(lock, wakeAt, [this, seenSeq]
                               { return shutdown_ || wakeSeq_ != seenSeq; });
        else
            wakeCv_.wait(lock, [this, seenSeq] { return shutdown_ || wakeSeq_ != seenSeq; });
    }
}

} // namespace translation
} // namespace liveai
