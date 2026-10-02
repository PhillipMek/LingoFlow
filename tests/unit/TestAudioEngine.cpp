#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("AudioEngine: blocks with no frames are ignored", "[audio][engine]")
{
    AudioEngine engine;
    NullAudioBackend backend(mono48k());

    std::string error;
    REQUIRE(engine.activate(backend, request(), error));

    float sample = 0.0f;
    const float* in[] = { &sample };
    float* out[] = { &sample };

    engine.processAudio(in, out, 0);
    engine.processAudio(nullptr, out, kFrames);
    engine.processAudio(in, nullptr, kFrames);

    CHECK(engine.blockCount() == 0);

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
