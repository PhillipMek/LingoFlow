// SubtitleRenderer: the GDI+ paint of a caption into the straight-alpha BGRA
// buffer that the NDI transport ships. These tests are the pixel-level proof
// of the two claims the feature rests on:
//
//   1. background = alpha 0 everywhere when no text is shown (the receiver
//      must show nothing, not "black", not "the old text");
//   2. NDIlib_FourCC_video_type_BGRA is NOT pre-multiplied (docs.ndi.video
//      frame-types, recorded in docs/ndi-protocol.md) - asserted structurally:
//      a partially transparent edge pixel of WHITE text carries full-white
//      RGB alongside a reduced alpha. A pre-multiplied buffer could not have
//      channel 255 under alpha < 255, so these tests cannot pass on a buffer
//      that got the convention wrong. That is the practical check the task
//      demands in place of claiming OBS compatibility from documentation.
//
// Windows-only: the renderer reports honestly off Windows, and CI (windows-
// latest) runs everything below.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_message.hpp>

#include <cstdint>
#include <string>

#include "Subtitles/SubtitleRenderer.h"

using namespace liveai::subtitles;

namespace {

SubtitleSettings settings1080()
{
    SubtitleSettings s;
    s.mode = OutputMode::video;
    s.video.width = 1920;
    s.video.height = 1080;
    return s;
}

struct PixelView
{
    const std::uint8_t* bgra = nullptr;
    int width = 0, height = 0;

    std::uint32_t alpha(int x, int y) const
    { return bgra[(static_cast<std::size_t>(y) * width + x) * 4u + 3]; }

    std::uint32_t blue(int x, int y) const
    { return bgra[(static_cast<std::size_t>(y) * width + x) * 4u + 0]; }

    std::uint32_t green(int x, int y) const
    { return bgra[(static_cast<std::size_t>(y) * width + x) * 4u + 1]; }

    std::uint32_t red(int x, int y) const
    { return bgra[(static_cast<std::size_t>(y) * width + x) * 4u + 2]; }
};

PixelView view(const RenderedSubtitleFrame& f)
{
    return PixelView{ f.bgra.data(), f.width, f.height };
}

/// alpha-histogram helpers over the whole buffer.
struct AlphaStats
{
    std::size_t transparent = 0;   // a == 0
    std::size_t partial = 0;       // 0 < a < 255
    std::size_t opaque = 0;        // a == 255
    std::size_t opaqueWhite = 0;   // a == 255 and r=g=b==255
    std::size_t opaqueBlack = 0;   // a == 255 and r=g=b==0
    std::size_t partialWhite = 0;  // 0 < a < 255 and r=g=b==255  <- the straight-alpha signature
    std::size_t nonzeroRgbUnderAlpha = 0; // 0 < a < 255 with any channel > a (impossible premultiplied)
};

AlphaStats scan(const RenderedSubtitleFrame& f)
{
    AlphaStats s;
    const PixelView p = view(f);
    for (int y = 0; y < f.height; ++y)
    {
        for (int x = 0; x < f.width; ++x)
        {
            const std::uint32_t a = p.alpha(x, y);
            const std::uint32_t b = p.blue(x, y);
            const std::uint32_t g = p.green(x, y);
            const std::uint32_t r = p.red(x, y);

            if (a == 0)
                ++s.transparent;
            else if (a == 255)
            {
                ++s.opaque;
                if (b == 255 && g == 255 && r == 255)
                    ++s.opaqueWhite;
                if (b == 0 && g == 0 && r == 0)
                    ++s.opaqueBlack;
            }
            else
            {
                ++s.partial;
                if (b == 255 && g == 255 && r == 255)
                    ++s.partialWhite;
                if (b > a || g > a || r > a)
                    ++s.nonzeroRgbUnderAlpha;
            }
        }
    }
    return s;
}

} // namespace

#ifdef _WIN32

TEST_CASE("SubtitleRenderer: geometry refuses loudly, never silently", "[subtitles][render]")
{
    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.video.width = -5;
    renderer.applySettings(settings);

    RenderedSubtitleFrame frame;
    std::string error;
    CHECK_FALSE(renderer.render("x", frame, error));
    CHECK_FALSE(error.empty());

    settings = settings1080();
    settings.video.width = 999999;
    renderer.applySettings(settings);
    CHECK_FALSE(renderer.render("x", frame, error));
}

TEST_CASE("SubtitleRenderer: an empty caption is a fully transparent frame",
          "[subtitles][render][alpha]")
{
    REQUIRE(SubtitleRenderer::available());

    SubtitleRenderer renderer;
    renderer.applySettings(settings1080());

    RenderedSubtitleFrame frame;
    std::string error;
    REQUIRE(renderer.render("", frame, error));

    CHECK(frame.width == 1920);
    CHECK(frame.height == 1080);
    REQUIRE(frame.bgra.size() == static_cast<std::size_t>(1920) * 1080 * 4);

    const auto stats = scan(frame);
    CHECK(stats.transparent == frame.width * frame.height);
    CHECK(stats.opaque == 0);
    CHECK(stats.partial == 0);
}

TEST_CASE("SubtitleRenderer: opaque text pixels carry the configured colour",
          "[subtitles][render][alpha]")
{
    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;   // bare glyphs: the edge test below is pure
    renderer.applySettings(settings);

    RenderedSubtitleFrame frame;
    std::string error;
    REQUIRE(renderer.render("Test text", frame, error));
    CHECK(frame.familyUsed == settings.style.fontFamily);   // Segoe UI Semibold is on every Windows

    const auto stats = scan(frame);
    CHECK(stats.opaqueWhite > 100);      // glyph bodies exist, at 1080p this is thousands
    CHECK(stats.transparent > 100);      // and the frame is mostly transparent
    CHECK(stats.partial > 0);            // anti-aliased edges exist (quality, not aliasing)
}

TEST_CASE("SubtitleRenderer: the buffer is straight (non-premultiplied) alpha, proven structurally",
          "[subtitles][render][alpha]")
{
    // White text, no outline: every anti-aliased edge pixel is white at partial
    // alpha. A pre-multiplied buffer would have to reduce the channels with the
    // alpha; GDI+ 32bppARGB - the documented convention of NDIlib fourcc_BGRA -
    // keeps them full. The test fails loudly if that ever changes.
    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;
    renderer.applySettings(settings);

    RenderedSubtitleFrame frame;
    std::string error;
    REQUIRE(renderer.render("Alpha probe", frame, error));

    const auto stats = scan(frame);
    CHECK(stats.partial > 0);
    CHECK(stats.partialWhite > 0);              // 0<a<255 with r=g=b=255 exists
    CHECK(stats.nonzeroRgbUnderAlpha > 0);      // channels ABOVE alpha exist: impossible premultiplied
}

TEST_CASE("SubtitleRenderer: the outline paints in its own colour around the glyphs",
          "[subtitles][render]")
{
    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 3;
    settings.style.outlineColorRgb = 0x000000;   // black around white text
    renderer.applySettings(settings);

    RenderedSubtitleFrame frame;
    std::string error;
    REQUIRE(renderer.render("Outline", frame, error));

    const auto stats = scan(frame);
    CHECK(stats.opaqueWhite > 0);     // fill
    CHECK(stats.opaqueBlack > 0);     // outline ring
}

TEST_CASE("SubtitleRenderer: shadow and box are opt-in pixels", "[subtitles][render]")
{
    SubtitleRenderer plain;
    auto settings = settings1080();
    settings.style.shadowEnabled = false;
    settings.style.backgroundEnabled = false;
    plain.applySettings(settings);

    RenderedSubtitleFrame noExtras;
    std::string error;
    REQUIRE(plain.render("Extra", noExtras, error));

    settings.style.shadowEnabled = true;
    settings.style.shadowOffsetPx = 8;
    SubtitleRenderer withShadow;
    withShadow.applySettings(settings);
    RenderedSubtitleFrame shadowed;
    REQUIRE(withShadow.render("Extra", shadowed, error));

    // The shadow puts ink where the plain render had pure transparency.
    std::size_t newInk = 0;
    const PixelView plainV = view(noExtras);
    const PixelView shadowV = view(shadowed);
    for (int y = 0; y < shadowed.height; ++y)
        for (int x = 0; x < shadowed.width; ++x)
            if (plainV.alpha(x, y) == 0 && shadowV.alpha(x, y) > 0)
                ++newInk;
    CHECK(newInk > 0);

    settings.style.shadowEnabled = false;
    settings.style.backgroundEnabled = true;
    SubtitleRenderer withBox;
    withBox.applySettings(settings);
    RenderedSubtitleFrame boxed;
    REQUIRE(withBox.render("Extra", boxed, error));

    // A box pixel at the block's corner (far from glyph ink at the frame edges):
    // dark, partially transparent - not the nothing the plain frame shows there.
    std::size_t boxInk = 0;
    const PixelView boxV = view(boxed);
    for (int y = 0; y < boxed.height; ++y)
        for (int x = 0; x < boxed.width; ++x)
            if (plainV.alpha(x, y) == 0 && boxV.alpha(x, y) >= 100 && boxV.alpha(x, y) <= 160)
                ++boxInk;
    CHECK(boxInk > 0);   // ~50% alpha region exists = the background box
}

TEST_CASE("SubtitleRenderer: an unavailable font family falls back and says so",
          "[subtitles][render][fonts]")
{
    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.fontFamily = "NoSuch Family At All";
    renderer.applySettings(settings);

    RenderedSubtitleFrame frame;
    std::string error;
    const bool painted = renderer.render("Fallback", frame, error);
    INFO(error);
    REQUIRE(painted);
    CHECK(frame.familyUsed != "NoSuch Family At All");   // fell back visibly
    CHECK(scan(frame).opaqueWhite > 0);                  // and still painted
}

TEST_CASE("SubtitleRenderer: Cyrillic, Unicode and long words survive the paint",
          "[subtitles][render][fonts]")
{
    SubtitleRenderer renderer;
    renderer.applySettings(settings1080());

    RenderedSubtitleFrame frame;
    std::string error;

    REQUIRE(renderer.render("Перевод звучит в реальном времени", frame, error));
    CHECK(scan(frame).opaqueWhite > 100);   // Cyrillic ink exists

    REQUIRE(renderer.render("Deutschsprachige Gesellschaftsversammlung", frame, error));
    CHECK(scan(frame).opaqueWhite > 100);

    // Mixed script + punctuation + math symbols: the promise is "no crash, ink
    // exists", NOT "every glyph renders" - GDI+ has no per-glyph fallback
    // guarantee here, and the docs say so (docs/ndi-obs.md limits).
    REQUIRE(renderer.render("Привет! мир? 2+2=4 → (c) §", frame, error));
    CHECK(scan(frame).opaqueWhite > 100);
}

TEST_CASE("SubtitleRenderer: a different geometry paints a correctly sized buffer",
          "[subtitles][render]")
{
    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.video.width = 1280;
    settings.video.height = 720;
    renderer.applySettings(settings);

    RenderedSubtitleFrame frame;
    std::string error;
    REQUIRE(renderer.render("Same text", frame, error));
    CHECK(frame.width == 1280);
    CHECK(frame.height == 720);
    CHECK(frame.bgra.size() == static_cast<std::size_t>(1280) * 720 * 4);

    // Proportional scaling: the same caption occupies a smaller ink height in
    // absolute pixels at 720p than at 1080p.
    auto full = settings1080();
    SubtitleRenderer renderer1080;
    renderer1080.applySettings(full);
    RenderedSubtitleFrame big;
    REQUIRE(renderer1080.render("Same text", big, error));

    CHECK(frame.layout.fontPx == Catch::Approx(big.layout.fontPx * (720.0f / 1080.0f)).epsilon(0.001f));
    CHECK(frame.layout.lineHeightPx == Catch::Approx(big.layout.lineHeightPx * (720.0f / 1080.0f)).epsilon(0.001f));
}

TEST_CASE("SubtitleRenderer: live settings changes take effect on the next render",
          "[subtitles][render]")
{
    // The transport applies settings without restarting anything; the renderer
    // contract is that the next render obeys them. White text on transparent:
    // changing the colour must change the opaque pixels' RGB.
    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;
    settings.style.textColorRgb = 0xFFFFFF;
    renderer.applySettings(settings);

    RenderedSubtitleFrame frame;
    std::string error;
    REQUIRE(renderer.render("Colour shift", frame, error));
    CHECK(scan(frame).opaqueWhite > 100);

    settings.style.textColorRgb = 0x00FF00;   // green text
    renderer.applySettings(settings);
    REQUIRE(renderer.render("Colour shift", frame, error));

    const auto stats = scan(frame);
    CHECK(stats.opaqueWhite == 0);            // no white remains...
    std::size_t green = 0;
    const PixelView p = view(frame);
    for (int y = 0; y < frame.height && green == 0; ++y)
        for (int x = 0; x < frame.width; ++x)
            if (p.alpha(x, y) == 255 && p.green(x, y) == 255 && p.red(x, y) == 0)
            { ++green; break; }
    CHECK(green > 0);                          // ...and the new colour is on screen
}

#else // !_WIN32

TEST_CASE("SubtitleRenderer: off Windows the renderer says what it is", "[subtitles][render]")
{
    CHECK_FALSE(SubtitleRenderer::available());
    SubtitleRenderer renderer;
    RenderedSubtitleFrame frame;
    std::string error;
    CHECK_FALSE(renderer.render("x", frame, error));
    CHECK_FALSE(error.empty());
}

#endif // _WIN32
