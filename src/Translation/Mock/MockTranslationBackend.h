#pragma once
//
// MockTranslationBackend - the behavioural offline translator for developer mode
// (task 019: "mock backend", "allow operation without ... OpenAI").
//
// What it is: a full citizen of the task 007 contract. It honours the lifecycle
// rules 1-6 exactly as the real backend must (the contract tests for it mirror
// the supervisor's), takes a SessionRequest, streams audio in, and hands
// translated audio and whole-line text snapshots back on a worker thread after
// a configurable delay. The "translation" it performs is an echo with a label:
// what goes in comes back, and every text event says "mock" in words, because
// a mock that produces plausible-looking translations is the one thing this
// product must never be able to confuse with the real service (AGENTS.md 19).
//
// Why it is worth shipping inside the binary:
//   * the whole pipeline - capture worker, ring buffers, jitter pre-roll,
//     sink acceptance, counters, the task 018 in-flight backlog - becomes
//     runnable and checkable on a laptop with no ASIO device and no API key;
//   * its configured delay is a KNOWN one, so the in-flight row of the latency
//     accounting can be verified against an artificial ground truth - the only
//     honest way to test that measurement mechanism without a venue;
//   * at a venue without internet it keeps the audio chain warm (gain staging,
//     metering, recording) while clearly announcing that nothing is translated.
//
// Isolation: this class is only ever constructed when the developer plan says
// mockTranslation (App/DeveloperMode.h); production defaults keep it dead
// code-path. The name() is the truth on every screen that shows it.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Translation/ITranslationBackend.h"

namespace liveai {
namespace translation {

class MockTranslationBackend final : public ITranslationBackend
{
public:
    struct Options
    {
        int latencyMs = 300;        ///< how long echoed audio "travels" before delivery
        int utteranceEndMs = 400;   ///< silence after which the open mock line is finalised
        std::string marker = "mock";   ///< every text event starts with it, on purpose
    };

    MockTranslationBackend() = default;
    explicit MockTranslationBackend(Options options);
    ~MockTranslationBackend() override;

    std::string_view name() const noexcept override { return "Mock echo (developer mode)"; }
    SessionState state() const noexcept override { return state_.load(std::memory_order_relaxed); }

    void setSink(ITranslationSink& sink) noexcept override { sink_ = &sink; }

    bool openSession(const SessionRequest& request, std::string& error) override;
    bool submitAudio(const float* samples, int frameCount, std::string& error) override;
    void closeSession() noexcept override;

    // ------------------------------------------------------------- evidence counters
    std::uint64_t submittedFrames() const noexcept { return submittedFrames_.load(std::memory_order_relaxed); }
    std::uint64_t deliveredFrames() const noexcept { return deliveredFrames_.load(std::memory_order_relaxed); }
    int finalsEmitted() const noexcept { return finalsEmitted_.load(std::memory_order_relaxed); }
    int latencyMs() const noexcept { return options_.latencyMs; }

private:
    struct Chunk
    {
        std::vector<float> samples;
        std::chrono::steady_clock::time_point arrival;
    };

    void workerLoop();

    Options options_;
    ITranslationSink* sink_ = nullptr;
    SessionRequest request_;
    std::atomic<SessionState> state_ { SessionState::closed };

    std::mutex queueMutex_;
    std::deque<Chunk> queue_;
    std::atomic<bool> running_ { false };
    std::thread thread_;

    std::atomic<std::uint64_t> submittedFrames_ { 0 };
    std::atomic<std::uint64_t> deliveredFrames_ { 0 };
    std::atomic<int> finalsEmitted_ { 0 };

    // Worker-thread state.
    std::uint64_t utteranceFrames_ = 0;
    std::chrono::steady_clock::time_point lastArrival_ {};
};

} // namespace translation
} // namespace liveai
