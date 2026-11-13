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

#include <chrono>
#include <cmath>
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

/// Ink-extent probes over the whole buffer. The baseline and the left-anchor
/// tests ask "did the glyphs move?", and these answer it in frame pixels:
/// the first row with any ink, the last row with any ink, the first column.
struct InkExtents
{
    int topRow = -1, bottomRow = -1;
    int leftCol = -1;
};

InkExtents ink(const RenderedSubtitleFrame& f)
{
    InkExtents e;
    const PixelView p = view(f);
    for (int y = 0; y < f.height; ++y)
    {
        bool rowInk = false;
        for (int x = 0; x < f.width; ++x)
            if (p.alpha(x, y) > 0)
            {
                rowInk = true;
                break;
            }
        if (rowInk)
        {
            if (e.topRow < 0)
                e.topRow = y;
            e.bottomRow = y;
        }
    }
    for (int x = 0; x < f.width; ++x)
    {
        for (int y = 0; y < f.height; ++y)
            if (p.alpha(x, y) > 0)
            {
                e.leftCol = x;
                break;
            }
        if (e.leftCol >= 0)
            break;
    }
    return e;
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

// ---------------------------------------------------------- NDI single-line mode
// The renderer passes the captions through layoutCaption's singleLineTrimHead
// mode: one row, the configured font size, the oldest characters trimmed off
// the head. These are the GDI+-measured proofs of that promise.

TEST_CASE("SubtitleRenderer: NDI captions are one fixed-size row trimmed at the head",
          "[subtitles][render][singleline]")
{
    REQUIRE(SubtitleRenderer::available());

    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;
    renderer.applySettings(settings);

    RenderedSubtitleFrame shortFrame;
    std::string error;
    REQUIRE(renderer.render("Short line", shortFrame, error));
    REQUIRE(shortFrame.layout.lines.size() == 1);
    CHECK(shortFrame.layout.lines[0] == "Short line");
    CHECK_FALSE(shortFrame.layout.headTrimmed);
    CHECK(shortFrame.layout.fontPx == 48.0f);   // the configured size at 1080p

    // Far more text than 85% of a 1920 px row at 48 px can ever show.
    const std::string longCaption =
        "The interpreter kept speaking about economics and logistics and medicine "
        "while the translated caption grew well past what this single row can show";

    RenderedSubtitleFrame longFrame;
    REQUIRE(renderer.render(longCaption, longFrame, error));

    CHECK(longFrame.layout.lines.size() == 1);                   // always one row
    CHECK(longFrame.layout.fontPx == shortFrame.layout.fontPx);  // the size never adapts
    CHECK_FALSE(longFrame.layout.shrunk);
    CHECK_FALSE(longFrame.layout.ellipsized);
    CHECK(longFrame.layout.headTrimmed);
    CHECK(longFrame.layout.lineHeightPx == shortFrame.layout.lineHeightPx);
    CHECK(longFrame.layout.blockHeight == shortFrame.layout.blockHeight);  // stable geometry

    const std::string& shown = longFrame.layout.lines.front();
    CHECK(shown.size() < longCaption.size());
    CHECK(longCaption.ends_with(shown));        // the newest characters survive
    CHECK(shown.find('\n') == std::string::npos);
    CHECK(shown.find('\t') == std::string::npos);

    // The trim rests on the GDI+ measurement: the painted row is inside its block.
    CHECK(longFrame.layout.maxLineWidth <= 1920.0f * 0.85f + 0.01f);
    CHECK(scan(longFrame).opaqueWhite > 100);   // and it really painted the suffix
}

TEST_CASE("SubtitleRenderer: Cyrillic trimming lands on character boundaries, never mid-sequence",
          "[subtitles][render][singleline][fonts]")
{
    SubtitleRenderer renderer;
    renderer.applySettings(settings1080());

    const std::string caption =
        "Переводчик говорил об экономике и логистике, а строка субтитров выросла "
        "настолько что в неё уже не помещались даже самые свежие слова переводчика";

    RenderedSubtitleFrame frame;
    std::string error;
    REQUIRE(renderer.render(caption, frame, error));

    REQUIRE(frame.layout.lines.size() == 1);
    const std::string& shown = frame.layout.lines.front();
    CHECK(frame.layout.headTrimmed);
    CHECK(caption.ends_with(shown));

    // The first byte of the row is a lead byte - no multibyte character was
    // cut in half at the head edge.
    REQUIRE(!shown.empty());
    CHECK((static_cast<unsigned char>(shown.front()) & 0xC0) != 0x80);

    // And the whole row decodes as complete UTF-8 sequences.
    std::size_t i = 0;
    while (i < shown.size())
    {
        const unsigned char lead = static_cast<unsigned char>(shown[i]);
        const std::size_t len = (lead & 0xE0) == 0xC0 ? 2u
                              : (lead & 0xF0) == 0xE0 ? 3u
                              : (lead & 0xF8) == 0xF0 ? 4u : 1u;
        REQUIRE(i + len <= shown.size());
        i += len;
    }
    CHECK(frame.layout.maxLineWidth <= 1920.0f * 0.85f + 0.01f);
    CHECK(scan(frame).opaqueWhite > 100);
}

TEST_CASE("SubtitleRenderer: a caption carrying line breaks still renders one row",
          "[subtitles][render][singleline]")
{
    SubtitleRenderer renderer;
    renderer.applySettings(settings1080());

    RenderedSubtitleFrame frame;
    std::string error;
    REQUIRE(renderer.render("first\r\nsecond\t\tthird", frame, error));
    REQUIRE(frame.layout.lines.size() == 1);
    CHECK(frame.layout.lines.front() == "first second third");
    CHECK(scan(frame).opaqueWhite > 100);

    // A whitespace-only caption is the transparent frame: the same clearing
    // promise the empty caption carries, reached through the layout, not a crash.
    RenderedSubtitleFrame blank;
    REQUIRE(renderer.render("  \n\t ", blank, error));
    CHECK(blank.layout.lines.empty());
    CHECK(scan(blank).transparent == static_cast<std::size_t>(blank.width) * blank.height);
}

TEST_CASE("SubtitleRenderer: geometry changes keep one row and the proportional font",
          "[subtitles][render][singleline]")
{
    const std::string caption =
        "One sentence measured at two resolutions: the row count and the style "
        "scale are the settings' answer, never the caption length's answer";

    SubtitleRenderer full;
    full.applySettings(settings1080());
    SubtitleRenderer small;
    auto smallSettings = settings1080();
    smallSettings.video.width = 1280;
    smallSettings.video.height = 720;
    small.applySettings(smallSettings);

    RenderedSubtitleFrame big, little;
    std::string error;
    REQUIRE(full.render(caption, big, error));
    REQUIRE(small.render(caption, little, error));

    CHECK(big.layout.lines.size() == 1);
    CHECK(little.layout.lines.size() == 1);
    CHECK(little.layout.fontPx == Catch::Approx(big.layout.fontPx * (720.0f / 1080.0f)).epsilon(0.001f));
    CHECK(big.layout.maxLineWidth <= 1920.0f * 0.85f + 0.01f);
    CHECK(little.layout.maxLineWidth <= 1280.0f * 0.85f + 0.01f);
}

TEST_CASE("SubtitleRenderer: a pathological word and a degenerate block stay honest",
          "[subtitles][render][singleline]")
{
    SubtitleRenderer renderer;
    renderer.applySettings(settings1080());
    RenderedSubtitleFrame frame;
    std::string error;

    // 5000 characters in one word: the trim is a binary search over cluster
    // starts, so this completes promptly with exactly one fitted row.
    const std::string word(5000, 'a');
    REQUIRE(renderer.render(word, frame, error));
    REQUIRE(frame.layout.lines.size() == 1);
    CHECK(frame.layout.fontPx == 48.0f);
    CHECK(frame.layout.headTrimmed);
    CHECK(frame.layout.maxLineWidth <= 1920.0f * 0.85f + 0.01f);
    CHECK(word.ends_with(frame.layout.lines.front()));

    // A block narrower than the newest single character: the character rides
    // (UTF-8 intact), the frame reports the clamp, and nothing hangs.
    auto narrow = settings1080();
    narrow.style.blockWidthPercent = 1;   // 19.2 px < any Cyrillic glyph at 48 px
    renderer.applySettings(narrow);
    RenderedSubtitleFrame lastChar;
    REQUIRE(renderer.render("абвгд", lastChar, error));
    REQUIRE(lastChar.layout.lines.size() == 1);
    CHECK(lastChar.layout.lines.front() == "\xd0\xb4");   // "д", whole two-byte sequence
    CHECK(lastChar.layout.headTrimmed);
    CHECK(std::isfinite(lastChar.layout.blockX));
    CHECK(std::isfinite(lastChar.layout.blockY));
}

// ------------------------------------------------- baseline stability & left anchor
// The readability promises of the live caption: a row placed by the FONT's
// metrics, not by the ink it happens to carry, and anchored to the safe-area
// left edge. Both are asserted on real pixels, because both were exactly what
// the streaming updates used to break.

TEST_CASE("SubtitleRenderer: the baseline is pinned - arriving descenders do not move the row",
          "[subtitles][render][baseline]")
{
    REQUIRE(SubtitleRenderer::available());

    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;
    renderer.applySettings(settings);

    RenderedSubtitleFrame noDesc, withDesc;
    std::string error;
    REQUIRE(renderer.render("aaaa", noDesc, error));
    REQUIRE(renderer.render("aaappp", withDesc, error));   // descenders join the row

    const auto plain = ink(noDesc);
    const auto desc = ink(withDesc);
    CHECK(plain.topRow >= 0);
    // The x-height letters start on the very same pixel row in both frames:
    // centering the tight ink bounds (the previous behavior) pulled the whole
    // row up the moment descenders appeared.
    CHECK(plain.topRow == desc.topRow);
    // The descender reaches BELOW the fixed baseline instead of relocating it.
    CHECK(desc.bottomRow > plain.bottomRow);

    // The layout geometry that feeds the placement is content-independent.
    CHECK(noDesc.layout.blockX == withDesc.layout.blockX);
    CHECK(noDesc.layout.blockY == withDesc.layout.blockY);
    CHECK(noDesc.layout.lineHeightPx == withDesc.layout.lineHeightPx);
}

TEST_CASE("SubtitleRenderer: the shipped default anchors the row to the safe-area left edge",
          "[subtitles][render][baseline]")
{
    REQUIRE(SubtitleRenderer::available());

    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;   // the default alignment is left
    renderer.applySettings(settings);

    RenderedSubtitleFrame shortFrame, grownFrame;
    std::string error;
    REQUIRE(renderer.render("word", shortFrame, error));
    REQUIRE(renderer.render("word words words", grownFrame, error));

    CHECK(shortFrame.layout.blockX == Catch::Approx(1920.0f * kSafeAreaPercent / 100.0f));
    CHECK(grownFrame.layout.blockX == shortFrame.layout.blockX);

    // Already-read words do not slide sideways when new ones arrive: the ink
    // starts on the same column for both captions (a centered block moved it).
    const auto plain = ink(shortFrame);
    const auto grown = ink(grownFrame);
    CHECK(plain.leftCol >= 0);
    CHECK(plain.leftCol == grown.leftCol);
    CHECK(ink(shortFrame).topRow == ink(grownFrame).topRow);
}

// --------------------------------------------------------------------- word fade-in
// setWordFadeEnabled is the transport's opt-in (a one-shot painter must never
// freeze mid-ramp). The ramp itself is tested with an injected clock, so every
// assertion below is exact wall-time, not a sleep.

TEST_CASE("SubtitleRenderer: the fade is opt-in and off by default", "[subtitles][render][fade]")
{
    REQUIRE(SubtitleRenderer::available());

    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;
    renderer.applySettings(settings);

    RenderedSubtitleFrame frame;
    std::string error;
    REQUIRE(renderer.render("static paint", frame, error));
    CHECK(scan(frame).opaqueWhite > 0);        // one-shot paints arrive finished...
    CHECK_FALSE(renderer.animationPending());  // ...and never owe a follow-up frame
}

TEST_CASE("SubtitleRenderer: a word ramps from invisible to opaque across its fade window",
          "[subtitles][render][fade]")
{
    REQUIRE(SubtitleRenderer::available());

    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;
    renderer.applySettings(settings);
    renderer.setWordFadeEnabled(true);

    const SteadyTime t0 = std::chrono::steady_clock::now();
    auto at = [](int milliseconds) { return std::chrono::milliseconds(milliseconds); };

    RenderedSubtitleFrame frame;
    std::string error;

    REQUIRE(renderer.render("one", frame, error, t0));
    const auto arrival = scan(frame);
    CHECK(arrival.opaque == 0);   // the frame the word ARRIVED on shows nothing yet
    CHECK(arrival.transparent == static_cast<std::size_t>(frame.width) * frame.height);
    CHECK(renderer.animationPending());

    REQUIRE(renderer.render("one", frame, error, t0 + at(120)));
    const auto mid = scan(frame);
    CHECK(mid.opaque == 0);       // mid-ramp: every ink pixel carries reduced alpha
    CHECK(mid.partial > 0);
    CHECK(mid.partialWhite > 0);  // straight alpha preserved through the animation
    CHECK(renderer.animationPending());

    REQUIRE(renderer.render("one", frame, error, t0 + at(SubtitleRenderer::kWordFadeInMs)));
    const auto done = scan(frame);
    CHECK(done.opaqueWhite > 100);
    CHECK_FALSE(renderer.animationPending());
}

TEST_CASE("SubtitleRenderer: new words fade in without restarting the words already on screen",
          "[subtitles][render][fade]")
{
    REQUIRE(SubtitleRenderer::available());

    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;
    renderer.applySettings(settings);
    renderer.setWordFadeEnabled(true);

    const SteadyTime t0 = std::chrono::steady_clock::now();
    auto at = [](int milliseconds) { return std::chrono::milliseconds(milliseconds); };

    RenderedSubtitleFrame frame;
    std::string error;

    REQUIRE(renderer.render("one", frame, error, t0));
    REQUIRE(renderer.render("one", frame, error, t0 + at(300)));
    const auto oneAlone = scan(frame);
    CHECK(oneAlone.opaqueWhite > 100);
    CHECK_FALSE(renderer.animationPending());

    // "two" arrives at the same instant: it starts invisible while "one" keeps
    // painting exactly what it painted last frame - no restart, no shift.
    REQUIRE(renderer.render("one two", frame, error, t0 + at(300)));
    const auto arriving = scan(frame);
    CHECK(arriving.opaqueWhite == oneAlone.opaqueWhite);
    CHECK(renderer.animationPending());

    // The ramp completes: the line grew, nothing about the old word moved.
    REQUIRE(renderer.render("one two", frame, error, t0 + at(600)));
    const auto settled = scan(frame);
    CHECK(settled.opaqueWhite > oneAlone.opaqueWhite);
    CHECK_FALSE(renderer.animationPending());

    // A replaced caption shares no words: ALL of it is new, it fades from zero.
    REQUIRE(renderer.render("three", frame, error, t0 + at(600)));
    CHECK(scan(frame).opaque == 0);
    CHECK(renderer.animationPending());
}

TEST_CASE("SubtitleRenderer: the head trim never restarts the animation",
          "[subtitles][render][fade][singleline]")
{
    REQUIRE(SubtitleRenderer::available());

    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;
    renderer.applySettings(settings);
    renderer.setWordFadeEnabled(true);

    const std::string longCaption =
        "The interpreter kept speaking about economics and logistics and medicine "
        "while the translated caption grew well past what this single row can show";

    const SteadyTime t0 = std::chrono::steady_clock::now();
    auto at = [](int milliseconds) { return std::chrono::milliseconds(milliseconds); };

    RenderedSubtitleFrame frame;
    std::string error;

    REQUIRE(renderer.render(longCaption, frame, error, t0));
    CHECK(scan(frame).opaque == 0);          // the whole row arrived together: all of it is ramping
    CHECK(renderer.animationPending());

    REQUIRE(renderer.render(longCaption, frame, error, t0 + at(300)));
    CHECK(scan(frame).opaqueWhite > 100);    // settled at full opacity
    CHECK_FALSE(renderer.animationPending());

    // One more word pushes the trim further into the head. The words that are
    // still on screen keep their (old) arrival times; only the newcomer fades.
    const std::string extended = longCaption + " now";
    REQUIRE(renderer.render(extended, frame, error, t0 + at(300)));
    CHECK(scan(frame).opaqueWhite > 0);      // the already-read words did NOT restart...
    CHECK(renderer.animationPending());      // ...and the new one is the only ramp in flight
    CHECK(frame.layout.headTrimmed);
    CHECK(extended.ends_with(frame.layout.lines.front()));
    CHECK(frame.layout.lines.front().find("now") != std::string::npos);

    REQUIRE(renderer.render(extended, frame, error, t0 + at(600)));
    CHECK_FALSE(renderer.animationPending());
    CHECK(scan(frame).opaqueWhite > 0);
}

TEST_CASE("SubtitleRenderer: a live style change does not restart word animations",
          "[subtitles][render][fade]")
{
    REQUIRE(SubtitleRenderer::available());

    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;
    renderer.applySettings(settings);
    renderer.setWordFadeEnabled(true);

    const SteadyTime t0 = std::chrono::steady_clock::now();
    auto at = [](int milliseconds) { return std::chrono::milliseconds(milliseconds); };

    RenderedSubtitleFrame frame;
    std::string error;
    REQUIRE(renderer.render("one two", frame, error, t0));
    REQUIRE(renderer.render("one two", frame, error, t0 + at(300)));
    CHECK_FALSE(renderer.animationPending());

    settings.style.fontSizePx = 64;
    renderer.applySettings(settings);
    REQUIRE(renderer.render("one two", frame, error, t0 + at(310)));
    CHECK_FALSE(renderer.animationPending());      // the words arrived at t0: their ramps are long done
    CHECK(scan(frame).opaqueWhite > 0);            // fully visible at the new size, not faded again
}

TEST_CASE("SubtitleRenderer: an ended caption clears the word timeline", "[subtitles][render][fade]")
{
    REQUIRE(SubtitleRenderer::available());

    SubtitleRenderer renderer;
    auto settings = settings1080();
    settings.style.outlineWidthPx = 0;
    renderer.applySettings(settings);
    renderer.setWordFadeEnabled(true);

    const SteadyTime t0 = std::chrono::steady_clock::now();
    auto at = [](int milliseconds) { return std::chrono::milliseconds(milliseconds); };

    RenderedSubtitleFrame frame;
    std::string error;
    REQUIRE(renderer.render("alpha", frame, error, t0));
    REQUIRE(renderer.render("alpha", frame, error, t0 + at(300)));
    CHECK(scan(frame).opaqueWhite > 0);
    CHECK_FALSE(renderer.animationPending());

    // The sentence ended (the transport latches the empty text): transparent
    // frame, no pending animation, timeline forgotten.
    REQUIRE(renderer.render("", frame, error, t0 + at(400)));
    CHECK(scan(frame).transparent == static_cast<std::size_t>(frame.width) * frame.height);
    CHECK_FALSE(renderer.animationPending());

    // The same words on a new sentence are new words: they fade in again.
    REQUIRE(renderer.render("alpha", frame, error, t0 + at(500)));
    CHECK(scan(frame).opaque == 0);
    CHECK(renderer.animationPending());
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
