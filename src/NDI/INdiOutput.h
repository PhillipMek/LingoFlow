#pragma once
//
// INdiOutput - subtitle transport boundary (the real outputs implement it).
//
// Boundary rules:
//   * Publishing is called from a worker/network thread, never from the audio
//     callback (spec "NDI failure must not stop audio").
//   * Publishing must ALSO never block its caller: the OpenAI receiver thread
//     is the single consumer of both translated audio and translated text, so
//     a publish() that waits on the transport can stall audio delivery one
//     level up. Implementations that touch the
//     network defer that work to their own thread - the product mounts the
//     real output behind NdiDispatch, and the drop-on-pressure rule
//     (a stalled consumer costs the oldest queued caption, never the
//     producer's time) is enforced by that structure, not by good intentions.
//   * No NDI SDK type appears in this interface. The subtitle appearance types
//     (Subtitles/) are NOT SDK types and ARE part of the contract: the video
//     transport renders captions from them, and settings changes reach a
//     running transport only through this seam (applySubtitleSettings) - the
//     alternative, a cast to a concrete output, is exactly the coupling the
//     boundary exists to prevent.

#include <cstdint>
#include <string>
#include <string_view>

#include "Subtitles/SubtitleStyle.h"

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

    /// Captions handed over since the current start (the diagnostics
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

    // ------------------------------------------------------------------ video
    // Burned-in subtitle video. Every accessor here has an honest default so
    // the metadata-only and Null outputs answer without knowing anything about
    // pixels: a transport that renders nothing "sent no video frames, had no
    // render errors, knows nothing of receivers". Adding them to the contract
    // (rather than casting to a concrete output in the diagnostics/UI) is what
    // keeps App free of transport types.

    /// Appearance + geometry for a running transport. The video output applies
    /// this live (the next rendered frame uses it; the NDI sender is per-frame
    /// sized, so no restart); transports that draw no text ignore it, which is
    /// the truthful answer, not a silent one - the operator sees the effect (or
    /// its absence) in the preview and the diagnostics. The default no-op is
    /// for outputs that never read settings; the real video output overrides.
    virtual void applySubtitleSettings(const subtitles::SubtitleSettings& settings)
    {
        static_cast<void>(settings);
    }

    /// Video frames handed to the SDK since the current start (resets on
    /// start(), kept as history after stop() - the same lifecycle rule as
    /// publishedFrames()).
    virtual std::uint64_t videoFramesSent() const noexcept { return 0; }

    /// Frames the renderer could not draw (font engine refusal, buffer
    /// failure) since the current start. Kept distinct from publishErrors():
    /// "the subtitle pixels never formed" reads differently from "the network
    /// refused the frame".
    virtual std::uint64_t renderErrors() const noexcept { return 0; }

    /// Receivers currently consuming this source, as the SDK reports it
    /// (send_get_no_connections polled at frame time), or -1 when the transport
    /// cannot know. Never proof a caption was DISPLAYED - only that a receiver
    /// is connected to the source; the venue checkpoint is where "the audience
    /// sees it" is actually established.
    virtual int receivers() const noexcept { return -1; }

    /// A one-line, operator-actionable description of the NDI runtime as this
    /// output sees it ("NDI runtime 6.3.2.0 loaded" / "runtime not found -
    /// install NDI 6"), or empty when the transport does not speak NDI.
    virtual std::string runtimeDescription() const { return {}; }

    /// The most recent init/render/send refusal this transport is standing
    /// under, as an actionable sentence, or empty while all is well. A count
    /// says something broke; this says what to do about it.
    virtual std::string lastFailure() const { return {}; }
};

} // namespace ndi
} // namespace liveai
