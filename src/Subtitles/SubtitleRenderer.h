#pragma once
//
// SubtitleRenderer - lays out one caption and paints it into a straight-alpha
// BGRA frame buffer (the pixels the NDI video transport sends).
//
// Why GDI+ and not JUCE: the architecture audit allows JUCE only in App and
// Platform (Audio/Translation/NDI/Config/Subtitles stay portable, and the real
// NDI output must live next to the SDK-free INdiOutput contract). The other
// candidate - a hand-rolled rasterizer - cannot promise "качественная отрисовка
// текста" honestly. GDI+ is the third: a Windows system component (no new
// dependency in the project's sense, no redistribution question), whose
// documented 32bpp ARGB format is straight, non-premultiplied alpha with bytes
// B,G,R,A - the exact pixel convention the NDI frame-type documentation
// specifies for fourcc_BGRA ("This data is not pre-multiplied",
// docs.ndi.video, recorded in docs/ndi-protocol.md). The rendered buffer
// therefore becomes the video frame unchanged; the test suite verifies the
// convention on real pixels instead of trusting the prose.
//
// Thread model: all GDI+ entry points in this file are serialized by one
// process-wide mutex. GDI+ documents thread safety for per-thread objects, but
// here the video render thread, the settings preview and unit tests do create
// distinct objects - and the cost of serializing a few-millisecond caption
// paint is nothing against the cost of being wrong about a raster library.
//
// The render is bounded and allocation-typed but never realtime: it runs on
// the NDI render thread (video output) or the GUI thread (preview), never in
// the audio callback (see the project rules; the NDI/INdiOutput boundary
// comment owns that guarantee).
//
// Baseline and words. A caption is a STREAM: partial snapshots replace the
// in-progress line, words join at its tail and the single-line layout lets
// the oldest leave its head. Two promises keep that readable:
//   * the baseline never moves - a line is placed by the FONT's metrics
//     (cell ascent/line spacing), never by the current glyphs' tight ink
//     bounds, so descenders arriving or leaving cannot pull the row up or
//     down;
//   * a new word fades in (opt-in, setWordFadeEnabled) instead of punching:
//     words keep a first-seen timestamp keyed to the canonical caption row,
//     and only words that were not on the previous row start their ramp.
//     Older words keep theirs - no animation restarts while the line grows
//     or the head is trimmed. The preview and every caller that renders one
//     static frame keep the fade OFF: a paint that is never followed up
//     would freeze mid-ramp, so animation is the transport's choice, not
//     the renderer's default.
//
// Non-Windows builds: everything compiles and fails honestly ("Windows-only"),
// same as the rest of the platform-specific corners of this product.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Subtitles/SubtitleLayout.h"
#include "Subtitles/SubtitleStyle.h"

namespace liveai {
namespace subtitles {

/// A point on the monotonic clock the fade ramp measures against. Callers on
/// the render thread get the real one; tests inject a scripted timeline.
using SteadyTime = std::chrono::steady_clock::time_point;
/// One rendered caption frame. `bgra` is w*4*height bytes, row stride w*4,
/// straight (non-premultiplied) alpha: transparent background = B,G,R,A all 0;
/// opaque text = A 255 with the text color.
struct RenderedSubtitleFrame
{
    std::vector<std::uint8_t> bgra;
    int width = 0;
    int height = 0;
    std::string familyUsed;      ///< the font family that was actually opened (fallback truth)
    LaidOutCaption layout;       ///< what produced the pixels (diagnostics/tests read it)
};

class SubtitleRenderer
{
public:
    /// How long a word takes to reach full opacity once it first appears.
    /// Fast on purpose: a caption is read while it is listened to, and the
    /// ramp must decorate the arrival, never delay the word.
    static constexpr int kWordFadeInMs = 240;

    /// Paints `captionText` with `settings.style` into `out` at
    /// `settings.video.width x settings.video.height`. An empty caption
    /// produces a fully transparent frame (return true) - the old text must
    /// not stay burned on the receiver when the sentence has ended.
    /// `atTime` overrides the monotonic clock (tests script the fade; the
    /// transport lets the default stand). Returns false only when nothing
    /// was painted: the caller keeps its last
    /// frame (or the transparent one), counts the error and keeps audio alive;
    /// the message is an operator-actionable sentence.
    bool render(std::string_view captionText, RenderedSubtitleFrame& out, std::string& error,
                std::optional<SteadyTime> atTime = {});

    /// Updates the settings for the NEXT render (no lock inside this object:
    /// the owner confining it to one thread owns the handover - the video
    /// output swaps settings under its own state mutex, the preview is GUI
    /// thread only).
    void applySettings(const SubtitleSettings& settings) { settings_ = settings; }

    /// The fade-in animation for newly arriving words, off by default (see
    /// the header: a one-shot painter must never freeze mid-ramp). Turning
    /// it off clears the word timeline; a caption re-animated from there
    /// fades as if it had just arrived. Same thread confinement as
    /// applySettings.
    void setWordFadeEnabled(bool enabled);

    /// True while the newest word's ramp has not reached full opacity: the
    /// owner must render the SAME caption again at the next tick (that is
    /// how the animation advances frames). Read on the confining thread
    /// right after render(), like every other member of this object.
    bool animationPending() const noexcept { return animationPending_; }

    /// GDI+ can start on this machine at all. Cached after the first answer.
    static bool available();

private:
    /// Forget the word timeline (caption ended, fade switched off).
    void resetFade();

    SubtitleSettings settings_;

    // ------------------------------------------------------------ fade state
    // Words of the canonical caption row as it read on the PREVIOUS render,
    // with the moment each one first arrived. Keyed by position from the
    // row's head, re-verified word by word against the current row: a shared
    // prefix keeps its timestamps (no restart), anything past the first
    // difference is new (fades). The head trim removes words the audience
    // already read, never resets the rest.
    bool wordFadeEnabled_ = false;
    bool animationPending_ = false;
    std::string fadeLine_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> fadeWords_;   ///< offset, length in fadeLine_
    std::vector<SteadyTime> fadeSeen_;                                  ///< parallel to fadeWords_
};

} // namespace subtitles
} // namespace liveai
