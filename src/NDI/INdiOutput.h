#pragma once
//
// INdiOutput - subtitle transport boundary (task 016 implements it).
//
// Boundary rules:
//   * Publishing is called from a worker/network thread, never from the audio
//     callback (SPEC "NDI failure must not stop audio").
//   * Publishing must ALSO never block its caller: the OpenAI receiver thread
//     is the single consumer of both translated audio and translated text, so
//     a publish() that waits on the transport can stall audio delivery one
//     level up (code review P1, 2026-10-05). Implementations that touch the
//     network defer that work to their own thread - the product mounts the
//     real output behind NdiDispatch, and the drop-on-pressure rule
//     (a stalled consumer costs the oldest queued caption, never the
//     producer's time) is enforced by that structure, not by good intentions.
//   * No NDI SDK type appears in this interface.

#include <cstdint>
#include <string>
#include <string_view>

namespace liveai {
namespace ndi {

enum class OutputState
{
    disabled = 0,     ///< feature switched off by configuration
    ready,            ///< started, no frame published yet
    publishing,       ///< frames are being accepted by at least one consumer
    faulted           ///< transport error; audio keeps running
};

std::string_view nameOf(OutputState state) noexcept;

/// One subtitle frame. `final == false` marks an in-progress line that a
/// receiver may replace; `final == true` is authoritative.
struct SubtitleFrame
{
    std::string text;
    bool final = false;
    long long sequence = 0;      ///< monotonically increasing, when available
};

class INdiOutput
{
public:
    virtual ~INdiOutput() = default;

    virtual std::string_view name() const noexcept = 0;
    virtual OutputState state() const noexcept = 0;

    /// Stream name visible to NDI receivers, e.g. "LingoFlow (EN->RU)".
    virtual bool start(std::string_view streamName, std::string& error) = 0;
    virtual void stop() noexcept = 0;

    /// Queue the frame and return: false means the frame was NOT accepted
    /// (output not started, or shutting down) - the caller records it in
    /// diagnostics and continues. A frame that WAS accepted may still be lost
    /// later (transport failure, back-pressure drop); those losses surface in
    /// droppedFrames()/publishErrors(), never by holding the caller hostage to
    /// the transport's timing.
    virtual bool publish(const SubtitleFrame& frame, std::string& error) = 0;

    /// Captions handed over since the current start (task 017: the diagnostics
    /// export and the SPEC 55 "Caption events" row need it through the contract,
    /// not through a cast to a concrete output). The default zero is honest for
    /// implementations that count nothing; the Null output overrides it with its
    /// real counter.
    virtual std::uint64_t publishedFrames() const noexcept { return 0; }

    /// Frames the implementation discarded rather than delay a producer
    /// (back-pressure, stop races, shutdown drain). Default zero says "counts
    /// nothing", the exporting code says which output it is talking about.
    virtual std::uint64_t droppedFrames() const noexcept { return 0; }

    /// Frames accepted by the implementation and then refused by the transport.
    /// Distinct from droppedFrames(): a drop was a choice under pressure, an
    /// error was the transport failing - a venue post-mortem reads them
    /// differently.
    virtual std::uint64_t publishErrors() const noexcept { return 0; }
};

} // namespace ndi
} // namespace liveai
