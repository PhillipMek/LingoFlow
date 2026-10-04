#pragma once
//
// NdiTimedTextOutput - the production INdiOutput (task 016, SPEC 38 Mode A):
// subtitle snapshots ride NDI metadata frames as well-formed TTML1 documents
// (the shape and the format choice are researched, not invented - see
// docs/ndi-protocol.md and Ndi/NdiTimedText.h).
//
// What this class owns: the sender lifecycle and the state words. What it
// deliberately does NOT own: any belief about delivery. The SDK's
// send_send_metadata returns void (verified against the 6.3.2.0 headers): the
// per-frame wire truth is not exposed to senders, so the honest signals are
//   * started-or-not          -> start() result and the error string,
   //   * a consumer connected    -> send_get_no_connections polled at each send,
//   * how many frames went out -> publishedFrames().
// There is no "the receiver displayed it" callback in this API, and pretending
// otherwise would be the AGENTS.md 19 kind of lie. The venue checkpoint is the
// place that claim actually gets checked, by eye, on a real receiver.
//
// Error handling within the honest surface: metadata delivery cannot fail
// visibly, but the transport can be lost between starts - NDI's own discovery
// and reconnection machinery runs inside the runtime for that. What this class
// guarantees is the product rule: no path here blocks or touches audio
// (publish runs on the text/network worker thread; the mutex is a UI/worker
// hand, never the callback), and every refusal leaves the show running.
//
// SDK types stay inside the .cpp: this header includes only the contract.

#include <atomic>
#include <cstdint>
#include <string>

#include "NDI/INdiOutput.h"

namespace liveai {
namespace ndi {
namespace real {

class NdiTimedTextOutput final : public INdiOutput
{
public:
    std::string_view name() const noexcept override { return "NDI timed text (TTML1 metadata)"; }

    OutputState state() const noexcept override { return state_.load(std::memory_order_relaxed); }

    bool start(std::string_view streamName, std::string& error) override;
    void stop() noexcept override;

    bool publish(const SubtitleFrame& frame, std::string& error) override;

    /// Captions handed to the SDK while the sender lives: reset by each start(),
    /// and after stop() it keeps the last run's number (history, like every other
    /// counter in this product). Displayed or not is the receiver's answer - the
    /// header comment says why we cannot know it from the sender side.
    std::uint64_t publishedFrames() const noexcept override { return published_.load(std::memory_order_relaxed); }

private:
    /// NDIlib_send_instance_t is a pointer to an opaque struct; kept as void* so
    /// no SDK type crosses this header (the cast lives in one .cpp).
    void* sender_ = nullptr;
    std::string streamName_;                       ///< kept for restarts and errors
    std::atomic<OutputState> state_{OutputState::disabled};
    std::atomic<std::uint64_t> published_{0};
};

} // namespace real
} // namespace ndi
} // namespace liveai
