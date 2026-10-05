#include "support/MockTranslationBackend.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace liveai {
namespace test {

using translation::SessionRequest;
using translation::SessionState;
using translation::TranslationError;

namespace {

using Emit = std::vector<std::function<void()>>;

/// Runs the collected callbacks in order, outside the mock's lock: a sink may
/// call back into anything (the supervisor does), and the mock must not sit on
/// its mutex while that happens.
void drain(Emit& emit)
{
    Emit local;
    local.swap(emit);
    for (auto&& fn : local)
        fn();
}

} // namespace

void MockTranslationBackend::announceServerExpiry(long long remainingMs)
{
    const std::lock_guard lock(mutex_);
    expiryRemainingMs_ = remainingMs;
}

bool MockTranslationBackend::serverSessionExpiryRemainingMs(long long& remainingMsOut) const noexcept
{
    const std::lock_guard lock(mutex_);
    if (expiryRemainingMs_ < 0)
    {
        remainingMsOut = 0;
        return false;
    }
    remainingMsOut = expiryRemainingMs_;
    return true;
}

translation::SessionState MockTranslationBackend::state() const noexcept
{
    std::lock_guard lock(mutex_);
    return state_;
}

void MockTranslationBackend::setSink(translation::ITranslationSink& sink) noexcept
{
    std::lock_guard lock(mutex_);
    sink_ = &sink;
}

bool MockTranslationBackend::openSession(const SessionRequest& request, std::string& error)
{
    error.clear();
    Emit emit;

    {
        std::lock_guard lock(mutex_);

        // Rule 1 of the contract: a session without a sink has nowhere to put
        // anything, so it is refused rather than born unusable.
        if (sink_ == nullptr)
        {
            error = "mock: setSink() must be called before openSession()";
            return false;
        }

        // Rule 2: one session at a time, and a refusal that changes nothing
        // (rule 3) reports nothing.
        if (state_ != SessionState::closed)
        {
            error = std::string("mock: a session is already ").append(nameOf(state_))
                  + "; closeSession() first";
            return false;
        }

        if (refuseOpen || openFailsRemaining > 0)
        {
            if (openFailsRemaining > 0)
                --openFailsRemaining;
            error = openError;
            return false;
        }

        lastRequest_ = request;
        openRequests_.push_back(request);
        ++sessionsOpened_;

        // Rule 6: a fresh session begins with an empty queue and cue position.
        pending_.clear();
        sessionSubmits_ = 0;

        transition(SessionState::connecting, emit);
        transition(SessionState::connected, emit);
    }

    drain(emit);
    return true;
}

bool MockTranslationBackend::submitAudio(const float* samples, int frameCount, std::string& error)
{
    error.clear();
    Emit emit;

    {
        std::lock_guard lock(mutex_);

        // Rule 4: accepted only while connected, and a refusal is an ordinary
        // answer, not a fault - the caller keeps operating.
        if (state_ != SessionState::connected)
        {
            ++refusedSubmits_;
            error = std::string("mock: no open session (state is ").append(nameOf(state_)) + ")";
            return false;
        }

        if (samples == nullptr || frameCount <= 0)
        {
            ++refusedSubmits_;
            error = "mock: empty audio block rejected";
            return false;
        }

        ++acceptedSubmits_;
        ++sessionSubmits_;
        translation::ITranslationSink* sink = sink_;

        // ------------------------------------------------------------- translated audio
        if (deliverFrames > 0)
        {
            pending_.insert(pending_.end(), samples, samples + frameCount);

            const int rate =
                deliverAtSampleRate > 0 ? deliverAtSampleRate : lastRequest_.outputSampleRate;

            while (static_cast<int>(pending_.size()) >= deliverFrames)
            {
                auto block = std::make_shared<std::vector<float>>(
                    pending_.begin(), pending_.begin() + deliverFrames);
                pending_.erase(pending_.begin(), pending_.begin() + deliverFrames);

                const float gain = negate ? -deliverGain : deliverGain;

                for (float& sample : *block)
                    sample *= gain;

                ++deliveredBlocks_;
                deliveredFrames_ += static_cast<std::uint64_t>(block->size());
                emit.push_back([sink, block, rate] {
                    sink->onTranslatedAudio(block->data(), static_cast<int>(block->size()), rate);
                });
            }
        }

        // ---------------------------------------------------------------------- text
        // Independent of the audio channel above: this script delivers text with no
        // audio at all, and the tests assert the sink really sees that.
        for (const TextCue& cue : textCues)
        {
            if (cue.afterSubmits != sessionSubmits_)
                continue;

            ++textEvents_;
            const std::string text = cue.text;
            const bool isFinal = cue.isFinal;
            emit.push_back([sink, text, isFinal] {
                if (isFinal)
                    sink->onFinalText(text);
                else
                    sink->onPartialText(text);
            });
        }

        // --------------------------------------------------------------------- errors
        // After the audio and text of the same submit, so an injected failure
        // arrives in the position the script says it should, and the fatal ones can
        // still be preceded by the delivery they interrupt.
        for (const ErrorCue& cue : errorCues)
        {
            if (cue.afterSubmits != sessionSubmits_)
                continue;

            ++errorEvents_;

            TranslationError event { cue.category, cue.message, cue.fatal };
            event.retryAfterMs = cue.retryAfterMs;
            emit.push_back([sink, event] { sink->onTranslationError(event); });

            if (cue.fatal)
                transition(SessionState::faulted, emit);
        }
    }

    drain(emit);
    return true;
}

void MockTranslationBackend::closeSession() noexcept
{
    Emit emit;
    {
        std::lock_guard lock(mutex_);

        // Rule 5: idempotent, and because every callback is issued from the
        // method itself (just after the lock is released), "nothing after
        // closeSession() returns" holds by construction here - which is exactly
        // the guarantee a real backend has to provide asynchronously.
        if (state_ == SessionState::closed)
            return;

        pending_.clear();
        transition(SessionState::closed, emit);
    }
    drain(emit);
}

void MockTranslationBackend::injectError(translation::TranslationErrorCategory category,
                                         const std::string& message,
                                         bool fatal,
                                         int retryAfterMs)
{
    Emit emit;
    {
        std::lock_guard lock(mutex_);
        if (sink_ == nullptr)
            return;

        ++errorEvents_;

        TranslationError error { category, message, fatal };
        error.retryAfterMs = retryAfterMs;
        emit.push_back([sink = sink_, error] { sink->onTranslationError(error); });

        if (fatal)
            transition(SessionState::faulted, emit);
    }
    drain(emit);
}

void MockTranslationBackend::transition(SessionState next, Emit& emit)
{
    if (state_ == next)
        return;

    state_ = next;

    if (sink_ != nullptr)
    {
        translation::ITranslationSink* sink = sink_;
        const SessionState reported = next;
        emit.push_back([sink, reported] { sink->onSessionStateChanged(reported); });
    }
}

// ------------------------------------------------------------------- observations

translation::SessionRequest MockTranslationBackend::lastRequest()
{
    std::lock_guard lock(mutex_);
    return lastRequest_;
}

std::vector<translation::SessionRequest> MockTranslationBackend::openRequests()
{
    std::lock_guard lock(mutex_);
    return openRequests_;
}

int MockTranslationBackend::acceptedSubmits()
{
    std::lock_guard lock(mutex_);
    return acceptedSubmits_;
}

int MockTranslationBackend::refusedSubmits()
{
    std::lock_guard lock(mutex_);
    return refusedSubmits_;
}

int MockTranslationBackend::deliveredBlocks()
{
    std::lock_guard lock(mutex_);
    return deliveredBlocks_;
}

std::uint64_t MockTranslationBackend::deliveredFrames()
{
    std::lock_guard lock(mutex_);
    return deliveredFrames_;
}

int MockTranslationBackend::textEvents()
{
    std::lock_guard lock(mutex_);
    return textEvents_;
}

int MockTranslationBackend::errorEvents()
{
    std::lock_guard lock(mutex_);
    return errorEvents_;
}

int MockTranslationBackend::sessionsOpened()
{
    std::lock_guard lock(mutex_);
    return sessionsOpened_;
}

int MockTranslationBackend::sessionSubmits()
{
    std::lock_guard lock(mutex_);
    return sessionSubmits_;
}

} // namespace test
} // namespace liveai
