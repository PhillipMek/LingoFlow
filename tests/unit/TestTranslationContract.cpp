#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "Translation/Null/NullTranslationBackend.h"
#include "support/MockTranslationBackend.h"

using namespace liveai;
using liveai::test::MockTranslationBackend;
using liveai::translation::ITranslationSink;
using liveai::translation::NullTranslationBackend;
using liveai::translation::SessionRequest;
using liveai::translation::SessionState;
using liveai::translation::TranslationError;
using liveai::translation::TranslationErrorCategory;

namespace {

/// One sink for every contract test: it appends a line per callback, so a test
/// can assert the exact sequence - and two identical runs can be compared
/// string-for-string, which is what "deterministic" means for a mock.
class TraceSink final : public ITranslationSink
{
public:
    void onTranslatedAudio(const float* samples, int frameCount, int sampleRate) override
    {
        std::string line = std::format("audio {}@{}:", frameCount, sampleRate);

        for (int i = 0; i < frameCount; ++i)
            line += std::format(" {:.9g}", samples[i]);

        trace.push_back(std::move(line));
    }

    void onPartialText(std::string_view text) override
    {
        trace.push_back("partial '" + std::string(text) + "'");
    }

    void onFinalText(std::string_view text) override
    {
        trace.push_back("final '" + std::string(text) + "'");
    }

    void onSessionStateChanged(SessionState state) override
    {
        trace.push_back(std::string("state ").append(nameOf(state)));
    }

    void onTranslationError(const TranslationError& error) override
    {
        trace.push_back(std::string("error ") + (error.fatal ? "fatal " : "note ")
                        + std::string(nameOf(error.category)) + " '" + error.message + "'");
    }

    std::vector<std::string> trace;
    int audioBlocks = 0;

    std::string dump() const
    {
        std::string all;

        for (const std::string& line : trace)
            all += line + "\n";

        return all;
    }
};

SessionRequest enRu(int rate = 48000)
{
    SessionRequest request;
    request.pair.input = "en";
    request.pair.output = "ru";
    request.instructions = "preserve meaning and numbers";
    request.inputSampleRate = rate;
    request.outputSampleRate = rate;
    return request;
}

std::vector<float> block(int frames, float value)
{
    return std::vector<float>(static_cast<std::size_t>(frames), value);
}

} // namespace

TEST_CASE("Translation error category names are stable", "[translation][contract]")
{
    CHECK(nameOf(TranslationErrorCategory::connection) == "connection");
    CHECK(nameOf(TranslationErrorCategory::rejectedRequest) == "rejected-request");
    CHECK(nameOf(TranslationErrorCategory::audioFormat) == "audio-format");
    CHECK(nameOf(TranslationErrorCategory::protocol) == "protocol");
    CHECK(nameOf(TranslationErrorCategory::internal) == "internal");
}

TEST_CASE("MockTranslationBackend: lifecycle rules of the contract", "[translation][contract][lifecycle]")
{
    // Rule 1: a session without a sink is refused, and nothing is reported,
    // because nothing changed.
    {
        MockTranslationBackend backend;
        std::string error;

        CHECK_FALSE(backend.openSession(enRu(), error));
        CHECK_FALSE(error.empty());
        CHECK(backend.state() == SessionState::closed);
    }

    TraceSink sink;
    MockTranslationBackend backend;
    backend.setSink(sink);

    std::string error;

    // Rule 3: the two transitions of a successful open are reported exactly
    // once, in order.
    REQUIRE(backend.openSession(enRu(), error));
    CHECK(sink.dump() == "state connecting\nstate connected\n");
    CHECK(backend.state() == SessionState::connected);
    CHECK(backend.sessionsOpened() == 1);

    // The request is remembered as given - the contract carries it, not the
    // other way around.
    CHECK(backend.lastRequest().pair.input == "en");
    CHECK(backend.lastRequest().outputSampleRate == 48000);

    // Rule 2: a second open refuses, changes nothing, reports nothing.
    const std::string before = sink.dump();
    CHECK_FALSE(backend.openSession(enRu(), error));
    CHECK_FALSE(error.empty());
    CHECK(backend.state() == SessionState::connected);
    CHECK(sink.dump() == before);
    CHECK(backend.sessionsOpened() == 1);

    // Rule 5: close reports one transition; a second close reports nothing.
    backend.closeSession();
    CHECK(sink.dump().find("state closed\n") != std::string::npos);
    CHECK(backend.state() == SessionState::closed);

    const std::string afterClose = sink.dump();
    backend.closeSession();
    CHECK(sink.dump() == afterClose);

    // Rule 6: reopen works, and it is a new session - the request from the
    // first one is gone.
    auto second = enRu();
    second.instructions = "a different instruction";
    REQUIRE(backend.openSession(second, error));
    CHECK(backend.sessionsOpened() == 2);
    CHECK(backend.lastRequest().instructions == "a different instruction");
    CHECK(backend.sessionSubmits() == 0);   // cue positions are per session

    backend.closeSession();

    // Rule 4: outside connected, submits are refused - with an error, not in
    // silence - and refused submits advance nothing.
    CHECK_FALSE(backend.submitAudio(block(480, 0.25f).data(), 480, error));
    CHECK_FALSE(error.empty());
    CHECK(backend.acceptedSubmits() == 0);
    CHECK(backend.refusedSubmits() == 1);
}

TEST_CASE("MockTranslationBackend: submitted audio becomes the scripted audio",
          "[translation][contract][lifecycle]")
{
    TraceSink sink;
    MockTranslationBackend backend;
    backend.setSink(sink);
    backend.deliverFrames = 480;
    backend.deliverGain = 0.5f;
    backend.negate = true;

    std::string error;
    REQUIRE(backend.openSession(enRu(), error));

    for (int submit = 0; submit < 5; ++submit)
        REQUIRE(backend.submitAudio(block(480, 0.4f).data(), 480, error));

    // One delivered block per 480 queued frames, and its value is predictable
    // without reading the mock's code: 0.4 * -0.5 = -0.2, exactly, 480 times.
    CHECK(backend.deliveredBlocks() == 5);
    CHECK(backend.deliveredFrames() == 5 * 480);

    int audioEvents = 0;

    for (const std::string& line : sink.trace)
        if (line.rfind("audio ", 0) == 0)
            ++audioEvents;

    CHECK(audioEvents == 5);

    // The audio lines follow the two state transitions; find the first of them.
    const std::string* firstAudio = nullptr;

    for (const std::string& line : sink.trace)
    {
        if (line.rfind("audio ", 0) == 0)
        {
            firstAudio = &line;
            break;
        }
    }

    REQUIRE(firstAudio != nullptr);
    CHECK(firstAudio->rfind("audio 480@48000:", 0) == 0);
    CHECK(firstAudio->find(" -0.2") != std::string::npos);
    CHECK(firstAudio->find(" 0.2") == std::string::npos);   // no sign confusion

    // And the session transitions bracket the audio: state, state, then blocks.
    CHECK(sink.trace.size() == 2 + 5);
}

TEST_CASE("MockTranslationBackend: audio arrives in whole delivered blocks only",
          "[translation][contract][lifecycle]")
{
    TraceSink sink;
    MockTranslationBackend backend;
    backend.setSink(sink);
    backend.deliverFrames = 1000;
    backend.deliverGain = 1.0f;

    std::string error;
    REQUIRE(backend.openSession(enRu(), error));

    // 3 x 480 = 1440 queued: exactly one block of 1000 is deliverable, and the
    // remaining 440 wait. A mock that delivered partial blocks would change
    // the arithmetic of every latency test, so this is pinned.
    for (int submit = 0; submit < 3; ++submit)
        REQUIRE(backend.submitAudio(block(480, 0.1f).data(), 480, error));

    CHECK(backend.deliveredBlocks() == 1);
    CHECK(backend.deliveredFrames() == 1000);

    REQUIRE(backend.submitAudio(block(480, 0.1f).data(), 480, error));   // 440 + 480 = 920 queued
    CHECK(backend.deliveredBlocks() == 1);                               // still below 1000

    REQUIRE(backend.submitAudio(block(480, 0.1f).data(), 480, error));   // 1400 queued
    CHECK(backend.deliveredBlocks() == 2);
    CHECK(backend.deliveredFrames() == 2000);
}

TEST_CASE("MockTranslationBackend: the same script twice yields the same trace",
          "[translation][contract][determinism]")
{
    const auto run = []
    {
        TraceSink sink;
        MockTranslationBackend backend;
        backend.setSink(sink);
        backend.deliverFrames = 240;
        backend.deliverGain = 0.25f;
        backend.negate = true;
        backend.textCues = { { 1, "hello", false }, { 3, "hello world", true } };

        std::string error;
        REQUIRE(backend.openSession(enRu(), error));

        for (int submit = 0; submit < 4; ++submit)
        {
            const auto data = block(480, 0.8f - 0.1f * static_cast<float>(submit));
            REQUIRE(backend.submitAudio(data.data(), 480, error));
        }

        backend.closeSession();
        return sink.dump();
    };

    const std::string first = run();
    const std::string second = run();

    // Not "similar" - identical. Everything else in this file builds on this
    // property being real, so it gets its own test.
    CHECK(first == second);
    CHECK_FALSE(first.empty());

    // A spot check that the trace is not trivially empty on either side.
    CHECK(first.find("state connecting") != std::string::npos);
    CHECK(first.find("audio 240@48000") != std::string::npos);
    CHECK(first.find("final 'hello world'") != std::string::npos);
}

TEST_CASE("MockTranslationBackend: text flows with no audio at all - the channels are independent",
          "[translation][contract][independence]")
{
    TraceSink sink;
    MockTranslationBackend backend;
    backend.setSink(sink);
    backend.deliverFrames = 0;                       // no audio channel in this script
    backend.textCues = { { 1, "bonjour", false }, { 2, "bonjour monsieur", true } };

    std::string error;
    REQUIRE(backend.openSession(enRu(), error));

    REQUIRE(backend.submitAudio(block(480, 0.3f).data(), 480, error));
    REQUIRE(backend.submitAudio(block(480, 0.3f).data(), 480, error));

    CHECK(backend.deliveredBlocks() == 0);
    CHECK(backend.textEvents() == 2);

    CHECK(sink.dump() == "state connecting\n"
                         "state connected\n"
                         "partial 'bonjour'\n"
                         "final 'bonjour monsieur'\n");

    // And the other direction: audio with no text cue at all.
    TraceSink audioOnlySink;
    MockTranslationBackend audioOnly;
    audioOnly.setSink(audioOnlySink);
    audioOnly.deliverFrames = 480;

    REQUIRE(audioOnly.openSession(enRu(), error));
    REQUIRE(audioOnly.submitAudio(block(480, 0.5f).data(), 480, error));

    CHECK(audioOnly.textEvents() == 0);
    CHECK(audioOnly.deliveredBlocks() == 1);
}

TEST_CASE("MockTranslationBackend: a fatal error ends the session, everything else keeps running",
          "[translation][contract][errors]")
{
    TraceSink sink;
    MockTranslationBackend backend;
    backend.setSink(sink);
    backend.deliverFrames = 480;
    backend.errorCues = { { 2, TranslationErrorCategory::connection, "link dropped", true } };

    std::string error;
    REQUIRE(backend.openSession(enRu(), error));

    REQUIRE(backend.submitAudio(block(480, 0.25f).data(), 480, error));
    REQUIRE(backend.submitAudio(block(480, 0.25f).data(), 480, error));   // the cue fires here

    CHECK(backend.state() == SessionState::faulted);
    CHECK(backend.errorEvents() == 1);

    // The transition and the error both arrive, in the order that can be
    // asserted: the error explains the fault, then the state says so.
    CHECK(sink.trace.back() == "state faulted");
    CHECK(sink.trace[sink.trace.size() - 2] == "error fatal connection 'link dropped'");

    // The audio already delivered is not revoked by the error - the sink keeps
    // what it got (rule of task 005: playback degrades to silence, not to data loss
    // of a different kind).
    CHECK(backend.deliveredBlocks() == 2);

    // Rule 4 applies to a faulted session too: submits are refused, not fatal again.
    CHECK_FALSE(backend.submitAudio(block(480, 0.25f).data(), 480, error));
    CHECK_FALSE(error.empty());
    CHECK(backend.errorEvents() == 1);           // no second error event

    // A fault is survivable: close, and the next session opens normally.
    backend.closeSession();
    CHECK(backend.state() == SessionState::closed);

    REQUIRE(backend.openSession(enRu(), error));
    CHECK(backend.state() == SessionState::connected);
    CHECK(backend.sessionsOpened() == 2);
}

TEST_CASE("MockTranslationBackend: a non-fatal error is an event, not a death",
          "[translation][contract][errors]")
{
    TraceSink sink;
    MockTranslationBackend backend;
    backend.setSink(sink);
    backend.deliverFrames = 480;
    backend.errorCues = { { 1, TranslationErrorCategory::protocol, "one malformed event", false } };

    std::string error;
    REQUIRE(backend.openSession(enRu(), error));

    REQUIRE(backend.submitAudio(block(480, 0.25f).data(), 480, error));

    CHECK(backend.state() == SessionState::connected);     // unchanged
    CHECK(backend.errorEvents() == 1);
    CHECK(sink.trace.back() == "error note protocol 'one malformed event'");

    // The session went on streaming right through it.
    REQUIRE(backend.submitAudio(block(480, 0.25f).data(), 480, error));
    CHECK(backend.deliveredBlocks() == 2);
}

TEST_CASE("MockTranslationBackend: the delivered rate is the requested one, or the override",
          "[translation][contract][rates]")
{
    TraceSink sink;
    MockTranslationBackend backend;
    backend.setSink(sink);
    backend.deliverFrames = 480;

    std::string error;
    auto request = enRu(24000);          // some rate; task 008 decides which rates are real
    REQUIRE(backend.openSession(request, error));
    REQUIRE(backend.submitAudio(block(480, 0.5f).data(), 480, error));

    // As requested: the mock stamps what the session said, inventing nothing.
    CHECK(sink.trace.back().rfind("audio 480@24000:", 0) == 0);

    // Overridden: this is the path that makes the controller's rejection test
    // possible, so it belongs to the mock, not to the product.
    TraceSink sink2;
    MockTranslationBackend rude;
    rude.setSink(sink2);
    rude.deliverFrames = 480;
    rude.deliverAtSampleRate = 16000;

    REQUIRE(rude.openSession(enRu(48000), error));
    REQUIRE(rude.submitAudio(block(480, 0.5f).data(), 480, error));
    CHECK(sink2.trace.back().rfind("audio 480@16000:", 0) == 0);
}

TEST_CASE("MockTranslationBackend: a refused open reports nothing", "[translation][contract][lifecycle]")
{
    TraceSink sink;
    MockTranslationBackend backend;
    backend.setSink(sink);
    backend.refuseOpen = true;

    std::string error;
    CHECK_FALSE(backend.openSession(enRu(), error));
    CHECK(error == "mock: refusing to open");
    CHECK(backend.state() == SessionState::closed);

    // Nothing changed, so nothing was reported - the mirror of the rule that
    // every real change is reported.
    CHECK(sink.trace.empty());
}

TEST_CASE("NullTranslationBackend stays a silent shell under the extended contract",
          "[translation][contract][null]")
{
    // The Null backend exists so that the product can run without a provider.
    // Under task 007 it must still produce no audio, no text and no error
    // events - silence is its whole feature (AGENTS.md 19).
    TraceSink sink;
    NullTranslationBackend backend;
    backend.setSink(sink);

    std::string error;
    REQUIRE(backend.openSession(enRu(), error));
    REQUIRE(backend.submitAudio(block(480, 0.25f).data(), 480, error));
    REQUIRE(backend.submitAudio(block(480, 0.25f).data(), 480, error));
    backend.closeSession();
    backend.closeSession();

    std::string states;

    for (const std::string& line : sink.trace)
        states += line + "\n";

    // Only state transitions, exactly once each, and nothing else.
    CHECK(states == "state connected\nstate closed\n");
    CHECK(backend.submittedCalls() == 2);
}
