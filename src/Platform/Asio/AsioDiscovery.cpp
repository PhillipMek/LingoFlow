#include "Platform/Asio/AsioDiscovery.h"

#include <windows.h>

#include <memory>
#include <set>
#include <thread>

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

/// Counts delivered callback blocks during a probe. Atomic increments only.
class CountingCallback final : public juce::AudioIODeviceCallback
{
public:
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                         float* const* outputChannelData, int numOutputChannels,
                                         int numSamples,
                                         const juce::AudioIODeviceCallbackContext&) override
    {
        (void)inputChannelData; (void)numInputChannels;
        (void)outputChannelData; (void)numOutputChannels;
        if (numSamples > 0)
            blocks_.fetch_add(1, std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice*) override { started_.store(true, std::memory_order_relaxed); }
    void audioDeviceStopped() override { started_.store(false, std::memory_order_relaxed); }
    void audioDeviceError(const juce::String& message) override { lastError_ = message.toStdString(); }

    std::uint64_t blocks() const noexcept { return blocks_.load(std::memory_order_relaxed); }
    const std::string& lastError() const noexcept { return lastError_; }

private:
    std::atomic<std::uint64_t> blocks_{ 0 };
    std::atomic<bool> started_{ false };
    std::string lastError_;
};

void scanRegistryKeys(HKEY root, const wchar_t* path, std::set<std::string>& out)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    wchar_t childName[256] = {};
    DWORD nameChars = 0;

    for (DWORD index = 0;; ++index)
    {
        nameChars = 256;
        const LONG result = RegEnumKeyExW(key, index, childName, &nameChars, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS)
            break;
        if (result != ERROR_SUCCESS)
            continue;

        std::string narrow = juce::String(childName).toStdString();
        if (!narrow.empty())
            out.insert(std::move(narrow));
    }

    RegCloseKey(key);
}

} // namespace

std::string_view discoveryBackendName() noexcept
{
    return "JUCE ASIO";
}

ScanResult scanAsioDevices()
{
    ScanResult result;

    std::unique_ptr<juce::AudioIODeviceType> type(createAsioTypeOrNull());

    if (type == nullptr)
    {
        result.notes.emplace_back("ASIO device type unavailable (JUCE built without JUCE_ASIO, or no ASIO host)");
        log::warning(kComponent, result.notes.back());
        return result;
    }

    result.asioTypeAvailable = true;

    // Registry-only: juce_ASIO_windows.cpp scanForDevices() opens
    // HKEY_LOCAL_MACHINE\software\asio and validates each CLSID. No driver DLL is
    // loaded, so this is safe for automated tests.
    type->scanForDevices();

    for (const auto& name : type->getDeviceNames(false))
    {
        asio::DeviceEntry entry;
        entry.name = name.toStdString();
        entry.id = entry.name;                       // JUCE selects devices by this name
        entry.driverName = type->getTypeName().toStdString();
        entry.registered = true;
        entry.driverLoaded = false;                  // deliberately not loaded by a scan
        entry.note = "registry-only scan; the driver DLL was not loaded";
        result.devices.push_back(std::move(entry));
    }

    log::info(kComponent, "scan: " + std::to_string(result.devices.size()) + " ASIO device(s)");
    for (const auto& device : result.devices)
        log::info(kComponent, asio::describeDevice(device));

    return result;
}

std::vector<std::string> registryAsioDriverNames()
{
    std::set<std::string> names;
    scanRegistryKeys(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", names);
    scanRegistryKeys(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\ASIO", names);
    return { names.begin(), names.end() };
}

asio::OpenAttempt probeAsioDevice(const std::string& deviceId,
                                 double requestedSampleRate,
                                 int requestedBufferFrames,
                                 bool startAndStop)
{
    asio::OpenAttempt attempt;
    attempt.capabilities.id = deviceId;
    attempt.capabilities.name = deviceId;

    std::unique_ptr<juce::AudioIODeviceType> type(createAsioTypeOrNull());

    if (type == nullptr)
    {
        attempt.error = "ASIO device type is not available in this build";
        log::error(kComponent, "probe '" + deviceId + "': " + attempt.error);
        return attempt;
    }

    type->scanForDevices();

    const juce::String name(deviceId);
    if (!type->getDeviceNames(false).contains(name))
    {
        attempt.error = "device '" + deviceId + "' is not in the ASIO device list";
        log::error(kComponent, "probe: " + attempt.error);
        return attempt;
    }

    std::unique_ptr<juce::AudioIODevice> device(type->createDevice(name, name));

    if (device == nullptr)
    {
        attempt.error = "could not instantiate the ASIO device object";
        log::error(kComponent, "probe '" + deviceId + "': " + attempt.error);
        return attempt;
    }

    // Everything below touches the driver: loading its DLL and ASIOInit inside open().
    attempt.capabilities.sampleRates = toStdRates(device->getAvailableSampleRates());
    attempt.capabilities.bufferSizes = toStdBuffers(device->getAvailableBufferSizes());
    attempt.capabilities.inputChannels = toStdNames(device->getInputChannelNames());
    attempt.capabilities.outputChannels = toStdNames(device->getOutputChannelNames());
    attempt.capabilities.controlPanelAvailable = device->hasControlPanel();

    const auto selection = asio::selectConfiguration(attempt.capabilities.sampleRates,
                                                     attempt.capabilities.bufferSizes,
                                                     requestedSampleRate,
                                                     requestedBufferFrames);
    for (const auto& note : selection.notes)
        log::info(kComponent, "'" + deviceId + "' " + note);

    const double rate = selection.sampleRate.value_or(requestedSampleRate);
    const int buffers = selection.bufferFrames.value_or(requestedBufferFrames);

    if (attempt.capabilities.inputChannels.empty())
    {
        attempt.error = "driver reports no input channels";
    }
    else if (attempt.capabilities.outputChannels.empty())
    {
        attempt.error = "driver reports no output channels";
    }
    else
    {
        const juce::String openError = device->open(channelMask(1), channelMask(1), rate, buffers);
        if (openError.isNotEmpty())
            attempt.error = openError.toStdString();
        else
            attempt.capabilities.opened = true;
    }

    if (!attempt.capabilities.opened)
    {
        device->close();
        log::error(kComponent, "open '" + deviceId + "' failed: " + attempt.error);
        return attempt;
    }

    attempt.success = true;
    attempt.capabilities.selectedSampleRate = device->getCurrentSampleRate();
    attempt.capabilities.selectedBufferFrames = device->getCurrentBufferSizeSamples();
    attempt.capabilities.inputLatencySamples = device->getInputLatencyInSamples();
    attempt.capabilities.outputLatencySamples = device->getOutputLatencyInSamples();
    attempt.capabilities.bitDepth = device->getCurrentBitDepth();

    log::info(kComponent, asio::describeCapabilities(attempt.capabilities));

    if (startAndStop)
    {
        CountingCallback callback;
        device->start(&callback);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (callback.blocks() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        const auto blocks = callback.blocks();
        device->stop();

        log::info(kComponent, "'" + deviceId + "' start/stop: callbacks=" + std::to_string(blocks)
                                  + " xruns=" + std::to_string(device->getXRunCount()));

        if (blocks == 0)
        {
            attempt.success = false;
            attempt.error = "device opened but delivered no callbacks";
            if (!callback.lastError().empty())
                attempt.error += ": " + callback.lastError();
            log::error(kComponent, "'" + deviceId + "' " + attempt.error);
        }
    }

    device->close();
    return attempt;
}

} // namespace platform
} // namespace liveai
