#include "Translation/Null/NullTranslationBackend.h"

namespace liveai {
namespace translation {

bool NullTranslationBackend::openSession(const SessionRequest& request, std::string& error)
{
    error.clear();

    if (state_ == SessionState::connected || state_ == SessionState::connecting)
    {
        error = "null backend already has a session";
        return false;
    }

    if (request.pair.input.empty() || request.pair.output.empty())
    {
        error = "null backend requires an input and an output language";
        state_ = SessionState::faulted;

        // A faulted session is reported to the sink: swallowing state changes is
        // exactly what makes operator debugging impossible.
        if (sink_ != nullptr)
            sink_->onSessionStateChanged(state_);

        return false;
    }

    lastRequest_ = request;
    state_ = SessionState::connected;

    if (sink_ != nullptr)
        sink_->onSessionStateChanged(state_);

    return true;
}

bool NullTranslationBackend::submitAudio(const float* samples, int frameCount, std::string& error)
{
    error.clear();

    if (state_ != SessionState::connected)
    {
        error = "null backend has no open session";
        return false;
    }

    if (samples == nullptr || frameCount <= 0)
    {
        error = "null backend rejects empty audio blocks";
        return false;
    }

    submittedFrames_ += static_cast<std::uint64_t>(frameCount);
    return true;
}

void NullTranslationBackend::closeSession() noexcept
{
    if (state_ == SessionState::closed)
        return;

    state_ = SessionState::closed;

    if (sink_ != nullptr)
        sink_->onSessionStateChanged(state_);
}

} // namespace translation
} // namespace liveai
