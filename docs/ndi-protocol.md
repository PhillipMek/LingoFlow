# NDI Integration Facts (normative source for the NDI implementation)

Dated: 2026-10-04. Anything here that comes from headers was read from the SDK
installed on the development machine; anything from the official documentation
is given with its URL and access date. API spellings were verified by
compilation against these headers - nothing in this file is from memory.

## What we implement and why

SPEC 35-39: NDI subtitle output for production receivers (OBS, vMix, graphics
systems), as a separate module behind `INdiOutput`, never in the audio path.
SPEC 38 defines two modes:

* **Mode A - timed-text metadata** (this task): the caption text travels as NDI
  metadata frames.
* **Mode B - rendered subtitle video** (future, per SPEC: "can be implemented
  after the metadata mode"): our app renders text into a video source. Not
  built; the project rules (build the current task, not the future one).

SPEC 38's own warning is carried into our docs and UI: NDI metadata transmission
does not guarantee that every NDI receiver renders captions - which is exactly
why this task ends in a REQUIRED real-receiver checkpoint, not a claim.

## SDK facts (read from C:\Program Files\NDI\NDI 6 SDK\Include, SDK 6.3.2.0, 2026-10-04)

* Sending is create/send/destroy on an opaque instance:
  `NDIlib_send_create(const NDIlib_send_create_t*)` returns **NULL if it fails**;
  the struct is `{ const char* p_ndi_name; const char* p_groups; bool clock_video,
  clock_audio; }` (Send.h). We pass `p_groups = nullptr` (default) and the clocked
  defaults, send no media frames at all (metadata only).
* `void NDIlib_send_send_metadata(NDIlib_send_instance_t, const NDIlib_metadata_frame_t*)`
  - **returns void** (Send.h + the DynamicLoad table). There is no per-frame
  delivery truth for senders in this API; the product therefore makes no delivery
  claim anywhere (README/UI say "handed to the SDK", never "displayed").
* `NDIlib_metadata_frame_t` (structs.h): `{ int length; int64_t timecode; char* p_data; }`
  - length counts UTF-8 characters INCLUDING the terminating NUL, 0 means
  "assume NULL-terminated"; timecode is in 100-nanosecond intervals;
  `NDIlib_send_timecode_synthesize = INT64_MAX` asks the SDK to pick it
  ("closest audio/video frame timecode ... otherwise synthesized from the last
  one known" - documented behavior, exactly right for sporadic captions).
* Consumer awareness: `int (*send_get_no_connections)(instance, timeout_in_ms)`
  is in the dynamic table (verified in Processing.NDI.DynamicLoad.h) - polled
  with timeout 0 at publish time to distinguish `ready` from `publishing`.
* Metadata frames received on the wire side (`NDIlib_recv_capture_v3`) must be
  released with `recv_free_metadata` (Recv.h) - the probe does.
* Discovery: `NDIlib_find_create_v2({p_groups})`, `find_wait_for_sources(instance,
  timeout_ms)`, `find_get_current_sources(instance, uint32_t* p_no_sources)`
  (TWO arguments - the three-argument form does not exist; learned from the
  compiler, fixed against Find.h). `send_get_source_name(instance)` can expose
  the sender's own `{name, URL}` - observed on this machine to return an empty
  URL for a metadata-only sender; the probe treats URL-first as an optimization,
  discovery as fallback, and reports the fact.
* Receiving bandwidth: `NDIlib_recv_bandwidth_metadata_only = -10` (structs.h) -
  what the probe's receiver uses: captions, no video.

## Dynamic loading (the license rule made concrete)

docs/licensing.md (owner decision, 2026-10-01): `Processing.NDI.Lib.x64.lib` is
NEVER linked into the product. The SDK ships exactly the sanctioned alternative,
and we use it verbatim (pattern from
`Examples\C++\NDIlib_DynamicLoad\NDIlib_DynamicLoad.cpp`, read 2026-10-04):

1. `getenv(NDILIB_REDIST_FOLDER)` (= `NDI_RUNTIME_DIR_V6`; on this machine
   `C:\Program Files\NDI\NDI 6 Runtime\v6`), append `\` + `NDILIB_LIBRARY_NAME`
   (= `Processing.NDI.Lib.x64.dll`), `LoadLibraryA`;
2. `GetProcAddress(dll, "NDIlib_v6_load")` -> the `NDIlib_v6` function table
   (in the 6.3 headers all version aliases - v2..v6 - are the same `NDIlib_v6_3`
   struct; field names used by this product: `initialize`, `version`,
   `send_create`, `send_destroy`, `send_send_metadata`,
   `send_get_no_connections`, `send_get_source_name`, `find_create_v2`,
   `find_wait_for_sources`, `find_get_current_sources`, `find_destroy`,
   `recv_create_v3`, `recv_capture_v3`, `recv_free_metadata`, `recv_destroy`);
3. fallback `LoadLibraryA(NDILIB_LIBRARY_NAME)` through the normal search path;
4. if none of that works: `start()` answers false with an actionable sentence and
   the show continues without subtitles.

Two table-level facts that shaped the code (both verified against
`Processing.NDI.DynamicLoad.h`, not assumed): `initialize` is marked deprecated
but the SDK's own example calls it and a false return means "cannot run NDI", so
we call it and honour a refusal; there is **no cleanup entry** in the table at
all (grep found none), so unloading the DLL ourselves at exit is not an offered
operation - the module is kept loaded for the process lifetime on purpose, which
is also what the SDK example effectively does.

## Timed-text format (official documentation, docs.ndi.video, accessed 2026-10-04)

Asked of the docs directly ("What is the exact XML format for NDI subtitle
metadata frames...?"), the answer was explicit and is recorded honestly:

* Metadata frame XML must be well-formed, contain ONE root element, and OMIT the
  XML prolog.
* For captions/timed text, NDI documents a set of standards the metadata can use:
  **TTML1, SDP-US, IMSC1, SMPTE-TT, EBU-TT, CFF-TT**. NDI does not prescribe one
  and does not prefer one; receivers should process well-formed XML and ignore
  elements/attributes they do not implement.
* The legacy `<TD><P>` subtitle structure sometimes remembered from the TriCaster
  era is **not in the current official documentation** (the docs' own query
  interface said so on 2026-10-04). We therefore do not send it as if it were an
  NDI contract.

Decision: we build **TTML1** documents (W3C namespace
`http://www.w3.org/ns/ttml`) - the most documented, standards-traceable member
of the set - in the minimal shape

```xml
<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p xml:id="seq-N">TEXT</p></div></body></tt>
```

one complete snapshot document per `INdiOutput::publish` (SPEC 38 "a receiver may
replace" + the snapshot semantics: last document wins). `xml:id` carries our
own pipeline sequence - a standard attribute used for identity, not an invented
protocol field. Text content is XML-escaped (& < >) and XML-1.0-forbidden
characters become spaces (a document a receiver cannot parse is the failure mode
worth preventing). SPEC 38's "exact format according to the target receiver"
plus SPEC 69's "if the receiver does not support timed text, provide an
alternative rendering mode [Mode B] rather than assuming display" make the venue
receiver's answer the next evidence step, recorded where it belongs - in the
human checkpoint below, not in the code.

## Live facts measured on the development machine

* The runtime loads and reports: `NDI SDK WIN64 16:38:09 Apr 14 2026 6.3.2.0`
  (probe stdout). Real `send_create` + `send_send_metadata` +
  `send_get_no_connections` + `send_destroy` execute cleanly against it (unit
  case `[ndi][real]`, passing in Debug and Release).
* Discovery on this laptop finds **zero** sources even while our own sender is
  up and `NDI Discovery Service.exe` is started: the machine runs AmneziaVPN
  (adapter "Up, 100 Gbps", 10.8.1.1) alongside Ethernet 192.168.1.81, and NDI's
  announcements do not traverse it here. `probe list` prints `0 NDI source(s)
  visible`; `probe recv` exits 3 (honest "not found"). This is a property of
  this network stack, not of the transport - and precisely why the task's
  HUMAN_CHECKPOINT ("real NDI receiver") stays open: on the venue network the
  same two probe commands (`send` / `recv`, or `selfcheck`) take ten seconds and
  prove the whole path.

## Where each fact lives in code

| Fact | Implementation |
|---|---|
| sender lifecycle, NULL on failure | `NDI/Real/NdiTimedTextOutput.cpp::start` |
| metadata frame shape, length incl. NUL, synthesized timecode | same file `publish` |
| TTML1 document + escaping | `NDI/NdiTimedText.cpp` (unit-tested shape) |
| consumer poll (ready vs publishing) | `publish` + `send_get_no_connections(timeout 0)` |
| dynamic load per license | `NDI/Real/NdiRuntime.cpp` |
| discovery + metadata-only receiver (evidence tool) | `NDI/Real/NdiProbeMain.cpp` |

## Open by design (the checkpoint)

The REQUIRED human checkpoint for subtitles, run sheet `docs/rig-checklist.md`
(step 11): on the venue network - `probe selfcheck` green, the operator's
receiver (the graphics/vision tool actually in the show) displays the live
captions, and unplugging that receiver leaves audio and translation untouched
with the chip dropping `publishing` -> `ready`. If the venue receiver wants a
different timed-text dialect or Mode B, that observation decides the follow-up -
not this file guessing ahead of it.
