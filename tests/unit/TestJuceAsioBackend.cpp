//
// State-machine regression for the real ASIO adapter. The device itself is
// never opened: an impossible device name fails deterministically on any
// machine (with or without an ASIO driver), which is exactly the path that
// used to leave the backend faulted and refuse a second open().
//

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Audio/IAudioBackend.h"
#include "Platform/Asio/JuceAsioBackend.h"

namespace {

using liveai::audio::BackendState;
using liveai::audio::DeviceRequest;
using liveai::audio::IAudioProcessor;
using liveai::platform::JuceAsioBackend;

class SilentProcessor final : public IAudioProcessor
{
public:
    void processAudio(const float* const*, float* const*, int) noexcept override {}
    void onAudioConfigurationChanged(int, int) override {}
};

DeviceRequest impossibleRequest()
{
    DeviceRequest request;
    request.deviceId = "__lingoflow_no_such_asio_device__";
    return request;
}

} // namespace

TEST_CASE("JuceAsioBackend: a failed open leaves a retryable backend", "[asio][backend]")
{
    const DeviceRequest request = impossibleRequest();
    SilentProcessor processor;
    JuceAsioBackend backend(request.deviceId);

    std::string firstError;
    CHECK_FALSE(backend.open(processor, request, firstError));
    CHECK(backend.state() == BackendState::faulted);
    REQUIRE_FALSE(firstError.empty());

    std::string secondError;
    CHECK_FALSE(backend.open(processor, request, secondError));
    CHECK(backend.state() == BackendState::faulted);
    CHECK(secondError == firstError); // retried the open, not "already open"
    CHECK(secondError.find("already open") == std::string::npos);
}
