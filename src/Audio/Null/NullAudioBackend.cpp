#include "Audio/Null/NullAudioBackend.h"

#include <algorithm>
#include <cstring>

namespace liveai {
namespace audio {

NullAudioBackend::NullAudioBackend(DeviceCapabilities capabilities)
    : capabilities_(capabilities)
{
}

bool NullAudioBackend::open(IAudioProcessor& processor, const DeviceRequest& request, std::string& error)
{
    error.clear();

    if (state_ != BackendState::closed)
    {
        error = "null backend is already open";
        return false;
    }

    if (request.sampleRate <= 0 || request.bufferFrames <= 0)
    {
        error = "null backend requires a positive sample rate and buffer size";
        return false;
    }

    if (request.inputChannel < 1 || request.outputChannel < 1)
    {
        error = "null backend requires one-based, positive channel indices";
        return false;
    }

    processor_ = &processor;
    bufferFrames_ = request.bufferFrames;
    capabilities_.sampleRate = request.sampleRate;
    capabilities_.preferredBufferFrames = request.bufferFrames;
    capabilities_.minBufferFrames = request.bufferFrames;
    capabilities_.maxBufferFrames = request.bufferFrames;

    input_.assign(static_cast<std::size_t>(bufferFrames_) * static_cast<std::size_t>(std::max(1, capabilities_.inputChannels)), 0.0f);
    output_.assign(static_cast<std::size_t>(bufferFrames_) * static_cast<std::size_t>(std::max(1, capabilities_.outputChannels)), 0.0f);

    inputPointers_.assign(static_cast<std::size_t>(std::max(1, capabilities_.inputChannels)), nullptr);
    outputPointers_.assign(static_cast<std::size_t>(std::max(1, capabilities_.outputChannels)), nullptr);

    for (int channel = 0; channel < capabilities_.inputChannels; ++channel)
        inputPointers_[static_cast<std::size_t>(channel)] = input_.data() + static_cast<std::size_t>(channel) * bufferFrames_;

    for (int channel = 0; channel < capabilities_.outputChannels; ++channel)
        outputPointers_[static_cast<std::size_t>(channel)] = output_.data() + static_cast<std::size_t>(channel) * bufferFrames_;

    ++openCount_;
    state_ = BackendState::opened;
    return true;
}

bool NullAudioBackend::start(std::string& error)
{
    if (state_ != BackendState::opened || processor_ == nullptr)
    {
        error = "null backend must be opened before start";
        return false;
    }

    state_ = BackendState::running;
    error.clear();
    return true;
}

bool NullAudioBackend::stop(std::string& error)
{
    if (state_ != BackendState::running)
    {
        error = "null backend is not running";
        return false;
    }

    state_ = BackendState::opened;
    error.clear();
    return true;
}

void NullAudioBackend::close() noexcept
{
    // The processor belongs to the engine; forget it so a reopened backend can
    // never call a detached engine.
    processor_ = nullptr;
    state_ = BackendState::closed;
}

bool NullAudioBackend::renderOneBlock()
{
    if (state_ != BackendState::running || processor_ == nullptr)
        return false;

    std::memset(output_.data(), 0xFF, output_.size() * sizeof(float));   // poison: proves the engine writes it
    processor_->processAudio(inputPointers_.data(), outputPointers_.data(), capabilities_.preferredBufferFrames);
    return true;
}

void NullAudioBackend::fillInputWith(float value) noexcept
{
    for (auto& sample : input_)
        sample = value;
}

} // namespace audio
} // namespace liveai
