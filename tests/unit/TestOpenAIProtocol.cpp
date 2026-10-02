//
// Task 009: the OpenAI realtime translation backend against a scripted
// transport. These tests pin the behaviors the product depends on and that
// tasks 010/012 build on:
//
//   * the protocol shape of docs/openai-realtime-protocol.md section 17:
//     URL, headers, session.created -> session.update -> session.updated as
//     the usable trigger, the three client events, the graceful close drain;
//   * the contract rules of ITranslationBackend.h (transitions reported once,
//     submit only while connected, no sink callbacks after closeSession());
//   * the section 9 error mapping: provider vocabulary in, five categories
//     out, recoverable unless the transport dies;
//   * the audio framing of section 7 (base64 PCM16 little-endian mono 24 kHz)
//     including validation of the optional delta metadata, and the resampling
//     the backend owes the contract at non-wire rates.
//
// Everything is offline: the transport is the scripted fake. Timing is bounded
// by the small cadences configured on the fixture, never by sleeping blindly.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Network/Base64.h"
#include "Network/OpenAIRealtimeBackend.h"
#include "Security/ISecretStore.h"
#include "Translation/ITranslationBackend.h"
#include "support/FakeWebSocketTransport.h"

using namespace liveai;
using translation::SessionRequest;
using translation::SessionState;
using translation::TranslationError;
using translation::TranslationErrorCategory;

namespace {

// ------------------------------------------------------------------ fakes

class FakeSecrets final : public security::ISecretStore
{
public:
    std::string_view name() const noexcept override { return "Fake"; }

    security::SecretStatus store(std::string_view, std::string_view) override
    {
        return security::SecretStatus::stored;
    }

    std::optional<std::string> load(std::string_view identifier) override
    {
        if (available && identifier == security::kOpenAiApiKey)
            return std::string("test-api-key");
        return std::nullopt;
    }

    security::SecretStatus remove(std::string_view) override
    {
        return security::SecretStatus::stored;
    }

    std::vector<std::string> identifiers() const override { return {}; }

    bool available = true;
};

class RecordingSink final : public translation::ITranslationSink
{
public:
    struct AudioBlock
    {
        std::vector<float> samples;
        int sampleRate = 0;
    };

    void onTranslatedAudio(const float* samples, int frameCount, int sampleRate) override
    {
        maybeViolation();
        const std::lock_guard<std::mutex> lock (mutex_);
        audio_.push_back(AudioBlock { std::vector<float> (samples, samples + frameCount),
                                      sampleRate });
    }

    void onPartialText(std::string_view text) override
    {
        maybeViolation();
        const std::lock_guard<std::mutex> lock (mutex_);
        partials_.emplace_back(text);
    }

    void onFinalText(std::string_view text) override
    {
        maybeViolation();
        const std::lock_guard<std::mutex> lock (mutex_);
        finals_.emplace_back(text);
    }

    void onSessionStateChanged(SessionState state) override
    {
        maybeViolation();
        const std::lock_guard<std::mutex> lock (mutex_);
        states_.push_back(state);
    }

    void onTranslationError(const TranslationError& error) override
    {
        maybeViolation();
        const std::lock_guard<std::mutex> lock (mutex_);
        errors_.push_back(error);
    }

    /// closeSession() returning must end all callbacks (contract rule 5);
    /// tests arm this and any callback afterwards is a hard violation.
    void armAfterCloseExpectation() { closedReturned_.store(true); }
    int violations() const { return violations_.load(); }

    std::vector<SessionState> states() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return states_;
    }

    std::vector<TranslationError> errors() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return errors_;
    }

    std::vector<AudioBlock> audio() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return audio_;
    }

    std::string joinedPartials() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        std::string out;
        for (const std::string& p : partials_)
            out += p;
        return out;
    }

    std::size_t partialCount() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return partials_.size();
    }

    std::size_t finalCount() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return finals_.size();
    }

private:
    void maybeViolation()
    {
        if (closedReturned_.load())
            ++violations_;
    }

    mutable std::mutex mutex_;
    std::vector<SessionState> states_;
    std::vector<TranslationError> errors_;
    std::vector<AudioBlock> audio_;
    std::vector<std::string> partials_;
    std::vector<std::string> finals_;
    std::atomic<bool> closedReturned_ { false };
    std::atomic<int> violations_ { 0 };
};

// --------------------------------------------------------------- event scripts

std::string createdEvent()
{
    return R"({"type":"session.created","session":{"id":"sess_fake","type":"translation","model":"gpt-realtime-translate","expires_at":4000000000,"audio":{"input":{"noise_reduction":null,"transcription":null},"output":{"language":"es"}}}})";
}

std::string updatedEvent(const std::string& language = "ru")
{
    return R"({"type":"session.updated","session":{"id":"sess_fake","type":"translation","model":"gpt-realtime-translate","audio":{"input":{"noise_reduction":null,"transcription":null},"output":{"language":")"
         + language + R"("}}}})";
}

std::string audioDeltaEvent(const std::vector<std::int16_t>& samples,
                             const std::string& extraFields = {})
{
    const std::string b64 = network::base64Encode(
        reinterpret_cast<const std::uint8_t*>(samples.data()),
        samples.size() * sizeof(std::int16_t));
    // extraFields is a continuation like `,"channels":2` - it must come AFTER
    // the closing quote of the delta value, not inside it.
    return std::string(R"({"type":"session.output_audio.delta","delta":")") + b64 + R"(")"
         + extraFields + "}";
}

std::string closedEvent()
{
    return R"({"type":"session.closed"})";
}

// ------------------------------------------------------------------- fixture

class Scenario {
public:
    Scenario()
    {
        options.cadenceMs = 40;
        options.handshakeTimeoutMs = 2000;
        options.closeDrainTimeoutMs = 400;
        options.maxQueuedInputMs = 10000; // 10 s of headroom, plenty for tests

        // The backend retains the transport OBJECT until the next open or the
        // destructor (closeSession released the socket itself). Tests observe
        // sent frames after close; a production caller has the same stable
        // handle. The factory is a plain create call.
        fakeFactory = [this] {
            auto t = std::make_unique<test::FakeWebSocketTransport>();
            t->connectOutcome = nextConnect;
            for (const std::string& event : handshakeScript)
                t->queueMessage(event);
            fake = t.get();
            return t;
        };

        backend = std::make_unique<network::OpenAIRealtimeBackend>(secrets, options, fakeFactory);
        backend->setSink(sink);
    }

    SessionRequest Request() const
    {
        SessionRequest r;
        r.pair.input = "en";
        r.pair.output = "ru";
        r.inputSampleRate = 24000; // identity: submitted frames are wire frames
        r.outputSampleRate = 24000;
        return r;
    }

    std::string open(const SessionRequest& request)
    {
        std::string error;
        opened = backend->openSession(request, error);
        return error;
    }

    bool openDefault()
    {
        return open(Request()).empty() && opened;
    }

    /// Poll a predicate with a hard bound (the loop is wall-clock paced).
    static bool waitFor(const std::function<bool()>& pred, int ms = 3000)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds (ms);
        while (std::chrono::steady_clock::now() < end)
        {
            if (pred())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds (2));
        }
        return pred();
    }

    static std::string audioField(const std::string& frame)
    {
        const std::size_t p = frame.find(R"("audio":")");
        if (p == std::string::npos)
            return {};
        const std::size_t start = p + 9;
        const std::size_t end = frame.find('"', start);
        return frame.substr(start, end - start);
    }

    FakeSecrets secrets;
    network::OpenAIRealtimeOptions options;
    network::TransportFactory fakeFactory;
    test::FakeWebSocketTransport* fake = nullptr;
    RecordingSink sink;
    std::unique_ptr<network::OpenAIRealtimeBackend> backend;

    network::ConnectResult nextConnect { true, 101, {} };
    std::vector<std::string> handshakeScript { createdEvent(), updatedEvent() };
    bool opened = false;
};

std::vector<float> constantBlock(int frames, float value)
{
    return std::vector<float> (static_cast<std::size_t>(frames), value);
}

} // namespace

// ------------------------------------------------------------------ handshake

TEST_CASE("OpenAI backend: the documented URL, auth header and handshake order",
          "[openai][protocol]")
{
    Scenario s;
    REQUIRE(s.openDefault());

    CHECK(s.fake->lastUrl() == "api.openai.com/v1/realtime/translations?model=gpt-realtime-translate");
    CHECK(s.fake->lastPort() == 443);

    const std::vector<std::string> headers = s.fake->lastHeaders();
    CHECK(std::find(headers.begin(), headers.end(), "Authorization: Bearer test-api-key")
          != headers.end());

    // Transitions: connecting, then connected exactly once (contract rules 3/4).
    const auto states = s.sink.states();
    REQUIRE(states.size() == 2);
    CHECK(states[0] == SessionState::connecting);
    CHECK(states[1] == SessionState::connected);
    CHECK(s.backend->state() == SessionState::connected);

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
    s.backend.reset();
}

TEST_CASE("OpenAI backend: session.update carries only documented fields", "[openai][protocol]")
{
    Scenario s;
    REQUIRE(s.openDefault());

    const auto sent = s.fake->sent();
    REQUIRE_FALSE(sent.empty());
    const std::string& update = sent.front();

    CHECK(update.find(R"("type":"session.update")") != std::string::npos);
    CHECK(update.find(R"("session":{"audio")") != std::string::npos); // wrapper shape, live-verified
    CHECK(update.find(R"("language":"ru")") != std::string::npos);
    CHECK(update.find("evt_") != std::string::npos); // optional event_id, sent for error correlation

    // Absent by protocol decision (sections 12.1/12.7): no instructions, no
    // guessing about transcription or noise reduction, model is not mutable.
    CHECK(update.find("instructions") == std::string::npos);
    CHECK(update.find("transcription") == std::string::npos);
    CHECK(update.find("noise_reduction") == std::string::npos);
    CHECK(update.find("gpt-realtime-translate") == std::string::npos);

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}

// ----------------------------------------------------------------- refusals

TEST_CASE("OpenAI backend: refused upgrade maps HTTP status per section 9", "[openai][protocol][faults]")
{
    struct Case
    {
        int status;
        TranslationErrorCategory expected;
    };
    const Case cases[] = {
        { 401, TranslationErrorCategory::rejectedRequest },
        { 403, TranslationErrorCategory::rejectedRequest },
        { 429, TranslationErrorCategory::connection },
        { 500, TranslationErrorCategory::connection },
        { 503, TranslationErrorCategory::connection },
    };

    for (const Case& c : cases)
    {
        Scenario s;
        s.nextConnect = network::ConnectResult { false, c.status, {} };
        const std::string error = s.open(s.Request());
        CHECK_FALSE(s.opened);
        CHECK_FALSE(error.empty());

        // The attempt was real: connecting was reported, and it must transition
        // out (contract rule 3 allows closed OR faulted; we fault honestly).
        const auto states = s.sink.states();
        REQUIRE(states.size() == 2);
        CHECK(states[0] == SessionState::connecting);
        CHECK(states[1] == SessionState::faulted);

        const auto errors = s.sink.errors();
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].category == c.expected);
        CHECK(errors[0].fatal);

        s.sink.armAfterCloseExpectation();
        s.backend->closeSession();
    }
}

TEST_CASE("OpenAI backend: refused upgrades carry the section 9 recovery hints",
          "[openai][protocol][faults]")
{
    // 429 with Retry-After: a retryable connection failure whose hint must
    // cross the seam so the task 010 policy can honour "wait at least this
    // long" (protocol section 9).
    {
        Scenario s;
        network::ConnectResult cr { false, 429, {} };
        cr.retryAfterSec = 5;
        s.nextConnect = cr;
        s.open(s.Request());

        const auto errors = s.sink.errors();
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].category == TranslationErrorCategory::connection);
        CHECK(errors[0].retryAfterMs == 5000);
        s.sink.armAfterCloseExpectation();
        s.backend->closeSession();
    }

    // 429 with the documented billing/account code is a different answer:
    // retrying will not restore access, so it maps to the category the
    // recovery policy stops on. The code name is classification input and
    // must never appear in what crosses the seam.
    {
        Scenario s;
        network::ConnectResult cr { false, 429, {} };
        cr.retryAfterSec = 30;
        cr.refusalBody = R"({"error":{"message":"Your account credit balance is exhausted.",)"
                         R"("type":"billing_error","code":"credit_balance_exhausted","param":null}})";
        s.nextConnect = cr;
        s.open(s.Request());

        const auto errors = s.sink.errors();
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].category == TranslationErrorCategory::rejectedRequest);
        CHECK(errors[0].retryAfterMs == 30000); // the hint travels anyway; the policy will not use it
        CHECK(errors[0].message.find("credit") == std::string::npos);
        CHECK(errors[0].message.find("billing") == std::string::npos);
        s.sink.armAfterCloseExpectation();
        s.backend->closeSession();
    }

    // A 429 whose body we cannot classify keeps the plain table default
    // (retryable) - never silently upgraded to terminal, never silently
    // dropped (AGENTS.md 19).
    {
        Scenario s;
        network::ConnectResult cr { false, 429, {} };
        cr.refusalBody = "not json at all";
        s.nextConnect = cr;
        s.open(s.Request());

        const auto errors = s.sink.errors();
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].category == TranslationErrorCategory::connection);
        CHECK(errors[0].retryAfterMs == 0); // no hint -> the policy's own backoff
        s.sink.armAfterCloseExpectation();
        s.backend->closeSession();
    }
}

TEST_CASE("OpenAI backend: missing credentials refuse the open without touching the sink",
          "[openai][protocol][security]")
{
    Scenario s;
    s.secrets.available = false;
    const std::string error = s.open(s.Request());
    CHECK_FALSE(s.opened);
    CHECK(error.find("credential") != std::string::npos);
    CHECK(s.sink.states().empty());
    CHECK(s.sink.errors().empty());
    // The credential value itself must never leak into the error string.
    CHECK(error.find("test-api-key") == std::string::npos);
    CHECK(s.backend->state() == SessionState::closed);
}

TEST_CASE("OpenAI backend: local validation rejects, reporting nothing (rule 3)",
          "[openai][contract][openai]")
{
    // 1. No sink attached (rule 1). A default-constructed backend resolves
    //    credentials/transport only after this check, so no fake is needed.
    {
        FakeSecrets secrets;
        network::OpenAIRealtimeBackend backend (secrets);
        std::string error;
        CHECK_FALSE(backend.openSession(SessionRequest {}, error));
        CHECK(error.find("setSink") != std::string::npos);
    }

    Scenario s;
    std::string error;

    // 2. Unknown model identifier (protocol section 11: exactly one snapshot).
    auto req = s.Request();
    req.model = "gpt-realtime-imagined";
    error = s.open(req);
    CHECK_FALSE(s.opened);
    CHECK(error.find("model") != std::string::npos);

    // 3. Empty target language.
    req = s.Request();
    req.model.clear();
    req.pair.output.clear();
    error = s.open(req);
    CHECK_FALSE(s.opened);
    CHECK(error.find("language") != std::string::npos);

    // 4. Rates outside the documented conversion envelope: refused, not guessed.
    req = s.Request();
    req.pair.output = "ru";
    req.inputSampleRate = 44100;
    error = s.open(req);
    CHECK_FALSE(s.opened);
    CHECK(error.find("input sample rate") != std::string::npos);

    req = s.Request();
    req.outputSampleRate = 0; // contract: zero means the caller does not know
    error = s.open(req);
    CHECK_FALSE(s.opened);
    CHECK(error.find("output sample rate") != std::string::npos);

    CHECK(s.sink.states().empty());
    CHECK(s.sink.errors().empty());
    CHECK(s.backend->state() == SessionState::closed);
}

TEST_CASE("OpenAI backend: double open is refused while a session is open", "[openai][contract]")
{
    Scenario s;
    REQUIRE(s.openDefault());

    const auto statesBefore = s.sink.states().size();
    const std::string error = s.open(s.Request());
    CHECK_FALSE(s.opened);
    CHECK(error.find("already") != std::string::npos);
    CHECK(s.sink.states().size() == statesBefore);

    // closeSession is idempotent (rule 5).
    s.backend->closeSession();
    s.backend->closeSession();
    const auto states = s.sink.states();
    REQUIRE(states.back() == SessionState::closed);
    size_t closedCount = 0;
    for (const SessionState st : states)
        if (st == SessionState::closed)
            ++closedCount;
    CHECK(closedCount == 1);
    s.sink.armAfterCloseExpectation();
}

TEST_CASE("OpenAI backend: submitAudio is accepted only while connected (rule 4)",
          "[openai][contract]")
{
    Scenario s;
    std::string error;
    auto block = constantBlock(960, 0.5f);
    CHECK_FALSE(s.backend->submitAudio(block.data(), 960, error)); // closed
    CHECK_FALSE(error.empty());
    CHECK(s.sink.errors().empty()); // refusal is an answer, not a fault

    REQUIRE(s.openDefault());
    error.clear();
    CHECK(s.backend->submitAudio(block.data(), 960, error));
    CHECK(error.empty());

    CHECK_FALSE(s.backend->submitAudio(nullptr, 10, error));
    CHECK_FALSE(s.backend->submitAudio(block.data(), 0, error));

    s.backend->closeSession();
    CHECK_FALSE(s.backend->submitAudio(block.data(), 960, error));
    s.sink.armAfterCloseExpectation();
}

TEST_CASE("OpenAI backend: input queue backpressure refuses instead of dropping (rule 4)",
          "[openai][faults]")
{
    Scenario s;
    s.options.cadenceMs = 1000;   // slow consumption
    s.options.maxQueuedInputMs = 20; // cap = 24000 * 20/1000 = 480 frames
    s.backend = std::make_unique<network::OpenAIRealtimeBackend>(s.secrets, s.options,
                                                                 s.fakeFactory);
    s.backend->setSink(s.sink);
    REQUIRE(s.openDefault());

    std::string error;
    // 481 frames cannot fit the cap even in an empty queue: deterministic.
    CHECK_FALSE(s.backend->submitAudio(constantBlock(481, 0.1f).data(), 481, error));
    CHECK(error.find("queue full") != std::string::npos);

    error.clear();
    CHECK(s.backend->submitAudio(constantBlock(480, 0.1f).data(), 480, error)); // cap exactly
    CHECK(s.sink.errors().empty()); // the refusal itself is not a fault event

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}

// ------------------------------------------------------------------ append wire

TEST_CASE("OpenAI backend: the append cadence streams base64 PCM16 and pads with silence",
          "[openai][audio]")
{
    Scenario s;
    REQUIRE(s.openDefault()); // cadence 40 ms -> 960 wire frames per chunk at 24 kHz

    const auto block = constantBlock(960, 0.5f); // exactly one chunk
    std::string error;
    REQUIRE(s.backend->submitAudio(block.data(), 960, error));

    // The loop's very first tick may fire before the submit lands, so frame
    // order is not the assertion: what must exist is a data frame (the 0.5f
    // chunk: int16 16384 = 0x4000, LE bytes 00 40) AND a silence frame
    // (all zeros) after the queue ran dry.
    auto classify = [&](std::vector<std::uint8_t>& d) -> int {
        if (d.size() != 1920)
            return 0;
        bool data = true, silence = true;
        for (std::size_t i = 0; i < d.size(); i += 2)
        {
            if (d[i] != 0x00 || d[i + 1] != 0x40)
                data = false;
            if (d[i] != 0x00 || d[i + 1] != 0x00)
                silence = false;
        }
        return data ? 1 : (silence ? 2 : 0);
    };

    int sawData = 0, sawSilence = 0;
    const bool both = Scenario::waitFor([&] {
        sawData = 0;
        sawSilence = 0;
        for (const std::string& frame : s.fake->sent())
        {
            if (frame.find(R"("type":"session.input_audio_buffer.append")") == std::string::npos)
                continue;
            std::vector<std::uint8_t> d;
            const std::string payload = Scenario::audioField(frame);
            if (network::base64Decode(payload, d))
            {
                const int c = classify(d);
                if (c == 1)
                    ++sawData;
                if (c == 2)
                    ++sawSilence;
            }
        }
        return sawData > 0 && sawSilence > 0;
    });
    INFO("data frames: " << sawData << ", silence frames: " << sawSilence);
    CHECK(both);

    // 960 frames * 2 bytes = 1920 -> base64 length 2560 with padding.
    bool sizeChecked = false;
    for (const std::string& frame : s.fake->sent())
        if (frame.find("session.input_audio_buffer.append") != std::string::npos)
        {
            CHECK(Scenario::audioField(frame).size() == 2560);
            sizeChecked = true;
            break;
        }
    CHECK(sizeChecked);

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}

TEST_CASE("OpenAI backend: 48 kHz input is decimated to the 24 kHz wire", "[openai][audio][resampler]")
{
    Scenario s;
    auto req = s.Request();
    req.inputSampleRate = 48000;
    s.options.cadenceMs = 40; // chunk = 1920 input frames -> 960 wire frames
    REQUIRE(s.open(req).empty());

    const auto block = constantBlock(1920, 0.5f);
    std::string error;
    REQUIRE(s.backend->submitAudio(block.data(), 1920, error));

    // One of the appends must carry the decimated chunk: 960 PCM16 samples
    // whose steady tail sits at the DC level of 0.5f (16384, LE 00 40). The
    // first tick may still be a silence frame, so scan all appends.
    const bool found = Scenario::waitFor([&] {
        for (const std::string& frame : s.fake->sent())
        {
            if (frame.find("session.input_audio_buffer.append") == std::string::npos)
                continue;
            std::vector<std::uint8_t> decoded;
            if (!network::base64Decode(Scenario::audioField(frame), decoded))
                continue;
            if (decoded.size() != 1920)
                continue;
            bool tailOk = true;
            for (std::size_t i = decoded.size() - 800; i < decoded.size(); i += 2)
            {
                const std::int16_t v = static_cast<std::int16_t>(decoded[i] | (decoded[i + 1] << 8));
                if (v < 16284 || v > 16484)
                {
                    tailOk = false;
                    break;
                }
            }
            if (tailOk)
                return true;
        }
        return false;
    });
    INFO("no appended chunk carried the expected 24 kHz decimated tail");
    CHECK(found);

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}

// ------------------------------------------------------------------- deltas

TEST_CASE("OpenAI backend: translated audio delta is decoded, validated and delivered",
          "[openai][audio]")
{
    Scenario s;
    REQUIRE(s.openDefault());

    s.fake->queueMessage(audioDeltaEvent({ 1000, -1000, 32767, -32768 }));

    REQUIRE(Scenario::waitFor([&] { return !s.sink.audio().empty(); }));
    const auto block = s.sink.audio().front();
    CHECK(block.sampleRate == 24000);
    REQUIRE(block.samples.size() == 4);
    CHECK(block.samples[0] == Catch::Approx(1000.0f / 32768.0f));
    CHECK(block.samples[1] == Catch::Approx(-1000.0f / 32768.0f));
    CHECK(block.samples[3] == Catch::Approx(-1.0f));
    CHECK(s.sink.errors().empty());

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}

TEST_CASE("OpenAI backend: output rate conversion delivers at the requested rate",
          "[openai][audio][resampler]")
{
    Scenario s;
    auto req = s.Request();
    req.outputSampleRate = 48000;
    REQUIRE(s.open(req).empty());

    s.fake->queueMessage(audioDeltaEvent({ 8000, -8000, 8000, -8000 }));
    REQUIRE(Scenario::waitFor([&] { return !s.sink.audio().empty(); }));
    const auto block = s.sink.audio().front();
    CHECK(block.sampleRate == 48000);
    CHECK(block.samples.size() == 8); // 2:1 interpolation of the 4-sample delta

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}

TEST_CASE("OpenAI backend: delta metadata mismatch drops the block, keeps the session",
          "[openai][audio][faults]")
{
    Scenario s;
    REQUIRE(s.openDefault());

    // Protocol section 7: the optional fields are authoritative when present.
    s.fake->queueMessage(audioDeltaEvent({ 1, 2 }, R"(,"channels":2)"));
    s.fake->queueMessage(audioDeltaEvent({ 1, 2 }, R"(,"sample_rate":48000)"));
    s.fake->queueMessage(audioDeltaEvent({ 1, 2 }, R"(,"format":"pcm32")"));
    s.fake->queueMessage(R"({"type":"session.output_audio.delta","delta":"!!!not base64!!!"})");

    REQUIRE(Scenario::waitFor([&] { return s.sink.errors().size() >= 4; }));
    CHECK(s.sink.audio().empty());

    int audioFormat = 0, protocol = 0;
    for (const auto& e : s.sink.errors())
    {
        CHECK_FALSE(e.fatal); // a dropped block is an event, not a death sentence
        if (e.category == TranslationErrorCategory::audioFormat)
            ++audioFormat;
        if (e.category == TranslationErrorCategory::protocol)
            ++protocol;
    }
    CHECK(audioFormat == 3);
    CHECK(protocol == 1);
    CHECK(s.backend->state() == SessionState::connected);

    // A valid block after the invalid ones still flows: the session survived.
    s.fake->queueMessage(audioDeltaEvent({ 512, -512 }));
    REQUIRE(Scenario::waitFor([&] { return !s.sink.audio().empty(); }));

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}

TEST_CASE("OpenAI backend: transcript deltas append verbatim; no final events exist",
          "[openai][protocol][text]")
{
    Scenario s;
    REQUIRE(s.openDefault());

    // Section 8: fragments carry their own spacing; never add a space between
    // deltas. There are no *.done events in the protocol (section 6), so 009
    // maps everything to partials - final text is task 013's typed pipeline.
    s.fake->queueMessage(R"({"type":"session.output_transcript.delta","delta":"Привет"})");
    s.fake->queueMessage(R"({"type":"session.output_transcript.delta","delta":" миру"})");
    s.fake->queueMessage(R"({"type":"session.input_transcript.delta","delta":"Hello"})");

    REQUIRE(Scenario::waitFor([&] { return s.sink.joinedPartials().find("Привет") != std::string::npos; }));
    REQUIRE(Scenario::waitFor([&] { return s.sink.joinedPartials().size() == std::string("Привет миру").size(); }));
    CHECK(s.sink.joinedPartials() == "Привет миру");
    CHECK(s.sink.finalCount() == 0);

    // The source transcript has no sink channel before 013 and must not be
    // misdelivered as translated text (the "Hello" above must not appear).
    CHECK(s.sink.joinedPartials().find("Hello") == std::string::npos);

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}

// -------------------------------------------------------------------- errors

TEST_CASE("OpenAI backend: in-session error events map to categories and stay recoverable",
          "[openai][protocol][faults]")
{
    Scenario s;
    REQUIRE(s.openDefault());

    s.fake->queueMessage(
        R"({"type":"error","error":{"type":"invalid_request_error","message":"Unsupported audio format, expected PCM16"}})");
    s.fake->queueMessage(
        R"({"type":"error","error":{"type":"invalid_request_error","message":"Invalid language code"}})");
    s.fake->queueMessage(R"({"type":"error","error":{"type":"server_error","message":"Temporary failure"}})");
    s.fake->queueMessage(R"({"type":"no_such_event_from_the_reference"})");
    s.fake->queueMessage("{not json at all");

    REQUIRE(Scenario::waitFor([&] { return s.sink.errors().size() >= 5; }));
    const auto errors = s.sink.errors();

    CHECK(std::any_of(errors.begin(), errors.end(), [](const TranslationError& e) {
        return e.category == TranslationErrorCategory::audioFormat && !e.fatal;
    }));
    CHECK(std::any_of(errors.begin(), errors.end(), [](const TranslationError& e) {
        return e.category == TranslationErrorCategory::rejectedRequest && !e.fatal;
    }));
    CHECK(std::any_of(errors.begin(), errors.end(), [](const TranslationError& e) {
        return e.category == TranslationErrorCategory::internal && !e.fatal;
    }));
    CHECK(std::any_of(errors.begin(), errors.end(), [](const TranslationError& e) {
        return e.category == TranslationErrorCategory::protocol && !e.fatal; // unknown event + malformed
    }));

    // The provider's error-type names must not cross the sink seam (section 9).
    for (const auto& e : errors)
        CHECK(e.message.find("invalid_request_error") == std::string::npos);

    // Section 9: none of this killed the session.
    CHECK(s.backend->state() == SessionState::connected);
    std::string error;
    CHECK(s.backend->submitAudio(constantBlock(960, 0.25f).data(), 960, error));

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}

TEST_CASE("OpenAI backend: transport death mid-session is fatal -> faulted, reopenable",
          "[openai][faults]")
{
    Scenario s;
    REQUIRE(s.openDefault());

    network::ReceiveEvent dropped;
    dropped.kind = network::ReceiveKind::closed;
    dropped.closeReason = "keepalive ping timeout";
    s.fake->queueEvent(dropped);

    REQUIRE(Scenario::waitFor([&] { return s.backend->state() == SessionState::faulted; }));
    const auto errors = s.sink.errors();
    REQUIRE_FALSE(errors.empty());
    const auto& last = errors.back();
    CHECK(last.category == TranslationErrorCategory::connection);
    CHECK(last.fatal);
    CHECK(last.message.find("keepalive ping timeout") != std::string::npos); // peer reason kept

    std::string error;
    CHECK_FALSE(s.backend->submitAudio(constantBlock(10, 0.1f).data(), 10, error)); // faulted

    s.backend->closeSession(); // faulted -> closed is the contract's reset path
    auto states = s.sink.states();
    REQUIRE(states.size() == 4);
    CHECK(states[2] == SessionState::faulted);
    CHECK(states[3] == SessionState::closed);

    // Rule 6: a new session after the fault is unrelated and reaches connected.
    s.opened = false;
    REQUIRE(s.openDefault());
    states = s.sink.states();
    REQUIRE(states.size() == 6);
    CHECK(states[4] == SessionState::connecting);
    CHECK(states[5] == SessionState::connected);

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}

// ---------------------------------------------------------------- graceful close

TEST_CASE("OpenAI backend: closeSession sends session.close and drains the late deltas",
          "[openai][close]")
{
    Scenario s;
    REQUIRE(s.openDefault());

    // Pile the drain events behind the gate so the loop can only see them after
    // close was requested: this is the measured behavior (protocol section 15:
    // most late translated audio arrives after session.close).
    s.fake->gate(true);
    s.fake->queueMessage(audioDeltaEvent({ 700, -700 }));
    s.fake->queueMessage(audioDeltaEvent({ 800, -800 }));
    s.fake->queueMessage(closedEvent());

    // A helper thread releases the gate while closeSession() blocks in join,
    // mirroring a server that takes a moment to flush.
    std::thread releaser ([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds (80));
        s.fake->gate(false);
    });

    s.backend->closeSession();
    releaser.join();
    s.sink.armAfterCloseExpectation();

    // The close frame was actually sent.
    bool sawClose = false;
    for (const std::string& frame : s.fake->sent())
        if (frame.find(R"("type":"session.close")") != std::string::npos)
            sawClose = true;
    CHECK(sawClose);

    // Both drain deltas were delivered BEFORE the closed transition completed:
    // early socket drop would have lost them (rule 5 is load-bearing).
    CHECK(s.sink.audio().size() == 2);

    const auto states = s.sink.states();
    REQUIRE(states.size() == 3);
    CHECK(states[0] == SessionState::connecting);
    CHECK(states[1] == SessionState::connected);
    CHECK(states[2] == SessionState::closed);
    CHECK(s.sink.errors().empty());
    CHECK(s.backend->state() == SessionState::closed);

    // No sink callback may follow closeSession() returning (rule 5).
    std::this_thread::sleep_for(std::chrono::milliseconds (60));
    CHECK(s.sink.violations() == 0);
}

TEST_CASE("OpenAI backend: a stalled handshake is bounded and reported",
          "[openai][faults]")
{
    Scenario s;
    s.options.handshakeTimeoutMs = 250;
    s.handshakeScript.clear(); // the service says nothing after the upgrade
    s.backend = std::make_unique<network::OpenAIRealtimeBackend>(s.secrets, s.options,
                                                                 s.fakeFactory);
    s.backend->setSink(s.sink);

    std::string error;
    CHECK_FALSE(s.backend->openSession(s.Request(), error));
    CHECK(error.find("timed out") != std::string::npos);
    CHECK(s.backend->state() == SessionState::faulted);

    // The blocked receiver was released by the socket cancel (production
    // semantics mirrored by the fake): one connection-level fatal error, and
    // the real transitions reported once.
    const auto states = s.sink.states();
    REQUIRE(states.size() == 2);
    CHECK(states[0] == SessionState::connecting);
    CHECK(states[1] == SessionState::faulted);
    const auto errors = s.sink.errors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].category == TranslationErrorCategory::connection);
    CHECK(errors[0].fatal);

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
    CHECK(s.sink.states().size() == 3);
    CHECK(s.sink.states()[2] == SessionState::closed);
}

TEST_CASE("OpenAI backend: drain deadline bounds closeSession and reports honestly",
          "[openai][close][faults]")
{
    Scenario s;
    REQUIRE(s.openDefault());

    // Nothing is scripted: session.closed never arrives. closeSession() must
    // still return (bounded), tell the sink a protocol timeout happened
    // non-fatally, and end the session in closed.
    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();

    const auto errors = s.sink.errors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].category == TranslationErrorCategory::protocol);
    CHECK_FALSE(errors[0].fatal);

    const auto states = s.sink.states();
    REQUIRE(states.size() == 3);
    CHECK(states[2] == SessionState::closed);
    CHECK(s.backend->state() == SessionState::closed);
    CHECK(s.sink.violations() == 0);
}

TEST_CASE("OpenAI backend: server-initiated session.closed ends the session without an error",
          "[openai][close]")
{
    Scenario s;
    REQUIRE(s.openDefault());
    s.fake->queueMessage(closedEvent());

    REQUIRE(Scenario::waitFor([&] { return s.backend->state() == SessionState::closed; }));
    CHECK(s.sink.errors().empty()); // a documented lifecycle event is not a fault

    std::string error;
    CHECK_FALSE(s.backend->submitAudio(constantBlock(10, 0.0f).data(), 10, error));

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
    const auto states = s.sink.states();
    size_t closedCount = 0;
    for (const SessionState st : states)
        if (st == SessionState::closed)
            ++closedCount;
    CHECK(closedCount == 1);
}

// ---------------------------------------------------------------- instructions

TEST_CASE("OpenAI backend: instructions are ignored with a warning, never faked",
          "[openai][protocol][honesty]")
{
    Scenario s;
    auto req = s.Request();
    req.instructions = "use glossary X and a calm voice"; // the model cannot do this
    REQUIRE(s.open(req).empty());

    CHECK(s.backend->state() == SessionState::connected); // accepted...

    REQUIRE(s.sink.errors().size() == 1); // ...and reported as not applied
    CHECK(s.sink.errors()[0].category == TranslationErrorCategory::rejectedRequest);
    CHECK_FALSE(s.sink.errors()[0].fatal);
    CHECK(s.sink.errors()[0].message.find("instructions") != std::string::npos);

    bool sawUpdate = false;
    for (const std::string& frame : s.fake->sent())
    {
        if (frame.find("session.update") != std::string::npos)
        {
            sawUpdate = true;
            CHECK(frame.find("glossary") == std::string::npos); // nothing invented on the wire
        }
    }
    CHECK(sawUpdate);

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}

TEST_CASE("OpenAI backend: send failure while streaming faults the session", "[openai][faults]")
{
    Scenario s;
    REQUIRE(s.openDefault());

    s.fake->setFailSends(true);

    REQUIRE(Scenario::waitFor([&] { return s.backend->state() == SessionState::faulted; }));
    const auto errors = s.sink.errors();
    CHECK(errors.back().category == TranslationErrorCategory::connection);
    CHECK(errors.back().fatal);

    s.backend->closeSession();
    s.sink.armAfterCloseExpectation();
}
