// Code review P1 (2026-10-05): the OpenAI receiver thread feeds the jitter
// buffer AND ingests transcript events; the pipeline listener ran NDI SDK calls
// on that thread. NdiDispatch moves every transport call to its own worker, so
// the producer only ever enqueues. These tests fake a transport that STOPS
// inside publish (the stall that made the review P1) and prove: the caller is
// never the one waiting, the newest caption survives pressure, and the numbers
// tell the difference between a chosen drop and a transport refusal.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "NDI/NdiDispatch.h"

using liveai::ndi::NdiDispatch;
using liveai::ndi::OutputState;
using liveai::ndi::SubtitleFrame;

namespace {

/// A subtitle transport whose publish() can be frozen inside the fake "SDK",
/// with delivered frames recorded in order.
class GatedNdi final : public liveai::ndi::INdiOutput
{
public:
    std::string_view name() const noexcept override { return "gated"; }
    OutputState state() const noexcept override { return state_.load(std::memory_order_relaxed); }
    std::uint64_t publishedFrames() const noexcept override { return delivered_.size(); }

    bool start(std::string_view streamName, std::string& error) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (failStart_)
        {
            error = "gated: start refused";
            return false;
        }
        stream_ = std::string(streamName);
        started_ = true;
        state_.store(OutputState::ready, std::memory_order_relaxed);
        return true;
    }

    void stop() noexcept override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
        state_.store(OutputState::disabled, std::memory_order_relaxed);
    }

    bool publish(const SubtitleFrame& frame, std::string& error) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!started_)
        {
            error = "gated: not started";
            return false;
        }

        inFlight_ = true;
        cv_.notify_all();
        if (blockPublish_)
            cv_.wait(lock, [this] { return !blockPublish_; });   // the stall

        if (refuseNext_)
        {
            refuseNext_ = false;
            inFlight_ = false;
            error = "gated: transport refused";
            return false;
        }

        delivered_.push_back(frame);
        inFlight_ = false;
        cv_.notify_all();
        return true;
    }

    // ---- test controls -----------------------------------------------------
    void blockPublish()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        blockPublish_ = true;
    }

    void releasePublish()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        blockPublish_ = false;
        cv_.notify_all();
    }

    void refuseNextPublish()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        refuseNext_ = true;
    }

    void failStart()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        failStart_ = true;
    }

    bool inFlight()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inFlight_;
    }

    std::vector<SubtitleFrame> delivered()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return delivered_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool started_ = false;
    bool blockPublish_ = false;
    bool refuseNext_ = false;
    bool failStart_ = false;
    bool inFlight_ = false;
    std::string stream_;
    std::vector<SubtitleFrame> delivered_;
    std::atomic<OutputState> state_{ OutputState::disabled };
};

SubtitleFrame frame(const char* text, long long sequence, bool final = false)
{
    SubtitleFrame f;
    f.text = text;
    f.final = final;
    f.sequence = sequence;
    return f;
}

/// Poll-with-deadline: liveness helper for the test thread only (never part of
/// the product path). 5 s covers a slow CI box without masking a real wedge.
template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

} // namespace

TEST_CASE("NdiDispatch: the producer returns while the transport is stalled",
          "[ndi][dispatch][review]")
{
    auto gated = std::make_unique<GatedNdi>();
    GatedNdi* fake = gated.get();
    NdiDispatch dispatch(std::move(gated), 3);

    std::string error;
    REQUIRE(dispatch.start("LingoFlow EN->RU", error));

    fake->blockPublish();
    CHECK(dispatch.publish(frame("first", 1), error));        // accepted, SDK never touched here
    REQUIRE(waitFor([fake] { return fake->inFlight(); }));    // the WORKER is inside the stall
    CHECK(fake->delivered().empty());

    // The receiver-thread view: publish keeps answering "queued" under pressure.
    // Capacity 3 counts what the QUEUE holds (f1 is with the worker, not the
    // queue), so the fourth queued frame is the one that tips the drop.
    CHECK(dispatch.publish(frame("second", 2), error));
    CHECK(dispatch.publish(frame("third", 3, true), error));
    CHECK(dispatch.publish(frame("fourth", 4, true), error));
    CHECK(dispatch.publish(frame("fifth", 5, true), error));  // queue full: oldest QUEUED frame drops
    CHECK(dispatch.droppedFrames() >= 1);

    fake->releasePublish();
    REQUIRE(waitFor([&] { return dispatch.publishedFrames() == 4; }));

    const auto got = fake->delivered();
    CHECK(got[0].sequence == 1);                    // the in-flight one, first as promised
    CHECK(got.back().sequence == 5);                // the freshest caption survives
    CHECK(got[1].sequence == 3);                    // "second" was the chosen sacrifice
    CHECK(got[1].final == true);                    // frame fields rode along unchanged
    CHECK(got.back().text == "fifth");
    CHECK(dispatch.publishedFrames() == 4);
    CHECK(dispatch.droppedFrames() == 1);

    dispatch.stop();
    CHECK(dispatch.state() == OutputState::disabled);
}

TEST_CASE("NdiDispatch: a refused frame is a worker error, never a caller stall",
          "[ndi][dispatch][review]")
{
    auto gated = std::make_unique<GatedNdi>();
    GatedNdi* fake = gated.get();
    NdiDispatch dispatch(std::move(gated), 2);

    std::string error;
    REQUIRE(dispatch.start("LingoFlow EN->RU", error));

    fake->refuseNextPublish();
    CHECK(dispatch.publish(frame("doomed", 1), error));       // caller side: accepted...
    REQUIRE(waitFor([&] { return dispatch.publishErrors() == 1; }));   // ...and the worker owns the failure

    CHECK(dispatch.publishedFrames() == 0);
    CHECK(dispatch.droppedFrames() == 0);   // a refusal is counted as one, a drop as the other
    CHECK(fake->delivered().empty());

    // The producer is still free to move on:
    CHECK(dispatch.publish(frame("next", 2, true), error));
    REQUIRE(waitFor([&] { return dispatch.publishedFrames() == 1; }));

    dispatch.stop();
}

TEST_CASE("NdiDispatch: start failure passes straight through, publish stays honest",
          "[ndi][dispatch][review]")
{
    auto gated = std::make_unique<GatedNdi>();
    GatedNdi* fake = gated.get();
    NdiDispatch dispatch(std::move(gated), 2);

    fake->failStart();
    std::string error;
    CHECK_FALSE(dispatch.start("LingoFlow EN->RU", error));
    CHECK(error == "gated: start refused");
    CHECK(dispatch.state() == OutputState::disabled);

    CHECK_FALSE(dispatch.publish(frame("orphan", 1), error));   // never started: a clear NO, not silence
    CHECK(error == "NDI dispatch is not started");
    CHECK(dispatch.publishedFrames() == 0);
    CHECK(dispatch.droppedFrames() == 0);
}

TEST_CASE("NdiDispatch: stop drops what is queued and closes the door behind it",
          "[ndi][dispatch][review]")
{
    auto gated = std::make_unique<GatedNdi>();
    GatedNdi* fake = gated.get();
    NdiDispatch dispatch(std::move(gated), 8);

    std::string error;
    REQUIRE(dispatch.start("LingoFlow EN->RU", error));

    fake->blockPublish();
    CHECK(dispatch.publish(frame("in-flight", 1), error));
    REQUIRE(waitFor([fake] { return fake->inFlight(); }));
    CHECK(dispatch.publish(frame("queued-a", 2), error));
    CHECK(dispatch.publish(frame("queued-b", 3), error));

    dispatch.stop();

    fake->releasePublish();   // the accepted one is allowed to finish
    REQUIRE(waitFor([fake] { return fake->delivered().size() == 1; }));
    CHECK(fake->delivered()[0].sequence == 1);          // in-flight completes - it was accepted
    CHECK(dispatch.droppedFrames() == 2);               // queued ones are counted, not swallowed
    CHECK_FALSE(dispatch.publish(frame("after", 4), error));   // door closed
    CHECK(error == "NDI dispatch is not started");

    // destructor joins the worker even after stop() - the test completing IS the assertion
}

TEST_CASE("NdiDispatch: order survives pressure - the worker is the only transport caller",
          "[ndi][dispatch][review]")
{
    auto gated = std::make_unique<GatedNdi>();
    GatedNdi* fake = gated.get();
    NdiDispatch dispatch(std::move(gated), 100);

    std::string error;
    REQUIRE(dispatch.start("LingoFlow EN->RU", error));

    // No gating: publish continuously, then verify the worker delivered every
    // frame in the exact produced order - one consumer thread, one queue.
    constexpr long long kCount = 60;
    for (long long i = 1; i <= kCount; ++i)
        REQUIRE(dispatch.publish(frame("caption", i, i == kCount), error));

    REQUIRE(waitFor([&] { return dispatch.publishedFrames() == static_cast<std::uint64_t>(kCount); }));

    const auto got = fake->delivered();
    REQUIRE(static_cast<long long>(got.size()) == kCount);
    for (long long i = 0; i < kCount; ++i)
    {
        CHECK(got[static_cast<std::size_t>(i)].sequence == i + 1);
        CHECK(got[static_cast<std::size_t>(i)].final == (i + 1 == kCount));
    }
    CHECK(dispatch.droppedFrames() == 0);
    CHECK(dispatch.publishErrors() == 0);

    dispatch.stop();
}
