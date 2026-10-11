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
// Non-Windows builds: everything compiles and fails honestly ("Windows-only"),
// same as the rest of the platform-specific corners of this product.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Subtitles/SubtitleLayout.h"
#include "Subtitles/SubtitleStyle.h"

namespace liveai {
namespace subtitles {

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
    /// Paints `captionText` with `settings.style` into `out` at
    /// `settings.video.width x settings.video.height`. An empty caption
    /// produces a fully transparent frame (return true) - the old text must
    /// not stay burned on the receiver when the sentence has ended.
    /// Returns false only when nothing was painted: the caller keeps its last
    /// frame (or the transparent one), counts the error and keeps audio alive;
    /// the message is an operator-actionable sentence.
    bool render(std::string_view captionText, RenderedSubtitleFrame& out, std::string& error);

    /// Updates the settings for the NEXT render (no lock inside this object:
    /// the owner confining it to one thread owns the handover - the video
    /// output swaps settings under its own state mutex, the preview is GUI
    /// thread only).
    void applySettings(const SubtitleSettings& settings) { settings_ = settings; }

    /// GDI+ can start on this machine at all. Cached after the first answer.
    static bool available();

private:
    SubtitleSettings settings_;
};

} // namespace subtitles
} // namespace liveai
