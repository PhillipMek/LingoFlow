#pragma once
//
// IAudioBackend - the only contract between the audio engine and a device.
//
// Boundary rules (AGENTS.md 5 and 7):
//   * processAudio() runs on the device (realtime) thread. Implementations call
//     it directly; it must not allocate, block, touch the network, disk, JSON or
//     the UI.
//   * Everything else (open/close/start/stop, capability queries) is called from
//     a non-realtime thread only.
//   * Implementations must not know about translation or NDI.
//
// No device is implemented in this task: task 004/005 add the ASIO backend.

#include <string>
#include <string_view>

#include "Audio/AudioTypes.h"

namespace liveai {
namespace audio {

/// What the engine asks a backend to configure. Channel indices are one-based
/// (SPEC/Config): a real device must be opened with a channel mask, and "which
/// port carries the FOH feed" is operator knowledge, not driver knowledge.
struct DeviceRequest
{
    /// ASIO device name as reported by discovery (Platform/Asio/AsioDiscovery).
    /// Empty means "no device selected yet", which the composition root treats as
    /// "stay on the null backend" rather than as a license to pick any device.
    std::string deviceId;
    int sampleRate = 48000;
    int bufferFrames = 480;
    int inputChannel = 1;
    int outputChannel = 1;
};

/// Implemented by the AudioEngine; called by a backend.
class IAudioProcessor
{
public:
    virtual ~IAudioProcessor() = default;

    /// Called by the backend for every block of audio. `input` holds
    /// `inputChannels` pointers to `frameCount` frames, `output` holds
    /// `outputChannels` pointers to `frameCount` frames. Both are preallocated by
    /// the backend; the processor never resizes them.
    /// Realtime-safe by contract: the implementation must not allocate, block,
    /// log or touch the network/UI.
    virtual void processAudio(const float* const* input,
                              float* const* output,
                              int frameCount) noexcept = 0;

    /// Called once from a non-realtime thread right after the backend opened the
    /// device, before any processAudio() call.
    virtual void onAudioConfigurationChanged(int sampleRate, int bufferFrames) = 0;
};

/// Device side of the boundary: ASIO/SoundGrid, WAV file or a test null device.
class IAudioBackend
{
public:
    virtual ~IAudioBackend() = default;

    /// Human-readable backend name, e.g. "Null", "Waves SoundGrid ASIO".
    virtual std::string_view name() const noexcept = 0;

    virtual BackendState state() const noexcept = 0;

    /// Opens the device. `processor` is not owned and must stay alive until
    /// close() returns. Returns false with the failure recorded in `error`.
    virtual bool open(IAudioProcessor& processor, const DeviceRequest& request, std::string& error) = 0;

    virtual bool start(std::string& error) = 0;
    virtual bool stop(std::string& error) = 0;
    virtual void close() noexcept = 0;

    /// Valid after open(); meaningless before it.
    virtual DeviceCapabilities capabilities() const noexcept = 0;
};

} // namespace audio
} // namespace liveai
