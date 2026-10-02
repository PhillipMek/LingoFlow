#pragma once
//
// Null audio backend: a device-shaped no-op used by tests and by the architecture
// skeleton. It owns preallocated mono buffers and can be driven manually, so the
// realtime path can be exercised without ASIO hardware.
//
// NOT for production use as a substitute for a real device check: the ASIO
// backend is task 004/005 and requires a human SoundGrid verification.

#include <string>
#include <vector>

#include "Audio/IAudioBackend.h"

namespace liveai {
namespace audio {

class NullAudioBackend final : public IAudioBackend
{
public:
    explicit NullAudioBackend(DeviceCapabilities capabilities = {});

    std::string_view name() const noexcept override { return "Null"; }
    BackendState state() const noexcept override { return state_; }

    bool open(IAudioProcessor& processor, int sampleRate, int bufferFrames, std::string& error) override;
    bool start(std::string& error) override;
    bool stop(std::string& error) override;
    void close() noexcept override;
    DeviceCapabilities capabilities() const noexcept override { return capabilities_; }

    /// Non-realtime helper for tests and mock mode: calls the processor once with
    /// silent input/output blocks of the configured size. Returns false when the
    /// backend is not running.
    bool renderOneBlock();

    /// Input signal used by renderOneBlock(): 0.0 keeps the engine silent path,
    /// any other value proves the engine did not copy input to output.
    void fillInputWith(float value) noexcept;

    /// Last block handed to the processor by renderOneBlock(), for assertions.
    const std::vector<float>& lastOutputBlock() const noexcept { return output_; }

private:
    DeviceCapabilities capabilities_;
    BackendState state_ = BackendState::closed;
    IAudioProcessor* processor_ = nullptr;
    std::vector<float> input_;
    std::vector<float> output_;
    std::vector<const float*> inputPointers_;
    std::vector<float*> outputPointers_;
};

} // namespace audio
} // namespace liveai
