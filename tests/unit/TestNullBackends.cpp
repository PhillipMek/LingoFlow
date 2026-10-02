#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "NDI/Null/NullNdiOutput.h"
#include "Translation/ITranslationBackend.h"
#include "Translation/Null/NullTranslationBackend.h"

using liveai::ndi::NullNdiOutput;
using liveai::ndi::OutputState;
using liveai::ndi::SubtitleFrame;
using liveai::translation::ITranslationSink;
using liveai::translation::NullTranslationBackend;
using liveai::translation::SessionRequest;
using liveai::translation::SessionState;

namespace {

class RecordingSink final : public ITranslationSink
{
public:
    void onTranslatedAudio(const float* samples, int frameCount, int sampleRate) override
    {
        audioCalls.push_back({ samples != nullptr, frameCount, sampleRate });
    }

    void onPartialText(std::string_view text) override { partials.emplace_back(text); }
    void onFinalText(std::string_view text) override { finals.emplace_back(text); }

    void onSessionStateChanged(SessionState state) override { states.push_back(state); }

    struct AudioCall
    {
        bool hadPointer;
        int frameCount;
        int sampleRate;
    };

    std::vector<AudioCall> audioCalls;
    std::vector<std::string> partials;
    std::vector<std::string> finals;
    std::vector<SessionState> states;
};

SessionRequest enRu()
{
    SessionRequest request;
    request.pair.input = "en";
    request.pair.output = "ru";
    request.instructions = "preserve meaning and numbers";
    return request;
}

} // namespace

TEST_CASE("Translation state names are stable", "[translation]")
{
    CHECK(liveai::translation::nameOf(SessionState::closed) == "closed");
    CHECK(liveai::translation::nameOf(SessionState::connected) == "connected");
    CHECK(liveai::translation::nameOf(SessionState::reconnecting) == "reconnecting");
}

TEST_CASE("NullTranslationBackend: openSession validates the pair and reports state", "[translation]")
{
    NullTranslationBackend backend;
    RecordingSink sink;
    backend.setSink(sink);

    CHECK(backend.state() == SessionState::closed);

    std::string error;
    auto bad = enRu();
    bad.pair.output.clear();
    CHECK_FALSE(backend.openSession(bad, error));
    CHECK_FALSE(error.empty());
    CHECK(backend.state() == SessionState::faulted);

    backend.closeSession();
    CHECK(backend.state() == SessionState::closed);

    REQUIRE(backend.openSession(enRu(), error));
    CHECK(backend.state() == SessionState::connected);
    CHECK(backend.lastRequest().pair.input == "en");
    CHECK(backend.lastRequest().pair.output == "ru");
    CHECK(backend.lastRequest().instructions == "preserve meaning and numbers");

    // A second session on the same backend is refused, not silently replaced.
    CHECK_FALSE(backend.openSession(enRu(), error));
    CHECK(backend.state() == SessionState::connected);

    backend.closeSession();

    // sink observed: faulted, closed, connected, closed
    REQUIRE(sink.states.size() == 4);
    CHECK(sink.states[0] == SessionState::faulted);
    CHECK(sink.states[1] == SessionState::closed);
    CHECK(sink.states[2] == SessionState::connected);
    CHECK(sink.states[3] == SessionState::closed);
}

TEST_CASE("NullTranslationBackend: submitAudio requires an open session", "[translation]")
{
    NullTranslationBackend backend;
    RecordingSink sink;
    backend.setSink(sink);

    std::vector<float> samples(480, 0.1f);
    std::string error;

    CHECK_FALSE(backend.submitAudio(samples.data(), static_cast<int>(samples.size()), error));
    CHECK(error.find("no open session") != std::string::npos);

    REQUIRE(backend.openSession(enRu(), error));

    CHECK_FALSE(backend.submitAudio(nullptr, 480, error));   // no data: rejected
    CHECK_FALSE(backend.submitAudio(samples.data(), 0, error));

    backend.closeSession();
    CHECK_FALSE(backend.submitAudio(samples.data(), 480, error));
}

TEST_CASE("NullTranslationBackend: produces no audio and no text", "[translation][contracts]")
{
    // The null backend must never fake translation results (AGENTS.md 19).
    NullTranslationBackend backend;
    RecordingSink sink;
    backend.setSink(sink);

    std::string error;
    REQUIRE(backend.openSession(enRu(), error));
    std::vector<float> samples(480, 0.1f);
    REQUIRE(backend.submitAudio(samples.data(), static_cast<int>(samples.size()), error));
    backend.closeSession();

    CHECK(sink.audioCalls.empty());
    CHECK(sink.partials.empty());
    CHECK(sink.finals.empty());
}

TEST_CASE("NullNdiOutput: start/publish/stop state machine", "[ndi]")
{
    NullNdiOutput output;
    std::string error;

    CHECK(output.state() == OutputState::disabled);

    SubtitleFrame frame;
    frame.text = "hello";
    CHECK_FALSE(output.publish(frame, error));
    CHECK(error.find("not started") != std::string::npos);

    CHECK_FALSE(output.start("", error));
    CHECK(error.find("must not be empty") != std::string::npos);

    REQUIRE(output.start("LingoFlow EN->RU", error));
    CHECK(output.state() == OutputState::ready);
    CHECK(output.streamName() == "LingoFlow EN->RU");

    frame.final = true;
    frame.sequence = 1;
    CHECK(output.publish(frame, error));
    CHECK(output.publishedFrames() == 1);
    CHECK(output.state() == OutputState::publishing);

    frame.text.clear();
    CHECK_FALSE(output.publish(frame, error));    // empty text is dropped silently
    CHECK(error.empty());
    CHECK(output.publishedFrames() == 1);

    output.stop();
    CHECK(output.state() == OutputState::disabled);
    CHECK(output.publishedFrames() == 1);
}

TEST_CASE("NDI output state names are stable", "[ndi]")
{
    CHECK(liveai::ndi::nameOf(OutputState::disabled) == "disabled");
    CHECK(liveai::ndi::nameOf(OutputState::ready) == "ready");
    CHECK(liveai::ndi::nameOf(OutputState::publishing) == "publishing");
}
