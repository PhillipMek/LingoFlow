#include "Subtitles/SubtitleLayout.h"

#include <algorithm>
#include <cmath>

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


/// Returns the longest suffix that fits at the supplied font size. The search
/// works on UTF-8 code-point boundaries, so clipping never corrupts a character.
/// Binary search avoids repeatedly measuring every possible suffix of a long
/// accumulated live caption.
std::string fitTail(std::string_view text, float limitPx, float fontPx,
                    const LineMeasurer& measure)
{
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    if (text.empty())
        return {};

    const auto offsets = codepointOffsets(text);
    std::size_t low = 0;
    std::size_t high = offsets.size() - 1;
    while (low < high)
    {
        const std::size_t mid = low + (high - low) / 2;
        const std::string_view suffix = text.substr(offsets[mid]);
        if (measure(suffix, fontPx) <= limitPx)
            high = mid;
        else
            low = mid + 1;
    }

    // In a normal video frame at least one glyph fits. If a pathological
    // geometry is narrower than even the last glyph, retain that whole glyph;
    // the renderer clips its pixels at the frame boundary.
    if (low >= offsets.size() - 1)
        low = offsets.size() - 2;
    return std::string(text.substr(offsets[low]));
}

} // namespace

LaidOutCaption layoutCaption(const SubtitleStyle& style,
                             int frameWidth,
                             int frameHeight,
                             const LineMeasurer& measure,
                             std::string_view text,
                             CaptionLayoutMode mode)
{
    LaidOutCaption out;
    if (frameWidth <= 0 || frameHeight <= 0)
        return out;   // no frame: nothing to place, the caller renders nothing

    const float scale = static_cast<float>(frameHeight) / kReferenceFrameHeight;
    const float baseFontPx = static_cast<float>(style.fontSizePx) * scale;
    const float limitPx = static_cast<float>(frameWidth) * static_cast<float>(style.blockWidthPercent) / 100.0f;
    const int maxLines = (std::max)(1, style.maxLines);

    float fontPx = baseFontPx;
    std::vector<std::string> lines;

    if (mode == CaptionLayoutMode::singleLineTail)
    {
        // Live NDI captions must remain typographically stable. Keep the
        // configured font size and one line; as text accumulates, discard its
        // oldest prefix rather than shrinking or introducing line breaks.
        const std::string visible = fitTail(text, limitPx, fontPx, measure);
        if (!visible.empty())
            lines.push_back(visible);
        out.clipped = visible.size() < text.size();
    }
    else
    {
        // Adaptive layout: wrap at the configured size, shrinking (bounded)
        // until the caption fits the configured line count.
        lines = wrap(text, limitPx, fontPx, measure);
        while (lines.size() > static_cast<std::size_t>(maxLines) && fontPx > baseFontPx * kMinFontScale)
        {
            fontPx *= kShrinkStep;
            lines = wrap(text, limitPx, fontPx, measure);
            out.shrunk = true;
        }
    }

    // Adaptive layout only: at the floor font, ellipsize the tail so the block
    // cannot overflow. The live single-line mode clips the oldest prefix.
    if (mode == CaptionLayoutMode::adaptive
        && lines.size() > static_cast<std::size_t>(maxLines))
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
