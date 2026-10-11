#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include <string>

#include "NDI/NdiTimedText.h"
#include <catch2/catch_message.hpp>

using namespace liveai;

namespace {

ndi::SubtitleFrame frameOf(std::string text, bool final = false, long long sequence = 1)
{
    ndi::SubtitleFrame frame;
    frame.text = std::move(text);
    frame.final = final;
    frame.sequence = sequence;
    return frame;
}

} // namespace

TEST_CASE("NdiTimedText: one root, no prolog, and the format is the standard we cite",
          "[ndi][timedtext]")
{
    const std::string doc = ndi::buildTimedTextDocument(frameOf("Hello", false, 42));

    CHECK(doc.rfind("<tt ", 0) == 0);                       // single root element
    CHECK(doc.rfind("<?xml", 0) != 0);                      // the no-prolog rule
    CHECK(doc.find("http://www.w3.org/ns/ttml") != std::string::npos);   // TTML1 namespace
    CHECK(doc.find("seq-42") != std::string::npos);         // the pipeline's own identity
    CHECK(doc.find(">Hello</p>") != std::string::npos);
    CHECK(doc.find("</tt>") == doc.size() - 5);

    // No invented wire fields: no language we do not know, no fake timing, no
    // private attributes beyond the standard xml:id.
    CHECK(doc.find("xml:lang") == std::string::npos);
    CHECK(doc.find("begin=") == std::string::npos);
    CHECK(doc.find("final") == std::string::npos);
}

TEST_CASE("NdiTimedText: escaping is total for text content, forbidden characters are removed",
          "[ndi][timedtext]")
{
    CHECK(ndi::buildTimedTextDocument(frameOf("a & b < c > d")).find("a &amp; b &lt; c &gt; d") !=
          std::string::npos);

    // XML 1.0 forbids C0 controls (except tab/newline/CR) outright; a document a
    // receiver cannot parse would be worse than a space, so they become spaces.
    const std::string dirty = std::string("x") + static_cast<char>(0x01) + "y" +
                              static_cast<char>(0x7F) + "z";
    const std::string clean = ndi::buildTimedTextDocument(frameOf(dirty));
    CHECK(clean.find(">x y z<") != std::string::npos);

    // Newline and tab are legal XML content and stay untouched.
    CHECK(ndi::buildTimedTextDocument(frameOf("a\nb\tc")).find(">a\nb\tc<") != std::string::npos);

    // Multi-byte UTF-8 travels as bytes >= 0x80: Cyrillic captions survive whole.
    CHECK(ndi::buildTimedTextDocument(frameOf("Привет мир")).find("Привет мир") !=
          std::string::npos);
}

TEST_CASE("NdiTimedText: sequence is carried honestly, negatives clamp to the first id",
          "[ndi][timedtext]")
{
    CHECK(ndi::buildTimedTextDocument(frameOf("t", true, 7)).find("seq-7") != std::string::npos);
    CHECK(ndi::buildTimedTextDocument(frameOf("t", false, -3)).find("seq-0") != std::string::npos);
}

#ifdef LINGOFLOW_TESTS_WITH_NDI
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <thread>

#include "NDI/Real/NdiRuntime.h"
#include "NDI/Real/NdiSubtitleVideoOutput.h"
#include "Subtitles/SubtitleRenderer.h"

using liveai::ndi::real::NdiRuntime;
using liveai::ndi::real::NdiSubtitleVideoOutput;
using liveai::subtitles::OutputMode;

namespace {

/// The delivered metadata-mode lifecycle, ported to the unified output: the
/// SAME assertions (state words, refusal sentences, counter rules) now guard
/// the mode inside NdiSubtitleVideoOutput, so "the additional mode" is tested
/// as production runs it, not as a museum piece.
void metadataLifecycle(const char* prefix)
{
    NdiSubtitleVideoOutput output;
    output.applySubtitleSettings([]
    {
        liveai::subtitles::SubtitleSettings s;
        s.mode = OutputMode::metadata;
        return s;
    }());

    CHECK(output.state() == ndi::OutputState::disabled);
    CHECK(output.name() == std::string_view("NDI timed text (TTML1 metadata)"));

    std::string error;
    CHECK_FALSE(output.publish(frameOf("early"), error));
    CHECK(error.find("not started") != std::string::npos);

    const std::string name = std::string(prefix) + std::to_string(::GetCurrentProcessId());

    INFO("start error: " + error);
    REQUIRE(output.start(name, error));
    CHECK(output.state() == ndi::OutputState::ready);   // no consumer here: not "publishing"

    CHECK_FALSE(output.start(name + "-again", error));
    CHECK(error.find("already running") != std::string::npos);

    CHECK(output.publish(frameOf("тест", false, 1), error));
    CHECK(output.publish(frameOf("тест", true, 2), error));
    CHECK(output.publishedFrames() == 2);
    CHECK(output.videoFramesSent() == 0);   // metadata mode sends no video
    CHECK(output.state() == ndi::OutputState::ready);

    // Empty caption: accepted as a no-op, not counted as a failure.
    CHECK(output.publish(frameOf(""), error));
    CHECK(output.publishedFrames() == 2);

    output.stop();
    CHECK(output.state() == ndi::OutputState::disabled);
    CHECK(output.publishedFrames() == 2);   // the run's history survives the stop

    const bool restarted = output.start(name, error);
    INFO("restart error: " + error);
    REQUIRE(restarted);
    CHECK(output.publishedFrames() == 0);   // a fresh start counts from zero
    output.stop();
}

} // namespace

TEST_CASE("NdiSubtitleVideoOutput: the metadata mode keeps the delivered lifecycle",
          "[ndi][real]")
{
    if (!NdiRuntime::instance().available())
    {
        // No runtime: the honest branch (the product rule "NDI failure does not
        // stop the show" is proven by start() answering false with words).
        NdiSubtitleVideoOutput output;
        std::string error;
        CHECK_FALSE(output.start("lingoflow-test-unavailable", error));
        CHECK(error.find("runtime") != std::string::npos);
        CHECK(output.state() == ndi::OutputState::disabled);
        CHECK(output.runtimeDescription().find("unavailable") != std::string::npos);
        return;
    }

    metadataLifecycle("lingoflow-meta-unit-");
}

TEST_CASE("NdiSubtitleVideoOutput: video mode paces frames with nobody watching",
          "[ndi][real][video]")
{
    if (!NdiRuntime::instance().available())
    {
        NdiSubtitleVideoOutput output;
        std::string error;
        CHECK_FALSE(output.start("lingoflow-video-unavailable", error));
        CHECK(error.find("runtime") != std::string::npos);
        return;
    }

    liveai::subtitles::SubtitleSettings settings;
    settings.mode = OutputMode::video;
    settings.video.width = 640;      // a modest sender for a test network
    settings.video.height = 360;
    settings.video.fps = 20;
    NdiSubtitleVideoOutput output(settings);

    std::string error;
    const std::string name = "lingoflow-video-unit-" + std::to_string(::GetCurrentProcessId());
    INFO("start error: " + error);
    REQUIRE(output.start(name, error));

    // Frames flow from the first tick WITHOUT any caption and without any
    // consumer: the transparent stream is the contract (an idle NDI source
    // that sends nothing ages out on receivers - this is what stops that).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (output.videoFramesSent() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(output.videoFramesSent() > 0);
    CHECK(output.renderErrors() == 0);
    CHECK(output.receivers() == 0);
    CHECK(output.state() == ndi::OutputState::ready);   // no consumer: never "publishing"

    const auto firstBatch = output.videoFramesSent();

    // A caption changes the picture, not the lifecycle.
    CHECK(output.publish(frameOf("текст на экране", false, 1), error));

    // Live settings: geometry and look applied while running - the sender is
    // not rebuilt and the frame flow continues (the task's no-restart rule).
    liveai::subtitles::SubtitleSettings bigger = settings;
    bigger.video.width = 1280;
    bigger.video.height = 720;
    bigger.style.fontSizePx = 64;
    output.applySubtitleSettings(bigger);

    const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (output.videoFramesSent() <= firstBatch && std::chrono::steady_clock::now() < deadline2)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(output.videoFramesSent() > firstBatch);
    CHECK(output.renderErrors() == 0);
    CHECK(output.lastFailure().empty());

    output.stop();
    CHECK(output.state() == ndi::OutputState::disabled);

    // Restart: counters from zero, sender raised again (operator's Stop + Start).
    const bool restarted2 = output.start(name, error);
    INFO("restart error: " + error);
    REQUIRE(restarted2);
    CHECK(output.videoFramesSent() == 0);
    output.stop();
}

TEST_CASE("NDI video roundtrip: our rendered frames arrive through the runtime as BGRA with alpha",
          "[ndi][real][video][roundtrip]")
{
    // The heaviest automated claim of this feature: hand the runtime a frame
    // built by the PRODUCT renderer (empty, then captioned) over a sender
    // created through the product's dynamic loader, subscribe to it by its own
    // advertised URL (the probe's selfcheck pattern - no discovery luck
    // needed) and inspect the DELIVERED pixels. This proves the SDK accepts
    // our video buffers and a receiver reads them as BGRA with alpha 0 in the
    // background and real ink after a caption arrives - the wire half of
    // "OBS can use this". OBS's own compositing remains the venue checkpoint
    // (docs/ndi-obs.md): this test says nothing about what OBS displays.
    if (!NdiRuntime::instance().available())
    {
        WARN("no NDI runtime on this machine - the roundtrip is not running");
        return;
    }

    const NDIlib_v6& api = NdiRuntime::instance().api();

    liveai::subtitles::SubtitleSettings settings;
    settings.video.width = 640;
    settings.video.height = 360;
    settings.video.fps = 10;

    liveai::subtitles::SubtitleRenderer renderer;
    renderer.applySettings(settings);

    std::string error;
    liveai::subtitles::RenderedSubtitleFrame emptyFrame;
    const bool emptyPainted = renderer.render("", emptyFrame, error);
    INFO(error);
    REQUIRE(emptyPainted);
    liveai::subtitles::RenderedSubtitleFrame inkFrame;
    const bool inkPainted = renderer.render("\xd0\xbf\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb5\xd1\x80\xd0\xba\xd0\xb0 "
                                            "\xd1\x80\xd0\xb0\xd1\x83\xd0\xbd\xd0\xb4\xd1\x82\xd1\x80\xd0\xb8"
                                            "\xd0\xbf\xd0\xb0", inkFrame, error);
    INFO(error);
    REQUIRE(inkPainted);

    // Sanity of the claim itself before putting it on a wire: the ink buffer
    // must have opaque pixels locally.
    std::size_t inkOpaque = 0;
    for (std::size_t p = 0; p < static_cast<std::size_t>(inkFrame.width) * inkFrame.height; ++p)
        if (inkFrame.bgra[p * 4u + 3] == 255)
            ++inkOpaque;
    REQUIRE(inkOpaque > 50);

    const std::string name = "lingoflow-video-rt-" + std::to_string(::GetCurrentProcessId());

    NDIlib_send_create_t createDesc {};
    createDesc.p_ndi_name = name.c_str();
    createDesc.clock_video = true;
    createDesc.clock_audio = false;
    NDIlib_send_instance_t sender = api.send_create(&createDesc);
    REQUIRE(sender != nullptr);

    auto fillVideo = [](NDIlib_video_frame_v2_t& vf, liveai::subtitles::RenderedSubtitleFrame& f)
    {
        vf = NDIlib_video_frame_v2_t {};
        vf.xres = f.width;
        vf.yres = f.height;
        vf.FourCC = NDIlib_FourCC_video_type_BGRA;
        vf.frame_rate_N = 10;
        vf.frame_rate_D = 1;
        vf.picture_aspect_ratio = 0.0f;
        vf.frame_format_type = NDIlib_frame_format_type_progressive;
        vf.timecode = NDIlib_send_timecode_synthesize;
        vf.p_data = f.bgra.data();
        vf.line_stride_in_bytes = f.width * 4;
    };

    const NDIlib_source_t* own = api.send_get_source_name(sender);
    if (own == nullptr || own->p_url_address == nullptr || own->p_url_address[0] == '\0')
    {
        // The probe's documented machine fact: an isolated laptop can expose no
        // self-URL and no discovery route. That is not a transport defect - the
        // roundtrip is exactly what the venue receiver checkpoint (docs/ndi-obs.md,
        // docs/rig-checklist.md section 11) exists to establish on a real network.
        // Said out loud as a WARN, never as a silent pass.
        api.send_destroy(sender);
        WARN("sender advertised no URL - loopback roundtrip needs a venue network; "
             "frame pacing itself is covered by the lifecycle test");
        return;
    }

    NDIlib_recv_create_v3_t recvDesc {};
    recvDesc.source_to_connect_to.p_ndi_name = own->p_ndi_name;
    recvDesc.source_to_connect_to.p_url_address = own->p_url_address;
    recvDesc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    recvDesc.bandwidth = NDIlib_recv_bandwidth_highest;

    NDIlib_recv_instance_t receiver = api.recv_create_v3(&recvDesc);
    REQUIRE(receiver != nullptr);

    bool sawTransparentVideo = false;
    bool sawInkVideo = false;
    bool sawBgraDelivery = false;
    int receivedVideo = 0;

    // Phase 1: stream the empty frame until a fully transparent video frame
    // comes back; phase 2: the same with the captioned frame and opaque ink.
    for (int phase = 0; phase < 2 && !(sawTransparentVideo && sawInkVideo); ++phase)
    {
        liveai::subtitles::RenderedSubtitleFrame& current = phase == 0 ? emptyFrame : inkFrame;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);

        while (std::chrono::steady_clock::now() < deadline)
        {
            NDIlib_video_frame_v2_t vf {};
            fillVideo(vf, current);
            api.send_send_video_async_v2(sender, &vf);

            NDIlib_video_frame_v2_t got {};
            NDIlib_audio_frame_v3_t af {};
            NDIlib_metadata_frame_t mf {};
            const NDIlib_frame_type_e type = api.recv_capture_v3(receiver, &got, &af, &mf, 200);

            if (type == NDIlib_frame_type_video && got.p_data != nullptr)
            {
                ++receivedVideo;
                if (got.FourCC == NDIlib_FourCC_video_type_BGRA)
                    sawBgraDelivery = true;

                std::size_t opaque = 0;
                std::size_t transparent = 0;
                const std::size_t pixels = static_cast<std::size_t>(got.xres)
                                         * static_cast<std::size_t>(got.yres);
                for (std::size_t p = 0; p < pixels; ++p)
                {
                    const std::uint8_t a = got.p_data[p * 4u + 3];
                    if (a == 0)
                        ++transparent;
                    else if (a == 255)
                        ++opaque;
                }

                if (phase == 0 && got.xres == emptyFrame.width && opaque == 0
                    && transparent == pixels)
                {
                    sawTransparentVideo = true;
                    api.recv_free_video_v2(receiver, &got);
                    break;
                }
                if (phase == 1 && opaque > 50)
                {
                    sawInkVideo = true;
                    api.recv_free_video_v2(receiver, &got);
                    break;
                }
                api.recv_free_video_v2(receiver, &got);
            }
            else if (type == NDIlib_frame_type_error)
            {
                api.recv_free_metadata(receiver, &mf);
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(40));   // sender pacing
        }
    }

    api.recv_destroy(receiver);
    api.send_destroy(sender);

    INFO("received video frames: " + std::to_string(receivedVideo));
    CHECK(receivedVideo > 0);            // the loopback connection carried video at all
    CHECK(sawBgraDelivery);              // delivered in the BGRA family
    CHECK(sawTransparentVideo);          // alpha 0 across the whole empty frame, on the wire
    CHECK(sawInkVideo);                  // and real opaque ink for the caption
}

#endif // LINGOFLOW_TESTS_WITH_NDI
