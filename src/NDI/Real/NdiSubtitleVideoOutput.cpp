#include "NDI/Real/NdiSubtitleVideoOutput.h"

#include <chrono>
#include <utility>
#include <vector>

#include "NDI/NdiTimedText.h"
#include "NDI/Real/NdiRuntime.h"
#include "Subtitles/SubtitleRenderer.h"
#include "Utils/Log.h"

namespace liveai {
namespace ndi {
namespace real {
namespace {

constexpr std::string_view kLogComponent = "ndi";

constexpr std::string_view kNameVideo = "NDI subtitle video (BGRA, transparent background)";
constexpr std::string_view kNameMetadata = "NDI timed text (TTML1 metadata)";   // the delivered wording, kept
constexpr std::string_view kNameBoth = "NDI subtitle video + TTML1 metadata";

bool modeHasVideo(subtitles::OutputMode mode) noexcept
{
    return mode == subtitles::OutputMode::video || mode == subtitles::OutputMode::both;
}

bool modeHasMetadata(subtitles::OutputMode mode) noexcept
{
    return mode == subtitles::OutputMode::metadata || mode == subtitles::OutputMode::both;
}

/// The frame period for a validated-by-schema fps; the floor keeps a
/// hand-edited file from spinning the render thread.
std::chrono::microseconds framePeriod(int fps)
{
    if (fps <= 0)
        fps = 25;
    return std::chrono::microseconds(1'000'000 / fps);
}

} // namespace

NdiSubtitleVideoOutput::NdiSubtitleVideoOutput(subtitles::SubtitleSettings settings)
    : settings_(std::move(settings))
{
    modeMirror_.store(static_cast<int>(settings_.mode), std::memory_order_relaxed);
}

NdiSubtitleVideoOutput::~NdiSubtitleVideoOutput()
{
    stop();   // idempotent; the render thread must be gone before members unwind
}

std::string_view NdiSubtitleVideoOutput::name() const noexcept
{
    switch (static_cast<subtitles::OutputMode>(modeMirror_.load(std::memory_order_relaxed)))
    {
        case subtitles::OutputMode::metadata: return kNameMetadata;
        case subtitles::OutputMode::both:     return kNameBoth;
        case subtitles::OutputMode::video:    break;
    }
    return kNameVideo;
}

bool NdiSubtitleVideoOutput::start(std::string_view streamName, std::string& error)
{
    const NdiRuntime& runtime = NdiRuntime::instance();
    if (!runtime.available())
    {
        error = "NDI output cannot start: " + runtime.loadError();
        return false;
    }

    const std::lock_guard<std::mutex> transportLock(transportMutex_);

    if (sender_ != nullptr)
    {
        error = "NDI output is already running as '" + streamName_ + "' - stop it first";
        return false;
    }

    streamName_ = std::string(streamName.empty() ? std::string_view("LingoFlow") : streamName);

    NDIlib_send_create_t create {};
    create.p_ndi_name = streamName_.c_str();
    create.p_groups = nullptr;
    create.clock_video = true;
    // clock_audio false: this source carries video and/or metadata, never
    // audio. The SDK header says that when one thread submits both media
    // kinds only video should be clocked; with no audio at all, clocking it
    // would assert a stream that does not exist.
    create.clock_audio = false;

    sender_ = runtime.api().send_create(&create);
    if (sender_ == nullptr)
    {
        error = "NDIlib_send_create refused the source name '" + streamName_ +
                "' (name collision on this network, or the runtime declined)";
        state_.store(OutputState::disabled, std::memory_order_relaxed);
        return false;
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        caption_.clear();               // a fresh start shows fresh words
        ++captionVersion_;
        lastFailure_.clear();
        stopping_ = false;
    }
    metadataSent_.store(0, std::memory_order_relaxed);
    videoFrames_.store(0, std::memory_order_relaxed);
    renderErrors_.store(0, std::memory_order_relaxed);
    receivers_.store(-1, std::memory_order_relaxed);
    state_.store(OutputState::ready, std::memory_order_relaxed);

    renderThread_ = std::thread([this] { renderLoop(); });

    const subtitles::SubtitleSettings snapshot = [this]
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return settings_;
    }();
    log::info(kLogComponent,
              std::string("NDI subtitle output started as '") + streamName_ + "' (mode "
                  + std::string(subtitles::nameOf(snapshot.mode))
                  + (modeHasVideo(snapshot.mode)
                         ? ", " + std::to_string(snapshot.video.width) + "x"
                               + std::to_string(snapshot.video.height) + "@"
                               + std::to_string(snapshot.video.fps) + " fps BGRA, straight alpha"
                         : ", metadata only")
                  + ", runtime-loaded, never linked)");
    return true;
}

void NdiSubtitleVideoOutput::stop() noexcept
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();

    if (renderThread_.joinable())
        renderThread_.join();   // bounded by the frame period: the deadline wait
                                // wakes on the notify, never on the network

    {
        const std::lock_guard<std::mutex> transportLock(transportMutex_);
        if (sender_ != nullptr)
        {
            // After the join this is the sole remaining caller; and the SDK
            // counts send_destroy itself as a synchronizing event, so no
            // async-submitted buffer can still be owned when we return.
            NdiRuntime::instance().api().send_destroy(
                static_cast<NDIlib_send_instance_t>(sender_));
            sender_ = nullptr;
        }
    }

    receivers_.store(-1, std::memory_order_relaxed);
    state_.store(OutputState::disabled, std::memory_order_relaxed);
}

bool NdiSubtitleVideoOutput::publish(const SubtitleFrame& frame, std::string& error)
{
    const std::lock_guard<std::mutex> transportLock(transportMutex_);

    if (sender_ == nullptr)
    {
        error = "NDI output is not started";
        return false;
    }

    subtitles::OutputMode mode;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        caption_ = frame.text;   // empty clears the screen (video): the
                                 // transparent frame says "this line has ended"
        ++captionVersion_;
        mode = settings_.mode;
    }

    if (modeHasMetadata(mode))
    {
        // The delivered Mode A path, unchanged in its rules: empty text is an
        // accepted no-op (never a counted failure); a non-empty caption sends
        // the whole current TTML document, each frame replacing the last.
        if (!frame.text.empty())
        {
            const std::string document = buildTimedTextDocument(frame);

            NDIlib_metadata_frame_t metadata {};
            metadata.length = static_cast<int>(document.size()) + 1;   // includes the terminator
            metadata.timecode = NDIlib_send_timecode_synthesize;
            metadata.p_data = const_cast<char*>(document.c_str());     // read synchronously

            // Void return: no per-frame truth is exposed to senders (see the
            // delivered header comment); the honest signals are the counters,
            // the state and the polled connection count below.
            NdiRuntime::instance().api().send_send_metadata(
                static_cast<NDIlib_send_instance_t>(sender_), &metadata);

            metadataSent_.fetch_add(1, std::memory_order_relaxed);
        }

        receivers_.store(NdiRuntime::instance().api().send_get_no_connections(
                             static_cast<NDIlib_send_instance_t>(sender_), 0),
                         std::memory_order_relaxed);
        state_.store(receivers_.load(std::memory_order_relaxed) > 0 ? OutputState::publishing
                                                                    : OutputState::ready,
                     std::memory_order_relaxed);
    }

    return true;
}

void NdiSubtitleVideoOutput::applySubtitleSettings(const subtitles::SubtitleSettings& settings)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (settings_ == settings)
            return;   // no spurious redraw for a save that changed nothing
        settings_ = settings;
        ++settingsVersion_;
        modeMirror_.store(static_cast<int>(settings.mode), std::memory_order_relaxed);
    }
    cv_.notify_all();   // the next frame carries the new look without waiting out the period
}

std::string NdiSubtitleVideoOutput::runtimeDescription() const
{
    const NdiRuntime& runtime = NdiRuntime::instance();
    if (!runtime.available())
        return "NDI runtime unavailable: " + runtime.loadError();
    return "NDI runtime " + runtime.versionText() + " (loaded dynamically; never linked)";
}

std::string NdiSubtitleVideoOutput::lastFailure() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return lastFailure_;
}

void NdiSubtitleVideoOutput::renderLoop()
{
    subtitles::SubtitleRenderer renderer;

    std::vector<subtitles::RenderedSubtitleFrame> slots(2);   // double buffer:
    // the SDK header states async-submitted memory stays owned until a
    // synchronizing event, and the next async submission is exactly that
    // event - so the thread draws into the slot the previous submission
    // released, and never touches the one in flight.
    std::size_t drawSlot = 1;
    std::size_t lastSent = 1;
    bool haveFrame[2] = { false, false };

    std::uint64_t drawnCaptionVersion = 0xFFFFFFFFFFFFFFFFull;
    std::uint64_t drawnSettingsVersion = 0xFFFFFFFFFFFFFFFFull;

    auto nextDue = std::chrono::steady_clock::now();

    for (;;)
    {
        std::string caption;
        subtitles::SubtitleSettings settings;
        std::uint64_t captionVersion = 0;
        std::uint64_t settingsVersion = 0;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock, nextDue, [this] { return stopping_; });
            if (stopping_)
                return;   // stop() owns the join; queued captions are its problem, not the thread's

            caption = caption_;
            captionVersion = captionVersion_;
            settings = settings_;
            settingsVersion = settingsVersion_;
        }

        if (modeHasVideo(settings.mode))
        {
            // Draw only when the picture (caption or style/geometry) changed
            // since the last render; the steady state re-sends the current
            // buffer, which is what keeps the receiver's clock fed and the
            // old text exactly where the audience left it.
            const bool needDraw = !haveFrame[drawSlot]
                || captionVersion != drawnCaptionVersion
                || settingsVersion != drawnSettingsVersion;

            bool drew = false;
            if (needDraw)
            {
                renderer.applySettings(settings);
                subtitles::RenderedSubtitleFrame& target = slots[drawSlot];
                std::string error;
                if (renderer.render(caption, target, error, subtitles::CaptionLayoutMode::singleLineTail))
                {
                    drawnCaptionVersion = captionVersion;
                    drawnSettingsVersion = settingsVersion;
                    haveFrame[drawSlot] = true;
                    drew = true;

                    const std::lock_guard<std::mutex> lock(mutex_);
                    lastFailure_.clear();   // a good frame closes the standing sentence
                }
                else
                {
                    renderErrors_.fetch_add(1, std::memory_order_relaxed);
                    if (!haveFrame[drawSlot])
                    {
                        // The first frame must exist even when the renderer
                        // refuses: a transparent frame is honest (nothing
                        // shows), and the stream starts. The failure is
                        // counted and worded below, never hidden - and it
                        // never reaches the audio path.
                        target.width = settings.video.width > 0 ? settings.video.width : 2;
                        target.height = settings.video.height > 0 ? settings.video.height : 2;
                        target.bgra.assign(static_cast<std::size_t>(target.width)
                                               * static_cast<std::size_t>(target.height) * 4u,
                                           0u);
                        drawnCaptionVersion = captionVersion;
                        drawnSettingsVersion = settingsVersion;
                        haveFrame[drawSlot] = true;
                    }
                    const std::lock_guard<std::mutex> lock(mutex_);
                    if (lastFailure_ != error)
                    {
                        lastFailure_ = error;
                        log::warning(kLogComponent, "subtitle render: " + error);
                    }
                }
            }

            const std::size_t sending = drew ? drawSlot : lastSent;
            if (haveFrame[sending] && !slots[sending].bgra.empty())
            {
                subtitles::RenderedSubtitleFrame& out = slots[sending];

                NDIlib_video_frame_v2_t vf {};
                vf.xres = out.width;
                vf.yres = out.height;
                vf.FourCC = NDIlib_FourCC_video_type_BGRA;
                vf.frame_rate_N = settings.video.fps > 0 ? settings.video.fps : 25;
                vf.frame_rate_D = 1;                       // whole-number rates by design
                vf.picture_aspect_ratio = 0.0f;            // square pixels (the SDK reads 0 as xres/yres)
                vf.frame_format_type = NDIlib_frame_format_type_progressive;
                vf.timecode = NDIlib_send_timecode_synthesize;
                vf.p_data = out.bgra.data();
                vf.line_stride_in_bytes = out.width * 4;
                vf.p_metadata = nullptr;
                vf.timestamp = 0;

                {
                    const std::lock_guard<std::mutex> transportLock(transportMutex_);
                    if (sender_ == nullptr)
                        return;   // stopped while drawing: the sender is gone, the loop is done
                    NdiRuntime::instance().api().send_send_video_async_v2(
                        static_cast<NDIlib_send_instance_t>(sender_), &vf);
                    receivers_.store(NdiRuntime::instance().api().send_get_no_connections(
                                         static_cast<NDIlib_send_instance_t>(sender_), 0),
                                     std::memory_order_relaxed);
                }
                videoFrames_.fetch_add(1, std::memory_order_relaxed);

                // The submission synchronized the buffer the PREVIOUS one held;
                // that slot is now ours to redraw into.
                if (drew)
                {
                    lastSent = drawSlot;
                    drawSlot = 1 - drawSlot;
                }

                state_.store(receivers_.load(std::memory_order_relaxed) > 0 ? OutputState::publishing
                                                                            : OutputState::ready,
                             std::memory_order_relaxed);
            }
        }

        nextDue += framePeriod(settings.video.fps);
        const auto now = std::chrono::steady_clock::now();
        if (nextDue <= now)
            nextDue = now + framePeriod(settings.video.fps);   // never a catch-up burst
    }
}

} // namespace real
} // namespace ndi
} // namespace liveai
