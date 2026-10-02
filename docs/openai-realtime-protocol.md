# OpenAI Realtime Protocol — verified facts for the translation backend

Task 008 deliverable. This file replaces the placeholder: "Task 008 must replace
this file with information verified against current official OpenAI documentation."

**Every protocol assumption made by tasks 009-018 must come from this file.** Where
the documentation is silent, section 14 says so explicitly, and the code must not fill
the gap with guesses (AGENTS.md 8, 19).

- Verified: 2026-10-02, by fetching the live official documentation pages listed in
  section 15. No statement here is recalled from memory or from older protocol
  versions; the pre-GA beta protocol is marked invalid below.
- Re-verification: every page has a Markdown twin - append `.md` to the URL - and the
  index is `https://developers.openai.com/api/llms.txt`. If task 009 hits behavior
  that contradicts this file, re-fetch the pages before believing either side.

---

## 1. What integration we are building, and what we are not

OpenAI currently has three distinct realtime voice surfaces. Only the first applies
to LingoFlow:

| Surface | Endpoint family | Purpose | Ours? |
| --- | --- | --- | --- |
| **Realtime translation** | `/v1/realtime/translations` | Continuous speech-to-speech interpretation, no turns, no tools | **Yes** [R1][R2][R3] |
| Realtime (voice agents) | `/v1/realtime` | Assistant sessions: conversation state, `response.create`, tools | No [R5] |
| GPT-Live | `/v1/live/sessions` | Full-duplex conversation with a delegated backend | No [R5][R6] |

`gpt-realtime-translate` is a real, currently documented model released **May 7, 2026**
[R7]. This settles the SPEC v2.1 reference to "OpenAI GPT-Realtime-Translate": the model
exists, is the right product for live interpretation [R4][R8], and its identifier is
`gpt-realtime-translate` [R4].

Transport choice: LingoFlow is a desktop application whose ASIO engine already owns the
raw audio stream. The guide is explicit - "Use WebSockets when your server already
receives raw audio, such as ... broadcast ingest, or a media worker" [R1]. The WebRTC
path (`POST /v1/realtime/translations/calls`, SDP offer/answer) exists for browser
media capture [R1][R8]; **we do not use it**, and consequently we do not need
`client_secrets` (those exist to avoid exposing a key to browsers; our key stays in the
local process behind the credential store, task 015).

## 2. Endpoint and connection

- WebSocket URL, model selected in the query string:
  `wss://api.openai.com/v1/realtime/translations?model=gpt-realtime-translate` [R1][R8].
- `model` and the session `type` are fixed at creation. `type` is always `"translation"`
  in server events [R3].
- The first server event after connecting is `session.created` [R3]. The official Ruby
  example asserts this and treats any other first event as a failure [R1].
- The model is supported **only** on the `v1/realtime/translations` route; the model page
  lists every other endpoint (including plain `v1/realtime`) as "Not supported" [R4].
- IPv6 to `api.openai.com` is available since Sep 1, 2026 [R7]; no action needed.

## 3. Authentication

- Header on the WebSocket upgrade request: `Authorization: Bearer <API key>` [R1][R6][R8].
- `OpenAI-Safety-Identifier` header: optional, recommended by OpenAI, "doesn't require
  them"; value should be a stable privacy-preserving hash, not an identity [R5][R6].
  Note for task 021: any identifier we invent must not be re-identifiable operator data.
- Do **not** send `OpenAI-Beta: realtime=v1`. The Realtime API **Beta was deprecated and
  removed from the API on May 12, 2026** [R5][R7]. Any assumption based on the 2024-2025
  beta protocol (event names like `response.audio.delta`, `input_audio_buffer.append`,
  `conversation.item.*`) is invalid, and the translation endpoint never used them anyway.
- Ephemeral client secrets (`/v1/realtime/translations/client_secrets`, `ek_...`) are for
  browser/mobile clients; a trusted server (our app) uses the standard key [R1][R8].

## 4. Session lifecycle and the translation model

Translation sessions have **no turn lifecycle**: "Translation starts from the audio
stream itself. Keep appending audio, including silence between phrases, and handle
output events as they arrive" - and "You don't call `response.create`" [R1][R8]. There is
no assistant turn, no tool call and no conversation state [R8].

Mapping to the task 007 contract (our six lifecycle rules in `ITranslationBackend.h`):

| Contract rule | Protocol fact |
| --- | --- |
| 1. sink attached before open | The server emits `session.created` immediately on connect [R3]; callbacks can start without any client action. |
| 2. one session at a time | One WebSocket = one translation session [R1]. Nothing in the docs allows multiplexing sessions on a connection. |
| 3. transitions reported once | `session.created`/`session.updated`/`session.closed` are the state events [R3]. |
| 4. submit only while connected | Audio appends are fire-and-forget ("Audio appends have no acknowledgment" is documented for Live; for translation the client events reference shows no ack event for `session.input_audio_buffer.append` [R2][R6]) - a failed socket surfaces via transport error, not per-append. |
| 5. `closeSession` flushes, then no callbacks | Documented and load-bearing: send `session.close`; "the server flushes pending input audio and emits any remaining translated output before closing the session"; then `session.closed` arrives; "Closing the socket immediately can drop translated audio still draining" [R1][R2][R3]. Implementation must stop appending, keep reading until `session.closed`, and only then close the socket. |
| 6. reopen resets per-session state | There is no documented resume/continue for translation sessions (section 10). A reconnect is a brand-new WebSocket, therefore a brand-new `session.created` [R3]. |

The `session.close` client event is "only supported for translation sessions" [R1] -
another reason the voice-agent docs must not be copied into our backend.

## 5. Client events - the complete list

The reference page for translation client events contains exactly three [R2]. Anything
else is not protocol and must not be invented:

1. **`session.update`** — fields it may carry: `audio.output.language`,
   `audio.input.transcription` (`{model}` or `null`), `audio.input.noise_reduction`
   (`{type: "near_field" | "far_field"}` or `null`). `type` and `model` cannot be
   changed. Optional `event_id`. Successful update returns `session.updated` [R2][R3].
   **Mid-session language change is supported**: update `audio.output.language` on an
   open session (relevant to task 015's UI; no reconnect needed).
2. **`session.input_audio_buffer.append`** — `{audio: <base64 PCM16 mono 24 kHz
   little-endian>, event_id?}`. Section 7 defines the byte contract [R2].
3. **`session.close`** — `{event_id?}`; graceful flush-and-close (section 4) [R2].

## 6. Server events - the complete list

The reference page for translation server events contains exactly seven [R3]. Sink
mapping for tasks 009/012/013:

| Event | Payload (verified fields) | Goes to |
| --- | --- | --- |
| `error` | `error{message, type, code?, event_id?, param?}` | `onTranslationError` (section 9 mapping) |
| `session.created` | `session{id "sess_…", type "translation", model, expires_at, audio{…}}`, first event on connect | state → connected (after our `session.update` is confirmed by `session.updated` — decision, section 14.3) |
| `session.updated` | same `session` object, resolved config | state → configured / language-changed |
| `session.closed` | nothing else | state → closed; end of callbacks (rule 5) |
| `session.input_transcript.delta` | `delta` (source-language text), `elapsed_ms?` | source subtitles (013), only if `audio.input.transcription` configured [R2][R3] |
| `session.output_transcript.delta` | `delta` (translated text), `elapsed_ms?` | translated subtitles (013) |
| `session.output_audio.delta` | `delta` (base64), optional `sample_rate`, `channels`, `format: "pcm16"`, `elapsed_ms?` | `onTranslatedAudio` via controller → jitter buffer (012) |

No `*.done` events exist in the reference. Do not wait for one; do not invent one.

## 7. Audio formats — the byte contract

**Input (what we must send)** [R2][R8]:

- base64-encoded **raw PCM16, little-endian, mono, 24 kHz**, no WAV container;
- "Unsupported websocket audio formats return a validation error" - there is no
  negotiated format and no server-side conversion [R2];
- the engine consumes **200 ms frames**. Shorter chunks are buffered until a frame is
  complete; longer chunks are split into back-to-back 200 ms frames. Recommended send
  size: 200 ms chunks = 4800 samples = 9600 bytes = one frame of audio [R2];
- **keep appending continuously, including silence between phrases**. If the client
  stops and later resumes, "model time treats the resumed audio as contiguous with the
  previous audio rather than as a real-world pause" [R2]. A stall is not a pause: the
  translation drifts. Our streaming worker must therefore produce a steady 200 ms
  cadence (or the equivalent bytes/time) whenever the session is open - this is an
  explicit task 012 design constraint.

**Output (what we can expect back)**:

- `session.output_audio.delta` carries PCM16 whose "length can vary"; clients must decode
  and queue the whole delta, never assume a fixed size [R3];
- the cookbook states translated audio is emitted as **base64 24 kHz mono PCM16 in
  200 ms chunks** [R8]; the event's `format` field enum is `"pcm16"` [R3];
- `sample_rate` and `channels` are *optional* fields on the event [R3]. The backend must
  treat them as authoritative when present and validate against what it assumed; the
  value the real API sends must be observed at the human checkpoint (14.2).

**Sample-rate consequences for task 009** (contract mapping, section 13): the engine
runs at its device rate (48 kHz in dev defaults [R-doc]); the provider speaks 24 kHz in
both directions. A resampler is therefore mandatory **inside the backend**, on the
network/worker thread (2:1 both ways with 48k devices). It must never be in the audio
callback (AGENTS.md 5), and the existing no-resampling-in-the-controller rule (007) is
compatible: the controller compares delivered blocks against `outputSampleRate`, and
the backend's job is to deliver at that rate or refuse.

## 8. Text events

- Transcript deltas are **append-only fragments**; "Clients should not insert
  unconditional spaces between deltas" [R3]. Concatenate verbatim; the fragments
  already carry their own spacing (verified pattern in examples: `" hear"`,
  `" escuch"`).
- `elapsed_ms` (both transcript events and audio delta): alignment metadata derived from
  the translation frame, advances in 200 ms increments, **may repeat across events** -
  "Treat it as alignment metadata, not a unique transcript-delta identifier" [R3].
  Task 013 may use it to align subtitles with audio; it must not key state on it.
- Source transcripts exist only when `audio.input.transcription.model` is configured
  [R2][R3]; the documented example model is `gpt-realtime-whisper` (released same day
  as the translate model, "streaming speech-to-text") [R7][R8].
- The `session.created` example shows the resolved transcription object containing a
  `language` field, although the `session.update` schema only exposes `model` [R3].
  Server-resolved field: parse tolerantly, never send it based on the example.

## 9. Errors

**In-session `error` event** [R3]: `error.type` is a string, documented examples
`"invalid_request_error"`, `"server_error"`; `error.code`, `error.event_id` (the client
event that caused it) and `error.param` are optional. Official stance: "Most errors are
recoverable and the session will stay open"; logging them is explicitly recommended.
So an `error` event alone is **not** a session loss - the sink stays connected unless the
transport itself dies.

**Connection-level (HTTP before upgrade)** - the API error table [R9] plus the
Sep 2, 2026 changelog change [R7]:

| HTTP | Documented meaning | Our category (contract, section 13) |
| --- | --- | --- |
| 401 | invalid/revoked key, wrong org, IP not authorized | `rejectedRequest` (config error; reconnecting cannot help) |
| 403 | country/region not supported | `rejectedRequest` (with actionable detail) |
| 429 | `slow_down` (ramp-rate), plain rate limit, spend/usage limits, `credit_balance_exhausted` | `connection`, retry honoring `Retry-After` |
| 500 | server error while processing | `connection`, backoff retry |
| 503 | `server_is_overloaded` (model temporarily overloaded) | `connection`, backoff retry |

`Retry-After` header: "When the header is present, wait at least as long as it
specifies... If it's missing, use exponential backoff" [R7][R9]. Billing/quota 429s are
the documented exception: "Retrying billing, spend, or quota errors won't restore API
access" [R9] - treat as operator-actionable, retrying is pointless (task 010 policy).

**Mapping rule for task 009**: provider error names/types (`invalid_request_error`,
`server_error`, `slow_down`, `server_is_overloaded`, ...) are translated **only** into
the five product `TranslationErrorCategory` values plus `fatal`; nothing provider-named
crosses the sink seam (contract header, AGENTS.md 8). `audioFormat` on our side means
the 24 kHz PCM16 contract was violated locally; a provider validation error about our
audio maps to `audioFormat`, everything else the provider refuses the request maps to
`rejectedRequest`, transport death maps to `connection`, and anything we cannot classify
maps to `internal` - never dropped silently.

## 10. Reconnect and recovery semantics

What the documentation provides: production guidance to "Surface reconnecting, delayed,
and unavailable states" and "Track latency apart from translation quality" [R1]; the
`expires_at` field on every session object [R3]; the graceful-close handshake
(sections 4-5) [R1][R2].

What it does **not** provide (do not invent):

- no resume/attach protocol for translation sessions - reconnection means a new
  WebSocket, a new `session.created`, and a fresh `session.update`;
- no documented session duration value or expiry behavior beyond the presence of
  `expires_at` (seconds since epoch) [R3];
- no documented WebSocket close-code semantics for this endpoint;
- no "flush done" signal other than `session.closed` itself.

Consequences for task 010 (kept in product vocabulary): after a dropped transport the
backend reopens a session per contract rule 6 (state reset), replays the stored
`SessionRequest` (pair + rates + language), and the application decides what to do with
audio that was queued during the gap. Since provider time is contiguous over appended
audio (section 7), the honest options at 010 are: drop the gap buffer (translate drifts
but stays aligned with "now") - not yet chosen; the docs do not choose it for us.

## 11. Model identity and operating envelope

- Model ID: `gpt-realtime-translate`; default snapshot `gpt-realtime-translate`; the
  snapshots list contains exactly this one entry [R4]. Task 009's default value for the
  contract's opaque `model` string is therefore `gpt-realtime-translate`, and any other
  value is a `rejectedRequest`-style config error until docs say otherwise.
- Capabilities: input modalities audio; output audio + text; "streaming" is the only
  listed supported feature [R4].
- Billing: priced **by audio duration**, $0.034/minute [R4] — diagnostics/UI cost
  display (later tasks) should count minutes of session, not tokens.
- Rate limits are "minutes-of-audio per minute": Tier 1 = 50, Tier 2 = 200, Tier 3 = 400,
  Tier 4 = 650, Tier 5 = 850 [R4]. A single continuous session consumes 1 minute of
  audio per minute; the envelope matters for aggressive reconnect loops and for multiple
  simultaneous sessions (one per target language, section 12).
- No documented numeric latency guarantee for this model. The SPEC's 0.7-1.5 s end-to-end
  target stays a measured property of the rig (task 018, HUMAN CHECKPOINT), not a
  protocol promise.

## 12. Behavioral constraints that shape the product (verified)

1. **No custom prompting.** "This model does not currently support custom prompting or
   voice selection parameters" [R8]. ⚠️ **Conflict flag**: task 012's wording
   ("translation instructions") and `SessionRequest.instructions` cannot be honored by
   this endpoint. The 009 backend must not fake instruction support: accept the field,
   ignore with a warning (and count/report it as not applied). Owner decision needed at
   012; the field itself stays in the contract because other backends may support it.
2. **No glossaries or pronunciation guides**; the model "can sometimes substitute
   incorrect names or entities"; golden-set testing before launch is documented advice
   [R8]. Relevant to any "terminology" feature in SPEC.
3. **Source language is auto-detected**; the client only sets the target language [R8].
   Our `LanguagePair.input` is therefore a UI/config statement, not a wire field: the
   translation session request has no input-language parameter in `session.update`
   [R2]. Capability validation (011) must reflect that EN→X and RU→X both work by
   detection, not by declaration.
4. **Dynamic voice adaptation**: translated speech follows the source speaker's tone,
   pitch and style; in multi-speaker audio the output voice changes with the input [R8].
5. **Same-language speech may produce silence**: the model "tries not to translate
   speech that is already in the selected output language" [R8]. Documented production
   pattern: duck (not mute) the original audio and keep source captions available [R8].
   For our output path (012/016 NDI) this means the operator needs to understand why
   "nothing" comes back during an already-in-target-language segment; it is not an
   error and must not be counted as one.
6. **One session per target language** for multi-language audiences [R1]. Our MVP
   (one language per output channel) is the simple case of the documented architecture.
7. Input transcription and noise reduction are optional session features:
   `audio.input.transcription.model` (`gpt-realtime-whisper` in every example
   [R2][R8]) and `audio.input.noise_reduction.type` (`near_field` for close-talk mics
   like headsets, `far_field` for conference mics) [R2]. For a console feed via
   SoundGrid, neither default is documented; 009 must make both configurable or omit
   them (omit = don't send).

## 13. Capabilities manifest content for task 011

From [R8], the authoritative current language envelope:

- **Output (target) languages, 13**: Spanish, Portuguese, French, Japanese, **Russian**,
  Chinese, German, Korean, Hindi, Indonesian, Vietnamese, Italian, **English**.
  → The MVP requirement English ↔ Russian is satisfied in **both** directions
  (RU→EN and EN→RU are both *target* languages; as *sources* both are inside the 70+
  auto-detected input set).
- **Input languages: over 70, auto-detected**, explicitly enumerated in [R8]
  (Arabic … Yoruba; includes English and Russian).
- Source-transcript capability exists only via optional `gpt-realtime-whisper`
  transcription [R2][R8].

The docs name languages in English and show ISO-style codes (`"es"`, `"fr"`) in
examples [R1][R2]; the exact accepted code strings for all 13 targets are **not**
published as a code list. The manifest in 011 should carry codes for `"en"`/`"ru"` (used
pattern) and each code must be confirmed at the live checkpoint (14.4) before wider use.

## 14. Explicitly unverified — needs the live API (HUMAN CHECKPOINT)

This task verified documentation, not traffic; there is no `OPENAI_API_KEY` on this
machine. Before or during task 009/012, on a networked rig with a key:

1. **Model acceptance**: WS connect with `?model=gpt-realtime-translate` yields
   `session.created` with `type:"translation"`, `model:"gpt-realtime-translate"`.
2. **Output rate reality**: what `sample_rate`/`channels` values `session.output_audio.delta`
   actually carries (expected 24000/1 per [R8], but the field is optional in [R3]).
3. **Connected-state trigger**: whether we should treat `session.created` or the first
   `session.updated` as "connected & usable" (docs never say).
4. **Language code set**: submit each of the 13 target codes as `audio.output.language`
   and record accept/refuse (and the `error` shape when refused).
5. **Silence behavior** (12.5) and **flush latency of `session.close`** (rule 5): audible,
   operator-level checks at 012/018.
6. **`expires_at` semantics** and any idle-timeout: observe over a long session (024).

None of these block writing 009 (the backend can implement exactly what is documented);
they block declaring 009/012 PASS on real traffic.

## 15. References (all fetched 2026-10-02, official OpenAI properties)

- [R1] Realtime translation guide — `https://developers.openai.com/api/docs/guides/realtime-translation`
  (endpoint vs voice-agent table, transports, WS examples, `session.close` flush semantics,
  listen-along/conversational architecture, production checklist).
- [R2] Realtime translation client events (reference) —
  `https://developers.openai.com/api/reference/resources/realtime/translation-client-events`
  (`session.update` schema, `session.input_audio_buffer.append` byte/format/200 ms rules, `session.close`).
- [R3] Realtime translation server events (reference) —
  `https://developers.openai.com/api/reference/resources/realtime/translation-server-events`
  (`error`, `session.created/updated/closed`, both transcript deltas, `session.output_audio.delta`).
- [R4] Model page: GPT-Realtime-Translate —
  `https://developers.openai.com/api/docs/models/gpt-realtime-translate`
  (model ID, snapshots, endpoint support matrix, per-minute pricing, tier rate limits).
- [R5] Getting started with the Realtime API —
  `https://developers.openai.com/api/docs/guides/realtime`
  (beta→GA migration, no `OpenAI-Beta` header, ephemeral secrets, safety identifiers;
  "Other audio workflows" pointing at Live translation).
- [R6] WebSockets connection guide —
  `https://developers.openai.com/api/docs/guides/voice-websockets`
  (server-side Bearer auth vs browser subprotocol auth; voice-agent GA model
  `gpt-realtime-2.1` - *not* our endpoint; `session.start`/`session.started` belongs to
  GPT-Live, not to translation).
- [R7] API changelog — `https://developers.openai.com/api/docs/changelog`
  (May 7 2026: translate/whisper/realtime-2 launch; May 12 2026: Realtime Beta removed;
  Sep 2 2026: `slow_down` vs `server_is_overloaded` + `Retry-After`; Sep 1 2026: IPv6).
- [R8] Cookbook: Build Live Translation Apps with gpt-realtime-translate —
  `https://developers.openai.com/cookbook/examples/voice_solutions/realtime_translation_guide`
  (13 output + 70+ input languages; no prompting/voice selection; dynamic voice
  adaptation; same-language silence; 24 kHz output chunks; Twilio/LiveKit server
  patterns). Official OpenAI cookbook (`github.com/openai/openai-cookbook`).
- [R9] Error codes guide — `https://developers.openai.com/api/docs/guides/error-codes`
  (HTTP status table incl. 401/403/429/500/503 semantics and retry advice).
- [R-doc] `docs/device-defaults.md` (this repo): engine dev rate 48 kHz — context for
  section 7, not an OpenAI source.

## 16. What task 009 may rely on, in one paragraph

Open a WebSocket to `wss://api.openai.com/v1/realtime/translations?model=gpt-realtime-translate`
with the `Authorization: Bearer` header (and optionally `OpenAI-Safety-Identifier`),
expect `session.created` first, send `session.update` with
`audio.output.language` (and optional transcription/noise-reduction), stream base64
24 kHz mono little-endian PCM16 in ~200 ms chunks with continuous silence while the
session is open, deliver `session.output_audio.delta` (base64 PCM16, validate
`format`/`sample_rate`/`channels` when present) and `session.output_transcript.delta` /
`session.input_transcript.delta` on the sink thread that is not the audio callback, map
`error` events through the category table in section 9 without the session dying
(most are recoverable), and on close send `session.close`, keep reading until
`session.closed`, then drop the socket - with all resampling (48↔24) inside the
backend's worker threads and nothing invented beyond this file.
