#pragma once
//
// Internal JUCE glue for the ASIO platform adapter.
//
// Included only by the .cpp files of src/Platform/Asio: no public interface in this
// project exposes a JUCE type, so the portable core stays JUCE-free
// (tests/ArchitectureBoundaries.cmake allows JUCE headers just inside Platform).

#include <algorithm>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace liveai {
namespace platform {
namespace asio_detail {

inline juce::AudioIODeviceType* createAsioTypeOrNull()
{
    return juce::AudioIODeviceType::createAudioIODeviceType_ASIO();
}

inline std::vector<double> toStdRates(const juce::Array<double>& values)
{
    return { values.begin(), values.end() };
}

inline std::vector<int> toStdBuffers(const juce::Array<int>& values)
{
    return { values.begin(), values.end() };
}

inline std::vector<std::string> toStdNames(const juce::StringArray& names)
{
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(names.size()));
    for (const auto& name : names)
        out.push_back(name.toStdString());
    return out;
}

/// One-based channel index to a JUCE channel mask with exactly that bit set.
inline juce::BigInteger channelMask(int oneBasedIndex)
{
    juce::BigInteger mask;
    mask.setBit(oneBasedIndex - 1, true);
    return mask;
}

/// JUCE's channel lists are only known after the device object exists; this builds
/// the model used by Audio/Asio/AsioDeviceInfo for validation.
inline std::vector<std::string> channelNames(const juce::StringArray& names, int activeCount)
{
    std::vector<std::string> out;
    const int count = std::max(0, activeCount);
    out.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i)
        out.push_back(i < static_cast<int>(names.size()) ? names[i].toStdString() : ("channel " + std::to_string(i + 1)));

    return out;
}

} // namespace asio_detail
} // namespace platform
} // namespace liveai

