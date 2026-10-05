#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Translation/Mock/MockTranslationBackend.h"
#include "Utils/Log.h"

using namespace liveai;
using namespace std::chrono_literals;

namespace {

struct QuietLog
{
    QuietLog()
    {
        LogConfig cfg;
        cfg.level = LogLevel::off;
        cfg.writeConsole = false;
        log::configure(cfg);
    }
    ~QuietLog() { log::resetForTests(); }
};

/// Records every sink callback with the thread-safety the contract demands of
/// any sink ("implementations must tolerate interleaving").
class RecordingSink final : public translation::ITranslationSink
{
public:
    void onTranslatedAudio(const float* samples, int frameCount, int sampleRate) override
    {
        const std::lock_guard lock(mutex_);
        deliveredFrames += frameCount;
        lastRate = sampleRate;

        if (firstSample.empty())
            firstSample.assign(samples, samples + std::min(frameCount, 3));
    }

    void onPartialText(std::string_view text) override
    {
        const std::lock_guard lock(mutex_);
        partials.emplace_back(text);
    }

    void onFinalText(std::string_view text) override
    {
        const std::lock_guard lock(mutex_);
        finals.emplace_back(text);
    }

    void onSessionStateChanged(translation::SessionState state) override
    {
        const std::lock_guard lock(mutex_);
        states.push_back(state);
    }

    void onTranslationError(const translation::TranslationError&) override
    {
        errors.fetch_add(1, std::memory_order_relaxed);
    }

    int deliveredCount() const
    {
        const std::lock_guard lock(mutex_);
        return static_cast<int> (deliveredFrames);
    }

    std::vector<std::string> finalsCopy() const
    {
        const std::lock_guard lock(mutex_);
        return finals;
    }

    std::vector<translation::SessionState> statesCopy() const
    {
        const std::lock_guard lock(mutex_);
        return states;
    }

    mutable std::mutex mutex_;
    std::uint64_t deliveredFrames = 0;
    int lastRate = 0;
    std::vector<float> firstSample;
    std::vector<std::string> partials;
    std::vector<std::string> finals;
    std::vector<translation::SessionState> states;
    std::atomic<int> errors { 0 };
};

translation::SessionRequest monoRequest()
{
    translation::SessionRequest request;
    request.pair = { "en", "ru" };
    request.instructions = "echo everything";
    request.inputSampleRate = 48000;
    request.outputSampleRate = 48000;
    return request;
}

translation::MockTranslationBackend::Options fastOptions()
{
    translation::MockTranslationBackend::Options options;
    options.latencyMs = 40;
    options.utteranceEndMs = 120;
    return options;
}

} // namespace

TEST_CASE("Mock translation: the contract rules hold, the same ones the real backend lives by",
          "[translation][mock][contract]")
{
    QuietLog quiet;
    RecordingSink sink;
    translation::MockTranslationBackend backend(fastOptions());

    std::string error;

    // Rule 1: no sink, no session.
    translation::MockTranslationBackend unsinked(fastOptions());
    CHECK_FALSE(unsinked.openSession(monoRequest(), error));
    CHECK(unsinked.state() == translation::SessionState::closed);

    backend.setSink(sink);

    // Rates are not invented: zero rates and a mock asked to resample are refused.
    {
        auto request = monoRequest();
        request.inputSampleRate = 0;
        CHECK_FALSE(backend.openSession(request, error));
        CHECK_FALSE(error.empty());
    }
    {
        auto request = monoRequest();
        request.outputSampleRate = 24000;
        CHECK_FALSE(backend.openSession(request, error));
    }

    // Refusals above changed and reported nothing (rule 3): no state events yet.
    CHECK(sink.statesCopy().empty());

    REQUIRE(backend.openSession(monoRequest(), error));

    // Rule 2: a second open over the live session refuses.
    CHECK_FALSE(backend.openSession(monoRequest(), error));

    // And the state events are the real transitions, exactly once, in order.
    const auto states = sink.statesCopy();
    REQUIRE(states.size() >= 2);
    CHECK(states[0] == translation::SessionState::connecting);
    CHECK(states[1] == translation::SessionState::connected);

    backend.closeSession();
    backend.closeSession();   // idempotent, reports nothing further

    const auto afterClose = sink.statesCopy();
    REQUIRE(afterClose.size() == 3);
    CHECK(afterClose[2] == translation::SessionState::closed);

    // Rule 6: the session after the close is fresh.
    CHECK(backend.openSession(monoRequest(), error));
    CHECK(backend.submittedFrames() == 0);
    backend.closeSession();
}

TEST_CASE("Mock translation: the echo arrives labelled, at the requested rate, after its delay",
          "[translation][mock]")
{
    QuietLog quiet;
    RecordingSink sink;
    translation::MockTranslationBackend backend(fastOptions());

    std::string error;
    backend.setSink(sink);
    REQUIRE(backend.openSession(monoRequest(), error));

    std::vector<float> block(480);
    for (int i = 0; i < 480; ++i)
        block[i] = i % 2 == 0 ? 0.5f : -0.25f;

    // Submitting on a connected session succeeds...
    CHECK(backend.submitAudio(block.data(), 480, error));
    CHECK(backend.submittedFrames() == 480);

    // ...and the echo is not instant: at t=0 nothing has returned. The check is
    // deliberately a fact about the delay ("known ground truth" of 019), not a
    // flaky stopwatch - 10 ms into a 40 ms latency there is nothing to deliver.
    std::this_thread::sleep_for(10ms);
    CHECK(sink.deliveredCount() == 0);

    const auto deadline = std::chrono::steady_clock::now() + 3s;

    while (sink.deliveredCount() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);

    CHECK(sink.deliveredCount() == 480);
    CHECK(sink.lastRate == 48000);   // the requested output rate, delivered at face value

    // Text events exist and SAY mock - an unlabeled mock is indistinguishable
    // from a malfunctioning provider, and this task forbids that ambiguity.
    {
        const std::lock_guard lock(sink.mutex_);
        REQUIRE_FALSE(sink.partials.empty());
        CHECK(sink.partials.front().rfind("mock", 0) == 0);
    }

    // An utterance ends after the silence: exactly one final, containing the
    // echoed length in seconds.
    while (sink.finalsCopy().empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);

    const auto finals = sink.finalsCopy();
    REQUIRE(finals.size() == 1);
    CHECK(finals[0].rfind("mock final", 0) == 0);
    CHECK(finals[0].find("echoed") != std::string::npos);

    backend.closeSession();
}

TEST_CASE("Mock translation: closeSession means it - queued audio never speaks again",
          "[translation][mock][contract]")
{
    QuietLog quiet;
    RecordingSink sink;
    translation::MockTranslationBackend backend(fastOptions());

    std::string error;
    backend.setSink(sink);
    REQUIRE(backend.openSession(monoRequest(), error));

    std::vector<float> block(480, 0.1f);

    // Queue an echo that has NOT reached its latency yet, and close over its
    // head: the dropped-but-never-late behaviour is contract rule 5.
    CHECK(backend.submitAudio(block.data(), 480, error));
    backend.closeSession();

    // No sink callbacks after the return, including the queued one, and submits
    // on the closed session refuse without faulting anything (rule 4).
    const int eventsAtClose = static_cast<int>(sink.statesCopy().size());
    const auto deliveredAtClose = sink.deliveredCount();

    std::this_thread::sleep_for(300ms);   // well past the 40 ms latency

    CHECK(sink.deliveredCount() == deliveredAtClose);
    CHECK(sink.statesCopy().size() == static_cast<std::size_t>(eventsAtClose));
    CHECK_FALSE(backend.submitAudio(block.data(), 480, error));
    CHECK(backend.state() == translation::SessionState::closed);
}
