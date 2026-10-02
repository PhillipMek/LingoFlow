#include "Config/ConfigManager.h"

#include <algorithm>

namespace liveai {
namespace {

constexpr int kMinBufferFrames = 64;
constexpr int kMaxBufferFrames = 2048;
constexpr float kMinGainDb = -60.0f;
constexpr float kMaxGainDb = 12.0f;
constexpr int kMaxJitterBufferMs = 1000;

bool isSupportedSampleRate(int sampleRate)
{
    static constexpr int kSupported[] = { 44100, 48000, 88200, 96000 };
    return std::find(std::begin(kSupported), std::end(kSupported), sampleRate) != std::end(kSupported);
}

} // namespace

ConfigManager::ConfigManager()
{
    // SPEC "Audio": MVP prefers 48 kHz, float32, mono.
    current_.interpreterInstructions =
        "Preserve meaning, names, numbers, terminology and intent. Do not summarize, add "
        "explanations or comments. Prioritize low latency without sacrificing quality.";
}

bool ConfigManager::validate(const AppConfig& candidate, std::string& error)
{
    error.clear();

    if (!isSupportedSampleRate(candidate.sampleRate))
    {
        error = "unsupported sample rate: " + std::to_string(candidate.sampleRate);
        return false;
    }

    if (candidate.bufferFrames < kMinBufferFrames || candidate.bufferFrames > kMaxBufferFrames)
    {
        error = "buffer size must be between " + std::to_string(kMinBufferFrames) + " and "
              + std::to_string(kMaxBufferFrames) + " frames";
        return false;
    }

    if (candidate.inputChannel < 1 || candidate.outputChannel < 1)
    {
        error = "channel numbers are one-based and must be positive";
        return false;
    }

    if (candidate.inputGainDb < kMinGainDb || candidate.inputGainDb > kMaxGainDb
        || candidate.outputGainDb < kMinGainDb || candidate.outputGainDb > kMaxGainDb)
    {
        error = "gain must be between " + std::to_string(static_cast<int>(kMinGainDb)) + " and "
              + std::to_string(static_cast<int>(kMaxGainDb)) + " dB";
        return false;
    }

    if (candidate.inputLanguage.empty() || candidate.outputLanguage.empty())
    {
        error = "input and output language must not be empty";
        return false;
    }

    if (candidate.inputLanguage == candidate.outputLanguage)
    {
        error = "input and output language must differ";
        return false;
    }

    if (candidate.jitterBufferMs < 0 || candidate.jitterBufferMs > kMaxJitterBufferMs)
    {
        error = "jitter buffer must be between 0 and " + std::to_string(kMaxJitterBufferMs) + " ms";
        return false;
    }

    if (candidate.ndiEnabled && candidate.ndiStreamName.empty())
    {
        error = "NDI is enabled but the stream name is empty";
        return false;
    }

    return true;
}

bool ConfigManager::update(AppConfig candidate, std::string& error)
{
    if (!validate(candidate, error))
        return false;

    current_ = std::move(candidate);

    for (auto* listener : listeners_)
    {
        if (listener != nullptr)
            listener->onConfigChanged(current_);
    }

    return true;
}

void ConfigManager::addListener(IConfigListener& listener)
{
    listeners_.push_back(&listener);
}

void ConfigManager::resetForTests() noexcept
{
    current_ = AppConfig{};
    listeners_.clear();
}

} // namespace liveai
