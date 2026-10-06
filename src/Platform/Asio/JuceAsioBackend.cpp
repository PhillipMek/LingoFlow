#include "Platform/Asio/JuceAsioBackend.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "Audio/Asio/AsioChannelForwarding.h"
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

/// The zero-based physical positions of the enabled bits, ascending - the same
/// walk JUCE's own ASIO wrapper uses to build its buffer table, kept here so
/// the callback can map driver entries by position when the driver indexes
/// physically. Runs on the control thread only (audioDeviceAboutToStart).
void activeChannelPositions(const juce::BigInteger& mask, std::vector<int>& out)
{
    out.clear();
    for (int bit = 0; bit <= mask.getHighestBit(); ++bit)
        if (mask[bit])
            out.push_back(bit);
}

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
        if (numSamples <= 0 || outputChannelData == nullptr)
            return;   // no frames exist, or no wire exists: there is nothing to fill

        if (inputChannelData == nullptr)
        {
            // A driver that breaks its own input promise must still not hear the
            // previous block played back as if audio were flowing - and it must
            // not vanish from the record either. Code review P2 (2026-10-05):
            // the engine is still reached, with the violation instead of audio;
            // processAudio(nullptr, ...) owns the malformed count and the
            // every-output fill (IAudioProcessor contract), so this shows up on
            // the operator's band and in the export rather than only as the
            // absence of evidence. Vendored JUCE hands out non-null arrays (it
            // jasserts), which makes this a defensive path, not an expected one.
            asio::forwardActiveChannels(outputChannelData, numOutputChannels,
                                        outputPhysical_, outputViews_);
            processor_.processAudio(nullptr, outputViews_.data(), numSamples);
            return;
        }

        // Fixed-size forwarding: the views and the physical maps were built in
        // audioDeviceAboutToStart, so per block the callback only applies the
        // tested shape decision (Audio/Asio/AsioChannelForwarding.h). The shape
        // flags feed nothing but the evidence line at stop: relaxed stores.
        const auto inShape = asio::forwardActiveChannels(inputChannelData, numInputChannels,
                                                         inputPhysical_, inputViews_);
        const auto outShape = asio::forwardActiveChannels(outputChannelData, numOutputChannels,
                                                          outputPhysical_, outputViews_);

        if (inShape == asio::ChannelArrayShape::physicallyIndexed || outShape == asio::ChannelArrayShape::physicallyIndexed)
            sawPhysicalArrays_.store(true, std::memory_order_relaxed);
        if (inShape == asio::ChannelArrayShape::truncated || outShape == asio::ChannelArrayShape::truncated)
            sawTruncatedArrays_.store(true, std::memory_order_relaxed);

        processor_.processAudio(inputViews_.data(), outputViews_.data(), numSamples);
        blocks_.fetch_add(1, std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        // Control thread: the only place allowed to allocate.
        inputPhysical_.clear();
        outputPhysical_.clear();
        if (device != nullptr)
        {
            activeChannelPositions(device->getActiveInputChannels(), inputPhysical_);
            activeChannelPositions(device->getActiveOutputChannels(), outputPhysical_);
        }

        // The engine geometry this class serves guarantees at least one channel
        // per direction (capabilities_ uses max(1, ...)), and open() has already
        // rejected a selection outside the device's channel names, so a mask with
        // no bits set cannot legitimately reach here. The fallback keeps the view
        // count matching what the engine allocated; a driver that delivers nothing
        // then arrives as null, which the engine reads as "no data".
        if (inputPhysical_.empty())
            inputPhysical_.push_back(0);
        if (outputPhysical_.empty())
            outputPhysical_.push_back(0);

        inputViews_.assign(inputPhysical_.size(), nullptr);
        outputViews_.assign(outputPhysical_.size(), nullptr);
    }

    void audioDeviceStopped() override {}

    /// May be called from the driver thread, so nothing may be allocated here:
    /// only a flag is set, the message itself is read on a control thread through
    /// juce::AudioIODevice::getLastError().
    void audioDeviceError(const juce::String&) override { errorFlag_.store(true, std::memory_order_relaxed); }

    bool sawError() const noexcept { return errorFlag_.load(std::memory_order_relaxed); }

    /// Which channel-array layouts the driver actually used since the last
    /// resetArrayEvidence(): read on a control thread for the stop log line.
    bool sawPhysicalArrays() const noexcept { return sawPhysicalArrays_.load(std::memory_order_relaxed); }
    bool sawTruncatedArrays() const noexcept { return sawTruncatedArrays_.load(std::memory_order_relaxed); }
    void resetArrayEvidence() noexcept
    {
        sawPhysicalArrays_.store(false, std::memory_order_relaxed);
        sawTruncatedArrays_.store(false, std::memory_order_relaxed);
    }

private:
    audio::IAudioProcessor& processor_;
    std::atomic<std::uint64_t>& blocks_;
    std::vector<int> inputPhysical_;   ///< logical input channel -> driver index
    std::vector<int> outputPhysical_;  ///< logical output channel -> driver index
    std::vector<const float*> inputViews_;
    std::vector<float*> outputViews_;
    std::atomic<bool> errorFlag_{ false };
    std::atomic<bool> sawPhysicalArrays_{ false };
    std::atomic<bool> sawTruncatedArrays_{ false };
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
    // UI-01: the names this open already read to validate the selection are kept
    // for the discrete channel selector as well - existing state, published
    // cleanly, not a second query. Control thread writes, GUI thread reads
    // between open and close (capabilities() is polled, never realtime).
    capabilities_.inputChannelNames = inputNames;
    capabilities_.outputChannelNames = outputNames;
    capabilities_.preferredFormat = audio::SampleFormat::float32;

    inputLatency_ = device_->getInputLatencyInSamples();
    outputLatency_ = device_->getOutputLatencyInSamples();

    // Task 018: the driver's own latency figures travel through the contract, not
    // only through the log line. Note what 0 cannot mean: ASIO's query answers
    // 0 both for "this driver has no latency" and "I do not report latency", and
    // no API here distinguishes them - so the accounting labels a zero "not
    // reported" and never claims silence as speed.
    capabilities_.inputLatencySamples = inputLatency_;
    capabilities_.outputLatencySamples = outputLatency_;

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
    callback_->resetArrayEvidence();

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

    // Code review P0 evidence: which channel-array layout the driver actually
    // used. "compacted" is what vendored JUCE's ASIO wrapper promises today;
    // anything else means the stack hands out physically indexed (or short)
    // arrays - the forwarding handled it by the tested rule either way, but a
    // venue log should state the fact, not let a reader infer it from silence.
    const bool physical = (callback_ != nullptr && callback_->sawPhysicalArrays());
    const bool truncated = (callback_ != nullptr && callback_->sawTruncatedArrays());
    std::string arrays = "compacted";
    if (physical && truncated)
        arrays = "physically-indexed+truncated";
    else if (physical)
        arrays = "physically-indexed";
    else if (truncated)
        arrays = "truncated";

    log::info(kComponent, "stopped '" + deviceId_ + "' callbacks=" + std::to_string(callbackBlocks_.load())
                              + " xruns=" + asio::describeXRunCount(xrunCount_.load())
                              + " channel-arrays=" + arrays);

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
