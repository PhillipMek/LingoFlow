#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Translation/ReconnectSupervisor.h"
#include "support/MockTranslationBackend.h"

using namespace liveai;
using liveai::test::MockTranslationBackend;
using liveai::translation::ITranslationSink;
using liveai::translation::ReconnectSupervisor;
using liveai::translation::SessionRequest;
using liveai::translation::SessionState;
using liveai::translation::TranslationError;
using liveai::translation::TranslationErrorCategory;

namespace {

// --------------------------------------------------------------------- helpers

/// The application side of every supervisor test: a thread-safe recorder that
/// can be armed to turn any callback after a closeSession() return into a
/// failure (contract rule 5, the guarantee this wrapper must add on top of the
/// wrapped backend's own).
class RecordingSink final : public ITranslationSink
{
public:
    void onTranslatedAudio(const float* samples, int frameCount, int sampleRate) override
    {
        std::ignore = samples;
        record(std::format("audio {}@{}", frameCount, sampleRate));
    }

    void onPartialText(std::string_view text) override
    {
        record("partial '" + std::string(text) + "'");
    }

    void onFinalText(std::string_view text) override
    {
        record("final '" + std::string(text) + "'");
    }

    void onSessionStateChanged(SessionState state) override
    {
        record(std::string("state ").append(nameOf(state)));
    }

    void onTranslationError(const TranslationError& error) override
    {
        record(std::string("error ") + (error.fatal ? "fatal " : "note ")
               + std::string(nameOf(error.category)) + " '" + error.message + "'");
    }

    std::vector<std::string> trace() const
    {
        std::lock_guard lock(mutex_);
        return trace_;
    }

    int stateCount(std::string_view name) const
    {
        const std::string needle = "state " + std::string(name);
        std::lock_guard lock(mutex_);
        return static_cast<int>(std::count(trace_.begin(), trace_.end(), needle));
    }

    int errorCount(std::string_view needle) const
    {
        std::lock_guard lock(mutex_);
        int n = 0;
        for (const std::string& line : trace_)
            if (line.find(needle) != std::string::npos)
                ++n;
        return n;
    }

    int audioBlocks() const
    {
        std::lock_guard lock(mutex_);
        int n = 0;
        for (const std::string& line : trace_)
            if (line.rfind("audio ", 0) == 0)
                ++n;
        return n;
    }

    /// After arming, ANY callback is a rule-5 violation, recorded loudly.
    void armViolationCheck()
    {
        armed_.store(true);
    }

    int violations() const noexcept { return violations_.load(); }

private:
    void record(std::string line)
    {
        if (armed_.load())
            ++violations_;
        std::lock_guard lock(mutex_);
        trace_.push_back(std::move(line));
    }

    mutable std::mutex mutex_;
    std::vector<std::string> trace_;
    std::atomic<bool> armed_ { false };
    std::atomic<int> violations_ { 0 };
};

/// Tests drive wall-clock waits through the policy's own numbers, so recovery
/// runs in tens of milliseconds; polling with a generous cap keeps CI honest
/// without flaking on scheduling jitter.
bool waitUntil(const std::function<bool()>& pred, int timeoutMs = 2000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

ReconnectSupervisor::Policy fastPolicy(int initialMs = 20, int maxMs = 80, int maxAgeMs = 0)
{
    ReconnectSupervisor::Policy p;
    p.enabled = true;
    p.initialBackoffMs = initialMs;
    p.maxBackoffMs = maxMs;
    p.sessionMaxAgeMs = maxAgeMs;
    return p;
}

SessionRequest enRu()
{
    SessionRequest r;
    r.pair.input = "en";
    r.pair.output = "ru";
    r.instructions = "test instructions";
    r.model = "";
    r.inputSampleRate = 48000;
    r.outputSampleRate = 48000;
    return r;
}

/// Build a supervisor over a fresh mock the test can observe directly.
std::unique_ptr<ReconnectSupervisor> makeSupervisor(std::unique_ptr<MockTranslationBackend> mock,
                                                    ReconnectSupervisor::Policy policy,
                                                    MockTranslationBackend*& mockOut,
                                                    RecordingSink& sink)
{
    mockOut = mock.get();
    auto supervisor = std::make_unique<ReconnectSupervisor>(std::move(mock), std::move(policy));
    supervisor->setSink(sink);
    return supervisor;
}

std::string openAndRequire(ReconnectSupervisor& supervisor)
{
    std::string error;
    const bool opened = supervisor.openSession(enRu(), error);
    REQUIRE(opened);
    return error;
}

} // namespace

// --------------------------------------------------------------------- recovery

TEST_CASE("ReconnectSupervisor: a dropped session is replaced by a fresh one",
          "[translation][reconnect][faults]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(), fastPolicy(), m, sink);

    openAndRequire(*supervisor);
    REQUIRE(supervisor->state() == SessionState::connected);

    m->injectError(TranslationErrorCategory::connection, "transport died", /*fatal=*/true);

    // Recovery is under way: the composite state says reconnecting...
    CHECK(supervisor->state() == SessionState::reconnecting);
    // ...and it completes into a connected session.
    REQUIRE(waitUntil([&] { return supervisor->state() == SessionState::connected; }));

    // The fresh session replays the stored request verbatim (protocol section 10).
    REQUIRE(m->sessionsOpened() == 2);
    const auto requests = m->openRequests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0] == requests[1]);
    CHECK(requests[1].pair.output == "ru");
    CHECK(requests[1].inputSampleRate == 48000);

    // The application saw exactly the honest story: the real open, one fatal
    // error, one reconnecting, and the recovered session - no faulted/closed
    // flapping from the dying session leaked through.
    CHECK(sink.stateCount("connecting") == 1);
    CHECK(sink.stateCount("connected") == 2);
    CHECK(sink.stateCount("reconnecting") == 1);
    CHECK(sink.stateCount("faulted") == 0);
    CHECK(sink.stateCount("closed") == 0);
    CHECK(sink.errorCount("fatal connection 'transport died'") == 1);

    const auto stats = supervisor->stats();
    CHECK(stats.attempts == 1);
    CHECK(stats.recoveries == 1);
    CHECK(stats.proactiveReopens == 0);
    CHECK(stats.terminalFaults == 0);
    CHECK(stats.recoveringMsTotal >= 15); // we told it to wait 20 ms before the first attempt
}

TEST_CASE("ReconnectSupervisor: audio during the gap is refused and counted, never buffered",
          "[translation][reconnect][faults]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    // Long first wait and failing opens: the recovery window stays open long
    // enough to submit into it.
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(),
                                     fastPolicy(300, 600), m, sink);

    openAndRequire(*supervisor);
    m->openFailsRemaining = 50;
    m->injectError(TranslationErrorCategory::connection, "drop", /*fatal=*/true);

    std::vector<float> gap(64, 0.25f);
    std::string error;
    CHECK_FALSE(supervisor->submitAudio(gap.data(), static_cast<int>(gap.size()), error));
    CHECK_FALSE(error.empty());
    CHECK(supervisor->stats().gapRefusedFrames == gap.size());

    // The wrapped backend never even sees the gap audio: the refusal happens
    // at the seam, so nothing can be replayed later out of order.
    REQUIRE(m->sessionsOpened() == 1);

    // The service comes back: recovery completes on the next scheduled
    // attempt (policy waits of 300..600 ms, the waitUntil cap covers them).
    m->setOpenFails(0);
    REQUIRE(waitUntil([&] { return m->sessionsOpened() >= 2; }, 3000));
    CHECK(m->refusedSubmits() == 0);
    CHECK(supervisor->stats().gapRefusedFrames == gap.size()); // not double-counted
}

TEST_CASE("ReconnectSupervisor: backoff grows between failed attempts and caps",
          "[translation][reconnect][faults]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    // Doubles 30 -> 60 -> 80 (cap). All opens fail.
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(),
                                     fastPolicy(30, 80), m, sink);

    openAndRequire(*supervisor);
    m->openFailsRemaining = 1000; // keep it broken for the whole measurement

    const auto t0 = std::chrono::steady_clock::now();
    m->injectError(TranslationErrorCategory::connection, "drop", /*fatal=*/true);

    // By 500 ms the ladder 30+60+80+80 = 250 ms has elapsed, so at least four
    // attempts must have run; with per-attempt overhead a huge count means the
    // cap or the doubling broke.
    REQUIRE(waitUntil([&] { return supervisor->stats().attempts >= 4; }, 1500));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    CHECK(elapsed >= 250); // the waits were actually waited
    CHECK(supervisor->stats().attempts <= 25);

    // The noise of the failing attempts never reached the application: one
    // forwarded fatal error, one reconnecting, nothing else.
    CHECK(sink.errorCount("error fatal") == 1);
    CHECK(sink.stateCount("reconnecting") == 1);

    m->setOpenFails(0); // service comes back
    REQUIRE(waitUntil([&] { return supervisor->state() == SessionState::connected; }));
    CHECK(supervisor->stats().recoveries == 1);
}

TEST_CASE("ReconnectSupervisor: the service Retry-After hint is honoured",
          "[translation][reconnect][faults]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    // Policy would retry after 10 ms; the hint says at least 250.
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(),
                                     fastPolicy(10, 1000), m, sink);

    openAndRequire(*supervisor);

    const auto t0 = std::chrono::steady_clock::now();
    m->injectError(TranslationErrorCategory::connection, "slow_down", /*fatal=*/true,
                   /*retryAfterMs=*/250);
    REQUIRE(waitUntil([&] { return supervisor->state() == SessionState::connected; }));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    CHECK(elapsed >= 250);           // "wait at least as long as it specifies" (docs section 9)
    CHECK(supervisor->stats().attempts == 1); // and one wait was enough
}

TEST_CASE("ReconnectSupervisor: protocol-fatal is retryable (task 010 decision)",
          "[translation][reconnect][faults]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(), fastPolicy(), m, sink);

    openAndRequire(*supervisor);
    m->injectError(TranslationErrorCategory::protocol, "event stream broke", /*fatal=*/true);

    REQUIRE(waitUntil([&] { return supervisor->state() == SessionState::connected; }));
    CHECK(supervisor->stats().recoveries == 1);
    CHECK(sink.stateCount("faulted") == 0);
}

// ------------------------------------------------------------------ terminal faults

TEST_CASE("ReconnectSupervisor: errors a new session cannot fix end recovery",
          "[translation][reconnect][faults]")
{
    // rejectedRequest (bad pair, billing), audioFormat (our own
    // contract handling), authentication (the account gate) and internal
    // (unknown) are terminal by the policy in
    // ReconnectSupervisor.h - retrying those loops on a closed door.
    for (const auto category : { TranslationErrorCategory::rejectedRequest,
                                 TranslationErrorCategory::audioFormat,
                                 TranslationErrorCategory::authentication,
                                 TranslationErrorCategory::internal })
    {
        RecordingSink sink;
        MockTranslationBackend* m = nullptr;
        auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(), fastPolicy(), m, sink);

        openAndRequire(*supervisor);
        m->injectError(category, "terminal", /*fatal=*/true);

        REQUIRE(supervisor->state() == SessionState::faulted);
        CHECK(sink.stateCount("reconnecting") == 0);
        CHECK(sink.stateCount("faulted") == 1); // exactly once: synthesized, not duplicated
        CHECK(sink.errorCount("error fatal") == 1);
        CHECK(supervisor->stats().terminalFaults == 1);
        CHECK(supervisor->stats().attempts == 0);

        // And it stays terminal: no background retrying while the operator reads
        // the state.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        CHECK(m->sessionsOpened() == 1);
        CHECK(supervisor->stats().attempts == 0);
    }
}

TEST_CASE("ReconnectSupervisor: terminal is not permanent - operator reopening resumes recovery",
          "[translation][reconnect][faults]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(), fastPolicy(), m, sink);

    openAndRequire(*supervisor);
    m->injectError(TranslationErrorCategory::rejectedRequest, "bad key", true);
    REQUIRE(supervisor->state() == SessionState::faulted);

    // The operator fixes the key and starts again: the machine forgets the
    // terminal verdict, and a next transport drop recovers as usual.
    std::string error;
    REQUIRE(supervisor->openSession(enRu(), error));
    REQUIRE(waitUntil([&] { return supervisor->state() == SessionState::connected; }));

    m->injectError(TranslationErrorCategory::connection, "drop", true);
    REQUIRE(waitUntil([&] { return supervisor->stats().recoveries == 1; }));
    CHECK(supervisor->state() == SessionState::connected);
}

// ----------------------------------------------------------------------- pass-through

TEST_CASE("ReconnectSupervisor: non-fatal errors and streams pass through untouched",
          "[translation][reconnect]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(), fastPolicy(), m, sink);

    openAndRequire(*supervisor);

    m->deliverFrames = 4;
    m->textCues.push_back({ 1, "привет", false });
    std::vector<float> frames(4, 0.5f);
    std::string error;
    REQUIRE(supervisor->submitAudio(frames.data(), 4, error));

    m->injectError(TranslationErrorCategory::connection, "keepalive note", /*fatal=*/false);

    REQUIRE(waitUntil([&] { return sink.errorCount("note connection") == 1; }));
    CHECK(supervisor->state() == SessionState::connected);
    CHECK(sink.audioBlocks() == 1);
    CHECK(sink.trace()[3] == "partial 'привет'"); // audio, state order preserved from the mock
    CHECK(supervisor->stats().attempts == 0);     // an event is not a death sentence

    // After all of it the session is still the same single one.
    CHECK(m->sessionsOpened() == 1);
}

TEST_CASE("ReconnectSupervisor: disabled policy is pure pass-through",
          "[translation][reconnect]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    ReconnectSupervisor::Policy p = fastPolicy();
    p.enabled = false;
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(), std::move(p), m, sink);

    openAndRequire(*supervisor);
    m->injectError(TranslationErrorCategory::connection, "drop", true);

    // Exactly the 009 behavior: fatal error and faulted state reach the
    // application; nothing reopens on its own.
    CHECK(sink.errorCount("error fatal connection") == 1);
    CHECK(sink.stateCount("faulted") == 1);
    CHECK(sink.stateCount("reconnecting") == 0);
    CHECK(supervisor->state() == SessionState::faulted);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(m->sessionsOpened() == 1);
}

// ----------------------------------------------------------------- proactive reopen

TEST_CASE("ReconnectSupervisor: sessions reopen proactively before the provider ceiling",
          "[translation][reconnect]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    // Two age cycles of 100 ms each.
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(),
                                     fastPolicy(20, 40, 100), m, sink);

    openAndRequire(*supervisor);

    REQUIRE(waitUntil([&] { return supervisor->stats().proactiveReopens >= 2; }, 3000));
    CHECK(m->sessionsOpened() >= 3);
    CHECK(supervisor->state() == SessionState::connected);

    // The reopen is not an error: nothing fatal was reported, and the request
    // replayed unchanged every time.
    CHECK(sink.errorCount("error fatal") == 0);
    const auto requests = m->openRequests();
    for (const auto& r : requests)
        CHECK(r == requests[0]);

    CHECK(supervisor->stats().recoveries == 0); // proactive ones are not error recoveries

    // sessionAgeMs tracks the current session, not the application uptime.
    CHECK(supervisor->stats().sessionAgeMs < 5000);
}

TEST_CASE("ReconnectSupervisor: a server-announced expiry reopens even with the policy age off",
          "[translation][reconnect][expiry]")
{
    // Code review P1 (2026-10-05): sessionMaxAgeMs is the operator's prediction;
    // expires_at is the provider's own word. With the prediction switched off
    // entirely (maxAge 0), the announcement alone must still drive the
    // controlled reopen.
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    ReconnectSupervisor::Policy policy = fastPolicy(20, 40, /*maxAgeMs=*/0);
    policy.expirySafetyMarginMs = 20;

    auto mock = std::make_unique<MockTranslationBackend>();
    auto* raw = mock.get();
    raw->announceServerExpiry(150);   // the provider says: 150 ms to live
    auto supervisor = makeSupervisor(std::move(mock), policy, m, sink);

    openAndRequire(*supervisor);
    REQUIRE(supervisor->state() == SessionState::connected);

    // Deadline = 150 - 20 margin: the reopen must come long before "never".
    REQUIRE(waitUntil([&] { return supervisor->stats().proactiveReopens >= 2; }, 3000));
    CHECK(m->sessionsOpened() >= 3);
    CHECK(supervisor->state() == SessionState::connected);

    // The announcement is visible through the mounted chain, not just inside.
    long long remainingMs = 0;
    CHECK(supervisor->serverSessionExpiryRemainingMs(remainingMs));
    CHECK(remainingMs == 150);   // the mock's static knob, verbatim

    CHECK(supervisor->stats().recoveries == 0);   // an expiry reopen is not an error recovery
}

TEST_CASE("ReconnectSupervisor: the earlier of server expiry and policy age wins",
          "[translation][reconnect][expiry]")
{
    RecordingSink sink;

    SECTION("long policy, near expiry: the announcement rules")
    {
        MockTranslationBackend* m = nullptr;
        ReconnectSupervisor::Policy policy = fastPolicy(20, 40, 600000);   // ten minutes
        policy.expirySafetyMarginMs = 0;
        auto mock = std::make_unique<MockTranslationBackend>();
        mock->announceServerExpiry(120);
        auto supervisor = makeSupervisor(std::move(mock), policy, m, sink);
        openAndRequire(*supervisor);
        // Ten-minute policy would wait; the announced 120 ms must not.
        REQUIRE(waitUntil([&] { return supervisor->stats().proactiveReopens >= 1; }, 2000));
    }
    SECTION("short policy, far expiry: the policy cap rules")
    {
        MockTranslationBackend* m = nullptr;
        ReconnectSupervisor::Policy policy = fastPolicy(20, 40, 120);
        policy.expirySafetyMarginMs = 1000;   // would push any real deadline far out
        auto mock = std::make_unique<MockTranslationBackend>();
        mock->announceServerExpiry(120000);   // two hours announced - no reason to wait two hours
        auto supervisor = makeSupervisor(std::move(mock), policy, m, sink);
        openAndRequire(*supervisor);
        REQUIRE(waitUntil([&] { return supervisor->stats().proactiveReopens >= 1; }, 2000));
    }
}

TEST_CASE("ReconnectSupervisor: no announcement and no policy age arms no deadline",
          "[translation][reconnect][expiry]")
{
    // The pre-review behavior is preserved exactly: maxAge 0 plus a backend
    // that announces nothing (the interface default, the honest Null/Mock
    // answer) means a session that runs until something actually breaks.
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(),
                                     fastPolicy(20, 40, 0), m, sink);

    openAndRequire(*supervisor);
    long long remainingMs = 0;
    CHECK_FALSE(supervisor->serverSessionExpiryRemainingMs(remainingMs));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(supervisor->stats().proactiveReopens == 0);
    CHECK(supervisor->stats().attempts == 0);
    CHECK(supervisor->state() == SessionState::connected);
}

TEST_CASE("ReconnectSupervisor: session duration is measurable", "[translation][reconnect]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(), fastPolicy(), m, sink);

    openAndRequire(*supervisor);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(supervisor->stats().sessionAgeMs >= 50);

    supervisor->closeSession();
    CHECK(supervisor->stats().lastSessionMs >= 50);
    CHECK(supervisor->stats().sessionAgeMs == 0);
}

// ------------------------------------------------------------------------ shutdown

TEST_CASE("ReconnectSupervisor: closeSession joins the recovery loop; nothing calls back after",
          "[translation][reconnect][lifecycle]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(),
                                     fastPolicy(30, 60), m, sink);

    openAndRequire(*supervisor);
    m->openFailsRemaining = 1000;
    m->injectError(TranslationErrorCategory::connection, "drop", true);
    REQUIRE(supervisor->state() == SessionState::reconnecting);
    REQUIRE(waitUntil([&] { return supervisor->stats().attempts >= 1; }));

    supervisor->closeSession();
    sink.armViolationCheck(); // rule 5 from here on: any callback is a violation

    CHECK(supervisor->state() == SessionState::closed);
    CHECK(sink.stateCount("closed") == 1); // reported during the call, before the return

    const auto attemptsAtClose = supervisor->stats().attempts;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(supervisor->stats().attempts == attemptsAtClose); // the loop really stopped
    CHECK(sink.violations() == 0);

    sink.armViolationCheck(); // keep it armed through the destructor
    supervisor.reset();
    CHECK(sink.violations() == 0);
}

TEST_CASE("ReconnectSupervisor: audio keeps flowing after a recovery", "[translation][reconnect]")
{
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(), fastPolicy(), m, sink);

    openAndRequire(*supervisor);
    m->injectError(TranslationErrorCategory::connection, "drop", true);
    REQUIRE(waitUntil([&] { return supervisor->state() == SessionState::connected; }));

    m->deliverFrames = 4;
    std::vector<float> frames(4, 0.25f);
    std::string error;
    REQUIRE(supervisor->submitAudio(frames.data(), 4, error));
    REQUIRE(waitUntil([&] { return sink.audioBlocks() >= 1; }));
}

TEST_CASE("ReconnectSupervisor: isRetryable matches the documented policy",
          "[translation][reconnect]")
{
    // The task 010 decision (docs section 9), refined by code review P2
    // (2026-10-05): transport deaths, broken event streams and everything the
    // provider itself labels transient ("come back later": rate limits,
    // overload) are fixed - or at least retried honestly - by a new session.
    // Refusals of WHO asks (authentication), of WHAT the request IS
    // (rejectedRequest, billing included), our contract-side format failures
    // (audioFormat) and the unknown are not.
    CHECK(ReconnectSupervisor::isRetryable(TranslationErrorCategory::connection));
    CHECK(ReconnectSupervisor::isRetryable(TranslationErrorCategory::protocol));
    CHECK(ReconnectSupervisor::isRetryable(TranslationErrorCategory::rateLimited));
    CHECK(ReconnectSupervisor::isRetryable(TranslationErrorCategory::serviceOverloaded));
    CHECK_FALSE(ReconnectSupervisor::isRetryable(TranslationErrorCategory::authentication));
    CHECK_FALSE(ReconnectSupervisor::isRetryable(TranslationErrorCategory::rejectedRequest));
    CHECK_FALSE(ReconnectSupervisor::isRetryable(TranslationErrorCategory::audioFormat));
    CHECK_FALSE(ReconnectSupervisor::isRetryable(TranslationErrorCategory::internal));
}

TEST_CASE("ReconnectSupervisor: transient refusals recover, the account gate does not",
          "[translation][reconnect][faults]")
{
    // The behavioral half of the same decision: a finer vocabulary is decoration
    // until the policy table acts differently on it.
    RecordingSink sink;
    MockTranslationBackend* m = nullptr;
    auto supervisor = makeSupervisor(std::make_unique<MockTranslationBackend>(), fastPolicy(), m, sink);
    openAndRequire(*supervisor);

    m->injectError(TranslationErrorCategory::rateLimited, "come back later", /*fatal=*/true);
    REQUIRE(waitUntil([&] { return supervisor->state() == SessionState::connected; }));
    CHECK(supervisor->stats().recoveries == 1);
    CHECK(supervisor->stats().terminalFaults == 0);

    m->injectError(TranslationErrorCategory::serviceOverloaded, "overloaded", /*fatal=*/true);
    REQUIRE(waitUntil([&] { return supervisor->state() == SessionState::connected; }));
    CHECK(supervisor->stats().recoveries == 2);

    // The account gate: terminal, no background hammering of a door that only
    // the operator can open.
    m->injectError(TranslationErrorCategory::authentication, "the key was refused", /*fatal=*/true);
    CHECK(supervisor->state() == SessionState::faulted);
    CHECK(supervisor->stats().terminalFaults == 1);
    const std::uint64_t attemptsAfter = supervisor->stats().attempts;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(supervisor->stats().attempts == attemptsAfter);
    CHECK(supervisor->stats().recoveries == 2);   // and no "recoveries" were invented
}
