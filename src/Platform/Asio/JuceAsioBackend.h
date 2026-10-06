#pragma once
//
// JuceAsioBackend - implements audio::IAudioBackend on top of a real ASIO device.
//
// Boundary: the engine sees IAudioBackend only; everything JUCE and
// driver specific stops in this file. The backend knows nothing about translation,
// NDI or the UI.
//
// Realtime contract: audioDeviceIOCallbackWithContext forwards the driver's channel
// pointers into IAudioProcessor::processAudio through the tested shape decision in
// Audio/Asio/AsioChannelForwarding.h, applied against the physical channel map built
// in audioDeviceAboutToStart. It allocates nothing, locks nothing, logs nothing and
// touches no other subsystem. The only allocations happen in audioDeviceAboutToStart,
// which JUCE calls on the control thread before the first block.
//
// open()/start()/stop()/close() run on non-realtime threads only.

#include <atomic>
#include <memory>
#include <string>

#include "Audio/IAudioBackend.h"

namespace juce { class AudioIODevice; class AudioIODeviceType; }

namespace liveai {
namespace platform {

class JuceAsioBackend final : public audio::IAudioBackend
{
public:
    /// deviceId is the ASIO device name as reported by AsioDiscovery::scanAsioDevices().
    explicit JuceAsioBackend(std::string deviceId);
    ~JuceAsioBackend() override;

    std::string_view name() const noexcept override { return displayName_; }
    audio::BackendState state() const noexcept override { return state_; }

    bool open(audio::IAudioProcessor& processor, const audio::DeviceRequest& request, std::string& error) override;
    bool start(std::string& error) override;
    bool stop(std::string& error) override;
    void close() noexcept override;
    audio::DeviceCapabilities capabilities() const noexcept override { return capabilities_; }

    /// Callback blocks delivered since start() (relaxed atomic, safe to read anywhere).
    std::uint64_t callbackBlocks() const noexcept { return callbackBlocks_.load(std::memory_order_relaxed); }

    /// Driver-reported xruns captured at the last stop(). -1 means the driver does
    /// not expose a counter at all (asio::describeXRunCount renders that honestly).
    int lastXRunCount() const noexcept { return xrunCount_.load(std::memory_order_relaxed); }

    /// Latencies the driver reported for the active configuration. 0 until open().
    int inputLatencySamples() const noexcept { return inputLatency_; }
    int outputLatencySamples() const noexcept { return outputLatency_; }

private:
    class Callback;

    std::string deviceId_;
    std::string displayName_;
    audio::BackendState state_ = audio::BackendState::closed;
    audio::IAudioProcessor* processor_ = nullptr;
    audio::DeviceCapabilities capabilities_;
    std::unique_ptr<juce::AudioIODeviceType> type_;
    std::unique_ptr<juce::AudioIODevice> device_;
    std::unique_ptr<Callback> callback_;
    std::atomic<std::uint64_t> callbackBlocks_{ 0 };
    std::atomic<int> xrunCount_{ 0 };
    int inputLatency_ = 0;
    int outputLatency_ = 0;
};

} // namespace platform
} // namespace liveai
