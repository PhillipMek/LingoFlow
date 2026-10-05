#include "Audio/AudioEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Diagnostics/DiagnosticsManager.h"

namespace liveai {
namespace {

/// How much input a ring must be able to hold without dropping. Two seconds is a
/// deliberate bound, not a measurement: it is long enough that a short network stall
/// does not lose microphone audio, and short enough that memory stays predictable
/// (48000 * 2 * 4 B = 384 KB per channel).
constexpr int kInputRingSeconds = 2;

/// Minimum size in device blocks, so a tiny block size does not produce a ring that
/// overflows on the first scheduler hiccup.
constexpr std::size_t kMinBlocksBuffered = 4;

/// SPEC "Output Jitter Buffer" suggests 20-500 ms. Config validates 0-1000; the
/// engine clamps to the SPEC window so a hand-edited file cannot ask for a second
/// of stored delay without anyone noticing.
constexpr int kJitterMinMs = 0;
constexpr int kJitterMaxMs = 500;

std::size_t clampedJitterMs(int jitterBufferMs) noexcept
{
    return static_cast<std::size_t>(std::clamp(jitterBufferMs, kJitterMinMs, kJitterMaxMs));
}

/// Silence EVERY output channel this callback was promised. Device callbacks
/// treat output buffer content as undefined: touching only channel 0 leaves
/// stale audio - or raw uninitialised memory - on the wire for every other
/// channel, which is the opposite of what the defensive paths owe the audience
/// (code review P0, 2026-10-05). `channels` is the geometry the backend
/// advertised at configuration; 0 means nothing was ever configured - the only
/// way to reach this from a real product flow is the teardown window between
/// pipelineReady_ going false and the counts going 0, and the honest minimal
/// answer there is the contract's first channel. Realtime-safe: memset and a
/// bounded loop over pointers the caller handed over.
void silenceOutputs(float* const* output, int channels, std::size_t frames) noexcept
{
    if (output == nullptr)
        return;

    const int promised = channels > 0 ? channels : 1;

    for (int channel = 0; channel < promised; ++channel)
    {
        float* destination = output[channel];

        if (destination != nullptr)
            std::memset(destination, 0, frames * sizeof(float));
    }
}

} // namespace

AudioEngine::AudioEngine(DiagnosticsManager* diagnostics) noexcept
    : diagnostics_(diagnostics)
{
}

// --------------------------------------------------------------------------- sizing

std::size_t AudioEngine::inputRingCapacity(int sampleRate, int blockFrames) noexcept
{
    const std::size_t framesPerSecond = sampleRate > 0 ? static_cast<std::size_t>(sampleRate) : 48000;
    const std::size_t block = blockFrames > 0 ? static_cast<std::size_t>(blockFrames) : 480;

    const std::size_t bySeconds = framesPerSecond * static_cast<std::size_t>(kInputRingSeconds);
    const std::size_t byBlocks = block * kMinBlocksBuffered;

    return std::max(bySeconds, byBlocks);
}

std::size_t AudioEngine::jitterTargetFrames(int sampleRate, int jitterBufferMs) noexcept
{
    const std::size_t framesPerSecond = sampleRate > 0 ? static_cast<std::size_t>(sampleRate) : 48000;
    return framesPerSecond * clampedJitterMs(jitterBufferMs) / 1000;
}

std::size_t AudioEngine::jitterCapacity(int sampleRate, int jitterBufferMs, int blockFrames) noexcept
{
    const std::size_t target = jitterTargetFrames(sampleRate, jitterBufferMs);
    const std::size_t block = blockFrames > 0 ? static_cast<std::size_t>(blockFrames) : 480;

    // Room for the pre-roll plus one more pre-roll plus two blocks of slack, so the
    // producer is never the reason a block is dropped.
    const std::size_t byTarget = target * 2 + block * 2;

    return std::max(byTarget, block * kMinBlocksBuffered);
}

// ------------------------------------------------------------------------- lifecycle

bool AudioEngine::buildPipeline(int sampleRate, int blockFrames, int inputChannels, int outputChannels,
                                std::string& error)
{
    // A device that reports no channels is treated as mono rather than as an error:
    // the driver has been observed to open with 1x1 configurations and the pipeline
    // must still be able to run silence.
    inputChannels = std::clamp(inputChannels, 1, 64);
    outputChannels = std::clamp(outputChannels, 1, 64);

    const std::size_t ringCapacity = inputRingCapacity(sampleRate, blockFrames);
    const std::size_t jitterCapacityForChannel = jitterCapacity(sampleRate, jitterMs_.load(std::memory_order_relaxed), blockFrames);
    const std::size_t jitterTarget = jitterTargetFrames(sampleRate, jitterMs_.load(std::memory_order_relaxed));

    std::vector<std::unique_ptr<audio::AudioRingBuffer>> rings;
    std::vector<std::unique_ptr<audio::AudioJitterBuffer>> jitters;
    std::vector<std::unique_ptr<audio::LevelMeter>> inMeters;
    std::vector<std::unique_ptr<audio::LevelMeter>> outMeters;

    try
    {
        rings.reserve(static_cast<std::size_t>(inputChannels));
        inMeters.reserve(static_cast<std::size_t>(inputChannels));

        for (int channel = 0; channel < inputChannels; ++channel)
        {
            rings.push_back(std::make_unique<audio::AudioRingBuffer>(ringCapacity));
            inMeters.push_back(std::make_unique<audio::LevelMeter>());
        }

        jitters.reserve(static_cast<std::size_t>(outputChannels));
        outMeters.reserve(static_cast<std::size_t>(outputChannels));

        for (int channel = 0; channel < outputChannels; ++channel)
        {
            jitters.push_back(std::make_unique<audio::AudioJitterBuffer>(jitterCapacityForChannel, jitterTarget));
            outMeters.push_back(std::make_unique<audio::LevelMeter>());
        }

        // Gain stages and their scratch are allocated here as well. They are the only
        // part of the pipeline that has to be allocated before the first callback and
        // never inside it (AGENTS.md 5).
        growGainStages(inputChannels, outputChannels);
    }
    catch (...)
    {
        // Allocation failure at setup time. This is the last place where it is
        // allowed to be handled at all - never inside processAudio().
        error = "could not allocate the audio pipeline (" + std::to_string(ringCapacity) + " input frames, "
              + std::to_string(jitterCapacityForChannel) + " jitter frames, "
              + std::to_string(static_cast<std::size_t>(inputChannels) * kGainChunkFrames)
              + " gain scratch frames)";
        return false;
    }

    if (rings.empty() || jitters.empty() || !rings.front()->valid() || !jitters.front()->valid())
    {
        error = "audio pipeline geometry is invalid";
        return false;
    }

    inputRings_ = std::move(rings);
    outputJitters_ = std::move(jitters);
    inputMeters_ = std::move(inMeters);
    outputMeters_ = std::move(outMeters);

    // Land the gain stages on the operator's current levels before the device runs: the
    // first block must already play the level the UI shows, not glide into it.
    applyGainToStages(sampleRate);

    inputChannels_.store(static_cast<int>(inputRings_.size()), std::memory_order_relaxed);
    outputChannels_.store(static_cast<int>(outputJitters_.size()), std::memory_order_relaxed);

    pipelineReady_.store(true, std::memory_order_release);
    return true;
}

void AudioEngine::releasePipeline() noexcept
{
    pipelineReady_.store(false, std::memory_order_release);

    inputRings_.clear();
    outputJitters_.clear();
    inputMeters_.clear();
    outputMeters_.clear();

    inputChannels_.store(0, std::memory_order_relaxed);
    outputChannels_.store(0, std::memory_order_relaxed);
}

bool AudioEngine::activate(audio::IAudioBackend& backend, const audio::DeviceRequest& request, std::string& error)
{
    if (backend_ != nullptr)
    {
        error = "audio engine already activated";
        return false;
    }

    if (!backend.open(*this, request, error))
        return false;

    const auto capabilities = backend.capabilities();

    // The pipeline has to exist before the first callback: processAudio() may not
    // allocate. Anything that fails here leaves the device closed again.
    if (!buildPipeline(capabilities.sampleRate > 0 ? capabilities.sampleRate : request.sampleRate,
                       capabilities.preferredBufferFrames > 0 ? capabilities.preferredBufferFrames : request.bufferFrames,
                       capabilities.inputChannels,
                       capabilities.outputChannels,
                       error))
    {
        backend.close();
        return false;
    }

    backend_ = &backend;

    if (!backend.start(error))
    {
        backend.close();
        backend_ = nullptr;
        releasePipeline();
        return false;
    }

    onAudioConfigurationChanged(capabilities.sampleRate, capabilities.preferredBufferFrames);

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

    // The device is stopped, so no callback can be running: the buffers are released
    // only after that. Consumers must have been stopped by their owner beforehand.
    consumerAttached_.store(false, std::memory_order_relaxed);
    releasePipeline();

    if (diagnostics_ != nullptr)
        diagnostics_->noteAudioBackendStopped();
}

void AudioEngine::setJitterBufferMs(int jitterBufferMs) noexcept
{
    const int clamped = static_cast<int>(clampedJitterMs(jitterBufferMs));
    jitterMs_.store(clamped, std::memory_order_relaxed);

    const std::size_t target = jitterTargetFrames(sampleRate_.load(std::memory_order_relaxed), clamped);

    for (auto& jitter : outputJitters_)
    {
        if (jitter != nullptr)
            jitter->setTargetFrames(target);
    }
}

int AudioEngine::bufferBlockMs() const noexcept
{
    const int rate = sampleRate_.load(std::memory_order_relaxed);
    const int frames = bufferFrames_.load(std::memory_order_relaxed);

    if (rate <= 0 || frames <= 0)
        return 0;   // no geometry yet: zero is the honest answer, not a guessed default

    return static_cast<int>(std::llround(static_cast<double>(frames) * 1000.0 / rate));
}

int AudioEngine::pipelineBufferDelayMs() const noexcept
{
    return bufferBlockMs() * 2 + jitterBufferMs();
}

// ---------------------------------------------------------------------------- gain (006)

namespace {

/// Applies the stage window to a requested gain before the engine remembers it as "what
/// the operator asked for", and counts anything that had to be altered. A refused value
/// keeps the previous one: inventing 0 dB for a NaN request would be a level nobody set,
/// and inventing silence would be a fault nobody caused.
float rememberGain(float requested,
                   std::atomic<float>& stored,
                   std::atomic<std::uint64_t>& clamped,
                   std::atomic<std::uint64_t>& rejected) noexcept
{
    if (!std::isfinite(requested))
    {
        rejected.fetch_add(1, std::memory_order_relaxed);
        return stored.load(std::memory_order_relaxed);
    }

    const float windowed = std::clamp(requested, audio::GainStage::kMinGainDb, audio::GainStage::kMaxGainDb);

    if (windowed != requested)
        clamped.fetch_add(1, std::memory_order_relaxed);

    stored.store(windowed, std::memory_order_relaxed);
    return windowed;
}

} // namespace

void AudioEngine::setInputGainDb(float gainDb) noexcept
{
    rememberGain(gainDb, inputGainDb_, gainClamped_, gainRejected_);

    // The stages implement the same window, so the two views of "the current gain"
    // cannot disagree. They are handed the raw request and count it as clamped too.
    for (auto& stage : inputStages_)
    {
        if (stage != nullptr)
            stage->setGainDb(gainDb);
    }
}

void AudioEngine::setOutputGainDb(float gainDb) noexcept
{
    rememberGain(gainDb, outputGainDb_, gainClamped_, gainRejected_);

    for (auto& stage : outputStages_)
    {
        if (stage != nullptr)
            stage->setGainDb(gainDb);
    }
}

void AudioEngine::setInputMuted(bool muted) noexcept
{
    inputMuted_.store(muted, std::memory_order_relaxed);

    for (auto& stage : inputStages_)
    {
        if (stage != nullptr)
            stage->setMuted(muted);
    }
}

void AudioEngine::setOutputMuted(bool muted) noexcept
{
    outputMuted_.store(muted, std::memory_order_relaxed);

    for (auto& stage : outputStages_)
    {
        if (stage != nullptr)
            stage->setMuted(muted);
    }
}

void AudioEngine::setGainRampMs(int rampMs) noexcept
{
    const int windowed = std::clamp(rampMs, 0, audio::GainStage::kMaxRampMs);
    gainRampMs_.store(windowed, std::memory_order_relaxed);

    // setRampMs rather than configure: it changes only the length of the next glide, so
    // calling it while the device runs cannot jump the coefficient.
    for (auto& stage : inputStages_)
    {
        if (stage != nullptr)
            stage->setRampMs(windowed);
    }

    for (auto& stage : outputStages_)
    {
        if (stage != nullptr)
            stage->setRampMs(windowed);
    }
}

float AudioEngine::appliedInputGainDb() const noexcept
{
    // Non-realtime read path, same rule as inputRing() and the meters: valid while the
    // pipeline is stable. Before the first activate() nothing has been applied yet, so
    // the requested value is the answer.
    const auto& front = inputStages_.empty() ? nullptr : inputStages_.front().get();

    const float linear = front != nullptr
                             ? front->appliedLinear()
                             : audio::GainStage::dbToLinear(inputGainDb_.load(std::memory_order_relaxed));

    return audio::linearToDb(linear);
}

float AudioEngine::appliedOutputGainDb() const noexcept
{
    const auto& front = outputStages_.empty() ? nullptr : outputStages_.front().get();

    const float linear = front != nullptr
                             ? front->appliedLinear()
                             : audio::GainStage::dbToLinear(outputGainDb_.load(std::memory_order_relaxed));

    return audio::linearToDb(linear);
}

std::uint64_t AudioEngine::sumInputStages(std::uint64_t (audio::GainStage::*counter)() const noexcept) const noexcept
{
    std::uint64_t total = 0;

    for (const auto& stage : inputStages_)
    {
        if (stage != nullptr)
            total += (stage.get()->*counter)();
    }

    return total;
}

std::uint64_t AudioEngine::sumOutputStages(std::uint64_t (audio::GainStage::*counter)() const noexcept) const noexcept
{
    std::uint64_t total = 0;

    for (const auto& stage : outputStages_)
    {
        if (stage != nullptr)
            total += (stage.get()->*counter)();
    }

    return total;
}

std::uint64_t AudioEngine::inputClippedFrames() const noexcept
{
    return sumInputStages(&audio::GainStage::clippedInFrames);
}

std::uint64_t AudioEngine::inputGainClippedFrames() const noexcept
{
    return sumInputStages(&audio::GainStage::clippedOutFrames);
}

std::uint64_t AudioEngine::outputGainClippedFrames() const noexcept
{
    return sumOutputStages(&audio::GainStage::clippedOutFrames);
}

std::uint64_t AudioEngine::nonFiniteInputFrames() const noexcept
{
    return sumInputStages(&audio::GainStage::nonFiniteInFrames);
}

bool AudioEngine::takeInputClipIndicator() noexcept
{
    bool any = false;

    for (auto& stage : inputStages_)
    {
        if (stage != nullptr && stage->takeClipIndicator())
            any = true;
    }

    return any;
}

bool AudioEngine::takeOutputClipIndicator() noexcept
{
    bool any = false;

    for (auto& stage : outputStages_)
    {
        if (stage != nullptr && stage->takeClipIndicator())
            any = true;
    }

    return any;
}

void AudioEngine::growGainStages(int inputChannels, int outputChannels)
{
    const auto grow = [](std::vector<std::unique_ptr<audio::GainStage>>& stages, int needed)
    {
        // Existing stages are kept, not rebuilt: they hold the operator's level and the
        // clipping history of the run so far.
        while (static_cast<int>(stages.size()) < needed)
            stages.push_back(std::make_unique<audio::GainStage>());
    };

    grow(inputStages_, inputChannels);
    grow(outputStages_, outputChannels);

    const std::size_t needed = static_cast<std::size_t>(inputChannels) * kGainChunkFrames;

    if (gainScratch_.size() < needed)
        gainScratch_.resize(needed, 0.0f);
}

void AudioEngine::applyGainToStages(int sampleRate)
{
    const int rampMs = gainRampMs_.load(std::memory_order_relaxed);

    for (auto& stage : inputStages_)
    {
        if (stage == nullptr)
            continue;

        stage->setGainDb(inputGainDb_.load(std::memory_order_relaxed));
        stage->setMuted(inputMuted_.load(std::memory_order_relaxed));
        stage->configure(sampleRate, rampMs);
    }

    for (auto& stage : outputStages_)
    {
        if (stage == nullptr)
            continue;

        stage->setGainDb(outputGainDb_.load(std::memory_order_relaxed));
        stage->setMuted(outputMuted_.load(std::memory_order_relaxed));
        stage->configure(sampleRate, rampMs);
    }
}

// -------------------------------------------------------------------------- realtime

void AudioEngine::processAudio(const float* const* input,
                              float* const* output,
                              int frameCount) noexcept
{
    // Realtime thread: fixed-size work only. No allocation, no logging, no network,
    // no filesystem, no locks. Relaxed atomics and memcpy are all that appears here.
    if (frameCount <= 0)
        return;   // an empty block is not work, and not a defect either: there are
                  // no frames to fill, so buffer content cannot reach an audience

    const std::size_t frames = static_cast<std::size_t>(frameCount);

    // Anything below this point is the backend breaking its contract: the interface
    // promises `inputChannels` readable input pointers and `outputChannels` writable
    // output pointers for frameCount frames. The engine reports it instead of
    // pretending it processed audio, and still leaves silence on the wire.
    if (output == nullptr)
    {
        malformedCallbacks_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (input == nullptr)
    {
        malformedCallbacks_.fetch_add(1, std::memory_order_relaxed);

        // Every promised output channel goes silent, not just the first: a
        // multi-output geometry leaving stale bytes on channel 1 is exactly how
        // "looks connected, plays something else" reaches the audience.
        silenceOutputs(output, outputChannels_.load(std::memory_order_relaxed), frames);

        return;
    }

    blocks_.fetch_add(1, std::memory_order_relaxed);
    frames_.fetch_add(static_cast<std::uint64_t>(frameCount), std::memory_order_relaxed);

    if (diagnostics_ != nullptr)
        diagnostics_->countAudioBlock(frameCount);

    const bool ready = pipelineReady_.load(std::memory_order_acquire);

    // ------------------------------------------------------------- input side
    if (ready)
    {
        const int channels = inputChannels_.load(std::memory_order_relaxed);
        const bool forwarding = consumerAttached_.load(std::memory_order_relaxed);

        for (int channel = 0; channel < channels; ++channel)
        {
            const float* source = input[channel];

            if (source == nullptr)
                continue;

            // SPEC "Audio Ring Buffer" puts Input Gain between the callback and the ring,
            // so the translator is handed the level the operator chose rather than the
            // level the console produced. The device pointer is const, so the block goes
            // through the preallocated scratch; a block bigger than one chunk is done in
            // chunks and counted, never truncated.
            audio::GainStage* stage = inputStages_[static_cast<std::size_t>(channel)].get();
            float* scratch = gainScratch_.data() + static_cast<std::size_t>(channel) * kGainChunkFrames;

            if (stage == nullptr)
                continue;   // growGainStages() sizes this with the channel count

            if (frames > kGainChunkFrames)
                oversized_.fetch_add(1, std::memory_order_relaxed);

            auto* ring = inputRings_[static_cast<std::size_t>(channel)].get();

            std::size_t offset = 0;

            while (offset < frames)
            {
                const std::size_t take = std::min<std::size_t>(kGainChunkFrames, frames - offset);

                stage->process(source + offset, scratch, take);

                // The meter reads the post-gain block, so the gain knob visibly moves the
                // meter. Whether the signal was already at full scale on the way in is the
                // stage's own count, which attenuation cannot hide.
                inputMeters_[static_cast<std::size_t>(channel)]->measure(scratch, take);

                inputCaptured_.fetch_add(static_cast<std::uint64_t>(take), std::memory_order_relaxed);

                if (!forwarding)
                {
                    // Nobody is draining the rings yet (no translation worker, no
                    // loopback). Counting this as an overrun would be a lie: nothing
                    // overflowed, there was simply no consumer.
                    inputDropped_.fetch_add(static_cast<std::uint64_t>(take), std::memory_order_relaxed);
                    offset += take;
                    continue;
                }

                const std::size_t written = ring != nullptr ? ring->write(scratch, take) : 0;

                inputForwarded_.fetch_add(static_cast<std::uint64_t>(written), std::memory_order_relaxed);

                if (written != take)
                {
                    overruns_.fetch_add(1, std::memory_order_relaxed);

                    // Counted at engine level on purpose: deactivate() destroys the ring
                    // objects, and a counter that reads through to them would silently
                    // report 0 after shutdown, exactly when the operator wants to see it.
                    ringDropped_.fetch_add(static_cast<std::uint64_t>(take - written), std::memory_order_relaxed);

                    if (diagnostics_ != nullptr)
                        diagnostics_->countOverrun();
                }

                offset += take;
            }
        }
    }

    // ------------------------------------------------------------ output side
    if (ready)
    {
        const int outChannels = outputChannels_.load(std::memory_order_relaxed);

        for (int channel = 0; channel < outChannels; ++channel)
        {
            float* destination = output[channel];

            if (destination == nullptr)
                continue;

            // The jitter buffer is the ONLY source of output audio, which is what makes
            // it impossible for the microphone to reach the audience by accident.
            const std::size_t taken = outputJitters_[static_cast<std::size_t>(channel)]->readOrSilence(destination, frames);

            if (taken != frames)
            {
                outputSilence_.fetch_add(static_cast<std::uint64_t>(frames - taken), std::memory_order_relaxed);
                underruns_.fetch_add(1, std::memory_order_relaxed);

                if (diagnostics_ != nullptr)
                    diagnostics_->countUnderrun();
            }

            // SPEC "Audio Pipeline" puts Output Gain after the jitter buffer and before
            // the wire, so the operator can level-match translated audio against the
            // source without touching what the translator was fed. In place, because this
            // buffer belongs to the callback. Applied before the meter on purpose: the
            // audience and the meter must see the same thing.
            audio::GainStage* stage = outputStages_[static_cast<std::size_t>(channel)].get();

            if (stage != nullptr)
                stage->process(destination, destination, frames);

            outputMeters_[static_cast<std::size_t>(channel)]->measure(destination, frames);
        }

        return;
    }

    // No pipeline: nothing was ever configured, so the count is 0 and only the
    // contract's first channel can be honestly answered - a real backend always
    // goes through activate(), which builds the pipeline before the device
    // starts; this branch exists for direct calls in tests. A callback landing
    // in the teardown window (ready already false, counts not yet zeroed)
    // silences the whole geometry it still advertises.
    silenceOutputs(output, outputChannels_.load(std::memory_order_relaxed), frames);
}

void AudioEngine::onAudioConfigurationChanged(int sampleRate, int bufferFrames)
{
    // Non-realtime: called right after the device is opened.
    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    bufferFrames_.store(bufferFrames, std::memory_order_relaxed);

    if (diagnostics_ != nullptr)
        diagnostics_->noteAudioGeometry(sampleRate, bufferFrames);
}

// -------------------------------------------------------------------------- readouts

audio::AudioRingBuffer* AudioEngine::inputRing(int channel) noexcept
{
    if (channel < 0 || static_cast<std::size_t>(channel) >= inputRings_.size())
        return nullptr;

    return inputRings_[static_cast<std::size_t>(channel)].get();
}

audio::AudioJitterBuffer* AudioEngine::outputJitter(int channel) noexcept
{
    if (channel < 0 || static_cast<std::size_t>(channel) >= outputJitters_.size())
        return nullptr;

    return outputJitters_[static_cast<std::size_t>(channel)].get();
}

const audio::LevelMeter* AudioEngine::inputMeter(int channel) const noexcept
{
    if (channel < 0 || static_cast<std::size_t>(channel) >= inputMeters_.size())
        return nullptr;

    return inputMeters_[static_cast<std::size_t>(channel)].get();
}

const audio::LevelMeter* AudioEngine::outputMeter(int channel) const noexcept
{
    if (channel < 0 || static_cast<std::size_t>(channel) >= outputMeters_.size())
        return nullptr;

    return outputMeters_[static_cast<std::size_t>(channel)].get();
}

std::size_t AudioEngine::jitterFillFrames() const noexcept
{
    if (outputJitters_.empty() || outputJitters_.front() == nullptr)
        return 0;

    return outputJitters_.front()->available();
}

} // namespace liveai
