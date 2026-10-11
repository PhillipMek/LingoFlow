# Subtitles in OBS via NDI (the video output)

Dated 2026-10-11. This guide is written from the SDK facts recorded in
`docs/ndi-protocol.md` and the product behavior pinned by the test suite. It is
NOT a claim that OBS has been tested here: the developer machine has the NDI
runtime but no OBS session, and the project rules reserve "the audience saw it"
for a human checkpoint at the venue. Everything below that concerns OBS's own
compositing is marked as such.

## What LingoFlow sends

One NDI source ("LingoFlow" by default, or your stream name) carrying:

* **video** (the default `video` mode): 1920x1080 BGRA frames at 25 fps,
  caption text burned in, the rest of every frame fully transparent
  (alpha = 0). New and changed captions appear without restarting the source;
  an ended line clears the picture to full transparency.
* **metadata** (`metadata` mode): TTML1 timed-text frames - the original
  transport, kept as an additional mode for receivers that parse captions.
* **both**: video frames AND the TTML metadata on the same source (one sender
  always, by design - two senders with one name collide on the network).

Appearance (font, size, colours, outline, shadow, box, alignment, margins,
max lines, line spacing), resolution and frame rate are edited live under
Settings → Subtitles / NDI; the checkerboard preview shows the transparency
before anything goes on air. Appearance/geometry changes apply to the running
source; only the stream name and the enabled switch need Stop + Start.

## Enabling it in LingoFlow

1. Settings → **Subtitles / NDI**: tick **Publish NDI output**, pick
   **Video - burned-in subtitles (OBS-ready)** (or `Video + metadata`).
2. Choose resolution and fps the OBS machine can afford (1920x1080 @ 25 is the
   default; 1280x720 costs about a quarter of the bandwidth).
3. Press **Apply settings**. The NDI state line must read `ready` or
   `publishing`; `disabled`/`faulted` means subtitles are NOT going out - the
   note beside it says why (the usual cause is a missing NDI runtime, see
   Prerequisites; audio translation keeps running either way).
4. (Optional, machine check) run the NDI probe in video mode - it publishes
   burned-in captions from the same renderer and sender the app uses
   (`lingoflow_ndi_probe video "Test Subtitles" 20`, built with the app;
   on the venue kit it lives in `tools\`): frames counted and
   `N connection(s)` prove the wire; they never claim a display.

## Adding it in OBS

Requirements: OBS Studio with an NDI capture plugin - the community-maintained
**ndi-obs** plugin (previously DistroAV), installed via the OBS-NDI installer.
OBS itself does not speak NDI without such a plugin (an OBS-side fact, stated
from the plugin's existence, not from testing here).

1. In OBS, add a source of type **NDI Source** (sometimes listed as
   "NDI Dual-Channel Capture / Video Capture") to the scene you want subtitles
   in.
2. Select the source group `All`, and the NDI source name you configured
   (e.g. `LingoFlow`).
3. Place and scale it ABOVE your camera/screen layers - the subtitle picture is
   only the text: everything else is transparent, so the layers below show
   through, and the text sits wherever you position the source.
4. If the plugin's color-space options include an alpha / "use stream alpha"
   switch, leave alpha ENABLED. The frames carry straight (non-premultiplied)
   alpha, which is NDI's documented BGRA convention (docs/ndi-protocol.md);
   nothing in the LingoFlow chain premultiplies them.

### Checking transparency worked (in this order)

* over a mid-gray background: white text, black outline, no black box around
  the frame → alpha is being honoured;
* if the whole source paints an OPAQUE BLACK rectangle over your scene, alpha
  is being dropped somewhere in the receiver chain - the frame itself carries
  it (the test suite proves the wire alpha; the receiver's choice to use it is
  the venue checkpoint);
* captions you never see at all, while the LingoFlow diagnostics show video
  frames climbing: the connection is fine, the display is not - check the
  layer order and the plugin first.

## Prerequisites and limits (read before promising anything)

* **NDI runtime** on the LingoFlow machine (NDI 6 Runtime / full SDK; the app
  loads `Processing.NDI.Lib.x64.dll` dynamically and links nothing - the
  product starts and translates audio normally with the runtime absent, only
  the subtitle output says "unavailable").
* **GDI+** (gdiplus.dll, every Windows 10/11 install) does the caption raster;
  the font falls back to `Segoe UI` then `Arial` if the configured family is
  missing - the preview's amber note tells you when that happens.
* The wire is full-range BGRA/straight alpha; **NDI's transmission of the
  alpha channel is format-declared, and this repo's tests prove the SENDER
  side** (frames leave as declared, a loopback receiver reads alpha back).
  Whether a particular OBS version + plugin + capture/output color format
  combination PRESERVES that alpha end-to-end is not claimed here: community
  threads document premultiplied/straight confusion in both directions
  (e.g. DistroAV issue #899). That end-to-end judgement is the venue check
  this product deliberately never self-certifies.
* Captions are burned into pixels: a video-mode receiver needs no caption
  parser at all; metadata mode is for receivers that do (and for `both` they
  coexist on one source).
* Frame rate is whole-number (24/25/30/50/60). 29.97-style fractional rates
  are not configured by this product (no invented N/D pairs - see
  docs/ndi-protocol.md on frame-rate fields).
