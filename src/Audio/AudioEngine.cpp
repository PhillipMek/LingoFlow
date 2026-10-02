#include "Audio/AudioEngine.h"

#include <algorithm>
#include <cstring>

#include "Diagnostics/DiagnosticsManager.h"

namespace liveai {

AudioEngine::AudioEngine(DiagnosticsManager* diagnostics) noexcept
    : diagnostics_(diagnostics)
{
}

bool AudioEngine::activate(audio::IAudioBackend& backend, int sampleRate, int bufferFrames, std::string& error)
{
    if (backend_ != nullptr)
    {
        error = "audio engine already activated";
        return false;
    }

    if (!backend.open(*this, sampleRate, bufferFrames, error))
        return false;

    backend_ = &backend;

    if (!backend.start(error))
    {
        backend.close();
        backend_ = nullptr;
        return false;
    }

    onAudioConfigurationChanged(backend.capabilities().sampleRate,
                                backend.capabilities().preferredBufferFrames);

    if (diagnostics_ != nullptr)
        diagnostics_->noteAudioBackend(backend.name());

    return true;
}

void AudioEngine::deactivate() noexcept
{
    if (backend_ == nullptr)
        return;

    std::string ignored;
    backend_->stop(ignored);
    backend_->close();
    backend_ = nullptr;

    if (diagnostics_ != nullptr)
        diagnostics_->noteAudioBackendStopped();
}

void AudioEngine::processAudio(const float* const* input,
                               float* const* output,
                               int frameCount) noexcept
{
    // Realtime thread: fixed-size work only. No allocation, no logging, no
    // network, no filesystem, no locks. Atomic relaxed counters are permitted.
    if (frameCount <= 0)
        return;

    if (input == nullptr || output == nullptr || output[0] == nullptr)
        return;

    // Silence is the correct output until translated audio exists (task 012):
    // an underrun must never leak unprocessed microphone audio to the audience.
    std::memset(output[0], 0, static_cast<std::size_t>(frameCount) * sizeof(float));

    blocks_.fetch_add(1, std::memory_order_relaxed);
    frames_.fetch_add(static_cast<std::uint64_t>(frameCount), std::memory_order_relaxed);

    if (diagnostics_ != nullptr)
        diagnostics_->countAudioBlock(frameCount);
}

void AudioEngine::onAudioConfigurationChanged(int sampleRate, int bufferFrames)
{
    // Non-realtime: called right after the device is opened.
    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    bufferFrames_.store(bufferFrames, std::memory_order_relaxed);

    if (diagnostics_ != nullptr)
        diagnostics_->noteAudioGeometry(sampleRate, bufferFrames);
}

} // namespace liveai
