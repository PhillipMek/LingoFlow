#pragma once
//
// NdiSubtitleVideoOutput - the production subtitle transport: burned-in
// subtitles as a REAL NDI video source, with TTML timed-text metadata as a
// companion mode on the same sender.
//
// This is what the feature request turns into code: OBS (and every other
// plain NDI receiver) gets the subtitles as video pixels with a transparent
// background - a normal NDI Video Source, no parsing, no scripts. Frames are
// BGRA (fourcc_BGRA, straight alpha - the convention documented in
// docs/ndi-protocol.md and asserted pixel-wise by the test suite), paced at
// the configured frame rate; with no caption the source streams a fully
// transparent frame, so a finished sentence never stays burned on the
// audience's screen. Mode "metadata" streams exactly the delivered Mode A
// (TTML snapshots, no video - the shipped semantics of the former
// NdiTimedTextOutput, carried here with its rules and its tests); mode "both"
// adds the same metadata frames to the same sender, because a second sender
// with the same name is a collision, not a feature.
//
// Threading (the project rules' "NDI thread"; NdiDispatch owns the queue in
// front of every transport, and its worker is the caller thread of publish()):
//   * publish() latches the newest caption into ONE slot under the state mutex
//     (an empty text clears the screen) and, in the metadata-carrying modes,
//     submits the TTML snapshot inline - the same bounded transport hand the
//     delivered metadata output used from this same thread. It never renders,
//     never waits on the network, never touches audio;
//   * the render thread (always started by start(); in the video-less modes it
//     waits idle, which keeps one lifecycle for every mode) owns the video
//     pacing: wake on the frame deadline, draw only when the caption or the
//     settings changed since the last frame it produced, and submit with
//     send_send_video_async_v2. One persistent buffer pair: the SDK states
//     async memory stays owned until a synchronizing event, and the next
//     async submission of the other buffer is that event - alternate, safe,
//     one producer;
//   * start/stop/applySubtitleSettings are control-thread calls; stop() joins
//     the render thread before send_destroy (the join is bounded by the frame
//     period - the deadline wait wakes on the stop notify), and one transport
//     mutex serializes every SDK call this instance makes, exactly as the
//     delivered output serialized its metadata submits;
//   * audio is on none of these paths. A render failure counts in
//     renderErrors(), is worded in lastFailure(), keeps sending the last
//     picture it has, and never reaches the show. A sender the runtime refuses
//     answers start() with an actionable sentence - the app still starts, the
//     operator chip still says what is true.
//
// Live settings: appearance and geometry are applied by applySubtitleSettings
// and take effect on the next rendered frame - no sender restart, because NDI
// carries xres/yres/frame rate in EVERY frame. Only the stream name is a
// sender property (Stop + Start to change it; the settings UI says so).
//
// SDK types stay inside the .cpp, as everywhere in this directory.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "NDI/INdiOutput.h"

namespace liveai {
namespace ndi {
namespace real {

class NdiSubtitleVideoOutput final : public INdiOutput
{
public:
    /// The output starts from `settings`; a running instance is updated later
    /// by applySubtitleSettings (settings thread), never by re-mounting. The
    /// composition root may mount blind - the controller pushes the real
    /// settings before every start() (ApplicationController::startNdi), so a
    /// default-constructed transport never shows a frame of its own guessing.
    explicit NdiSubtitleVideoOutput(subtitles::SubtitleSettings settings = {});
    ~NdiSubtitleVideoOutput() override;

    NdiSubtitleVideoOutput(const NdiSubtitleVideoOutput&) = delete;
    NdiSubtitleVideoOutput& operator=(const NdiSubtitleVideoOutput&) = delete;

    /// Names the MODE, truthfully: a metadata-mode source is not a video
    /// output, and the operator must be able to tell which wire story the
    /// running transport tells from this string alone.
    std::string_view name() const noexcept override;
    OutputState state() const noexcept override { return state_.load(std::memory_order_relaxed); }

    bool start(std::string_view streamName, std::string& error) override;
    void stop() noexcept override;

    /// Latches the newest caption (an empty text CLEARS the screen: the
    /// transparent frame is the statement "this line has ended"), and submits
    /// the TTML snapshot inline when the mode carries metadata. Returns false
    /// only when the sender is not running.
    bool publish(const SubtitleFrame& frame, std::string& error) override;

    void applySubtitleSettings(const subtitles::SubtitleSettings& settings) override;

    /// TTML metadata frames sent since the current start (the delivered
    /// publishedFrames semantics; zero in the video-only mode).
    std::uint64_t publishedFrames() const noexcept override { return metadataSent_.load(std::memory_order_relaxed); }
    std::uint64_t videoFramesSent() const noexcept override { return videoFrames_.load(std::memory_order_relaxed); }
    std::uint64_t renderErrors() const noexcept override { return renderErrors_.load(std::memory_order_relaxed); }
    int receivers() const noexcept override { return receivers_.load(std::memory_order_relaxed); }

    std::string runtimeDescription() const override;
    std::string lastFailure() const override;

private:
    void renderLoop();

    // ------------------------------------------------------------- sender side
    void* sender_ = nullptr;                       ///< NDIlib_send_instance_t (opaque here)
    std::string streamName_;
    std::atomic<OutputState> state_{OutputState::disabled};
    std::atomic<std::uint64_t> metadataSent_{0};   ///< captions that went as TTML
    std::atomic<std::uint64_t> videoFrames_{0};
    std::atomic<std::uint64_t> renderErrors_{0};
    std::atomic<int> receivers_{-1};               ///< last poll, from the render/publish thread
    std::atomic<int> modeMirror_{static_cast<int>(subtitles::OutputMode::video)}; ///< for name()

    // ------------------------------------------------------------- render thread
    std::thread renderThread_;
    bool stopping_ = false;                        ///< guarded by mutex_, published to the loop

    // The state the render thread reads and the producer/control threads write.
    mutable std::mutex mutex_;                     ///< guards the block below (the state hand)
    std::condition_variable cv_;                   ///< wakes the render thread for a clean join
    subtitles::SubtitleSettings settings_;         ///< applied appearance + geometry
    std::uint64_t settingsVersion_ = 0;            ///< bumps on every accepted change
    std::string caption_;                          ///< the newest latched text
    std::uint64_t captionVersion_ = 0;             ///< bumps on every publish()
    std::string lastFailure_;                      ///< sentence for the most recent refusal

    std::mutex transportMutex_;                    ///< every SDK call of this instance, ordered
                                                   ///< transportMutex_ -> mutex_ when both are held
};

} // namespace real
} // namespace ndi
} // namespace liveai
