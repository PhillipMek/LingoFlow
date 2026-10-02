#include "Audio/AudioLoopback.h"

#include <algorithm>
#include <chrono>

namespace liveai {
namespace audio {
namespace {

/// Scratch size per channel: enough for several device blocks so one wake-up can move
/// a whole backlog. Sized at start(), never touched by the realtime side.
constexpr std::size_t kScratchFrames = 4096;

constexpr int kMinPollIntervalMs = 1;
constexpr int kMaxPollIntervalMs = 50;

} // namespace

AudioLoopback::AudioLoopback(AudioEngine& engine) noexcept
    : engine_(engine)
{
}

AudioLoopback::~AudioLoopback()
{
    stop();
}

void AudioLoopback::setPollIntervalMs(int intervalMs) noexcept
{
    pollIntervalMs_.store(std::clamp(intervalMs, kMinPollIntervalMs, kMaxPollIntervalMs),
                          std::memory_order_relaxed);
}

bool AudioLoopback::start(std::string& error)
{
    error.clear();

    if (running_.load(std::memory_order_relaxed))
    {
        error = "loopback is already running";
        return false;
    }

    if (!engine_.pipelineReady())
    {
        error = "loopback needs a running audio pipeline: activate the engine first";
        return false;
    }

    const int pairs = std::min(engine_.inputChannels(), engine_.outputChannels());

    if (pairs < 1)
    {
        error = "loopback found no channel pair to connect";
        return false;
    }

    channelPairs_.store(pairs, std::memory_order_relaxed);

    try
    {
        scratch_.assign(static_cast<std::size_t>(pairs) * kScratchFrames, 0.0f);

        engine_.attachInputConsumer();
        attached_.store(true, std::memory_order_release);

        stopRequested_.store(false, std::memory_order_relaxed);
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread(&AudioLoopback::run, this);
    }
    catch (const std::exception& exception)
    {
        engine_.detachInputConsumer();
        attached_.store(false, std::memory_order_release);
        running_.store(false, std::memory_order_relaxed);
        error = std::string("could not start the loopback worker: ") + exception.what();
        return false;
    }

    return true;
}

void AudioLoopback::stop() noexcept
{
    stopRequested_.store(true, std::memory_order_relaxed);

    if (thread_.joinable())
        thread_.join();

    if (attached_.exchange(false, std::memory_order_acq_rel))
        engine_.detachInputConsumer();

    running_.store(false, std::memory_order_relaxed);
}

void AudioLoopback::run() noexcept
{
    // Worker thread, not realtime: it may sleep. What it must never do is hold up the
    // audio callback, and it cannot: it only uses the consumer/producer ends of the
    // lock-free buffers, which never block.
    const std::size_t scratchPerChannel = kScratchFrames;

    while (!stopRequested_.load(std::memory_order_relaxed))
    {
        bool moved = false;
        const int pairs = channelPairs_.load(std::memory_order_relaxed);

        for (int channel = 0; channel < pairs; ++channel)
        {
            auto* ring = engine_.inputRing(channel);
            auto* jitter = engine_.outputJitter(channel);

            if (ring == nullptr || jitter == nullptr)
                continue;

            const std::size_t index = static_cast<std::size_t>(channel) * scratchPerChannel;
            const std::size_t available = ring->readable();

            if (available == 0)
                continue;

            const std::size_t take = std::min<std::size_t>(available, scratchPerChannel);
            const std::size_t got = ring->read(scratch_.data() + index, take);

            if (got == 0)
                continue;

            const std::size_t written = jitter->write(scratch_.data() + index, got);

            transferred_.fetch_add(static_cast<std::uint64_t>(written), std::memory_order_relaxed);
            moved = true;
        }

        if (!moved)
            std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs_.load(std::memory_order_relaxed)));
    }
}

} // namespace audio
} // namespace liveai
