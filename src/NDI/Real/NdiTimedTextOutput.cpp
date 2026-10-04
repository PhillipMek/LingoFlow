#include "NDI/Real/NdiTimedTextOutput.h"

#include <mutex>
#include <string>
#include <utility>

#include "NDI/NdiTimedText.h"
#include "NDI/Real/NdiRuntime.h"

namespace liveai {
namespace ndi {
namespace real {

namespace {

std::mutex transportMutex;   ///< one lock for every sender mutation/poll in the process

bool pollHasConsumer(void* sender)
{
    const NdiRuntime& runtime = NdiRuntime::instance();
    if (!runtime.available() || sender == nullptr)
        return false;

    // Timeout 0: a poll, never a wait. This runs on the text worker thread at
    // publish time; a blocking query would be an NDI dependency on the show's
    // clock, and that is exactly the dependency AGENTS.md forbids.
    return runtime.api().send_get_no_connections(
               static_cast<NDIlib_send_instance_t>(sender), 0) > 0;
}

} // namespace

bool NdiTimedTextOutput::start(std::string_view streamName, std::string& error)
{
    const NdiRuntime& runtime = NdiRuntime::instance();

    if (!runtime.available())
    {
        error = "NDI output cannot start: " + runtime.loadError();
        return false;
    }

    std::lock_guard lock(transportMutex);

    if (sender_ != nullptr)
    {
        error = "NDI output is already running as '" + streamName_ + "' - stop it first";
        return false;
    }

    streamName_ = std::string(streamName.empty() ? std::string_view("LingoFlow") : streamName);

    NDIlib_send_create_t create {};
    create.p_ndi_name = streamName_.c_str();
    create.p_groups = nullptr;                       // default group
    create.clock_video = true;                       // irrelevant without media frames;
    create.clock_audio = true;                       // kept at the SDK default values

    // The SDK copies the name at creation (documented behavior of
    // NDIlib_send_create); streamName_ lives on regardless.
    sender_ = runtime.api().send_create(&create);

    if (sender_ == nullptr)
    {
        error = "NDIlib_send_create refused the source name '" + streamName_ +
                "' (name collision on this network, or the runtime declined)";
        state_.store(OutputState::disabled, std::memory_order_relaxed);
        return false;
    }

    published_.store(0, std::memory_order_relaxed);
    state_.store(OutputState::ready, std::memory_order_relaxed);
    return true;
}

void NdiTimedTextOutput::stop() noexcept
{
    // The interface method is noexcept, so the lock is acquired the way the rest
    // of this product does it: contention here means a stuck worker, and
    // process-mutex exhaustion is not a condition this can recover from any
    // better by throwing - it would std::terminate the same way. Kept honest in
    // the comment rather than hidden in a try/catch that swallows.
    std::lock_guard lock(transportMutex);

    if (sender_ == nullptr)
    {
        state_.store(OutputState::disabled, std::memory_order_relaxed);
        return;
    }

    NdiRuntime::instance().api().send_destroy(static_cast<NDIlib_send_instance_t>(sender_));
    sender_ = nullptr;
    state_.store(OutputState::disabled, std::memory_order_relaxed);
}

bool NdiTimedTextOutput::publish(const SubtitleFrame& frame, std::string& error)
{
    std::lock_guard lock(transportMutex);

    if (sender_ == nullptr)
    {
        error = "NDI output is not started";
        return false;
    }

    if (frame.text.empty())
    {
        // Nothing to show: accepted as a no-op, not an error. The text pipeline
        // never emits empty captions (task 013), so this guard only protects
        // the seam against a future caller - it must not be counted as a failure.
        return true;
    }

    const std::string document = buildTimedTextDocument(frame);

    NDIlib_metadata_frame_t metadata {};
    metadata.length = static_cast<int>(document.size()) + 1;   // includes the terminator
    metadata.timecode = NDIlib_send_timecode_synthesize;       // the SDK's own clock story
    metadata.p_data = const_cast<char*>(document.c_str());     // read synchronously

    // Void return: no per-frame truth is available here (see header). The call
    // itself is the documented fast submit; connection state is polled after.
    NdiRuntime::instance().api().send_send_metadata(
        static_cast<NDIlib_send_instance_t>(sender_), &metadata);

    published_.fetch_add(1, std::memory_order_relaxed);

    state_.store(pollHasConsumer(sender_) ? OutputState::publishing : OutputState::ready,
                 std::memory_order_relaxed);
    return true;
}

} // namespace real
} // namespace ndi
} // namespace liveai
