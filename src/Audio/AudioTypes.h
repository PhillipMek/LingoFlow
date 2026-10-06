#pragma once
//
// Audio domain types. Deliberately JUCE-free and protocol-free: nothing here
// knows about OpenAI, NDI or the UI layer.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace liveai {
namespace audio {

/// Sample layout handled by the engine. MVP uses float32 mono 48 kHz
/// (spec "Audio"); other formats exist so conversion can be added without
/// changing interfaces.
enum class SampleFormat
{
    unknown = 0,
    float32,      ///< interleaved or planar 32-bit float
    int16,
    int24In32,
    int32
};

enum class BackendState
{
    closed = 0,   ///< backend object exists, no device is opened
    opened,       ///< device opened, not running
    running,      ///< callback is being driven by the device
    faulted       ///< device error; recovery is the backend's job
};

std::string_view nameOf(SampleFormat format) noexcept;
std::string_view nameOf(BackendState state) noexcept;

/// Device capabilities reported after a successful open.
struct DeviceCapabilities
{
    SampleFormat preferredFormat = SampleFormat::float32;
    int sampleRate = 48000;
    int minBufferFrames = 32;
    int maxBufferFrames = 4096;
    int preferredBufferFrames = 480;
    int inputChannels = 1;
    int outputChannels = 1;

    /// The full channel names the driver reports (device totals, not the engine's
    /// selection - `inputChannels`/`outputChannels above stay the ACTIVE count).
    /// Known while a device is open, empty whenever none is: the UI redesign's discrete
    /// channel selector enumerates these, and an empty list honestly means
    /// "not opened yet" - the selector falls back to generic numbering instead
    /// of inventing names.
    std::vector<std::string> inputChannelNames;
    std::vector<std::string> outputChannelNames;

    /// What the driver itself reports as its input/output latency, in samples
    /// (the latency accounting). 0 means "no usable answer": the backend did not
    /// ask, or the driver's query answered zero - ASIO's zero conflates "no
    /// latency" with "I do not report", and the accounting labels it "not
    /// reported" rather than choosing a story. Only a positive value is taken
    /// as a reported fact (JuceAsioBackend copies getInputLatencyInSamples()).
    int inputLatencySamples = 0;
    int outputLatencySamples = 0;
};

/// Mono block handed to the engine from the backend callback. Pointers are only
/// valid for the duration of the call; the engine must copy into pre-allocated
/// buffers if it needs them longer.
struct MonoBlock
{
    const float* samples = nullptr;   ///< one channel, `frames` consecutive samples
    int frames = 0;
};

/// Mono block the engine must fill for the device. Silence is a valid, required
/// outcome when no translated audio is available.
struct MutableMonoBlock
{
    float* samples = nullptr;
    int frames = 0;
};

} // namespace audio
} // namespace liveai
