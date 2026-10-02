#include "support/MockTranslationBackend.h"

#include <algorithm>

namespace liveai {
namespace test {

using translation::SessionRequest;
using translation::SessionState;
using translation::TranslationError;

bool MockTranslationBackend::openSession(const SessionRequest& request, std::string& error)
{
    error.clear();

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

    if (refuseOpen)
    {
        error = openError;
        return false;
    }

    lastRequest_ = request;
    ++sessionsOpened_;

    // Rule 6: a fresh session begins with an empty queue and cue position.
    pending_.clear();
    sessionSubmits_ = 0;

    transition(SessionState::connecting);
    transition(SessionState::connected);

    return true;
}

bool MockTranslationBackend::submitAudio(const float* samples, int frameCount, std::string& error)
{
    error.clear();

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

    // ------------------------------------------------------------- translated audio
    if (deliverFrames > 0)
    {
        pending_.insert(pending_.end(), samples, samples + frameCount);

        const int rate = deliverAtSampleRate > 0 ? deliverAtSampleRate : lastRequest_.outputSampleRate;

        while (static_cast<int>(pending_.size()) >= deliverFrames)
        {
            std::vector<float> block(pending_.begin(), pending_.begin() + deliverFrames);
            pending_.erase(pending_.begin(), pending_.begin() + deliverFrames);

            const float gain = negate ? -deliverGain : deliverGain;

            for (float& sample : block)
                sample *= gain;

            ++deliveredBlocks_;
            deliveredFrames_ += static_cast<std::uint64_t>(block.size());
            sink_->onTranslatedAudio(block.data(), static_cast<int>(block.size()), rate);
        }
    }

    // ---------------------------------------------------------------------- text
    // Independent of the audio channel above: this script delivers text with no
    // audio at all, and the tests assert the sink really sees that.
    for (const TextCue& cue : textCues)
    {
        if (cue.afterSubmits != sessionSubmits_)
            continue;

        if (cue.isFinal)
        {
            ++textEvents_;
            sink_->onFinalText(cue.text);
        }
        else
        {
            ++textEvents_;
            sink_->onPartialText(cue.text);
        }
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

        const TranslationError error_event{ cue.category, cue.message, cue.fatal };
        sink_->onTranslationError(error_event);

        if (cue.fatal)
            transition(SessionState::faulted);
    }

    return true;
}

void MockTranslationBackend::closeSession() noexcept
{
    // Rule 5: idempotent, and because every callback is issued synchronously
    // from the method above, "nothing after closeSession() returns" holds by
    // construction here - which is exactly the guarantee a real backend has to
    // provide asynchronously.
    if (state_ == SessionState::closed)
        return;

    pending_.clear();
    transition(SessionState::closed);
}

void MockTranslationBackend::transition(SessionState next)
{
    if (state_ == next)
        return;

    state_ = next;

    if (sink_ != nullptr)
        sink_->onSessionStateChanged(state_);
}

} // namespace test
} // namespace liveai
