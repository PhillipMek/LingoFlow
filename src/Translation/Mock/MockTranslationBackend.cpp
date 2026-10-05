#include "Translation/Mock/MockTranslationBackend.h"

#include <chrono>
#include <format>
#include <system_error>
#include <utility>

#include "Utils/Log.h"

namespace liveai {
namespace translation {
namespace {

constexpr std::string_view kComponent = "translation";

using clock = std::chrono::steady_clock;

} // namespace

MockTranslationBackend::MockTranslationBackend(Options options)
    : options_ (std::move (options))
{
}

MockTranslationBackend::~MockTranslationBackend()
{
    closeSession();
}

bool MockTranslationBackend::openSession(const SessionRequest& request, std::string& error)
{
    // Rule 1: a session without a sink is refused, not "half-open".
    if (sink_ == nullptr)
    {
        error = "mock backend has no sink - refusing to open a session nobody would hear";
        return false;
    }

    // Rule 2: opening over a live or faulted session changes nothing and reports nothing.
    if (state_.load(std::memory_order_relaxed) != SessionState::closed)
    {
        error = "a mock session is already open; close it first";
        return false;
    }

    // The mock echoes at the request's rate and does not resample - so the two
    // rates must be the ones it can honour, or the session is refused honestly
    // (rule 3: refused, state stays closed, nothing reported).
    if (request.inputSampleRate <= 0 || request.outputSampleRate <= 0)
    {
        error = "mock backend needs both sample rates (got " + std::to_string (request.inputSampleRate)
              + " in / " + std::to_string (request.outputSampleRate) + " out)";
        return false;
    }

    if (request.inputSampleRate != request.outputSampleRate)
    {
        error = "mock backend echoes at one rate and cannot resample (in "
              + std::to_string (request.inputSampleRate) + " Hz, out "
              + std::to_string (request.outputSampleRate) + " Hz)";
        return false;
    }

    request_ = request;

    sink_->onSessionStateChanged(SessionState::connecting);
    state_.store(SessionState::connecting, std::memory_order_relaxed);

    submittedFrames_.store(0, std::memory_order_relaxed);
    deliveredFrames_.store(0, std::memory_order_relaxed);
    finalsEmitted_.store(0, std::memory_order_relaxed);
    utteranceFrames_ = 0;

    running_.store(true, std::memory_order_relaxed);

    try
    {
        thread_ = std::thread(&MockTranslationBackend::workerLoop, this);
    }
    catch (const std::system_error& exception)
    {
        running_.store(false, std::memory_order_relaxed);
        error = std::string("could not start the mock delivery worker: ") + exception.what();
        state_.store(SessionState::closed, std::memory_order_relaxed);
        sink_->onSessionStateChanged(SessionState::closed);
        return false;
    }

    state_.store(SessionState::connected, std::memory_order_relaxed);
    sink_->onSessionStateChanged(SessionState::connected);

    log::warning(kComponent,
                 "MOCK translation session opened (developer mode): audio is echoed after "
                 + std::to_string(options_.latencyMs)
                 + " ms and NO real translation runs - this is not a provider session");

    return true;
}

bool MockTranslationBackend::submitAudio(const float* samples, int frameCount, std::string& error)
{
    if (frameCount <= 0 || samples == nullptr)
    {
        error = "mock backend received an empty audio block";
        return false;
    }

    // Rule 4: accepted only while connected; a refused submit is not a fault.
    if (state_.load(std::memory_order_relaxed) != SessionState::connected)
    {
        error = "mock session is not connected";
        return false;
    }

    Chunk chunk;
    chunk.samples.assign(samples, samples + frameCount);
    chunk.arrival = clock::now();

    {
        const std::lock_guard lock(queueMutex_);
        lastArrival_ = chunk.arrival;   // utterance-end timing counts submissions,
                                        // not polls - see workerLoop
        queue_.push_back(std::move(chunk));
        submittedFrames_.fetch_add(static_cast<std::uint64_t>(frameCount), std::memory_order_relaxed);
    }

    return true;
}

void MockTranslationBackend::closeSession() noexcept
{
    // Rule 5: after this returns there are no sink callbacks, and in-flight
    // worker work is joined - the queued-but-undelivered echo is DROPPED,
    // because delivering it after a deliberate close would be the "late audio
    // from a dead session" class of bug the contract names.
    const bool wasOpen = state_.load(std::memory_order_relaxed) != SessionState::closed;

    {
        const std::lock_guard lock(queueMutex_);
        running_.store(false, std::memory_order_relaxed);
        queue_.clear();
    }

    if (thread_.joinable())
        thread_.join();

    if (!wasOpen)
        return;

    ITranslationSink* sink = sink_;

    if (utteranceFrames_ > 0 && sink != nullptr)
    {
        sink->onFinalText(std::format("{} final: {} s echoed (session closed)",
                                      options_.marker,
                                      static_cast<double>(utteranceFrames_)
                                          / (request_.inputSampleRate > 0 ? request_.inputSampleRate : 1)));
        finalsEmitted_.fetch_add(1, std::memory_order_relaxed);
    }

    utteranceFrames_ = 0;

    state_.store(SessionState::closed, std::memory_order_relaxed);

    if (sink != nullptr)
        sink->onSessionStateChanged(SessionState::closed);

    // Rule 6: per-session state is reset - the next openSession() is a fresh mock.
    request_ = {};
}

void MockTranslationBackend::workerLoop()
{
    ITranslationSink* sink = sink_;
    const auto rate = request_.outputSampleRate;

    for (;;)
    {
        // Drain everything that has "travelled" its latency. Ownership of the
        // samples moves here so the deliveries happen outside the lock.
        std::vector<Chunk> due;

        {
            const std::lock_guard lock(queueMutex_);

            if (!running_.load(std::memory_order_relaxed))
                return;   // closeSession() joined us; no callbacks from here on

            const auto now = clock::now();

            while (!queue_.empty()
                   && now - queue_.front().arrival >= std::chrono::milliseconds(options_.latencyMs))
            {
                due.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }

        for (const auto& chunk : due)
        {
            sink->onTranslatedAudio(chunk.samples.data(), static_cast<int>(chunk.samples.size()), rate);

            deliveredFrames_.fetch_add(chunk.samples.size(), std::memory_order_relaxed);
            utteranceFrames_ += chunk.samples.size();

            // Whole-line snapshots per task 013's meaning of partial text: every
            // call carries the entire current line, never a fragment of it.
            sink->onPartialText(std::format("{} line: {:.1f} s of audio echoed (no model ran)",
                                            options_.marker,
                                            static_cast<double>(utteranceFrames_) / rate));
        }

        // An utterance ends when nothing has been submitted for a while and the
        // queue has drained - the same shape the real backend's speech stop has,
        // so TextPipeline's settle behaviour is exercised identically.
        bool endOfUtterance = false;

        {
            const std::lock_guard lock(queueMutex_);

            const bool idle = queue_.empty() && utteranceFrames_ > 0
                              && (clock::now() - lastArrival_) >= std::chrono::milliseconds(options_.utteranceEndMs);

            endOfUtterance = idle;
        }

        if (endOfUtterance)
        {
            const std::uint64_t frames = utteranceFrames_;
            utteranceFrames_ = 0;

            sink->onFinalText(std::format("{} final: {:.1f} s echoed (no model ran)",
                                          options_.marker,
                                          static_cast<double>(frames) / rate));
            finalsEmitted_.fetch_add(1, std::memory_order_relaxed);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace translation
} // namespace liveai
