#pragma once
//
// ASIO device model and selection policy - deliberately JUCE-free.
//
// The JUCE adapter (Audio/Asio/JuceAsio.*) fills these structs from the ASIO host
// API and implements IAudioBackend; everything that decides *what is usable* lives
// here, so it is unit-testable without a driver, without a device and without JUCE.
//
// Honesty rule (AGENTS.md 19): a field that the driver did not report stays empty
// or zero. Nothing here invents channel names, sample rates or latencies, and
// formatted output always says where a value came from.

#include <optional>
#include <string>
#include <vector>

#include "Audio/AudioTypes.h"

namespace liveai {
namespace asio {

/// One ASIO device as seen by the host, before it is opened.
struct DeviceEntry
{
    std::string id;            ///< stable identifier used to re-select the device
    std::string name;          ///< driver-reported device name
    std::string driverName;    ///< backend type, e.g. "ASIO"
    bool registered = false;   ///< found through the ASIO registry scan
    bool driverLoaded = false; ///< the driver DLL was loaded (only after open/probe)
    std::string note;          ///< provenance/limitation of this record
};

/// Capabilities read from an opened device, plus what the engine decided to use.
struct DeviceCapabilitiesReport
{
    std::string id;
    std::string name;

    std::vector<std::string> inputChannels;
    std::vector<std::string> outputChannels;

    std::vector<double> sampleRates;
    std::vector<int> bufferSizes;

    std::optional<double> selectedSampleRate;
    std::optional<int> selectedBufferFrames;

    int inputLatencySamples = 0;
    int outputLatencySamples = 0;
    int bitDepth = 0;
    bool controlPanelAvailable = false;

    /// True when open() succeeded. When false, `error` explains it and all the
    /// vectors above stay empty: an unopened device reports nothing.
    bool opened = false;
    std::string error;
};

/// Result of trying to use a device for the engine.
struct OpenAttempt
{
    bool success = false;
    std::string error;
    DeviceCapabilitiesReport capabilities;
};

enum class RateChoice { exactMatch, nearestAvailable, noneAvailable };
enum class BufferChoice { exactMatch, nearestAvailable, noneAvailable };

struct Selection
{
    std::optional<double> sampleRate;
    std::optional<int> bufferFrames;
    RateChoice rateChoice = RateChoice::noneAvailable;
    BufferChoice bufferChoice = BufferChoice::noneAvailable;
    std::vector<std::string> notes;
};

/// Picks the requested rate if the device offers it, otherwise the nearest one
/// above 44100 that we can actually use, and records what happened.
Selection selectConfiguration(const std::vector<double>& availableSampleRates,
                              const std::vector<int>& availableBufferSizes,
                              double requestedSampleRate,
                              int requestedBufferFrames);

/// Human-readable, log-safe one-line summary. Contains no secrets.
std::string describeDevice(const DeviceEntry& entry);
std::string describeCapabilities(const DeviceCapabilitiesReport& report);

/// Driver xrun counters are optional: JUCE reports -1 when the driver does not
/// expose one, and that must never be displayed as "0 underruns".
std::string describeXRunCount(int reportedValue);

/// Validates the channel indices chosen by the operator against a report.
/// Channels are one-based (SPEC/Config); returns an error message when out of range.
bool validateChannelSelection(const DeviceCapabilitiesReport& report,
                              int inputChannel,
                              int outputChannel,
                              std::string& error);

} // namespace asio
} // namespace liveai
