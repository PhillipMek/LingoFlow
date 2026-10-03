#include "App/TranslationStreamer.h"

#include <algorithm>
#include <chrono>

#include "Diagnostics/DiagnosticsManager.h"
#include "Utils/Log.h"

namespace liveai {
namespace {

constexpr std::string_view kComponent = "translation";

/// One wake-up hands the backend at most this many frames. 4096 frames is ~85 ms
/// at 48 kHz - several device blocks, so a backlog is drained in one go and the
/// backend's own queue keeps the 200 ms append cadence (protocol section 7).
/// Allocated in start(), never touched by the realtime side.
constexpr int kScratchFrames = 4096;

constexpr int kMinPollIntervalMs = 1;
constexpr int kMaxPollIntervalMs = 50;

} // namespace

TranslationStreamer::TranslationStreamer(AudioEngine& engine,
                                         translation::ITranslationBackend& backend,
                                         DiagnosticsManager* diagnostics) noexcept
    : engine_(engine)
    , backend_(backend)
    , diagnostics_(diagnostics)
{
}

TranslationStreamer::~TranslationStreamer()
{
    stop();
}

void TranslationStreamer::setPollIntervalMs(int intervalMs) noexcept
{
    pollIntervalMs_.store(std::clamp(intervalMs, kMinPollIntervalMs, kMaxPollIntervalMs),
                          std::memory_order_relaxed);
}

bool TranslationStreamer::start(std::string& error)
{
    error.clear();

    if (running_.load(std::memory_order_relaxed))
    {
        error = "translation streaming is already running";
        return false;
    }

    if (!engine_.pipelineReady())
    {
        error = "translation streaming needs a running audio pipeline: activate the engine first";
        return false;
    }

    if (engine_.inputRing(0) == nullptr)
    {
        error = "translation streaming found no input channel to drain";
        return false;
    }

    try
    {
        scratch_.assign(static_cast<std::size_t>(kScratchFrames), 0.0f);

        engine_.attachInputConsumer();
        attached_.store(true, std::memory_order_release);

        stopRequested_.store(false, std::memory_order_relaxed);
        refusalLogged_.store(false, std::memory_order_relaxed);
        crashed_.store(false, std::memory_order_relaxed);
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread(&TranslationStreamer::run, this);
    }
    catch (const std::exception& exception)
    {
        engine_.detachInputConsumer();
        attached_.store(false, std::memory_order_release);
        running_.store(false, std::memory_order_relaxed);
        error = std::string("could not start the translation streaming worker: ") + exception.what();
        return false;
    }

    return true;
}

void TranslationStreamer::stop() noexcept
{
    stopRequested_.store(true, std::memory_order_relaxed);

    if (thread_.joinable())
        thread_.join();

    if (attached_.exchange(false, std::memory_order_acq_rel))
        engine_.detachInputConsumer();

    running_.store(false, std::memory_order_relaxed);
}

void TranslationStreamer::run() noexcept
{
    // Worker thread, not realtime: it allocates nothing here (the scratch came from
    // start()), but it may sleep, log and call into the backend's non-blocking
    // submitAudio(). What it must never do is hold up the audio callback, and it
    // cannot: the ring reads are the consumer end of a lock-free buffer that never
    // blocks. A backend that throws at this level is a defect, not an operating
    // state: log it at critical and let the worker leave - the engine's own
    // overflow accounting then tells the operator the capture is going nowhere.
    try
    {
        std::string lastRefusal;

        while (!stopRequested_.load(std::memory_order_relaxed))
        {
            auto* ring = engine_.inputRing(0);

            if (ring == nullptr)
            {
                // The pipeline can only be gone if deactivate() raced this thread -
                // the controller's stop order forbids it. Survive rather than crash.
                std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs_.load(std::memory_order_relaxed)));
                continue;
            }

            const std::size_t available = ring->readable();

            if (available == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs_.load(std::memory_order_relaxed)));
                continue;
            }

            const std::size_t take = std::min<std::size_t>(available, kScratchFrames);
            const std::size_t got = ring->read(scratch_.data(), take);

            if (got == 0)
                continue;

            const std::uint64_t frames = static_cast<std::uint64_t>(got);

            std::string error;

            if (backend_.submitAudio(scratch_.data(), static_cast<int>(got), error))
            {
                submittedFrames_.fetch_add(frames, std::memory_order_relaxed);

                if (diagnostics_ != nullptr)
                    diagnostics_->countTranslationSubmittedFrames(frames);

                refusalLogged_.store(false, std::memory_order_relaxed);
                continue;
            }

            // Refused: the frames were already consumed from the ring. That IS the
            // gap policy - drop and count, resume aligned with the room.
            gapFrames_.fetch_add(frames, std::memory_order_relaxed);

            if (diagnostics_ != nullptr)
                diagnostics_->countTranslationGapFrames(frames);

            if (error != lastRefusal)
            {
                lastRefusal = error;
                log::warning(kComponent,
                             "capture refused by the translation backend (" + std::to_string(frames)
                                 + " frames dropped per the gap policy): " + error);
            }
        }
    }
    catch (const std::exception& exception)
    {
        crashed_.store(true, std::memory_order_relaxed);
        running_.store(false, std::memory_order_relaxed);  ///< this worker is no longer streaming
        log::critical(kComponent,
                      std::string("translation streaming worker died: ") + exception.what());
    }
    catch (...)
    {
        crashed_.store(true, std::memory_order_relaxed);
        running_.store(false, std::memory_order_relaxed);
        log::critical(kComponent, "translation streaming worker died: unknown exception");
    }
}

} // namespace liveai
