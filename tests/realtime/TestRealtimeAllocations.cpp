#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <thread>

#include "Audio/AudioEngine.h"
#include "Audio/AudioLoopback.h"
#include "Audio/GainStage.h"
#include "Audio/Null/NullAudioBackend.h"

// Proof for the PASS criterion "no allocations in callback" (AGENTS.md 5).
//
// Counting is done by replacing global operator new/delete for this test binary only
// (tests/CMakeLists.txt builds a separate executable for it, so no other suite is
// affected). The counters stay off except inside the measured window, and the code
// under test cannot avoid the accounting: any heap allocation it performs goes through
// the operators below.

namespace {

std::atomic<bool> countingEnabled{ false };
std::atomic<std::uint64_t> allocationCount{ 0 };

void beginCounting() noexcept
{
    allocationCount.store(0, std::memory_order_relaxed);
    countingEnabled.store(true, std::memory_order_seq_cst);
}

std::uint64_t endCounting() noexcept
{
    countingEnabled.store(false, std::memory_order_seq_cst);
    return allocationCount.load(std::memory_order_relaxed);
}

void* allocate(std::size_t size)
{
    if (countingEnabled.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);

    void* memory = std::malloc(size != 0 ? size : 1);

    if (memory == nullptr)
        throw std::bad_alloc();

    return memory;
}

} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using namespace liveai;
using liveai::audio::AudioLoopback;
using liveai::audio::NullAudioBackend;

constexpr int kRate = 48000;
constexpr int kFrames = 480;

audio::DeviceRequest makeRequest()
{
    audio::DeviceRequest request;
    request.sampleRate = kRate;
    request.bufferFrames = kFrames;
    return request;
}

} // namespace

TEST_CASE("Realtime path: the audio callback allocates nothing", "[realtime][allocation]")
{
    NullAudioBackend backend;
    AudioEngine engine;

    std::string error;
    REQUIRE(engine.activate(backend, makeRequest(), error));

    // Attach a consumer first: the forwarding branch (the one that runs in a real
    // show) is the branch that must stay allocation-free.
    AudioLoopback loopback(engine);
    loopback.setPollIntervalMs(1);
    REQUIRE(loopback.start(error));

    std::array<float, static_cast<std::size_t>(kFrames)> in{};
    std::array<float, static_cast<std::size_t>(kFrames)> out{};
    in.fill(0.25f);

    const float* inPointers[] = { in.data() };
    float* outPointers[] = { out.data() };

    // Warm-up outside the measured window: the first callback touches code paths
    // that may still be lazily initialised by the runtime.
    for (int block = 0; block < 50; ++block)
        engine.processAudio(inPointers, outPointers, kFrames);

    beginCounting();

    for (int block = 0; block < 5000; ++block)
        engine.processAudio(inPointers, outPointers, kFrames);

    const std::uint64_t allocations = endCounting();

    // Malformed calls must not allocate either - they are the error path a driver can
    // trigger at any moment.
    beginCounting();
    engine.processAudio(nullptr, outPointers, kFrames);
    engine.processAudio(inPointers, nullptr, kFrames);
    engine.processAudio(inPointers, outPointers, 0);
    const std::uint64_t errorPathAllocations = endCounting();

    loopback.stop();
    engine.deactivate();

    CHECK(allocations == 0);
    CHECK(errorPathAllocations == 0);

    // The accounting has to close. A synthetic burst can outrun the worker, in which
    // case the ring drops - that is allowed; audio vanishing without being counted is not.
    CHECK(engine.inputFramesCaptured() == engine.inputFramesForwarded() + engine.inputRingDroppedFrames());
    CHECK(engine.inputFramesCaptured() >= static_cast<std::uint64_t>(5050 * kFrames));
    CHECK(engine.inputFramesForwarded() > 0);
}

TEST_CASE("Realtime path: the lock-free buffers allocate nothing while running",
          "[realtime][allocation]")
{
    // Ring and jitter buffers are constructed here (allocation is allowed at setup),
    // then hammered inside the counting window.
    NullAudioBackend backend;
    AudioEngine engine;

    std::string error;
    REQUIRE(engine.activate(backend, makeRequest(), error));

    auto* ring = engine.inputRing(0);
    auto* jitter = engine.outputJitter(0);
    REQUIRE(ring != nullptr);
    REQUIRE(jitter != nullptr);

    std::array<float, static_cast<std::size_t>(kFrames)> block{};
    block.fill(0.5f);
    std::array<float, static_cast<std::size_t>(kFrames)> sink{};

    beginCounting();

    for (int iteration = 0; iteration < 20000; ++iteration)
    {
        ring->write(block.data(), block.size());
        jitter->write(block.data(), block.size());
        ring->readOrSilence(sink.data(), sink.size());
        jitter->readOrSilence(sink.data(), sink.size());
    }

    const std::uint64_t allocations = endCounting();

    engine.deactivate();

    CHECK(allocations == 0);
}

TEST_CASE("Realtime path: a gain change costs no allocations and no locks", "[audio][realtime][gain]")
{
    NullAudioBackend backend;
    AudioEngine engine;

    std::string error;
    REQUIRE(engine.activate(backend, makeRequest(), error));
    engine.attachInputConsumer();

    std::array<float, static_cast<std::size_t>(kFrames)> in{};
    std::array<float, static_cast<std::size_t>(kFrames)> out{};
    in.fill(0.25f);

    const float* inPointers[] = { in.data() };
    float* outPointers[] = { out.data() };

    for (int block = 0; block < 50; ++block)
        engine.processAudio(inPointers, outPointers, kFrames);

    // An operator dragging the slider is the case that has to be proven: the request is
    // published as atomic state and the callback picks it up, with no allocation on either
    // side (SPEC "Input Gain DSP Requirements").
    beginCounting();

    for (int block = 0; block < 2000; ++block)
    {
        engine.setInputGainDb(static_cast<float>(-24 + block % 48));
        engine.setOutputGainDb(static_cast<float>(block % 12));
        engine.setInputMuted(block % 400 == 0);
        engine.processAudio(inPointers, outPointers, kFrames);
    }

    engine.setInputGainDb(std::nanf("1"));          // refused requests must not allocate either
    engine.setOutputGainDb(std::numeric_limits<float>::infinity());

    const std::uint64_t allocations = endCounting();

    engine.deactivate();

    CHECK(allocations == 0);
    CHECK(engine.gainRequestsRejected() == 2);
    CHECK(engine.gainRequestsClamped() == 0);   // every value used above was inside the window
    CHECK(engine.inputGainDb() == 7.0f);        // the last request that was a level: -24 + (1999 % 48) = -24 + 31
    CHECK(std::isfinite(engine.appliedInputGainDb()));
    CHECK(engine.blockCount() == 2050);          // warm-up plus the measured window
}
