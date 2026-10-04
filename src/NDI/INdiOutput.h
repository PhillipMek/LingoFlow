#pragma once
//
// INdiOutput - subtitle transport boundary (task 016 implements it).
//
// Boundary rules:
//   * Publishing is called from a worker/network thread, never from the audio
//     callback (SPEC "NDI failure must not stop audio").
//   * Implementations must be drop-on-pressure: a stalled NDI consumer discards
//     the oldest frame instead of blocking the producer.
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

    /// Non-blocking publish. Returns false when the frame was dropped or the
    /// output is not started; the caller records it in diagnostics and continues.
    virtual bool publish(const SubtitleFrame& frame, std::string& error) = 0;

    /// Captions handed over since the current start (task 017: the diagnostics
    /// export and the SPEC 55 "Caption events" row need it through the contract,
    /// not through a cast to a concrete output). The default zero is honest for
    /// implementations that count nothing; the Null output overrides it with its
    /// real counter.
    virtual std::uint64_t publishedFrames() const noexcept { return 0; }
};

} // namespace ndi
} // namespace liveai
