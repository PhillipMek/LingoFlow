# Agent State

Project: LingoFlow v2.1
Current task: ndi-subtitle-single-line
Status: DONE (unit-tested, full suite green) - venue HUMAN CHECKPOINT pending (below)

## This task (2026-10-17)

NDI burned-in subtitle video is laid out in the new `LayoutMode::singleLineTrimHead`
mode: one row, the configured font size (never shrunk under text length), no wrapping,
and on overflow the OLDEST characters are trimmed from the head so the longest fitting
suffix stays on screen.

- Cause: `SubtitleLayout::layoutCaption` had one rule set - greedy wrap + bounded font
  shrink + tail ellipsis (its rules 2-4). For NDI video that produced the observed
  font-size changes, multi-line growth and block-height jumps.
- Change: `layoutCaption` takes an explicit `LayoutMode` (default `fitToBlock` = the
  delivered rules, unchanged); the NDI rasterizer (`SubtitleRenderer::render`) is the
  single product caller and now passes `singleLineTrimHead`. Trim = binary search over
  displayed-cluster starts, decided by the same injected LineMeasurer (GDI+ ink widths)
  the paint uses. Cluster starts: never mid-UTF-8 (structural), never before a
  combining mark/joiner/selector table, ZWJ glues the next code point, regional
  indicators pair into flags - a documented approximation of UAX #29 (no Hangul jamo,
  no Indic composition; recorded in the source).
- `LaidOutCaption.headTrimmed` is the new diagnostics flag; `shrunk`/`ellipsized`
  never act in single-line mode. Geometry (line height, anchors, margins, alignment,
  colors, outline, box) is untouched shared code.
- Not changed: TextPipeline accumulation, NdiDispatch, TTML metadata mode
  (`NdiTimedText`), the operator screen (its own JUCE label path), config schema.
- UI caption updated ("Max lines (NDI video shows one line)"); docs/ndi-obs.md
  describes the single-line + head-trim rule.
- Verification: `cmake --build build --config Debug` clean (MSVC /W4, no new warnings);
  `lingoflow_tests.exe` 437 cases / ~10.1M assertions PASS, `[subtitles]` tag 38 cases
  PASS (14 new layout cases, 6 new renderer cases); full `ctest -C Debug` 453/453 PASS
  including architecture-boundary and realtime-safety gates.

## HUMAN CHECKPOINT (required, not self-certifiable)

- Real OBS/NDI receiver: one-line stability, head-trim readability and alpha under a
  live caption stream at 1920x1080 and 720p (the product tests prove the sender; the
  venue proves the display).
- Long-run: caption-rate redraw performance on the venue kit (trim is O(log n)
  measurements by design; the claim must be watched, not asserted).

## History

The repository was cleaned for production use (commit 723949c removed the previous
AGENT_STATE.md and the numbered task files); per-task evidence lives in the git log
and docs/.
