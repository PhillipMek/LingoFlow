#pragma once
//
// NdiDispatch - the NDI worker thread that keeps subtitle transport off the
// threads that matter (the project rules lists "NDI thread"; the project rules: "NDI
// failure must not stop translation audio"; An earlier review, 2026-10-05).
//
// The chain that made it necessary: the OpenAI receiver thread is this
// product's single consumer of BOTH translated audio and translated text.
// TextPipeline's listener used to run synchronously on that thread; the
// controller's listener called INdiOutput::publish; the real output called the
// NDI SDK under its transport mutex - and stop() from the settings thread held
// that same mutex across blocking create/destroy calls. Any stall inside the
// transport therefore stalled the receiver thread, audio deltas stopped
// feeding the jitter buffer, and a subtitle outage could take the translation
// down with it. INdiOutput always promised "non-blocking publish" and
// "drop-on-pressure"; this adapter is the structure that makes those promises
// true regardless of what the SDK decides to do. (TextPipeline later moved its
// listener off the ingesting thread as well - An earlier review, 2026-10-05, with
// its own dispatch worker - but the SDK-blocking argument never depended on
// which caller thread arrives, only on this queue being in front of the SDK.)
//
// Shape:
//   * publish() on a caller thread: enqueue into a bounded queue and return.
//     Full queue drops the OLDEST queued frame - an unread stale caption is
//     worth less than the fresh one the audience has not seen - and counts it
//     in droppedFrames(). It never touches the transport.
//   * one worker thread owns every downstream publish() call, in order.
//   * start()/stop() stay on the control thread (settings/GUI): they are
//     configuration feedback, never the realtime path, and the receiver thread
//     is never a caller. Blocking here stalls a settings dialog, not a show -
//     the worker keeps forwarding captions meanwhile or queues drops honestly.
//
// If the transport wedges: captions are lost (counted, exported, logged), the
// receiver thread keeps moving, the audience keeps hearing the translation.
//
// Realtime: NOT the audio callback, and nothing on the audio path calls into
// it; caller-side cost is a mutex, a bounded deque move and a condition
// variable notify - the same pattern as the translation streamer's queue.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "NDI/INdiOutput.h"

namespace liveai {
namespace ndi {

class NdiDispatch final : public INdiOutput
{
public:
    /// Wraps the transport to which all publishing is deferred. Ownership of
    /// `downstream` transfers; it is touched only from the worker thread and,
    /// for control operations (start/stop/state/name), from the caller threads
    /// documented above.
    explicit NdiDispatch(std::unique_ptr<INdiOutput> downstream,
                         std::size_t queueCapacity = 64);

    /// Stops accepting, releases the worker (any in-flight SDK call is allowed
    /// to finish first; whatever remains queued is dropped, counted, and gone -
    /// a shutdown never hangs on subtitles), then stops the transport.
    ~NdiDispatch() override;

    NdiDispatch(const NdiDispatch&) = delete;
    NdiDispatch& operator=(const NdiDispatch&) = delete;

    std::string_view name() const noexcept override { return name_; }
    OutputState state() const noexcept override { return state_.load(std::memory_order_relaxed); }

    bool start(std::string_view streamName, std::string& error) override;
    void stop() noexcept override;

    /// Enqueue and return: true = queued for the worker. A queued frame can
    /// still be lost later (transport failure, shutdown drain); those losses
    /// appear in droppedFrames()/publishErrors(), never back in the caller's
    /// hands, because waiting for them is exactly the stall this class removes.
    bool publish(const SubtitleFrame& frame, std::string& error) override;

    std::uint64_t publishedFrames() const noexcept override { return delivered_.load(std::memory_order_relaxed); }
    std::uint64_t droppedFrames() const noexcept override { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t publishErrors() const noexcept override { return workerErrors_.load(std::memory_order_relaxed); }

private:
    void workerLoop();

    std::unique_ptr<INdiOutput> downstream_;
    std::string name_;              ///< captured at construction; served without touching downstream_

    std::size_t capacity_;

    mutable std::mutex mutex_;      ///< guards queue_ + exitRequested_
    std::condition_variable cv_;    ///< wakes the worker
    std::deque<SubtitleFrame> queue_;
    bool exitRequested_ = false;

    std::atomic<bool> accepting_{ false };
    std::atomic<bool> firstTransportError_{ false };  ///< log the refused caption once per start
    std::atomic<OutputState> state_{ OutputState::disabled };
    std::atomic<std::uint64_t> delivered_{ 0 };
    std::atomic<std::uint64_t> dropped_{ 0 };
    std::atomic<std::uint64_t> workerErrors_{ 0 };

    std::thread worker_;
};

} // namespace ndi
} // namespace liveai
