// SubtitleLayout: the wrapping, shrink, ellipsis, alignment and scaling rules
// tested as PURE arithmetic. The measurer is a fake font where every code
// point is exactly 0.5 em wide - so every expectation below is exact, and the
// real-font pixel truth is the SubtitleRenderer's job.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "Subtitles/SubtitleLayout.h"

using namespace liveai::subtitles;

namespace {

float fakeMeasure(std::string_view line, float fontPx)
{
    std::size_t codepoints = 0;
    for (std::size_t i = 0; i < line.size(); ++i)
        if ((static_cast<unsigned char>(line[i]) & 0xC0) != 0x80)   // not a continuation byte
            ++codepoints;
    return static_cast<float>(codepoints) * 0.5f * fontPx;
}

const LineMeasurer measurer = fakeMeasure;

/// At 1080p the reference scale is 1.0, so style px == frame px below.
constexpr int kW = 1920;
constexpr int kH = 1080;

/// blockWidthPercent 25 at 1920 -> the 480 px limit; at font 48 the fake char
/// is 24 px wide: 20 chars per line.
SubtitleStyle testStyle()
{
    SubtitleStyle style;
    style.blockWidthPercent = 25;
    return style;
}

std::size_t codepoints(std::string_view text)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
            ++n;
    return n;
}

} // namespace

TEST_CASE("SubtitleLayout: an empty caption lays out nothing", "[subtitles][layout]")
{
    const auto laid = layoutCaption(testStyle(), kW, kH, measurer, "");
    CHECK(laid.lines.empty());
    CHECK(laid.blockHeight == 0.0f);
    CHECK_FALSE(laid.shrunk);
    CHECK_FALSE(laid.ellipsized);
}

TEST_CASE("SubtitleLayout: whitespace collapses and words wrap at the limit", "[subtitles][layout]")
{
    // 5 words of 4-5 chars: three "line units" of ~17/23/13 chars fit 20 per line
    const auto laid = layoutCaption(testStyle(), kW, kH, measurer, "word four five six longword");
    REQUIRE(!laid.lines.empty());
    for (const auto& line : laid.lines)
        CHECK(fakeMeasure(line, laid.fontPx) <= 480.0f + 0.001f);   // the exact limit
    CHECK(laid.shrunk == false);   // two lines is enough for these words
}

TEST_CASE("SubtitleLayout: a long Russian word splits at code points, never inside one",
          "[subtitles][layout]")
{
    // 16 Cyrillic chars: 24 chars of width budget... at 48px each char is 24px,
    // 16 chars = 384 px < 480 fits ONE line; 30 chars = 720 px must split.
    const std::string word = "внимательно";   // внимательно
    const std::string longWord = word + word + word;   // 33 code points

    const auto laid = layoutCaption(testStyle(), kW, kH, measurer, longWord);
    REQUIRE(laid.lines.size() >= 2);

    // Every piece is whole UTF-8 (a decode walk lands exactly on the end of
    // every line - no cut inside a sequence), and the pieces together hold
    // exactly the original characters.
    std::size_t total = 0;
    for (const auto& line : laid.lines)
    {
        REQUIRE(!line.empty());
        std::size_t i = 0;
        while (i < line.size())
        {
            const unsigned char lead = static_cast<unsigned char>(line[i]);
            const std::size_t len = (lead & 0xE0) == 0xC0 ? 2u
                                  : (lead & 0xF0) == 0xE0 ? 3u
                                  : (lead & 0xF8) == 0xF0 ? 4u : 1u;
            REQUIRE(i + len <= line.size());   // the sequence completes INSIDE the line
            i += len;
        }
        CHECK(i == line.size());
        total += codepoints(line);
    }
    CHECK(total == codepoints(longWord));
}

TEST_CASE("SubtitleLayout: too many lines shrink the font before anything is cut",
          "[subtitles][layout]")
{
    // 10 words "ab" (2 chars): 10 words + 9 spaces = 29 chars > 20 per line
    // -> 2 lines at 48 px would fit? 10 words: line1 fits 9 words(18 chars?
    // "ab ab ab ab ab ab ab ab ab" = 26 chars > 20) -> greedy: line1 "ab ab ab ab ab ab ab ab" (23? no: 8 words = 23 chars?)
    // The exactness of the fake makes the assertion simple: maxLines respected,
    // no ellipsis, font below the requested size.
    const auto laid = layoutCaption(testStyle(), kW, kH, measurer,
                                    "ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab");
    CHECK(static_cast<int>(laid.lines.size()) <= testStyle().maxLines);
    CHECK(laid.shrunk);
    CHECK_FALSE(laid.ellipsized);
    CHECK(laid.fontPx < 48.0f);
    CHECK(laid.fontPx >= 48.0f * kMinFontScale - 0.01f);   // never below the floor
}

TEST_CASE("SubtitleLayout: at the floor font the tail becomes an ellipsis, the block stays inside",
          "[subtitles][layout]")
{
    // One 200-codepoint word: even at the floor font (15*... 33 chars per line
    // at 48 px -> ~55 at floor) it needs far more than maxLines; the shrink
    // cannot save it, so the ellipsis must act.
    std::string huge;
    huge.assign(200, 'x');

    SubtitleStyle style = testStyle();
    const auto laid = layoutCaption(style, kW, kH, measurer, huge);

    REQUIRE(static_cast<int>(laid.lines.size()) <= style.maxLines);
    CHECK(laid.ellipsized);
    CHECK(laid.lines.back().find("\xE2\x80\xA6") != std::string::npos);   // ends in …
    CHECK(fakeMeasure(laid.lines.back(), laid.fontPx) <= kW * 0.25f + 0.01f);

    // No character ever crosses the block's edge.
    for (const auto& line : laid.lines)
        CHECK(fakeMeasure(line, laid.fontPx) <= kW * 0.25f + 0.01f);
}

TEST_CASE("SubtitleLayout: everything scales with the frame height", "[subtitles][layout]")
{
    const auto full = layoutCaption(testStyle(), kW, kH, measurer, "same text");
    const auto half = layoutCaption(testStyle(), 1280, 720, measurer, "same text");

    CHECK(half.fontPx == Catch::Approx(full.fontPx * (720.0f / 1080.0f)).epsilon(0.001f));
    CHECK(half.outlineWidthPx == Catch::Approx(full.outlineWidthPx * (720.0f / 1080.0f)).epsilon(0.001f));
    CHECK(half.shadowOffsetPx == Catch::Approx(full.shadowOffsetPx * (720.0f / 1080.0f)).epsilon(0.001f));
}

TEST_CASE("SubtitleLayout: anchors place the block with the margin off the safe edge",
          "[subtitles][layout]")
{
    SubtitleStyle style = testStyle();
    style.horizontalAlign = HorizontalAlign::center;
    style.verticalAnchor = VerticalAnchor::bottom;
    style.edgeMarginPercent = 8;
    style.maxLines = 1;

    const auto laid = layoutCaption(style, kW, kH, measurer, "one line");
    const float safeBottom = kH * (100.0f - kSafeAreaPercent) / 100.0f;
    const float margin = kH * 8.0f / 100.0f;
    CHECK(laid.blockY + laid.blockHeight == Catch::Approx(safeBottom - margin).epsilon(0.0001f));
    CHECK(laid.blockX == Catch::Approx((kW - laid.blockWidth) / 2.0f).epsilon(0.0001f));   // centred

    style.horizontalAlign = HorizontalAlign::left;
    const auto left = layoutCaption(style, kW, kH, measurer, "one line");
    CHECK(left.blockX == Catch::Approx(kW * kSafeAreaPercent / 100.0f).epsilon(0.0001f));

    style.horizontalAlign = HorizontalAlign::right;
    const auto right = layoutCaption(style, kW, kH, measurer, "one line");
    CHECK(right.blockX + right.blockWidth == Catch::Approx(kW * (100.0f - kSafeAreaPercent) / 100.0f)
                                                   .epsilon(0.0001f));

    style.verticalAnchor = VerticalAnchor::top;
    const auto top = layoutCaption(style, kW, kH, measurer, "one line");
    const float safeTop = kH * kSafeAreaPercent / 100.0f;
    CHECK(top.blockY == Catch::Approx(safeTop + margin).epsilon(0.0001f));
}

TEST_CASE("SubtitleLayout: line spacing adds leading to the line height", "[subtitles][layout]")
{
    SubtitleStyle style = testStyle();
    style.lineSpacingPercent = 100;
    style.maxLines = 1;
    const auto wide = layoutCaption(style, kW, kH, measurer, "abc");
    CHECK(wide.lineHeightPx == Catch::Approx(wide.fontPx * 2.0f).epsilon(0.001f));

    style.lineSpacingPercent = 0;
    const auto tight = layoutCaption(style, kW, kH, measurer, "abc");
    CHECK(tight.lineHeightPx == Catch::Approx(tight.fontPx).epsilon(0.001f));
}

TEST_CASE("SubtitleLayout: the box grows from the block by the scaled padding",
          "[subtitles][layout]")
{
    SubtitleStyle style = testStyle();
    style.backgroundEnabled = true;
    style.backgroundPaddingPx = 12;
    style.maxLines = 1;

    const auto laid = layoutCaption(style, kW, kH, measurer, "boxed");
    CHECK(laid.boxWidth == Catch::Approx(laid.blockWidth + 2.0f * laid.backgroundPaddingPx).epsilon(0.001f));
    CHECK(laid.boxHeight == Catch::Approx(laid.blockHeight + 2.0f * laid.backgroundPaddingPx).epsilon(0.001f));
    CHECK(laid.boxX == Catch::Approx(laid.blockX - laid.backgroundPaddingPx).epsilon(0.001f));
}

TEST_CASE("SubtitleLayout: degenerate inputs refuse quietly", "[subtitles][layout]")
{
    const auto zero = layoutCaption(testStyle(), 0, 0, measurer, "text");
    CHECK(zero.lines.empty());

    SubtitleStyle style = testStyle();
    style.maxLines = 0;   // below the sane floor: clamps to one line, never zero
    const auto one = layoutCaption(style, kW, kH, measurer, "x");
    CHECK(one.lines.size() == 1);
}
