#pragma once
//
// Null NDI output: transport-shaped no-op for the skeleton and tests. It accepts
// and counts frames, publishes nothing, and never blocks.

#include <atomic>
#include <cstdint>
#include <string>

#include "NDI/INdiOutput.h"

namespace liveai {
namespace ndi {

class NullNdiOutput final : public INdiOutput
{
public:
    std::string_view name() const noexcept override { return "Null"; }
    OutputState state() const noexcept override { return state_; }

    bool start(std::string_view streamName, std::string& error) override;
    void stop() noexcept override;
    bool publish(const SubtitleFrame& frame, std::string& error) override;

    std::uint64_t publishedFrames() const noexcept override { return published_.load(std::memory_order_relaxed); }
    std::string_view streamName() const noexcept { return streamName_; }

private:
    OutputState state_ = OutputState::disabled;
    std::string streamName_;
    /// Publishes run on the caller's worker thread (sink path) while the
    /// operator/tests read the count from another thread: atomic relaxed, the
    /// same rule the real NDI output will follow earlier.
    std::atomic<std::uint64_t> published_ { 0 };
};

} // namespace ndi
} // namespace liveai
