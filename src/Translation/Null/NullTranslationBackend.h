#pragma once
//
// Null translation backend: session-shaped no-op for the architecture skeleton
// and for tests. It changes state and records the request, but produces no audio
// and no text - deliberately, so that nothing in the product can mistake it for
// a working translation (AGENTS.md 19 forbids fake success).
//
// The behavioural mock of Developer/Mock mode is a DIFFERENT class on purpose:
// Translation/Mock/MockTranslationBackend.cpp (task 019) echoes audio and emits
// labelled text; this one produces nothing at all, so "Null" can never be
// mistaken for "working" anywhere - in tests, in smoke runs, or at a venue.

#include <string>

#include "Translation/ITranslationBackend.h"

namespace liveai {
namespace translation {

class NullTranslationBackend final : public ITranslationBackend
{
public:
    std::string_view name() const noexcept override { return "Null"; }
    SessionState state() const noexcept override { return state_; }

    void setSink(ITranslationSink& sink) noexcept override { sink_ = &sink; }

    bool openSession(const SessionRequest& request, std::string& error) override;
    bool submitAudio(const float* samples, int frameCount, std::string& error) override;
    void closeSession() noexcept override;

    /// What the caller asked for - used by tests and diagnostics.
    const SessionRequest& lastRequest() const noexcept { return lastRequest_; }
    int submittedCalls() const noexcept { return submittedCalls_; }
    float submittedFrames() const noexcept { return static_cast<float>(submittedFrames_); }

private:
    ITranslationSink* sink_ = nullptr;
    SessionState state_ = SessionState::closed;
    SessionRequest lastRequest_;
    int submittedCalls_ = 0;
    long long submittedFrames_ = 0;
};

} // namespace translation
} // namespace liveai
