#include "NDI/NdiDispatch.h"

#include <utility>

#include "Utils/Log.h"

namespace liveai {
namespace ndi {
namespace {

constexpr std::string_view kComponent = "ndi";

} // namespace

NdiDispatch::NdiDispatch(std::unique_ptr<INdiOutput> downstream, std::size_t queueCapacity)
    : downstream_(std::move(downstream))
    , name_(downstream_ != nullptr ? std::string(downstream_->name()) + " (dispatched)" : "ndi-dispatch")
    , capacity_(queueCapacity > 0 ? queueCapacity : 1)
{
    worker_ = std::thread([this] { workerLoop(); });
}

NdiDispatch::~NdiDispatch()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        exitRequested_ = true;

        // Whatever is still queued will never be watched as a caption again;
        // it is counted, not forgotten. Subtitles are the least important bytes
        // in the process at shutdown - the worker must not hang on them, and
        // it must not hang the app either.
        dropped_.fetch_add(static_cast<std::uint64_t>(queue_.size()), std::memory_order_relaxed);
        queue_.clear();
    }
    cv_.notify_all();

    if (worker_.joinable())
        worker_.join();

    if (downstream_ != nullptr)
        downstream_->stop();   // idempotent, and the worker is gone: sole thread now
}

bool NdiDispatch::start(std::string_view streamName, std::string& error)
{
    if (downstream_ == nullptr)
    {
        error = "NDI dispatch wraps no output";
        return false;
    }

    // Control thread (startup/settings): a network registration may take a
    // moment here, and that is the whole point of this class - it is NEVER the
    // receiver thread. A GUI waiting for a configuration answer is a bounded,
    // honest cost; a stalled show is not.
    const bool ok = downstream_->start(streamName, error);
    accepting_.store(ok, std::memory_order_relaxed);
    firstTransportError_.store(false, std::memory_order_relaxed);
    state_.store(downstream_->state(), std::memory_order_relaxed);
    return ok;
}

void NdiDispatch::stop() noexcept
{
    accepting_.store(false, std::memory_order_relaxed);

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        dropped_.fetch_add(static_cast<std::uint64_t>(queue_.size()), std::memory_order_relaxed);
        queue_.clear();
    }

    if (downstream_ != nullptr)
    {
        downstream_->stop();
        state_.store(downstream_->state(), std::memory_order_relaxed);
    }
}

bool NdiDispatch::publish(const SubtitleFrame& frame, std::string& error)
{
    if (!accepting_.load(std::memory_order_relaxed))
    {
        error = "NDI dispatch is not started";
        return false;
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (exitRequested_)
        {
            error = "NDI dispatch is shutting down";
            return false;
        }

        if (queue_.size() >= capacity_)
        {
            // Drop the OLDEST queued caption for the newest one: the audience
            // can never read the stale text anyway, and the freshest snapshot
            // is the only one that can still land on the screen.
            queue_.pop_front();
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.push_back(frame);
    }

    cv_.notify_one();
    return true;
}

void NdiDispatch::workerLoop()
{
    for (;;)
    {
        SubtitleFrame frame;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return exitRequested_ || !queue_.empty(); });
            if (exitRequested_)
                return;   // the destructor counted and cleared whatever remained

            frame = std::move(queue_.front());
            queue_.pop_front();
        }

        if (!accepting_.load(std::memory_order_relaxed))
        {
            // Stopped between enqueue and delivery: an intended loss, counted
            // as a drop, never as a transport error - the log must not cry
            // wolf at a settings change.
            dropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        std::string error;
        if (downstream_->publish(frame, error))
        {
            delivered_.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            workerErrors_.fetch_add(1, std::memory_order_relaxed);
            if (!error.empty() && !firstTransportError_.exchange(true, std::memory_order_relaxed))
                log::warning(kComponent, "NDI worker: transport refused a caption: " + error);
        }

        state_.store(downstream_->state(), std::memory_order_relaxed);
    }
}

} // namespace ndi
} // namespace liveai
