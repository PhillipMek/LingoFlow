#include "Audio/Dev/SimulatedDeviceBackend.h"

#include <algorithm>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <system_error>
#include <utility>

#include "Utils/Log.h"

namespace liveai {
namespace audio {
namespace {

constexpr std::string_view kComponent = "dev";

constexpr double kTwoPi = 6.2831853071795864769252867665590057683943387987502;

/// A block counts as "late" when the pacing thread woke up more than this past
/// its deadline. Windows timer granularity lives around this value even on an
/// idle machine, so the threshold reports real trouble, not scheduler jitter.
constexpr auto kLateTolerance = std::chrono::milliseconds (2);

double dbToLinearLocal(double db) noexcept
{
    return std::pow (10.0, db / 20.0);
}

} // namespace

// ============================================================================ base

SimulatedDeviceBackend::SimulatedDeviceBackend(std::string recordPath)
    : recordPath_ (std::move (recordPath))
{
}

SimulatedDeviceBackend::~SimulatedDeviceBackend()
{
    if (state_ != BackendState::closed)
    {
        std::string ignored;
        stop (ignored);
        close();
    }
}

bool SimulatedDeviceBackend::open(IAudioProcessor& processor, const DeviceRequest& request, std::string& error)
{
    if (state_ != BackendState::closed)
    {
        error = "the simulated device is already open";
        return false;
    }

    if (request.bufferFrames <= 0)
    {
        error = "a simulated device needs a positive block size, got "
              + std::to_string (request.bufferFrames);
        return false;
    }

    int rate = 0;

    if (!openSource (request, rate, error))
        return false;

    if (rate <= 0)
    {
        error = "the developer source reported a sample rate of " + std::to_string (rate);
        closeSource();
        return false;
    }

    processor_ = &processor;

    capabilities_ = {};
    capabilities_.sampleRate = rate;
    capabilities_.minBufferFrames = request.bufferFrames;
    capabilities_.maxBufferFrames = request.bufferFrames;
    capabilities_.preferredBufferFrames = request.bufferFrames;
    capabilities_.inputChannels = 1;
    capabilities_.outputChannels = 1;
    capabilities_.preferredFormat = SampleFormat::float32;
    // Latency stays 0 and is rendered "not reported" by the 018 accounting: a
    // simulated device has no driver to answer that question, and pretending
    // it adds no delay would be the kind of number the task forbids.

    if (!recordPath_.empty() && !writer_.open (recordPath_, rate, error))
    {
        closeSource();
        processor_ = nullptr;
        return false;
    }

    state_ = BackendState::opened;
    log::info (kComponent, std::string ("developer source '") + std::string (name()) + "' opened at "
               + std::to_string (rate) + " Hz, block " + std::to_string (request.bufferFrames)
               + (recordPath_.empty() ? ", recording off" : ", recording to '" + recordPath_ + "'"));
    return true;
}

bool SimulatedDeviceBackend::start(std::string& error)
{
    if (state_ != BackendState::opened)
    {
        error = "the simulated device must be opened before it can start";
        return false;
    }

    const int frames = capabilities_.preferredBufferFrames;

    input_.assign (static_cast<std::size_t> (frames), 0.0f);
    output_.assign (static_cast<std::size_t> (frames), 0.0f);
    inputPointers_ = { input_.data() };
    outputPointers_ = { output_.data() };

    stopRequested_.store (false, std::memory_order_relaxed);
    deliveredBlocks_.store (0, std::memory_order_relaxed);
    lateBlocks_.store (0, std::memory_order_relaxed);

    try
    {
        thread_ = std::thread (&SimulatedDeviceBackend::run, this);
    }
    catch (const std::system_error& exception)
    {
        error = std::string ("could not start the simulated device thread: ") + exception.what();
        return false;
    }

    state_ = BackendState::running;
    return true;
}

bool SimulatedDeviceBackend::stop(std::string& error)
{
    if (state_ != BackendState::running)
        return true;   // idempotent, like every backend here

    stopRequested_.store (true, std::memory_order_relaxed);

    if (thread_.joinable())
        thread_.join();

    state_ = BackendState::opened;

    log::info (kComponent,
               std::string ("developer source '") + std::string (name()) + "' delivered "
                   + std::to_string (deliveredBlocks_.load()) + " blocks, "
                   + std::to_string (lateBlocks_.load()) + " were late (simulator pacing, not audio timing)");
    error.clear();
    return true;
}

void SimulatedDeviceBackend::close() noexcept
{
    if (state_ == BackendState::running)
    {
        std::string ignored;
        stop (ignored);
    }

    if (state_ == BackendState::closed)
        return;

    if (!recordPath_.empty())
    {
        std::string recordError;

        if (!writer_.close (recordError))
        {
            recordError_ = recordError;
            log::error (kComponent, "the rehearsal recording was not finalized: " + recordError);
        }
        else if (writer_.framesWritten() > 0)
        {
            log::info (kComponent, "rehearsal recording: " + std::to_string (writer_.framesWritten())
                                      + " frames written to '" + recordPath_ + "'");
        }
    }

    closeSource();
    processor_ = nullptr;
    state_ = BackendState::closed;
}

void SimulatedDeviceBackend::consumeOutput(const float* data, int frames)
{
    writer_.write (data, static_cast<std::uint64_t> (frames));
}

void SimulatedDeviceBackend::run()
{
    using clock = std::chrono::steady_clock;

    const int frames = capabilities_.preferredBufferFrames;
    const auto blockPeriod =
        std::chrono::microseconds ((frames * 1'000'000LL) / capabilities_.sampleRate);

    auto deadline = clock::now();

    while (!stopRequested_.load (std::memory_order_relaxed))
    {
        provideInput (input_.data(), frames);
        processor_->processAudio (inputPointers_.data(), outputPointers_.data(), frames);
        consumeOutput (output_.data(), frames);

        deliveredBlocks_.fetch_add (1, std::memory_order_relaxed);

        deadline += blockPeriod;
        const auto now = clock::now();

        if (now < deadline)
        {
            std::this_thread::sleep_until (deadline);
        }
        else if (now - deadline > kLateTolerance)
        {
            // The laptop could not keep pace (or slept). Reset the deadline:
            // a burst of catch-up blocks would flood the rings, and every late
            // block is counted, so this is visible, not swallowed.
            lateBlocks_.fetch_add (1, std::memory_order_relaxed);
            deadline = now;
        }
    }
}

// ============================================================================ WAV

WavFileAudioBackend::WavFileAudioBackend(std::string inputPath, std::string recordPath)
    : SimulatedDeviceBackend (std::move (recordPath)),
      inputPath_ (std::move (inputPath))
{
}

bool WavFileAudioBackend::openSource(const DeviceRequest& request, int& outSampleRate, std::string& error)
{
    if (!wavio::readMono (inputPath_, samples_, error))
        return false;

    if (samples_.sampleRate != request.sampleRate)
    {
        error = "the WAV file '" + inputPath_ + "' is " + std::to_string (samples_.sampleRate)
              + " Hz but the settings ask for " + std::to_string (request.sampleRate)
              + " Hz: set the sample rate to the file's rate (this product never resamples silently)";
        return false;
    }

    position_ = 0;
    wrappedOnce_ = false;
    outSampleRate = samples_.sampleRate;
    return true;
}

void WavFileAudioBackend::provideInput(float* dest, int frames)
{
    int filled = 0;

    while (filled < frames)
    {
        const std::size_t remaining = samples_.samples.size() - position_;
        const std::size_t wanted = static_cast<std::size_t> (frames - filled);
        const std::size_t takeCount = wanted < remaining ? wanted : remaining;

        std::copy_n (samples_.samples.begin() + static_cast<std::ptrdiff_t> (position_),
                     static_cast<std::ptrdiff_t> (takeCount), dest + filled);

        filled += static_cast<int> (takeCount);
        position_ += takeCount;

        if (position_ >= samples_.samples.size())
        {
            position_ = 0;

            if (!wrappedOnce_)
            {
                wrappedOnce_ = true;
                log::debug (kComponent, "WAV source reached the end of the file and loops");
            }
        }
    }
}

// ============================================================================ tone

TestToneAudioBackend::TestToneAudioBackend(double frequencyHz, double levelDb, std::string recordPath)
    : SimulatedDeviceBackend (std::move (recordPath)),
      frequencyHz_ (frequencyHz),
      levelDb_ (levelDb)
{
}

bool TestToneAudioBackend::openSource(const DeviceRequest& request, int& outSampleRate, std::string& error)
{
    if (frequencyHz_ <= 0.0 || frequencyHz_ >= request.sampleRate / 2.0)
    {
        error = "a " + std::to_string (frequencyHz_) + " Hz tone does not fit a "
              + std::to_string (request.sampleRate) + " Hz device (needs 0 < f < Nyquist)";
        return false;
    }

    phase_ = 0.0;
    phaseStep_ = kTwoPi * frequencyHz_ / static_cast<double> (request.sampleRate);
    amplitude_ = static_cast<float> (dbToLinearLocal (levelDb_));
    outSampleRate = request.sampleRate;
    return true;
}

void TestToneAudioBackend::provideInput(float* dest, int frames)
{
    // Phase-continuous across blocks: a break in the sine at a block boundary
    // would put clicks into the very signal used to judge the mix.
    for (int i = 0; i < frames; ++i)
    {
        dest[i] = amplitude_ * static_cast<float> (std::sin (phase_));
        phase_ += phaseStep_;

        if (phase_ >= kTwoPi)
            phase_ -= kTwoPi;
    }
}

} // namespace audio
} // namespace liveai
