#include "Platform/Asio/JuceAsioBackend.h"

#include <algorithm>
#include <vector>

#include "Audio/Asio/AsioDeviceInfo.h"
#include "Platform/Asio/JuceAsioCommon.h"
#include "Utils/Log.h"

namespace liveai {
namespace platform {
namespace {

constexpr std::string_view kComponent = "asio";

using asio_detail::channelMask;
using asio_detail::createAsioTypeOrNull;
using asio_detail::toStdBuffers;
using asio_detail::toStdNames;
using asio_detail::toStdRates;

} // namespace

/// Forwards the ASIO callback into the engine. This class is the realtime path.
class JuceAsioBackend::Callback final : public juce::AudioIODeviceCallback
{
public:
    Callback(audio::IAudioProcessor& processor, std::atomic<std::uint64_t>& blocks)
        : processor_(processor), blocks_(blocks)
    {
    }

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                         float* const* outputChannelData, int numOutputChannels,
                                         int numSamples,
                                         const juce::AudioIODeviceCallbackContext&) override
    {
        if (numSamples <= 0 || inputChannelData == nullptr || outputChannelData == nullptr)
            return;

        // Fixed-size forwarding: views were sized in audioDeviceAboutToStart.
        for (int channel = 0; channel < inputCount_; ++channel)
            inputViews_[static_cast<std::size_t>(channel)] = channel < numInputChannels
                ? inputChannelData[channel] : nullptr;

        for (int channel = 0; channel < outputCount_; ++channel)
            outputViews_[static_cast<std::size_t>(channel)] = channel < numOutputChannels
                ? outputChannelData[channel] : nullptr;

        processor_.processAudio(inputViews_.data(), outputViews_.data(), numSamples);
        blocks_.fetch_add(1, std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        // Control thread: the only place allowed to allocate.
        inputCount_ = std::max(1, device != nullptr ? device->getActiveInputChannels().countNumberOfSetBits() : 1);
        outputCount_ = std::max(1, device != nullptr ? device->getActiveOutputChannels().countNumberOfSetBits() : 1);
        inputViews_.assign(static_cast<std::size_t>(inputCount_), nullptr);
        outputViews_.assign(static_cast<std::size_t>(outputCount_), nullptr);
    }

    void audioDeviceStopped() override {}

    /// May be called from the driver thread, so nothing may be allocated here:
    /// only a flag is set, the message itself is read on a control thread through
    /// juce::AudioIODevice::getLastError().
    void audioDeviceError(const juce::String&) override { errorFlag_.store(true, std::memory_order_relaxed); }

    bool sawError() const noexcept { return errorFlag_.load(std::memory_order_relaxed); }

private:
    audio::IAudioProcessor& processor_;
    std::atomic<std::uint64_t>& blocks_;
    int inputCount_ = 1;
    int outputCount_ = 1;
    std::vector<const float*> inputViews_;
    std::vector<float*> outputViews_;
    std::atomic<bool> errorFlag_{ false };
};

JuceAsioBackend::JuceAsioBackend(std::string deviceId)
    : deviceId_(std::move(deviceId))
    , displayName_("ASIO/" + deviceId_)
{
}

JuceAsioBackend::~JuceAsioBackend()
{
    close();
}

bool JuceAsioBackend::open(audio::IAudioProcessor& processor, const audio::DeviceRequest& request, std::string& error)
{
    error.clear();

    if (state_ != audio::BackendState::closed)
    {
        error = "ASIO backend is already open";
        return false;
    }

    // The request carries the operator's device choice, so a backend built by the
    // composition-root factory is told what to open. The constructor value stays as
    // the default for tools that build one backend per device (the probe).
    if (!request.deviceId.empty() && request.deviceId != deviceId_)
    {
        deviceId_ = request.deviceId;
        displayName_ = "ASIO/" + deviceId_;
    }

    if (deviceId_.empty())
    {
        error = "no ASIO device selected";
        return false;
    }

    processor_ = &processor;

    type_.reset(createAsioTypeOrNull());
    if (type_ == nullptr)
    {
        error = "this build has no ASIO support (JUCE_ASIO=0 or no ASIO host)";
        state_ = audio::BackendState::faulted;
        processor_ = nullptr;
        log::error(kComponent, "open '" + deviceId_ + "' failed: " + error);
        return false;
    }

    type_->scanForDevices();
    const juce::String name(deviceId_);

    if (!type_->getDeviceNames(false).contains(name))
    {
        error = "'" + deviceId_ + "' is not a registered ASIO device";
        state_ = audio::BackendState::faulted;
        processor_ = nullptr;
        type_.reset();
        log::error(kComponent, "open failed: " + error);
        return false;
    }

    device_.reset(type_->createDevice(name, name));
    if (device_ == nullptr)
    {
        error = "could not instantiate ASIO device '" + deviceId_ + "'";
        state_ = audio::BackendState::faulted;
        processor_ = nullptr;
        type_.reset();
        log::error(kComponent, "open failed: " + error);
        return false;
    }

    const std::vector<double> rates = toStdRates(device_->getAvailableSampleRates());
    const std::vector<int> buffers = toStdBuffers(device_->getAvailableBufferSizes());
    const std::vector<std::string> inputNames = toStdNames(device_->getInputChannelNames());
    const std::vector<std::string> outputNames = toStdNames(device_->getOutputChannelNames());

    const auto selection = asio::selectConfiguration(rates, buffers,
                                                     static_cast<double>(request.sampleRate),
                                                     request.bufferFrames);
    for (const auto& note : selection.notes)
        log::info(kComponent, "'" + deviceId_ + "' " + note);

    asio::DeviceCapabilitiesReport report;
    report.name = deviceId_;
    report.inputChannels = inputNames;
    report.outputChannels = outputNames;
    report.opened = true;   // channel lists are known, so validate against them

    if (!asio::validateChannelSelection(report, request.inputChannel, request.outputChannel, error))
    {
        state_ = audio::BackendState::faulted;
        processor_ = nullptr;
        device_.reset();
        type_.reset();
        log::error(kComponent, "open '" + deviceId_ + "' failed: " + error);
        return false;
    }

    const double rate = selection.sampleRate.value_or(static_cast<double>(request.sampleRate));
    const int bufferSize = selection.bufferFrames.value_or(request.bufferFrames);

    const juce::String openError = device_->open(channelMask(request.inputChannel),
                                                 channelMask(request.outputChannel),
                                                 rate,
                                                 bufferSize);

    if (openError.isNotEmpty())
    {
        error = openError.toStdString();
        state_ = audio::BackendState::faulted;
        processor_ = nullptr;
        device_->close();
        device_.reset();
        type_.reset();
        log::error(kComponent, "open '" + deviceId_ + "' failed: " + error);
        return false;
    }

    capabilities_ = {};
    capabilities_.sampleRate = static_cast<int>(device_->getCurrentSampleRate());
    capabilities_.preferredBufferFrames = device_->getCurrentBufferSizeSamples();
    capabilities_.minBufferFrames = buffers.empty() ? capabilities_.preferredBufferFrames : buffers.front();
    capabilities_.maxBufferFrames = buffers.empty() ? capabilities_.preferredBufferFrames : buffers.back();
    capabilities_.inputChannels = std::max(1, device_->getActiveInputChannels().countNumberOfSetBits());
    capabilities_.outputChannels = std::max(1, device_->getActiveOutputChannels().countNumberOfSetBits());
    capabilities_.preferredFormat = audio::SampleFormat::float32;

    inputLatency_ = device_->getInputLatencyInSamples();
    outputLatency_ = device_->getOutputLatencyInSamples();

    // Callback is a private nested type, so it is constructed here rather than
    // through std::make_unique outside the class.
    callback_.reset(new Callback(*processor_, callbackBlocks_));

    state_ = audio::BackendState::opened;
    log::info(kComponent, "opened '" + deviceId_ + "' rate=" + std::to_string(capabilities_.sampleRate)
                              + " buffer=" + std::to_string(capabilities_.preferredBufferFrames)
                              + " in=" + std::to_string(capabilities_.inputChannels)
                              + " out=" + std::to_string(capabilities_.outputChannels)
                              + " latency(in/out)=" + std::to_string(inputLatency_) + "/" + std::to_string(outputLatency_)
                              + " format=" + std::string(audio::nameOf(capabilities_.preferredFormat)));
    return true;
}

bool JuceAsioBackend::start(std::string& error)
{
    error.clear();

    if (state_ != audio::BackendState::opened || device_ == nullptr || callback_ == nullptr)
    {
        error = "ASIO backend must be opened before start";
        return false;
    }

    callbackBlocks_.store(0, std::memory_order_relaxed);
    xrunCount_.store(0, std::memory_order_relaxed);

    device_->start(callback_.get());
    state_ = audio::BackendState::running;

    log::info(kComponent, "started '" + deviceId_ + "'");
    return true;
}

bool JuceAsioBackend::stop(std::string& error)
{
    error.clear();

    if (state_ != audio::BackendState::running || device_ == nullptr)
        return true;   // stopping a stopped device is not an error

    device_->stop();

    xrunCount_.store(device_->getXRunCount(), std::memory_order_relaxed);
    state_ = audio::BackendState::opened;

    const std::string driverError = (callback_ != nullptr && callback_->sawError())
                                        ? device_->getLastError().toStdString()
                                        : std::string();
    log::info(kComponent, "stopped '" + deviceId_ + "' callbacks=" + std::to_string(callbackBlocks_.load())
                              + " xruns=" + asio::describeXRunCount(xrunCount_.load()));

    if (!driverError.empty())
        log::error(kComponent, "driver reported: " + driverError);

    return true;
}

void JuceAsioBackend::close() noexcept
{
    if (device_ != nullptr)
    {
        if (state_ == audio::BackendState::running)
            device_->stop();
        device_->close();
    }

    callback_.reset();
    device_.reset();
    type_.reset();
    processor_ = nullptr;
    capabilities_ = {};
    inputLatency_ = 0;
    outputLatency_ = 0;
    state_ = audio::BackendState::closed;
}

} // namespace platform
} // namespace liveai
