// SubtitleLayout: the wrapping, shrink, ellipsis, alignment and scaling rules
// tested as PURE arithmetic. The measurer is a fake font where every code
// point is exactly 0.5 em wide - so every expectation below is exact, and the
// real-font pixel truth is the SubtitleRenderer's job.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
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

// ======================================================= NDI single-line mode
//
// LayoutMode::singleLineTrimHead is the rule set the NDI video renderer asks
// for: one row, the configured font size, the head trimmed to the longest
// suffix that MEASURED width says fits. With the fake font (every code point
// exactly 24 px at 48 px em against the 480 px limit) every expectation here
// is exact, including which character the cut lands on.

TEST_CASE("SubtitleLayout NDI single-line: a caption that fits is shown whole",
          "[subtitles][layout][singleline]")
{
    const auto laid = layoutCaption(testStyle(), kW, kH, measurer, "short caption",
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(laid.lines.size() == 1);
    CHECK(laid.lines[0] == "short caption");
    CHECK_FALSE(laid.headTrimmed);
    CHECK_FALSE(laid.shrunk);       // the single-line mode never has either rule act
    CHECK_FALSE(laid.ellipsized);
    CHECK(laid.fontPx == 48.0f);    // the configured size at 1080p
}

TEST_CASE("SubtitleLayout NDI single-line: overflow trims the head, the newest suffix survives",
          "[subtitles][layout][singleline]")
{
    // 39 code points against a 20-code-point line: the answer is exactly the
    // newest fitting suffix, character for character.
    const std::string text = "This is a very long translated subtitle";
    const auto laid = layoutCaption(testStyle(), kW, kH, measurer, text,
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(laid.lines.size() == 1);
    CHECK(laid.lines[0] == text.substr(text.size() - 20));   // " translated subtitle"
    CHECK(laid.headTrimmed);
    CHECK(fakeMeasure(laid.lines[0], laid.fontPx) <= 480.0f + 0.001f);
    // The trim is maximal: the oldest character it dropped genuinely did not
    // fit - the claim is on measured width, never on a character count.
    CHECK(fakeMeasure(text.substr(text.size() - 21), laid.fontPx) > 480.0f);
}

TEST_CASE("SubtitleLayout NDI single-line: the font never depends on the text length",
          "[subtitles][layout][singleline]")
{
    SubtitleStyle style = testStyle();
    style.maxLines = 2;   // even a multi-line style lays out the row as one line

    const std::string huge(20000, 'x');
    const auto longLaid = layoutCaption(style, kW, kH, measurer, huge, LayoutMode::singleLineTrimHead);
    const auto shortLaid = layoutCaption(style, kW, kH, measurer, "a", LayoutMode::singleLineTrimHead);

    REQUIRE(longLaid.lines.size() == 1);
    CHECK(longLaid.fontPx == shortLaid.fontPx);      // the configured size, always
    CHECK(longLaid.fontPx == 48.0f);
    CHECK(longLaid.lines[0].size() == 20);
    CHECK(longLaid.blockHeight == shortLaid.blockHeight);   // stable geometry
    CHECK(longLaid.blockY == shortLaid.blockY);
    CHECK(longLaid.lineHeightPx == shortLaid.lineHeightPx);
    CHECK_FALSE(longLaid.shrunk);
    CHECK_FALSE(longLaid.ellipsized);
    CHECK(longLaid.lines[0].find("\xE2\x80\xA6") == std::string::npos);   // no ellipsis either
}

TEST_CASE("SubtitleLayout NDI single-line: break characters never reach the row",
          "[subtitles][layout][singleline]")
{
    const auto laid = layoutCaption(testStyle(), kW, kH, measurer,
                                    "alpha\n beta\t gamma\r\n  delta\v",
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(laid.lines.size() == 1);
    // 22 code points against the 20 budget: joined first, then the head trim
    // cut the oldest two - no break character can be anywhere in the answer.
    CHECK(laid.lines[0] == "pha beta gamma delta");
    CHECK(laid.headTrimmed);
    CHECK(laid.lines[0].find_first_of("\n\r\t\v\f") == std::string::npos);

    const auto fits = layoutCaption(testStyle(), kW, kH, measurer, "alpha\n beta\t gamma",
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(fits.lines.size() == 1);
    CHECK(fits.lines[0] == "alpha beta gamma");   // joined whole, nothing trimmed
    CHECK_FALSE(fits.headTrimmed);
}

TEST_CASE("SubtitleLayout NDI single-line: Cyrillic is cut between whole characters only",
          "[subtitles][layout][singleline]")
{
    const std::string word = "внимательно";        // 11 two-byte code points
    const std::string longWord = word + word + word;   // 33 > the 20-character budget

    const auto laid = layoutCaption(testStyle(), kW, kH, measurer, longWord,
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(laid.lines.size() == 1);
    const std::string& line = laid.lines[0];
    CHECK(laid.headTrimmed);
    CHECK(codepoints(line) == 20);
    CHECK(longWord.ends_with(line));   // exactly the newest characters

    // Every sequence completes INSIDE the line (the cut never split one).
    std::size_t i = 0;
    while (i < line.size())
    {
        const unsigned char lead = static_cast<unsigned char>(line[i]);
        const std::size_t len = (lead & 0xE0) == 0xC0 ? 2u
                              : (lead & 0xF0) == 0xE0 ? 3u
                              : (lead & 0xF8) == 0xF0 ? 4u : 1u;
        REQUIRE(i + len <= line.size());
        i += len;
    }
    CHECK_FALSE(line.empty());
    CHECK((static_cast<unsigned char>(line[0]) & 0xC0) != 0x80);   // lead byte, never a fragment
}

TEST_CASE("SubtitleLayout NDI single-line: combining marks never part from their base",
          "[subtitles][layout][singleline]")
{
    // 30 clusters of "q" + U+0301 COMBINING ACUTE: 60 code points against a
    // 20-code-point line - the trim must count whole clusters.
    std::string text;
    for (int k = 0; k < 30; ++k)
        text += "q\xCC\x81";   // UTF-8 for the accent: bytes CC 81

    const auto laid = layoutCaption(testStyle(), kW, kH, measurer, text,
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(laid.lines.size() == 1);
    const std::string& line = laid.lines[0];
    CHECK(line == text.substr(text.size() - 30));   // exactly 10 whole clusters
    CHECK(static_cast<unsigned char>(line[0]) == 'q');   // a base letter, not a stranded accent
    CHECK(fakeMeasure("q\xCC\x81" + line, laid.fontPx) > 480.0f);   // maximal per cluster
}

TEST_CASE("SubtitleLayout NDI single-line: ZWJ emoji sequences ride as one cluster",
          "[subtitles][layout][singleline]")
{
    // U+1F468 ZWJ U+1F469 ZWJ U+1F467 - one family: 5 code points, one cluster.
    const std::string family =
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";

    const std::string fits = family + std::string(15, 'y');      // exactly 20 code points
    const auto kept = layoutCaption(testStyle(), kW, kH, measurer, fits,
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(kept.lines.size() == 1);
    CHECK(kept.lines[0] == fits);
    CHECK_FALSE(kept.headTrimmed);

    const std::string overflow = family + std::string(16, 'y');  // 21 code points
    const auto cut = layoutCaption(testStyle(), kW, kH, measurer, overflow,
                                   LayoutMode::singleLineTrimHead);
    REQUIRE(cut.lines.size() == 1);
    // The fitting suffix is the family-FREE row: cutting inside the sequence
    // would be "longer" by a code point and is exactly what must not happen.
    CHECK(cut.lines[0] == std::string(16, 'y'));
    CHECK(cut.headTrimmed);
}

TEST_CASE("SubtitleLayout NDI single-line: regional-indicator flags are cut whole or not at all",
          "[subtitles][layout][singleline]")
{
    // U+1F1F7 U+1F1FA: the pair that renders as the RU flag (2 code points, 1 cluster).
    const std::string flag = "\xF0\x9F\x87\xB7\xF0\x9F\x87\xBA";

    const std::string overflow = flag + std::string(19, 'z');   // 21 code points
    const auto laid = layoutCaption(testStyle(), kW, kH, measurer, overflow,
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(laid.lines.size() == 1);
    CHECK(laid.lines[0] == std::string(19, 'z'));   // never "the tail half" + z's
    CHECK(laid.headTrimmed);

    const std::string fits = flag + std::string(18, 'z');       // exactly 20
    const auto whole = layoutCaption(testStyle(), kW, kH, measurer, fits,
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(whole.lines.size() == 1);
    CHECK(whole.lines[0] == fits);
    CHECK_FALSE(whole.headTrimmed);
}

TEST_CASE("SubtitleLayout NDI single-line: empty and degenerate captions stay honest",
          "[subtitles][layout][singleline]")
{
    const auto empty = layoutCaption(testStyle(), kW, kH, measurer, "",
                                     LayoutMode::singleLineTrimHead);
    CHECK(empty.lines.empty());
    CHECK(empty.blockHeight == 0.0f);
    CHECK_FALSE(empty.headTrimmed);

    const auto spaces = layoutCaption(testStyle(), kW, kH, measurer, "  \t\n\v ",
                                      LayoutMode::singleLineTrimHead);
    CHECK(spaces.lines.empty());

    // A block narrower than ONE character: the newest cluster rides (nothing
    // is split, no loop spins), the coordinates stay the clamp's answer.
    SubtitleStyle narrow = testStyle();
    narrow.blockWidthPercent = 1;   // 19.2 px < the fake font's 24 px character
    const auto ride = layoutCaption(narrow, kW, kH, measurer, "ab cd",
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(ride.lines.size() == 1);
    CHECK(ride.lines[0] == "d");
    CHECK(ride.headTrimmed);
    CHECK(ride.blockX >= kW * kSafeAreaPercent / 100.0f);
    CHECK(std::isfinite(ride.blockX));
    CHECK(std::isfinite(ride.blockY));

    const auto zeroFrame = layoutCaption(narrow, 0, 0, measurer, "text",
                                         LayoutMode::singleLineTrimHead);
    CHECK(zeroFrame.lines.empty());
}

TEST_CASE("SubtitleLayout NDI single-line: a 20k caption is trimmed with few measurements",
          "[subtitles][layout][singleline]")
{
    // The cut position is a binary search over cluster starts, not a step
    // through characters: a render thread at 25 fps must pay O(log n).
    int calls = 0;
    const LineMeasurer counting = [&](std::string_view line, float fontPx) {
        ++calls;
        return fakeMeasure(line, fontPx);
    };
    const std::string huge(20000, 'x');
    const auto laid = layoutCaption(testStyle(), kW, kH, counting, huge,
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(laid.lines.size() == 1);
    CHECK(laid.lines[0] == std::string(20, 'x'));
    CHECK(calls < 100);   // ~17 with this fake; a per-character loop would be ~20000
}

TEST_CASE("SubtitleLayout NDI single-line: alignment and anchors still place the row",
          "[subtitles][layout][singleline]")
{
    SubtitleStyle style = testStyle();
    style.horizontalAlign = HorizontalAlign::right;
    const auto laid = layoutCaption(style, kW, kH, measurer, std::string(30, 'q'),
                                    LayoutMode::singleLineTrimHead);
    REQUIRE(laid.lines.size() == 1);
    CHECK(laid.blockWidth == Catch::Approx(480.0f).epsilon(0.001f));
    CHECK(laid.blockX + laid.blockWidth ==
          Catch::Approx(kW * (100.0f - kSafeAreaPercent) / 100.0f).epsilon(0.0001f));

    style.horizontalAlign = HorizontalAlign::left;
    const auto left = layoutCaption(style, kW, kH, measurer, std::string(30, 'q'),
                                    LayoutMode::singleLineTrimHead);
    CHECK(left.blockX == Catch::Approx(kW * kSafeAreaPercent / 100.0f).epsilon(0.0001f));
}

TEST_CASE("SubtitleLayout NDI single-line: frame changes adapt the suffix, never the font",
          "[subtitles][layout][singleline]")
{
    const std::string text = "abcdefghijklmnopqrstuvwxyzabcd";   // 30 code points

    const auto full = layoutCaption(testStyle(), kW, kH, measurer, text,
                                    LayoutMode::singleLineTrimHead);
    const auto small = layoutCaption(testStyle(), 1280, 720, measurer, text,
                                     LayoutMode::singleLineTrimHead);
    const auto wide = layoutCaption(testStyle(), 2560, 1080, measurer, text,
                                   LayoutMode::singleLineTrimHead);

    // 1080p and 720p show the same count (the whole style scales together);
    // a wider 1080p frame fits more of the tail at the same font size.
    CHECK(full.lines[0] == text.substr(10));
    CHECK(small.lines[0] == text.substr(10));
    CHECK(small.fontPx == Catch::Approx(full.fontPx * (720.0f / 1080.0f)).epsilon(0.001f));
    CHECK(wide.lines[0] == text.substr(4));   // 640 px / 24 px = 26 characters
    CHECK(wide.fontPx == full.fontPx);
}

TEST_CASE("SubtitleLayout: fitToBlock stays the default, the single-line rule is opt-in",
          "[subtitles][layout][singleline]")
{
    // Every pre-existing case above already proves the wrapped rules survive
    // unchanged; this pins the seam itself: the default argument IS the old
    // mode, and the old mode never reports the new flag.
    const std::string huge(300, 'x');
    const auto legacy = layoutCaption(testStyle(), kW, kH, measurer, huge);
    const auto same = layoutCaption(testStyle(), kW, kH, measurer, huge, LayoutMode::fitToBlock);
    CHECK(same == legacy);
    CHECK((legacy.lines.size() > 1 || legacy.ellipsized || legacy.shrunk));
    CHECK_FALSE(legacy.headTrimmed);
}
