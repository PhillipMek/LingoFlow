#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

#include "Audio/AudioEngine.h"
#include "Audio/Null/NullAudioBackend.h"
#include "Diagnostics/DiagnosticsManager.h"

using namespace liveai;
using liveai::audio::BackendState;
using liveai::audio::DeviceCapabilities;
using liveai::audio::DeviceRequest;
using liveai::audio::IAudioBackend;
using liveai::audio::IAudioProcessor;
using liveai::audio::NullAudioBackend;

namespace {

constexpr int kRate = 48000;
constexpr int kFrames = 480;

DeviceCapabilities mono48k()
{
    DeviceCapabilities caps;
    caps.sampleRate = kRate;
    caps.preferredBufferFrames = kFrames;
    caps.inputChannels = 1;
    caps.outputChannels = 1;
    return caps;
}

/// The request used by these tests: 48 kHz, 10 ms, mono in/out (SPEC "Audio").
DeviceRequest request()
{
    DeviceRequest request;
    request.sampleRate = kRate;
    request.bufferFrames = kFrames;
    request.inputChannel = 1;
    request.outputChannel = 1;
    return request;
}

} // namespace

TEST_CASE("AudioEngine: activate opens and starts the backend", "[audio][engine]")
{
    DiagnosticsManager diagnostics;
    AudioEngine engine(&diagnostics);
    auto backend = std::make_unique<NullAudioBackend>(mono48k());
    auto* backendRef = backend.get();

    std::string error;
    REQUIRE(engine.activate(*backendRef, request(), error));
    CHECK(error.empty());

    CHECK(backendRef->state() == BackendState::running);
    CHECK(engine.backend() == backendRef);
    CHECK(engine.sampleRate() == kRate);
    CHECK(engine.bufferFrames() == kFrames);

    auto snapshot = diagnostics.snapshot();
    CHECK(snapshot.audioBackend == "Null");
    CHECK(snapshot.sampleRate == kRate);

    engine.deactivate();
    CHECK(backendRef->state() == BackendState::closed);
    CHECK(engine.backend() == nullptr);
    CHECK(diagnostics.snapshot().audioBackend.empty());
}

TEST_CASE("AudioEngine: a second activate without deactivating is refused", "[audio][engine]")
{
    AudioEngine engine;
    NullAudioBackend first(mono48k());
    NullAudioBackend second(mono48k());

    std::string error;
    CHECK(engine.activate(first, request(), error));
    CHECK_FALSE(engine.activate(second, request(), error));
    CHECK(error.find("already activated") != std::string::npos);

    engine.deactivate();
    CHECK(engine.activate(second, request(), error));
}

TEST_CASE("AudioEngine: callback produces silence, never the input signal", "[audio][engine][realtime]")
{
    // Until translated audio exists (task 012) the engine must not pass the
    // microphone through to the audience.
    AudioEngine engine;
    NullAudioBackend backend(mono48k());

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    backend.fillInputWith(0.75f);
    REQUIRE(backend.renderOneBlock());

    const auto& out = backend.lastOutputBlock();
    REQUIRE(static_cast<int>(out.size()) == kFrames);

    for (int i = 0; i < kFrames; ++i)
        REQUIRE(out[static_cast<std::size_t>(i)] == 0.0f);

    CHECK(engine.blockCount() == 1);
    CHECK(engine.frameCount() == static_cast<std::uint64_t>(kFrames));

    engine.deactivate();
}

TEST_CASE("AudioEngine: malformed and empty callbacks are not counted as blocks", "[audio][engine]")
{
    AudioEngine engine;
    NullAudioBackend backend(mono48k());

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    // The contract says every channel pointer refers to frameCount frames, so the
    // buffers here are honestly sized: a one-float array with frameCount 480 would
    // be a test lying about its own buffer, and would hide real overflow bugs.
    std::array<float, static_cast<std::size_t>(kFrames)> sample{};
    const float* in[] = { sample.data() };
    float* out[] = { sample.data() };

    engine.processAudio(in, out, 0);                 // empty block: nothing to do
    CHECK(engine.blockCount() == 0);
    CHECK(engine.malformedCallbacks() == 0);

    engine.processAudio(nullptr, out, kFrames);      // backend broke the contract
    CHECK(engine.blockCount() == 0);
    CHECK(engine.malformedCallbacks() == 1);
    CHECK(sample[0] == 0.0f);                        // silence, never stale memory

    engine.processAudio(in, nullptr, kFrames);       // also a contract violation
    CHECK(engine.blockCount() == 0);
    CHECK(engine.malformedCallbacks() == 2);

    engine.processAudio(in, out, kFrames);           // one correct callback
    CHECK(engine.blockCount() == 1);
    CHECK(engine.frameCount() == static_cast<std::uint64_t>(kFrames));
    CHECK(engine.malformedCallbacks() == 2);

    engine.deactivate();
}

TEST_CASE("AudioEngine: a malformed callback silences EVERY output channel",
          "[audio][engine][realtime]")
{
    // Code review P0 (2026-10-05): device output buffers start undefined, and
    // JUCE requires the callback to fill every channel. The old defensive paths
    // cleared only channel 0 - on a multi-output geometry the audience would
    // have kept hearing stale bytes (or uninitialised memory) on channel 1,
    // exactly when the system was already broken. Poison both outputs and
    // demand all of them back silent.
    DeviceCapabilities caps = mono48k();
    caps.outputChannels = 2;

    AudioEngine engine;
    NullAudioBackend backend(caps);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    REQUIRE(engine.outputChannels() == 2);

    std::array<float, static_cast<std::size_t>(kFrames)> buf0{};
    std::array<float, static_cast<std::size_t>(kFrames)> buf1{};
    buf0.fill(0.25f);   // stale show audio
    buf1.fill(0.5f);    // channel 1: the one the old code left untouched

    float* out[] = { buf0.data(), buf1.data() };

    engine.processAudio(nullptr, out, kFrames);   // backend broke the input promise

    CHECK(engine.malformedCallbacks() == 1);
    CHECK(engine.blockCount() == 0);

    for (int i = 0; i < kFrames; ++i)
    {
        REQUIRE(buf0[static_cast<std::size_t>(i)] == 0.0f);
        REQUIRE(buf1[static_cast<std::size_t>(i)] == 0.0f);   // the regression assertion
    }

    engine.deactivate();
}

TEST_CASE("AudioEngine: null entries among outputs are skipped, the rest go silent",
          "[audio][engine][realtime]")
{
    // A hole the driver hands us has no buffer to dirty, so silence must reach
    // every OTHER promised channel, and the null one must not be written or
    // crash: defensive paths cannot afford to fail harder than the failure.
    DeviceCapabilities caps = mono48k();
    caps.outputChannels = 3;

    AudioEngine engine;
    NullAudioBackend backend(caps);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    std::array<float, static_cast<std::size_t>(kFrames)> buf0{};
    std::array<float, static_cast<std::size_t>(kFrames)> buf2{};
    buf0.fill(0.25f);
    buf2.fill(0.5f);

    float* out[] = { buf0.data(), nullptr, buf2.data() };

    engine.processAudio(nullptr, out, kFrames);

    CHECK(engine.malformedCallbacks() == 1);

    for (int i = 0; i < kFrames; ++i)
    {
        REQUIRE(buf0[static_cast<std::size_t>(i)] == 0.0f);
        REQUIRE(buf2[static_cast<std::size_t>(i)] == 0.0f);
    }

    engine.deactivate();
}

TEST_CASE("AudioEngine: a null among promised inputs is a malformed callback, not a skipped channel",
          "[audio][engine][realtime]")
{
    // Code review P2 (2026-10-05): the old input loop did `if (source ==
    // nullptr) continue;` - a selected channel that vanished mid-show kept
    // the block counters looking healthy while the mix quietly lost it, and
    // partial audio is indistinguishable from quiet audio at the audience
    // end. The strict rule: refuse the whole block - one count, silence on
    // every writable output, nothing from a lying block reaches the rings.
    DeviceCapabilities caps = mono48k();
    caps.inputChannels = 2;

    AudioEngine engine;
    NullAudioBackend backend(caps);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    REQUIRE(engine.inputChannels() == 2);

    std::array<float, static_cast<std::size_t>(kFrames)> inBuf{};
    std::array<float, static_cast<std::size_t>(kFrames)> outBuf{};
    inBuf.fill(0.5f);
    outBuf.fill(0.25f);   // stale show audio

    const float* in[] = { inBuf.data(), nullptr };  // channel 1 has vanished
    float* out[] = { outBuf.data() };

    engine.processAudio(in, out, kFrames);

    CHECK(engine.malformedCallbacks() == 1);
    CHECK(engine.blockCount() == 0);
    CHECK(engine.inputFramesCaptured() == 0);   // the surviving channel is NOT mixed alone
    CHECK(engine.frameCount() == 0);            // and the block is not in the processed total

    for (int i = 0; i < kFrames; ++i)
        REQUIRE(outBuf[static_cast<std::size_t>(i)] == 0.0f);

    // Strict per block, not a latched fault: the next healthy block is
    // processed and counted normally.
    const float* inOk[] = { inBuf.data(), inBuf.data() };
    outBuf.fill(0.25f);
    engine.processAudio(inOk, out, kFrames);
    CHECK(engine.blockCount() == 1);
    CHECK(engine.malformedCallbacks() == 1);

    engine.deactivate();
}

TEST_CASE("AudioEngine: a null among promised outputs refuses the whole block",
          "[audio][engine][realtime]")
{
    // The output mirror of the same decision: a promised destination that
    // vanished means we can no longer honour the fill obligation on that
    // channel, so no audio goes out at all - the writable siblings get the
    // silence we still owe them, and the counters tell the operator that the
    // callback was malformed rather than that the show is quiet tonight.
    DeviceCapabilities caps = mono48k();
    caps.outputChannels = 2;

    AudioEngine engine;
    NullAudioBackend backend(caps);

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));
    REQUIRE(engine.outputChannels() == 2);

    std::array<float, static_cast<std::size_t>(kFrames)> inBuf{};
    std::array<float, static_cast<std::size_t>(kFrames)> outBuf0{};
    inBuf.fill(0.5f);
    outBuf0.fill(0.25f);

    const float* in[] = { inBuf.data() };
    float* out[] = { outBuf0.data(), nullptr };

    engine.processAudio(in, out, kFrames);

    CHECK(engine.malformedCallbacks() == 1);
    CHECK(engine.blockCount() == 0);

    for (int i = 0; i < kFrames; ++i)
        REQUIRE(outBuf0[static_cast<std::size_t>(i)] == 0.0f);

    engine.deactivate();
}

TEST_CASE("AudioEngine: null diagnostics pointer is tolerated", "[audio][engine]")
{
    AudioEngine engine;   // no DiagnosticsManager
    NullAudioBackend backend(mono48k());

    std::string error;
    CHECK(engine.activate(backend, request(), error));
    CHECK(backend.renderOneBlock());
    engine.deactivate();
}

TEST_CASE("AudioEngine: a backend that cannot start is closed again", "[audio][engine][faults]")
{
    struct UnstartableBackend final : IAudioBackend
    {
        std::string_view name() const noexcept override { return "Unstartable"; }
        BackendState state() const noexcept override { return state_; }
        bool open(IAudioProcessor&, const DeviceRequest&, std::string& error) override
        {
            state_ = BackendState::opened;
            error.clear();
            return true;
        }
        bool start(std::string& error) override
        {
            error = "device busy";
            state_ = BackendState::faulted;
            return false;
        }
        bool stop(std::string&) override { state_ = BackendState::closed; return true; }
        void close() noexcept override { state_ = BackendState::closed; }
        DeviceCapabilities capabilities() const noexcept override { return mono48k(); }

        BackendState state_ = BackendState::closed;
    };

    AudioEngine engine;
    UnstartableBackend backend;
    std::string error;

    CHECK_FALSE(engine.activate(backend, request(), error));
    CHECK(error == "device busy");
    CHECK(engine.backend() == nullptr);
    // The failed start must not leave the device open.
    CHECK(backend.state() == BackendState::closed);
}
