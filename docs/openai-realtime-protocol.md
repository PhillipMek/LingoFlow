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
  Implemented 2026-10-05 (code review P2): the product value is the lowercase SHA-256
  hex digest of the Windows installation GUID (`HKLM\SOFTWARE\Microsoft\Cryptography\
  MachineGuid`) derived once at the composition root - the wire carries neither the
  GUID, nor a username, nor a hostname, and no plaintext personal or operator
  identifier is ever sent [R12]. Any derivation failure or malformed option value
  omits the optional header (with a warning in the malformed case); the header never
  blocks a session. `src/Network/SafetyIdentifier.h` owns the primitives.
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

### 4bis. Session expiry - the server's own deadline (recheck 2026-10-05, code review P1)

The realtime session object carries `expires_at`: "Expiration timestamp for the
session, in seconds since epoch" [R10]. It is documented OPTIONAL - a server that
fills `session.updated` without it is behaving as documented, not failing - and the
conversations guide separately states "the maximum duration of a Realtime session is
60 minutes" [R11], the fact task 010's `sessionMaxAgeMs` policy was built to predict.

Decision (supervisor policy): where the server announces a concrete expiry, that
announcement is authoritative, and a controlled reopen is started at
`expires_at - expirySafetyMarginMs` (default 5 minutes, operator-tunable); the local
age policy remains as cap and fallback - when both are known the EARLIER deadline
wins, because the earlier one is the one that will actually cut the audio.

Clock handling, stated honestly: `expires_at` is the server's wall clock, our only
comparison point is ours, so the backend converts at arrival:
`remaining = expires_at*1000 - local_wall_now_ms`, projected onto the steady clock.
An announcement computing to a non-positive or absurd (over 24 h) remaining interval
is refused as unusable - which catches both a badly skewed local clock and a server
sending milliseconds where the docs say seconds - is logged as such, and leaves the
machine on the local age policy. A later `session.updated` that carries the field
refreshes the announcement; one that does NOT carry it keeps the last known value
(the deadline belongs to the session, not to the event, and the event completing our
handshake is exactly such an update). Only closing the session clears the state
(contract rule 6).

## 5. Client events - the complete list

The reference page for translation client events contains exactly three [R2]. Anything
else is not protocol and must not be invented:

1. **`session.update`** — wire shape (live-verified, section 15):
   `{"type":"session.update","session":{ ... }}` — the configuration travels
   inside a `session` object; without the wrapper the service rejects the event
   with `Missing required parameter: 'session'`. Fields it may carry:
   `audio.output.language`,
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
  and queue the whole delta, never assume a fixed size [R3]; that trust is bounded by
  the client (code review P1, 2026-10-05): a reassembled WebSocket message must fit the
  16 MiB transport budget (larger severs the connection as a transport fault, which the
  supervisor recovers), and a decoded audio delta must fit 256 KiB (larger is dropped as
  a section-7 shape anomaly, session untouched) - both margins are 13x-250x the live
  measured 19200-byte chunk of section 15;
- the cookbook states translated audio is emitted as **base64 24 kHz mono PCM16 in
  200 ms chunks** [R8]; the event's `format` field enum is `"pcm16"` [R3]. Measured
  live (section 15): the server delivered fixed 19200-byte (400 ms) deltas while
  `elapsed_ms` advanced 200 ms per delta, arriving in bursts above real time - the
  backend must trust the event fields, accept variable block sizes, and the jitter
  buffer must be sized for bursts, not for the nominal chunk time;
- `sample_rate` and `channels` are *optional* fields on the event [R3]. The backend must
  treat them as authoritative when present and validate against what it assumed; the
  value the real API sends must be observed at the human checkpoint (14.2).

**Device-rate envelope (code review P1, 2026-10-05)**: this backend accepts on
either side exactly the product's config set - 24000, 44100, 48000, 88200, 96000 Hz -
against the 24 kHz wire, and nothing else (AGENTS.md 8: no invented rates, 32000 Hz
stays a refusal). 44.1/88.2 were legal in `ConfigSchema` while the backend rejected
them, so a lawful operator choice used to die at Start Translation. Power-of-two
relations run through the halfband cascade; 24 <-> 44.1 (147/80) and 24 <-> 88.2
(147/40) run through an exact-periodic 49-tap Blackman polyphase - integer phase
table, zero timing drift across a show. Its quality is what the tests measure
(TestPcmResampler "[rational]" cases, 2026-10-05): 1 kHz passes within a 5 % RMS
window in every direction, tones above the new Nyquist are gone (16 kHz through
44.1->24 and 20 kHz through 88.2->24: output RMS < 0.02 from a 0.9-amplitude
input), a 6 kHz tone's interpolation image measures below 5e-3 at 18 kHz (in
range, and nothing legitimate can live there - pure leakage), every
polyphase row is DC-normalised to unity, and chunked streaming equals one-shot
bit for bit.

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
| 401 | invalid/revoked key, wrong org, IP not authorized | `authentication` (operator-actionable; retrying without a change repeats it) |
| 403 | country/region not supported | `authentication` (with actionable detail in the message) |
| 429 | `slow_down` (ramp-rate), plain rate limit, spend/usage limits, `credit_balance_exhausted` | `rateLimited`, retry honoring `Retry-After`; the documented billing code (`credit_balance_exhausted`) is `rejectedRequest` - retrying cannot restore access [R9] |
| 500 | server error while processing | `connection`, backoff retry |
| 503 | `server_is_overloaded` (model temporarily overloaded) | `serviceOverloaded`, backoff retry (transient by the provider's own word) |

`Retry-After` header: "When the header is present, wait at least as long as it
specifies... If it's missing, use exponential backoff" [R7][R9]. Billing/quota 429s are
the documented exception: "Retrying billing, spend, or quota errors won't restore API
access" [R9] - treat as operator-actionable, retrying is pointless (task 010 policy).

In-session `error` events may carry the same transient vocabulary in `error.code`
([R7] Sep 2 2026 separates `slow_down` from `server_is_overloaded`): the codes classify
to `rateLimited` / `serviceOverloaded` before the coarse `error.type` is consulted, and
an in-session error event alone never faults the session (the official stance above).

**Mapping rule for task 009**: provider error names/types (`invalid_request_error`,
`server_error`, `slow_down`, `server_is_overloaded`, ...) are translated **only** into
the product `TranslationErrorCategory` values (`connection`, `rejectedRequest`,
`audioFormat`, `protocol`, `rateLimited`, `serviceOverloaded`, `authentication`,
`internal`) plus `fatal`; nothing provider-named crosses the sink seam (contract header,
AGENTS.md 8). `audioFormat` on our side means the 24 kHz PCM16 contract was violated
locally; a provider validation error about our audio maps to `audioFormat`, everything
else the provider refuses about the request maps to `rejectedRequest`, the account gate
refusing us maps to `authentication`, the provider asking us to come back later maps to
`rateLimited`/`serviceOverloaded`, transport death maps to `connection`, and anything we
cannot classify maps to `internal` - never dropped silently.
(Task 010 policy over this vocabulary: `connection`, `protocol`, `rateLimited`,
`serviceOverloaded` are recoverable by a fresh session; `authentication`,
`rejectedRequest`, `audioFormat` and `internal` stop at `faulted` for the operator.)

## 10. Reconnect and recovery semantics

What the documentation provides: production guidance to "Surface reconnecting, delayed,
and unavailable states" and "Track latency apart from translation quality" [R1]; the
`expires_at` field on every session object [R3]; the graceful-close handshake
(sections 4-5) [R1][R2].

What it does **not** provide (do not invent):

- no resume/attach protocol for translation sessions - reconnection means a new
  WebSocket, a new `session.created`, and a fresh `session.update`;
- no documented session duration value or expiry behavior beyond the presence of
  `expires_at` (seconds since epoch) [R3]; the live probe observed `expires_at` =
  creation + 3600 s on every session (section 15) - treat one hour as the practical
  session ceiling and reopen proactively (task 010); what the server does AT expiry is
  still unobserved;
- no documented WebSocket close-code semantics for this endpoint;
- no "flush done" signal other than `session.closed` itself.

Consequences for task 010 (kept in product vocabulary): after a dropped transport the
backend reopens a session per contract rule 6 (state reset), replays the stored
`SessionRequest` (pair + rates + language), and the application decides what to do with
audio that was queued during the gap. Since provider time is contiguous over appended
audio (section 7), the honest options at 010 were: drop the gap buffer (translate drifts
but stays aligned with "now") - the docs do not choose it for us.

**Chosen by task 010 (owner decision, 2026-10-02), now implemented in
`src/Translation/ReconnectSupervisor`:**

- **Gap policy = drop.** Audio arriving while the session is down is refused at the seam
  and counted (`gapRefusedFrames`); it is never buffered for replay. A live interpreter
  that goes quiet during the outage and resumes aligned with the room beats one that
  catches up on stale minutes - drifting the translation arbitrarily far behind "now" is
  the worse failure for the audience, and the NDI text would follow the drift.
- **Retry forever until the operator acts.** While the application runs, retryable
  failures keep reopening with exponential backoff (default 1 s, doubling, capped 15 s) -
  a venue network blip must not end the event.
- **Retryable vs terminal is decided by category (section 9), not by counting tries:**
  `connection` and `protocol` are fixed by a fresh session; `rejectedRequest`,
  `audioFormat` and `internal` are terminal for the recovery loop (a new session cannot
  fix what the request itself is, or a contract violation on our side, or the unknown),
  reported as `faulted` - operator-actionable states, not something to hammer. A 401,
  a bad pair, or billing therefore stop recovery immediately; a dropped socket does not.
- **A service `Retry-After` (section 9) is honored as a floor:** the transport reads the
  header, the backend carries the hint across the seam as `TranslationError.retryAfterMs`,
  and the supervisor waits at least that long before the next attempt. It never replaces
  the backoff when absent.
- **Proactive reopen before the ceiling:** with the default age policy (55 minutes,
  below the 3600 s `expires_at` observed in section 15) the supervisor runs a normal
  close→open cycle before the provider expires the session, so a long event never reaches
  that path blind. What the server does AT expiry is still unobserved (024 confirms on a
  real event); with the age policy on, production should not hit it.
- **Recovery is transparent to the audio device and the engine:** the supervisor only
  speaks `ITranslationBackend`; it cannot touch ASIO (SPEC "Reliability"). It reports
  `reconnecting` to the application and hides the transient `faulted`/`closed`/`connecting`
  churn of the sessions it replaces, so the UI sees "connected -> reconnecting -> connected",
  not a machine-gun of internal state. Non-fatal errors, audio and text pass through.
- **`closeSession()` is a full stop:** the recovery loop is joined inside it, and no
  callback reaches the application after it returns (contract rule 5). This is what lets
  the controller close the session before the device during shutdown.

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
   ignore with a warning (and count/report it as not applied). **Resolved by the owner
   2026-10-03 (task 012)**: the notice is a LOG warning only - no `TranslationError`
   crosses the seam for it. The request itself is not refused, and `rejectedRequest` is
   the supervisor's terminal category (section 10), so an ordinary every-start capability
   fact must not appear as a failure in the operator's status. The field itself stays in
   the contract because other backends may support it.
   **Second half resolved by code review P1 (2026-10-05)**: requiring an
   operator to author a text the provider ignores is its own fake contract.
   `translation.instructions` became OPTIONAL with an empty default (the field
   round-trips and future models reuse it), the Settings dialog shows it
   read-only and says plainly "unsupported by gpt-realtime-translate" with the
   reason, and the diagnostics export pairs the value with an
   `instructions_effect` line - "not set" or "ignored - ...". The backend
   warning at open stays exactly as decided 2026-10-03: a config file carrying
   legacy text still learns the truth from the log.
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

**Frozen by task 011 (2026-10-03), as `src/Translation/LanguageRegistry` v1.** The live
checkpoint 14.4 closed the code question first (section 15): all 13 targets accepted as
ISO 639-1. The manifest therefore carries: `targets` = the 13 live-verified ISO 639-1
codes (`es pt fr ja ru zh de ko hi id vi it en`), `sources` = the [R8] enumeration
re-fetched 2026-10-02 - 74 named languages mapped to ISO 639-1, with ISO 639-2 `fil`/`haw`
for the two names that have no two-letter code (R8 lists Tagalog AND Filipino; both kept
because the source lists both). The source codes never cross the wire - the provider
auto-detects spoken language and `session.update` carries only the target (section 5) -
so they are the product's own identifiers for declaring and validating what is spoken.
Consumers of this single list: `ApplicationController::startSession` (operator gate),
`OpenAIRealtimeBackend::openSession` (offline refusal, with
`OpenAIRealtimeOptions::capabilities` as the seam a future dynamic manifest would
replace it through - dynamic discovery is not available with this key, section 15), and
from task 014 the UI dropdowns. Nothing else may carry a language list (FAIL criterion).

**UI consequence (P1 fix, 2026-10-06).** Because the source code never crosses the wire,
no operator-facing surface may present it as if it did. The main screen shows the target
as a selector and the source as a static line - `Source: Automatic detection` (the text
comes from `OperatorPanel::sourceDisplay`, i.e. from the tested model). The expectation
is set only in Settings -> Translation under the name "Expected source language", with a
visible note that OpenAI detects the spoken language itself and the setting is used by
LingoFlow for validation and diagnostics only; `checkPair` keeps gating the expectation
against the manifest (an expectation of a language the provider cannot detect is a
configuration defect worth refusing). The Diagnostics export states the two facts
separately: `source_expectation` and `provider_source_detection: automatic`.

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

*Update 2026-10-02: the owner supplied a key; items 1, 3, 4, 6 and 2 were verified live
the same day (2 fully, including an owner ear-check of the delivered stream) - results in
section 15. Item 5 and the long-run expiry semantics stay open.*

## 15. Live verification log (2026-10-02, owner-provided key, real traffic)

The owner supplied an `OPENAI_API_KEY` (User-scope environment variable; value never
logged or committed). Probes run against the live service, closing parts of section 14:

- **14.1 CLOSED - model acceptance**: WS upgrade to the translation endpoint returned
  `101 Switching Protocols`; the first server event was `session.created` with
  `type:"translation"`, `model:"gpt-realtime-translate"`, `id:"sess_…"` exactly as [R3].
- **Server defaults observed**: `audio.input = {noise_reduction: null, transcription:
  null}`, `audio.output.language = "es"`. We configure what we need; the default is not
  ours to rely on.
- **14.3 CLOSED (ordering)**: `session.update` sent immediately after `session.created`
  was answered by `session.updated` with the resolved config (~0.2 s round trip). Either
  event may be the "usable" trigger; 009 will treat `session.updated` as usable because
  our configuration must be confirmed before audio flows (this is now a documented
  product decision, not a protocol fact).
- **14.4 CLOSED - language codes**: all 13 target languages accepted as ISO 639-1 codes
  (`es pt fr ja ru zh de ko hi id vi it en`); `ru` verified explicitly with the resolved
  session echoing `language:"ru"`. Most ISO 639-3 forms (`spa`, `rus`, `deu`…) are also
  accepted; `zho` is REJECTED with `invalid_request_error/invalid_value`, and the error
  message itself disclosed the supported-value set (the 2-letter list incl. `af ar az be
  bg bs …` - the 70+ input languages of [R8] as codes). The 011 manifest should use
  ISO 639-1 two-letter codes only.
- **14.6 CLOSED (initial value)**: `expires_at` = creation time + 3600 s on every observed
  session. Session max duration 60 minutes: 010 MUST schedule a proactive reopen shortly
  before expiry (an operator-visible requirement for events longer than one hour).
- **14.2 CLOSED - output format**: 171 delivered `session.output_audio.delta`
  events across two complete runs (11 s of English TTS speech in, target `ru`). EVERY
  delta carried `sample_rate:24000, channels:1, format:"pcm16"` and exactly 19200 bytes
  = 400 ms of PCM at 24 kHz (the cookbook's "200 ms chunks" [R8] is not what arrived;
  the event fields were unanimous, and the raw stream was saved and measured).
  Content analysis of the concatenated 1.6 MB stream, since a byte stream cannot reveal
  its own rate directly: ~6.8 s of voiced content in ~34 s at the 24 kHz label - a
  normal speaking rate for the 124-character Russian output; the same 1.6 MB read at a
  48 kHz label would mean 3.4 s of speech for 124 characters (~2.5x speed, implausible).
  Dominant pitch periods 95-116 samples = 207-253 Hz at 24 kHz, consistent with the
  female TTS source under the model's documented voice adaptation [R8]. Even/odd sample
  MAD ratio 0.22 - a single continuous stream, not duplicated stereo pairs. Verdict:
  the delivered PCM is genuinely 24 kHz mono PCM16, as the events declare. **Owner
  ear-check 2026-10-02, decisive**: `out_as_24k_mono.wav` sounds normal, while
  `out_as_48k_mono.wav` and `out_as_24k_stereo.wav` both play ~2x fast - exactly the
  pattern of mislabeled 24 kHz mono. Translation QUALITY checks (real voice, latency)
  stay with 012/018.
- **NEW (measured) - delivery rate is bursty, above real time**: each delta advanced
  `elapsed_ms` by 200 ms while carrying 400 ms of PCM (2x), and after `session.close`
  the drain delivered ~200 KB/s (~4.7x real time). Arrival therefore runs consistently
  and burstily AHEAD of consumption. Consequences fixed for 012: the jitter buffer's
  default target (120 ms) and capacity (target+360 ms) are marginal for a single 400 ms
  delta - defaults must be re-sized, and overflow policy (007's counted drops) is the
  safety net, not an error path. This is measurement, not OpenAI documentation; 018
  re-measures on real audio.
- **Transcript deltas live**: `session.output_transcript.delta` (Russian text, 32 append-only
  deltas, spacing included in the fragments) and `session.input_transcript.delta` (source
  English) emitted with `elapsed_ms` advancing in 200 ms steps and repeating across
  events - matching [R3] word for word. `gpt-realtime-whisper` accepted as
  `audio.input.transcription.model`.
- **Graceful close live**: `session.close` → remaining queued `session.output_audio.delta`
  continued to arrive (68% of deltas post-close in one run) → then `session.closed`.
  Rule 5 (007) is exactly right: after sending close, the backend MUST keep draining
  until `session.closed`; closing the socket early drops translated audio.
- **NEW (not in docs) - transport keepalive**: one idle probe (connected, configured,
  no audio, silent) was closed by the server with WS close reason `keepalive ping
  timeout`. Respond to server pings (RFC-6455 pong) AND send periodic client pings
  (~4-15 s) in 009; also start streaming audio promptly after opening.
- **NEW - key scopes**: this key gets `403 Missing scopes: api.model.read` on
  `GET /v1/models` while working fine on the translations endpoint. Consequence for
  011: dynamic capability discovery via `/v1/models` is NOT available with this key -
  the versioned capability manifest (AGENTS.md 9) is the path, not a fallback.
- **NEW - quality caveat for synthetic input**: on TTS voice the translation stuttered
  and merged words ("сетидля"); this is why 012/018 human checks use real microphone
  audio. Also visible: translated audio duration far exceeded source duration at times
  (interpretation verbosity) - jitter buffer sizing (018) must budget for that.

Probes used a raw Node.js WebSocket client (built-ins only): handshake with the
`Authorization: Bearer` header, base64 24 kHz PCM16 chunks appended at a 200 ms cadence,
`session.update`/`session.close` as documented. No product source was involved.

**Same-day implementation validation (task 009, the product backend against the live
service):**

- **`session.update` requires the `session` wrapper**: the first backend draft sent the
  configuration at the top level and the service answered the `error` event
  `Missing required parameter: 'session'`. The probes above had always wrapped it
  (`{"type":"session.update","session":{…}}`); §5 now states the wire shape explicitly.
  With the wrapper the full lifecycle runs through the product backend.
- **Full-duplex transport facts (drives the backend's thread model)**: with one thread
  blocked inside the synchronous `WinHttpWebSocketReceive`, 39 concurrent sends from a
  second thread on the same handle all succeeded and the server answered them; an idle
  blocking receive is NOT released by any receive timeout (request-level and handle-level
  variants both ignored); `WinHttpWebSocketClose` DOES release the pending receive
  (~225 ms observed, `ERROR_WINHTTP_OPERATION_CANCELLED`). The backend is therefore
  receiver-thread + sender-thread, and every bounded wait ends via cancellation, never
  via sleep-polling of the socket.
- **Keepalive solved in layers**: WinHTTP's documented WebSocket keepalive interval
  (minimum 15 s) provides transport-level client pings, and the backend's continuous
  append cadence (including silence) keeps application traffic flowing; 19-20 s sessions
  with a 2 s trailing-silence tail never hit the "keepalive ping timeout" close. This
  refines the recommendation earlier in this section.
- **Final functional round-trip through the shipped binary**
  (`lingoflow_openai_probe`, in/24000 → out/48000, language ru): connected in 1.7 s;
  11.43 s of English TTS streamed with zero queue rejections; graceful close drained
  3.8 s; **1,593,600 translated samples delivered at 48000 Hz** (exercising the
  backend's live 24→48 upsample), 187 characters of Russian transcript, 0 errors,
  state transitions exactly `connecting → connected → closed`. Translated duration again
  far exceeded the source (sparse/slow interpretation) - the 012/018 burst-sizing
  conclusions stand.
- **Transcript accumulation does not reset between sentences (2026-10-03, task 013
  live re-run, `lingoflow_openai_probe` → ru)**: one 11.4 s English run delivered a
  single continuous `session.output_transcript.delta` stream of 164 characters that
  crossed two sentence boundaries without restarting or signalling anything - the
  provider offers no per-utterance text boundary at all, confirming sections 6/8
  from the live side. Consequences decided in 013: the product owns subtitle lines
  (the backend's settle rule `OpenAIRealtimeOptions::transcriptSettleMs` plus the
  closeSession flush), the sink's `onPartialText` is a whole-line SNAPSHOT
  (fragment-accumulating consumers double-write under snapshot partials - the 013
  probe fix shows exactly that before/after), and `elapsed_ms` keys nothing. Whether
  2500 ms of stream-pause is the right cut point for live speech is a rig
  observation (docs/rig-checklist.md step 9).

Remaining open items from section 14: 14.5 (silence/ducking behavior needs real speech
in the target language - 012), and the long-run semantics of `expires_at` (what the
server sends at expiry - 010/024).

## 16. References (R1-R9 fetched 2026-10-02, R10-R11 fetched 2026-10-05; official OpenAI properties)

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
- [R10] Realtime API reference, session object (fetched 2026-10-05) —
  `https://developers.openai.com/api/reference/resources/realtime`
  ("`expires_at`: optional number - Expiration timestamp for the session, in seconds
  since epoch"; session object re-carried in `session.updated`).
- [R11] Realtime conversations guide (fetched 2026-10-05) -
  `https://developers.openai.com/api/docs/guides/realtime-conversations`
  ("The maximum duration of a Realtime session is 60 minutes"; `session.created`
  on connect, `session.updated` answers configuration updates).
- [R12] Safety identifiers (fetched 2026-10-05) -
  `https://developers.openai.com/api/docs/guides/realtime` ("Safety identifiers")
  and `https://developers.openai.com/api/docs/guides/safety-best-practices`
  ("OpenAI recommends safety identifiers but doesn't require them"; "Use a stable,
  privacy-preserving value, such as a hashed internal user ID"; "Hash user email or
  internal user IDs to avoid passing any personal information"; for direct WebSocket
  connections from a trusted backend "set the header on the connection request").
- [R-doc] `docs/device-defaults.md` (this repo): engine dev rate 48 kHz — context for
  section 7, not an OpenAI source.

The live probe data of section 15 is this repository's own measurement record from
2026-10-02, not an OpenAI publication.

## 17. What task 009 may rely on, in one paragraph

Open a WebSocket to `wss://api.openai.com/v1/realtime/translations?model=gpt-realtime-translate`
with the `Authorization: Bearer` header (and optionally `OpenAI-Safety-Identifier`),
expect `session.created` first, send `session.update` (configuration inside a `session`
object - section 5) with `audio.output.language` (and optional
transcription/noise-reduction), stream base64
24 kHz mono little-endian PCM16 in ~200 ms chunks with continuous silence while the
session is open, deliver `session.output_audio.delta` (base64 PCM16, validate
`format`/`sample_rate`/`channels` when present) and `session.output_transcript.delta` /
`session.input_transcript.delta` on the sink thread that is not the audio callback, map
`error` events through the category table in section 9 without the session dying
(most are recoverable), and on close send `session.close`, keep reading until
`session.closed`, then drop the socket - with all resampling (48↔24) inside the
backend's worker threads and nothing invented beyond this file. Three live additions
(section 15): treat `session.updated` as the usable trigger; answer server pings AND
send periodic client pings (a silent connection was closed with `keepalive ping
timeout`); expect output deltas that arrive in bursts well above real time and continue
for a moment after `session.close`, so drain fully before dropping the socket.
