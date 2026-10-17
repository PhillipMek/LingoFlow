#include "Subtitles/SubtitleLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace liveai {
namespace subtitles {
namespace {

/// Byte length of the UTF-8 code point starting at `lead` (clamped to [1,4]).
/// A continuation byte in the lead position decodes as one byte and is carried
/// along as its own "code point" - the layout never needs to know that the
/// text was malformed, only that it must not split inside a sequence.
int codepointLength(unsigned char lead) noexcept
{
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

/// The value of the UTF-8 code point at `text[i]` given its lead-byte length.
/// Truncated or malformed tails fold only the continuation bytes that are
/// actually there - the layout needs stable identities to classify, not a
/// validator; boundaries stay byte-aligned for any input.
std::uint32_t codepointValue(std::string_view text, std::size_t i, int len)
{
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    if (len == 1)
        return lead;
    std::uint32_t cp = lead & static_cast<unsigned char>(0xFFu >> (len + 1));
    for (int k = 1; k < len; ++k)
    {
        if (i + static_cast<std::size_t>(k) >= text.size())
            break;   // the sequence runs off the end: fold only what is there
        const unsigned char byte = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
        if ((byte & 0xC0) != 0x80)
            break;
        cp = (cp << 6) | (byte & 0x3Fu);
    }
    return cp;
}

/// The code-point boundaries of `text`: byte offsets at which a new character
/// starts, ending with text.size(). Used for character-level word splitting.
std::vector<std::size_t> codepointOffsets(std::string_view text)
{
    std::vector<std::size_t> offsets;
    offsets.reserve(text.size() + 1);
    std::size_t i = 0;
    while (i < text.size())
    {
        offsets.push_back(i);
        i += static_cast<std::size_t>(codepointLength(static_cast<unsigned char>(text[i])));
    }
    offsets.push_back(text.size());
    return offsets;
}

/// Splits text into space-separated words (runs without spaces). Consecutive
/// spaces collapse: a caption is prose, not a table.
std::vector<std::string_view> words(std::string_view text)
{
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        while (pos < text.size() && text[pos] == ' ')
            ++pos;
        const std::size_t start = pos;
        while (pos < text.size() && text[pos] != ' ')
            ++pos;
        if (pos > start)
            out.emplace_back(text.substr(start, pos - start));
    }
    return out;
}

/// Greedy wrap into lines whose measured width never exceeds `limitPx`. A
/// single word too wide for a line on its own is split at code-point
/// boundaries, so no multibyte character is ever cut in half.
std::vector<std::string> wrap(const std::string_view text,
                              float limitPx,
                              float fontPx,
                              const LineMeasurer& measure)
{
    std::vector<std::string> lines;
    std::string current;

    auto flush = [&]
    {
        if (!current.empty())
        {
            lines.push_back(current);
            current.clear();
        }
    };

    for (std::string_view word : words(text))
    {
        const std::string withWord = current.empty()
                                         ? std::string(word)
                                         : current + ' ' + std::string(word);
        if (measure(withWord, fontPx) <= limitPx || current.empty())
        {
            // Fits, or the line is empty so a fresh one can only begin here
            // (a too-wide word alone still has to go somewhere).
            current = withWord;

            // If that single word is itself wider than the line, split it.
            if (measure(current, fontPx) > limitPx)
            {
                const auto offsets = codepointOffsets(current);
                std::size_t pieceStart = 0;
                for (std::size_t k = 1; k < offsets.size(); ++k)
                {
                    const std::string_view piece = std::string_view(current).substr(
                        pieceStart, offsets[k] - pieceStart);
                    // offsets[k-1] > pieceStart guarantees a non-empty cut: a
                    // single code point wider than the line cannot be split
                    // further and rides its piece (the clamp rules cover the
                    // degenerate frame that makes a whole character too wide).
                    if (measure(piece, fontPx) > limitPx && offsets[k - 1] > pieceStart)
                    {
                        lines.push_back(current.substr(
                            pieceStart, offsets[k - 1] - pieceStart));
                        pieceStart = offsets[k - 1];
                    }
                }
                current = current.substr(pieceStart);
            }
        }
        else
        {
            flush();
            current = std::string(word);
        }
    }
    flush();
    return lines;
}

/// ASCII whitespace as the single-line mode knows it. The wrapped mode's
/// rule is "split at the space" (its words() collapses spaces); here every
/// character that could break a line or open a gap - space, tab, the CR/LF
/// pair, vertical tab, form feed - is a separator, because the mode's whole
/// promise is that ONE row reaches the rasterizer and no break character
/// survives into it. Multibyte sequences are untouched: their bytes are all
/// >= 0x80 and can never collide with this test.
bool isLineBreakingSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/// The caption as exactly one row: runs of non-space joined by single spaces
/// (consecutive separators collapse, leading and trailing space disappears -
/// the same prose rule as words(), widened to all ASCII whitespace).
std::string joinToSingleLine(std::string_view text)
{
    std::string line;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        while (pos < text.size() && isLineBreakingSpace(text[pos]))
            ++pos;
        const std::size_t start = pos;
        while (pos < text.size() && !isLineBreakingSpace(text[pos]))
            ++pos;
        if (pos > start)
        {
            if (!line.empty())
                line += ' ';
            line.append(text.substr(start, pos - start));
        }
    }
    return line;
}

/// The code points that EXTEND the cluster before them instead of starting a
/// new one: combining marks (the General_Category Mn/Me blocks that carry
/// diacritics for Latin, Cyrillic, Greek, Hebrew, Arabic, Thai, Indic and the
/// symbol/phonetic supplements), zero-width joiners, variation selectors,
/// emoji skin-tone modifiers, keycap/tag/VS-supplement sequences. This is a
/// table, not a claim of completeness: the full UAX #29 Extend set, Indic
/// ligature composition and Hangul jamo rules are NOT implemented (the
/// project forbids inventing capability work, and a text-shaping library is
/// out of scope for this task). What the table guarantees is the property
/// the trim relies on: a cut never lands between a base character and the
/// marks written on top of it for every script an interpreter keyboard
/// realistically produces, and NEVER inside a multibyte code point - that
/// part is structural, the walk below only ever stops on code-point offsets.
bool extendsPreviousCluster(std::uint32_t cp) noexcept
{
    return (cp >= 0x0300 && cp <= 0x036F)       // combining diacritical marks
        || (cp >= 0x0483 && cp <= 0x0489)       // Cyrillic combining marks
        || (cp >= 0x0591 && cp <= 0x05BD)       // Hebrew points
        || cp == 0x05BF || (cp >= 0x05C1 && cp <= 0x05C2)
        || (cp >= 0x05C4 && cp <= 0x05C5) || cp == 0x05C7
        || (cp >= 0x0610 && cp <= 0x061A)       // Arabic signs
        || (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670
        || (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E4)
        || (cp >= 0x06E7 && cp <= 0x06E8) || (cp >= 0x06EA && cp <= 0x06ED)
        || (cp >= 0x0900 && cp <= 0x0903) || (cp >= 0x0936 && cp <= 0x093C)
        || (cp >= 0x093E && cp <= 0x094F) || (cp >= 0x0951 && cp <= 0x0957)
        || (cp >= 0x0962 && cp <= 0x0963)       // Devanagari and friends
        || (cp >= 0x0E31 && cp <= 0x0E31) || (cp >= 0x0E34 && cp <= 0x0E3A)
        || (cp >= 0x0EB1 && cp <= 0x0EB1) || (cp >= 0x0EB4 && cp <= 0x0EBC)
        || (cp >= 0x1AB0 && cp <= 0x1AFF)       // Latin extended - C
        || (cp >= 0x1DC0 && cp <= 0x1DFF)       // combining diacritical marks sup
        || cp == 0x200C || cp == 0x200D         // ZWNJ / ZWJ
        || (cp >= 0x20D0 && cp <= 0x20F0)       // combining marks for symbols
        || (cp >= 0x2CEF && cp <= 0x2CF1)       // Coptic
        || (cp >= 0xA66F && cp <= 0xA67F)       // Cyrillic extended combining
        || (cp >= 0xA69C && cp <= 0xA69D) || (cp >= 0xA69E && cp <= 0xA69F)
        || cp == 0xFB1E
        || (cp >= 0xFE00 && cp <= 0xFE0F)       // variation selectors
        || (cp >= 0xFE20 && cp <= 0xFE2F)       // combining half marks
        || (cp >= 0x1F3FB && cp <= 0x1F3FF)     // emoji skin-tone modifiers
        || (cp >= 0xE0020 && cp <= 0xE007F)     // tag characters (emoji tags)
        || (cp >= 0xE0100 && cp <= 0xE01EF);    // variation selectors sup
}

/// Byte offsets at which a new displayed cluster MAY start. A head-trim cut
/// at any of these boundaries is structurally safe (never half a code point)
/// and practically safe (the base+marks, joiner and flag-pair rules above).
/// The first offset is always 0; the list never contains text.size() - the
/// callers trim toward "at least one cluster remains".
std::vector<std::size_t> clusterStartOffsets(std::string_view text)
{
    std::vector<std::size_t> starts;
    std::size_t riRun = 0;      ///< consecutive regional indicators before the current one
    bool previousWasJoiner = false;
    std::size_t i = 0;
    while (i < text.size())
    {
        const int len = codepointLength(static_cast<unsigned char>(text[i]));
        const std::uint32_t cp = codepointValue(text, i, len);
        const bool regionalIndicator = cp >= 0x1F1E6 && cp <= 0x1F1FF;

        // Regional indicators pair into flags (UAX #29 GB12/GB13): the odd-
        // numbered member of a run rides with its pair; a cut only lands at
        // even boundaries, so "RU" never shows as half a flag.
        const bool glue = !starts.empty()
            && (extendsPreviousCluster(cp) || previousWasJoiner
                || (regionalIndicator && riRun % 2 == 1));
        if (!glue)
            starts.push_back(i);

        previousWasJoiner = (cp == 0x200D);   // ZWJ glues the NEXT code point on
        riRun = regionalIndicator ? riRun + 1 : 0;
        i += static_cast<std::size_t>(len);
    }
    return starts;
}

/// The single-line mode's overflow rule: the longest suffix of `line` whose
/// MEASURED width fits `limitPx`, cut at a cluster start. `line` must be the
/// product of joinToSingleLine and must already have failed the measure at
/// full width. A longer suffix is never narrower than a shorter one (glyph
/// runs grow when extended at the head), so the first fitting start is found
/// by binary search - O(log n) measurements, which is what keeps a caption
/// streaming in at a few words per second cheap on the render thread.
/// Returns the start index into `starts`; when even the LAST cluster is too
/// wide (a degenerate frame), it returns the last cluster's index: the
/// newest text rides, the block clamp flags the frame, and a character is
/// never split - the same honest resolution the wrapped mode gives.
std::size_t longestFittingSuffixStart(std::string_view line,
                                      const std::vector<std::size_t>& starts,
                                      float limitPx,
                                      float fontPx,
                                      const LineMeasurer& measure)
{
    std::size_t lo = 0, hi = starts.size() - 1;
    while (lo < hi)
    {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (measure(line.substr(starts[mid]), fontPx) <= limitPx)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;   // may name the last cluster even when it does not fit
}

} // namespace

LaidOutCaption layoutCaption(const SubtitleStyle& style,
                             int frameWidth,
                             int frameHeight,
                             const LineMeasurer& measure,
                             std::string_view text,
                             LayoutMode mode)
{
    LaidOutCaption out;
    if (frameWidth <= 0 || frameHeight <= 0)
        return out;   // no frame: nothing to place, the caller renders nothing

    const float scale = static_cast<float>(frameHeight) / kReferenceFrameHeight;
    const float baseFontPx = static_cast<float>(style.fontSizePx) * scale;
    const float limitPx = static_cast<float>(frameWidth) * static_cast<float>(style.blockWidthPercent) / 100.0f;

    float fontPx = baseFontPx;
    std::vector<std::string> lines;

    if (mode == LayoutMode::singleLineTrimHead)
    {
        // The NDI rule set: ONE row, at the configured font size, always.
        // The font never adapts to the text; when the row outgrows the block
        // the HEAD is trimmed - the newest words on the right are the ones
        // the audience cannot get back, the oldest have already been shown.
        const std::string line = joinToSingleLine(text);
        if (!line.empty())
        {
            if (measure(line, fontPx) <= limitPx)
            {
                lines.push_back(line);   // it fits whole: nothing is cut
            }
            else
            {
                const std::vector<std::size_t> starts = clusterStartOffsets(line);
                const std::size_t k = longestFittingSuffixStart(line, starts, limitPx, fontPx, measure);
                std::string suffix = line.substr(starts[k]);
                out.headTrimmed = suffix.size() < line.size();
                lines.push_back(std::move(suffix));
            }
        }
        // An empty caption, or one made only of whitespace, leaves the zero-
        // line result below: the transparent frame, as in the wrapped mode.
    }
    else
    {
        const int maxLines = (std::max)(1, style.maxLines);

        // Rules 1-3: wrap at the configured size, shrinking (bounded) until the
        // caption fits the line count.
        lines = wrap(text, limitPx, fontPx, measure);
        while (lines.size() > static_cast<std::size_t>(maxLines) && fontPx > baseFontPx * kMinFontScale)
        {
            fontPx *= kShrinkStep;
            lines = wrap(text, limitPx, fontPx, measure);
            out.shrunk = true;
        }

        // Rule 4: still over the limit at the floor font - ellipsize the tail so
        // the block cannot overflow, measuring the cut so "…" itself fits.
        if (lines.size() > static_cast<std::size_t>(maxLines))
        {
            out.ellipsized = true;
            std::vector<std::string> kept(lines.begin(), lines.begin() + maxLines - 1);
            std::string last;
            for (std::size_t i = static_cast<std::size_t>(maxLines - 1); i < lines.size(); ++i)
                last += (last.empty() ? "" : " ") + lines[i];

            const std::string ellipsis = "\xE2\x80\xA6"; // UTF-8 horizontal ellipsis, one code point
            while (!last.empty() && measure(last + ellipsis, fontPx) > limitPx)
            {
                const auto offsets = codepointOffsets(last);
                last = last.substr(0, offsets[offsets.size() - 2]);   // drop the final code point
            }
            if (last.empty())
                last = ellipsis;   // a frame this small shows the ellipsis alone; never an empty "clipped" caption
            else
                last += ellipsis;

            lines = std::move(kept);
            lines.push_back(std::move(last));
        }
    }

    // The caption is empty (or became empty): zero lines, transparent frame.
    if (lines.empty())
        return out;

    out.lines = lines;
    out.fontPx = fontPx;
    out.outlineWidthPx = static_cast<float>(style.outlineWidthPx) * scale;
    out.shadowOffsetPx = static_cast<float>(style.shadowOffsetPx) * scale;
    out.backgroundPaddingPx = static_cast<float>(style.backgroundPaddingPx) * scale;

    // Line geometry.
    out.lineHeightPx = fontPx * (100.0f + static_cast<float>(style.lineSpacingPercent)) / 100.0f;
    out.blockHeight = static_cast<float>(lines.size()) * out.lineHeightPx;
    out.lineWidths.reserve(lines.size());
    out.maxLineWidth = 0.0f;
    for (const std::string& line : lines)
    {
        const float w = measure(line, fontPx);
        out.lineWidths.push_back(w);
        out.maxLineWidth = (std::max)(out.maxLineWidth, w);
    }
    out.blockWidth = out.maxLineWidth;

    // Safe area + anchors (rule 5).
    const float safeLeft = static_cast<float>(frameWidth) * kSafeAreaPercent / 100.0f;
    const float safeRight = static_cast<float>(frameWidth) * (100.0f - kSafeAreaPercent) / 100.0f;
    const float safeTop = static_cast<float>(frameHeight) * kSafeAreaPercent / 100.0f;
    const float safeBottom = static_cast<float>(frameHeight) * (100.0f - kSafeAreaPercent) / 100.0f;
    const float margin = static_cast<float>(frameHeight) * static_cast<float>(style.edgeMarginPercent) / 100.0f;

    float blockX = 0.0f;
    switch (style.horizontalAlign)
    {
        case HorizontalAlign::left:   blockX = safeLeft; break;
        case HorizontalAlign::right:  blockX = safeRight - out.blockWidth; break;
        case HorizontalAlign::center: blockX = (safeLeft + safeRight - out.blockWidth) / 2.0f; break;
    }
    if (blockX < safeLeft) { blockX = safeLeft; out.clipped = true; }
    if (blockX + out.blockWidth > safeRight) { blockX = (std::max)(safeLeft, safeRight - out.blockWidth); out.clipped = true; }

    float blockY = 0.0f;
    if (style.verticalAnchor == VerticalAnchor::bottom)
    {
        blockY = safeBottom - margin - out.blockHeight;
        if (blockY < safeTop) { blockY = safeTop; out.clipped = true; }
    }
    else // top
    {
        blockY = safeTop + margin;
        if (blockY + out.blockHeight > safeBottom) { blockY = (std::max)(safeTop, safeBottom - out.blockHeight); out.clipped = true; }
    }

    out.blockX = blockX;
    out.blockY = blockY;

    if (style.backgroundEnabled)
    {
        out.boxX = (std::max)(0.0f, blockX - out.backgroundPaddingPx);
        out.boxY = (std::max)(0.0f, blockY - out.backgroundPaddingPx);
        out.boxWidth = (std::min)(static_cast<float>(frameWidth), blockX + out.blockWidth + out.backgroundPaddingPx) - out.boxX;
        out.boxHeight = (std::min)(static_cast<float>(frameHeight), blockY + out.blockHeight + out.backgroundPaddingPx) - out.boxY;
    }

    return out;
}

} // namespace subtitles
} // namespace liveai
