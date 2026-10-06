# LingoFlow

Windows desktop application for simultaneous speech translation at live events:

```text
SoundGrid/ASIO in  ->  audio engine  ->  OpenAI Realtime translation  ->  audio engine  ->  SoundGrid/ASIO out
                                       ->  NDI subtitle out (optional)
```

Status: **tasks 000-011 and 013-019 complete; 012 and 016 implemented** - their
open REQUIRED human checkpoints (012 live EN<->RU through ASIO+OpenAI; 015 key entry on
the target machine; 016 real NDI receiver on the venue network) are run from
`docs/rig-checklist.md`. The repository contains a JUCE/CMake
application, a portable core (`lingoflow_core`) with the module interfaces, a realtime
audio pipeline (lock-free ring buffer, output jitter buffer, input and output gain with
click-free gliding, level meters, clipping and underrun/overrun counters) with
input->output loopback, the translation backend contract with a deterministic test mock,
a real OpenAI realtime translation backend (`src/Network`, on the OS winhttp WebSocket
stack with the 24<->48 kHz resampler inside it), Null implementations of the translation
and NDI boundaries, versioned configuration with safe persistence, ASIO device discovery
and device lifecycle on top of JUCE, a reconnect/session-recovery supervisor, the
single language registry with its versioned OpenAI capability manifest, the mounted
translation pipeline (task 012: capture worker + supervisor + real backend in the
composition root), the typed text pipeline (task 013: snapshot events, bounded history,
UI-facing model), the operator window (task 014: a JUCE shell over the tested UiModel),
the Settings dialog with Windows-secure API-key storage (task 015), the real NDI
timed-text subtitle output behind the contract (task 016), the bounded event ring and
the structured diagnostics export (task 017), the honest
latency accounting with per-row kinds and no measured numbers in code (task 018), the
developer & mock mode that runs the whole core without SoundGrid and without an API key
(task 019), and 344 tests.
The OpenAI backend (`task 009`) codes against the protocol verified from the live official
documentation and frozen in `docs/openai-realtime-protocol.md`, spot-checked against the
real service on 2026-10-02 (dedicated `gpt-realtime-translate` endpoint, complete event
surface, 24 kHz PCM16 audio contract, graceful close, all 13 target language codes; every
citation and probe dated). The backend has completed one real end-to-end round-trip
through the shipped probe binary (English speech in, Russian translated audio + transcript
out, graceful close drained); the operator ear-check of translation quality and the
long-run/expiry and rig-routing behavior are the remaining human checkpoints (tasks
012/018/010). Since task 012 the windowed application itself carries the chain: capture
streams from the engine's input ring to the backend on a worker thread, and delivered
audio is the only source of the output jitter buffer - what a rig still has to confirm is
sound through a real SoundGrid path (run sheet `docs/rig-checklist.md`). Startup checks
(`--smoke`) deliberately stay offline on the Null backend; `--dev --smoke` (task 019) runs
the same offline startup through the simulated tone + mock-echo chain - the proof that the
core needs neither hardware nor a provider account.

Licences are decided and binding: **JUCE 9 under AGPLv3**, **ASIO SDK under GPLv3**,
which makes this product AGPLv3 and puts NDI behind runtime loading
(`docs/licensing.md`). Development defaults for the audio device, and the fact that
this PC will never see a SoundGrid server, are recorded in `docs/device-defaults.md`.

## Requirements

Verified on this machine (`docs/environment-report.md` has the full evidence):

| Component | Version | How it is obtained |
|---|---|---|
| Windows | 10 x64 | host |
| MSVC (Visual Studio 2022) | 19.44.35229 | local install |
| CMake | 3.31.6-msvc6 | bundled in Visual Studio, **not on PATH** |
| Ninja | 1.12.1 | bundled in Visual Studio, **not on PATH** |
| JUCE | 9.0.3 (AGPLv3) | **vendored**: `third_party/JUCE/` |
| Steinberg ASIO SDK | 2.3.4 (GPLv3) | **vendored**: `third_party/asiosdk/` |
| nlohmann/json | v3.12.0 (MIT) | CMake `FetchContent`; used only inside `src/Config/*.cpp` |
| Catch2 | v3.16.0 (BSL-1.0) | CMake `FetchContent` (needs network on first configure) |
| Waves SoundGrid ASIO driver | 16.5.197.301 | installed product (no server reachable on this PC) |
| NDI | 6.3.2.0 runtime **and** SDK (`C:\Program Files\NDI\NDI 6 SDK`, `NDI_SDK_DIR`) | installed product; referenced, never linked (see `docs/licensing.md`) |

## Configure and build

`cmake`, `ninja` and `cl` are not on `PATH`, so run everything from a `vcvars64`
shell and point CMake at the bundled Ninja. From the repository root:

```powershell
$vs    = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$vccmd = "`"$vs\VC\Auxiliary\Build\vcvars64.bat`""

# Build trees stay on an ASCII path (see "Build tree must be on an ASCII path")
$dbg = 'D:\LingoFlow\build-debug'
$rel = 'D:\LingoFlow\build-release'

cmd /c "call $vccmd && `"$cmake`" -S . -B $dbg -G Ninja -DCMAKE_BUILD_TYPE=Debug   -DCMAKE_MAKE_PROGRAM=`"$ninja`""
cmd /c "call $vccmd && `"$cmake`" --build $dbg"

cmd /c "call $vccmd && `"$cmake`" -S . -B $rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=`"$ninja`""
cmd /c "call $vccmd && `"$cmake`" --build $rel"
```

JUCE comes from `third_party/JUCE` by default; `-DLIVEAI_JUCE_PATH=C:/path/to/JUCE`
overrides it (for example to test another JUCE version). Nothing is downloaded: only
Catch2 and nlohmann/json are fetched, and `-DLIVEAI_FETCH_CATCH2=OFF` /
`-DLIVEAI_FETCH_NLOHMANN_JSON=OFF` switch to installed copies for offline builds.

Tests are configured by default; add `-DLIVEAI_BUILD_TESTS=OFF` to skip them.

## Run tests

344 CTest entries: Catch2 unit suites (including the gain-stage and translation-contract
suites, the task 009 base64 / PCM-resampler / OpenAI-protocol-and-lifecycle suites that
run the backend against a scripted offline transport, the task 010 reconnect-supervisor
suite that drives recovery against a threaded mock, the task 011 language-registry
suite that pins the frozen capability manifest and the pair-check rules, the task 012
capture-streaming suite that runs the worker against a recording backend, the task 013
text-pipeline suite - snapshot/eviction/duplicate rules and a two-writer race case -
and the task 014 UiModel suite that runs the whole operator screen headless through the
real controller; the task 015 credential suites - chain-store routing rules, a live
Windows Credential Manager round-trip including a child-process case proving the store
is not process memory, and the controller funnel that keeps the secret out of settings,
notes and logs; the task 016 NDI suites - the TTML document shape and escaping rules, and
the real output's lifecycle against the machine's loaded NDI runtime (sender created,
captions handed over, state polled, destroyed), the task 017 diagnostics suites - the
event ring's order, bounds and announced eviction; the renderer's redaction pass with
Config's own secret-shape predicate injected; atomic file landing; and the controller's
full export with a canary credential proven absent; the task 018 latency accounting rows -
kinds per row, the in-flight backlog arithmetic with its clamp and gap rules, a reporting
driver quoted with its frames versus a silent one rendered "not reported", and the export
shipping the same rows the screen shows; the task 019 developer suites - the WAV reader's
formats and its honest refusals, the mock backend's contract rules 1-6 with its labelled
echo and dropped-at-close queue, the plan that turns a default config into NOTHING at all
and a --dev flag into tone+mock without ever touching loopback, and the offline showpiece:
a WAV file in, the mock echo, and a WAV file out through the real controller with no
device and no key; plus the code-review suites of 2026-10-05 - ASIO channel forwarding
pinned under BOTH driver-array conventions, engine defensive paths silencing every
promised output channel against poisoned buffers, the NDI dispatch worker proven
producer-non-blocking against a transport frozen mid-publish, the server-announced
session expiry outranking the local age policy (protocol docs section 4bis, refused
when implausible), and the rational polyphase stage that makes the config's own
44.1/88.2 kHz device entries actually open a session with measured, pinned filter
quality, and the inbound size budgets (16 MiB reassembled message, 256 KiB audio
delta) with the arithmetic in a pure class the tests pin without any socket), the
end-to-end integration suite that now drives the whole pipeline through the real streaming
worker (with a supervisor-mounted outage case and a full interrupted-subtitle-line case
verified against the typed model), the realtime allocation suite in its own
binary, the architecture boundary audit plus its self-test, the realtime safety audit plus
its self-test, and the two device entries that run the ASIO tool's driver-free modes.

```powershell
cmd /c "call $vccmd && `"$cmake`" --test-dir $dbg --output-on-failure"
cmd /c "call $vccmd && `"$cmake`" --test-dir $rel --output-on-failure"
```

(`ctest.exe` sits next to the bundled `cmake.exe`; if it is on `PATH`, plain
`ctest --test-dir D:\LingoFlow\build-debug --output-on-failure` works too. Groups can be
selected by label: `-L architecture`, `-L realtime`, `-L device`.)

## Run the application

The executable is `LingoFlow.exe` (it is named by `PRODUCT_NAME`):

```powershell
$exe = "$dbg\src\LingoFlow_artefacts\Debug\LingoFlow.exe"

& $exe --smoke   # headless start/stop check: exit 0 when the start succeeded, 2 when it did not
& $exe           # operator window
```

`--smoke` starts the application lifecycle, stops it and exits without creating
a window. It is a startup/logging check only — it does not verify audio,
translation quality or the operator-visible UI.

The windowed run opens the operator screen (task 014): state chips with a plain-words
detail line, Start/Stop (Retry after a fault the operator has corrected), the device
selector fed by the ASIO registry scan, input/output language selectors from the single
capability registry (an unsupported pair warns before the session is even attempted),
sample-rate/buffer/channel selectors from the config schema's own ranges, live input and
output faders with mute and clip lamps over the engine's meters, the jitter pre-roll
slider, a counted-underruns/throughput diagnostics grid, a bounded subtitle panel reading
the task 013 model, and the outcome note of every settings action. The window contains no
audio, device or protocol logic: it paints `UiModel` values and calls controller methods,
and `UiModel` is tested headless against the real controller (which is how the PASS
criteria are verified without a human). A **Settings...** button opens the task 015
dialog: the masked API-key field (stored to the Windows Credential Manager, cleared from
the screen on success, never written to settings or logs), the translation instructions
and model hint, the recovery policy, NDI settings and the diagnostics block - committed
as one atomic draft through the same `updateSettings` funnel, with the log level taking
effect live and the log-file on/off waiting for an application restart. The main screen
also carries a permanent credential line ("stored" / "NOT stored - ... until it is
entered in Settings"), so an operator can see the key's state before pressing Start,
and an **Export diagnostics** button that writes the run's full report (counters,
geometry, states, settings, event ring) and names the written file in the note below it.

The application writes a log file to
`%APPDATA%\LingoFlow\logs\lingoflow.log` (JUCE's
`File::userApplicationDataDirectory`, which is the Roaming profile folder on
Windows).

### Paths and the `juceaide` limitation

JUCE's helper tool `juceaide` (it generates `*_resources.rc`) crashes with
"Unhandled exception" when a **non-ASCII path appears in its command-line
arguments**. This is about arguments, not about where the tools live - verified by
running `juceaide rcfile` directly with ASCII and non-ASCII argument paths.

The repository lives at `D:\work\LingoFlow` - an ASCII path, which it did not have
before (it used to sit under `D:\Рабочий\...`, where only the build tree could be ASCII:
CMake, Ninja and MSVC compiled and ran fine from the Cyrillic path, but `juceaide`
crashed when the build tree inherited it). Out-of-tree build trees on an ASCII path
remain the documented convention, because they keep the checkout clean and let the same
sources be configured for Debug and Release side by side:

```powershell
cmake -S . -B D:/LingoFlow/build-debug   # documented convention: one tree per config
cmake -S . -B build                      # also works: the source path is ASCII now
```

Both forms are measured, not assumed. Out-of-tree is the current evidence: after task 005
the suite is **112 CTest entries**, and `D:\LingoFlow\build-debug` and
`D:\LingoFlow\build-release` pass all 112 with zero compiler warnings. The in-tree form was
measured at this path on 2026-10-01 (then 84/84): `cmake -S . -B build` +
`cmake --build build` + `ctest`, with the CMake log showing `juceaide` configuring,
building, exporting and self-testing normally; that test tree was deleted again. Earlier,
the same in-tree form passed on a clone at `D:\LiveAI\clone-test` (a pre-rename path).

So the constraint is the path, not the layout: the checkout can live anywhere with an
ASCII path, and the vendored dependencies travel with it.

## Naming

The product is **LingoFlow**. It was called "Live AI Interpreter" through tasks
000-004. The rename covers the product name, the executable (`LingoFlow.exe`), the
CMake targets (`lingoflow_core`, `lingoflow_asio_probe`, `lingoflow_tests`), the
bundle id, the NDI stream name default, the data folder and the log file
(`lingoflow.log`).

Two things deliberately kept their old names, because they are internal and renaming
them would only churn the diff: the C++ namespace `liveai` and the `LIVEAI_*` CMake
options (`LIVEAI_JUCE_PATH`, `LIVEAI_BUILD_TESTS`, `LIVEAI_FETCH_*`).

Settings written before the rename (`%APPDATA%\Live AI Interpreter\config.json`) are
read once when there is no file at the new location, then rewritten to
`%APPDATA%\LingoFlow\config.json`. The old file is never modified or deleted, and the
old path is never written to again. `ConfigStore::startupFile()` resolves it,
`ApplicationController::loadSettings(..., persistTo)` performs the one-time move, and
both are covered by tests.

## Configuration

Settings live in one file, `config.json`, next to the log:

```text
%APPDATA%\LingoFlow\config.json          (Windows)
~/.lingoflow/config.json                              (other platforms)
```

The file is versioned (`schemaVersion`, currently `1`) and grouped into `audio`,
`translation`, `ndi`, `diagnostics`. Behaviour, all covered by tests:

| Situation | What happens |
|---|---|
| no file yet | defaults are used, **nothing is written** |
| one field invalid (bad rate, gain, language, log level...) | only that field falls back to its default, the rest is kept, each repair is logged as a warning |
| unknown field or section | reported as ignored, never written back out |
| not valid JSON | the file is renamed to `config.json.corrupt-<timestamp>`, `%...config.json.bak` is used if present, otherwise defaults |
| `schemaVersion` newer than this build | file is refused and left untouched, defaults are used |
| `schemaVersion` missing | treated as version 0, migrated to the current version |
| candidate settings fail validation | `update()` refuses them in full, `save()` refuses to write |

Saving is atomic: write `<file>.tmp`, read it back and re-parse it, copy the
previous file to `<file>.bak`, only then replace `<file>`.

The default values themselves, and why device-dependent numbers (channel counts,
names, latencies) are **not** defaulted on a machine without a SoundGrid server, are
specified in `docs/device-defaults.md`. A test asserts that the defaults in code
still match that table.

**Credentials are never in this file.** `api_key`, `token`, `secret`, `password`,
`credential`, `authorization`, `bearer` (and their spellings) are detected while
parsing, reported and dropped. They belong to `security::ISecretStore` - since task 015
that means the Windows Credential Manager first (`WindowsCredentialStore`), with the
environment variable as the documented development fallback (`ChainedSecretStore`), and
the operator enters the key through the masked Settings field. The application must keep
running when no credential exists: only the translation backend becomes unavailable.

## Device defaults and hardware status

This development PC has the Waves SoundGrid **driver** installed but will never have a
SoundGrid **server** reachable. The driver still opens and runs against that absent
server, so enumeration, capabilities and the whole open/start/stop/close lifecycle are
verified here (measured numbers in `docs/device-defaults.md`). What is **not**
verifiable here is whether real console audio arrives in the selected input channel and
leaves on the selected output - that stays a human check.

The defaults that follow from it: empty device identifiers (nothing is selected or
hardcoded), 48 kHz / 480 frames / mono in+out / 0 dB gain, `ndi.enabled = false`.
The SPEC fixes 48 kHz and leaves the block size to the implementation; 480 frames is
our 10 ms choice. The installed driver offers 256 only, so the engine falls back to the
nearest offered size and logs the fallback - never silently. The UI must never present
any of these defaults as measurements.

## Gain, mute and clipping (task 006)

Two independent digital trims, exactly where SPEC "Audio Pipeline" puts them: the input
trim is applied before the ring buffer (so the translator gets the level the operator
chose) and the output trim after the jitter buffer (so the audience level can be matched
without changing what the translator was fed). Behaviour, all covered by tests:

* **dB, not a linear knob.** `audio.inputGainDb` / `audio.outputGainDb` in settings; the
  stage converts with `exp2` against a documented constant, and the window it accepts is
  -96..+24 dB (SPEC suggests -24..+24 and keeps it configurable, so the stage window
  contains everything the configuration layer can validate). A request outside it is
  clamped and counted, not swallowed; a non-finite request is refused and the previous
  level stays.
* **Changes glide, they do not jump.** A gain or mute change moves the coefficient to its
  target over `kDefaultRampMs` (20 ms) and lands exactly on it, which is what makes the
  PASS criterion "no clicks" measurable: cutting a 500 Hz sine at its peak and dropping
  12 dB produces a 0.37 sample step with an instant change and nothing above the wave's
  own slew (~0.033) with the glide. The one setting that can click is a 0 ms glide, and it
  has to be asked for. `activate()` lands instead of gliding, so the first block of a run
  already plays the configured level.
* **Input mute and output mute are separate.** Muting the input stops feeding the
  translator while the audience keeps hearing the translation already in the buffer;
  muting the output silences the room while the translator keeps being fed. Neither is
  stored in settings: mute is live stage state, and a room that came back muted after a
  restart would be a fault nobody asked for.
* **This is not a limiter.** SPEC puts a limiter under "Future architecture", so samples
  above full scale are reported and passed through untouched. The only value rewritten is
  one that stopped being a number at all (a product that overflowed to infinity becomes
  full scale) - and every such sample is counted.
* **Clipping is counted three times, because there are three questions.** What arrived at
  full scale from the device (`inputClippedSamples`), what our own input trim produced on
  the way to the translator (`inputGainClippedSamples`) and what is heading at the audience
  (`outputGainClippedSamples`). These - like every `*Samples` engine counter - sum channel
  samples, not device frames: identical numbers on the single-channel capture the product
  translates on, C-times-larger on C channels, which the name says out loud (code review
  P2, 2026-10-05). Turning the gain down cannot hide the first one, which is
  the whole reason the meters read post-gain audio but the stage also counts pre-gain
  full-scale samples. A latching `takeInputClipIndicator()` / `takeOutputClipIndicator()`
  is what the UI lamp will use.
* **Non-finite samples become silence, counted.** One NaN from a driver silences one
  sample (`nonFiniteInputSamples`) instead of poisoning the translation stream or the room.
* **Levels survive a device restart.** The stages are not released by `deactivate()`, so a
  re-opened ASIO device starts with the same gains, the same mute state and the same
  clipping history.
* **`--smoke` logs the levels in effect**, so "my settings were applied" is distinguishable
  from "the defaults were used": `audio gains in effect: input -6.5 dB, output +3.0 dB,
  glide 20 ms`. The slider, the numeric box, the reset button and the mute button themselves
  are task 014; the engine API they will call already exists.

## Translation contract and the deterministic mock (task 007)

`src/Translation/ITranslationBackend.h` is now a complete contract, not a skeleton:
lifecycle, translated audio, text, errors and state, with six numbered rules that the
reference implementation enforces and the tests assert. What this means concretely:

* **Session requests carry rates.** The backend is told at what rate `submitAudio()` will
  arrive and at what rate the answer must be; every delivered block repeats its actual
  rate, so the receiver never trusts setup alone. Blocks at a wrong rate are refused and
  counted (`rejectedAudioFrames`), never silently resampled - resampling is the backend's
  job on its own worker threads (task 009), and the controller still refuses what arrives
  wrong; playing 16 kHz at 48 kHz is not an option.
* **Errors have product vocabulary.** Five categories (`connection`, `rejectedRequest`,
  `audioFormat`, `protocol`, `internal`) and a fatal flag; the task 009 backend maps
  provider failures onto them, so no OpenAI error name crosses the seam. A fatal error ends the
  session and changes nothing else: AGENTS.md 12 holds the audio path open, the reason
  goes to counters, log and `status().detail`.
* **`closeSession()` guarantees no callbacks after it returns.** That is what lets the
  controller stop the session before the device without a race.
* **Every state transition is reported exactly once; a refusal that changes nothing
  reports nothing.** This makes the trace assertable - which is what task 010's
  reconnect logic will be built on.
* **The deterministic mock lives in `tests/support/`, not in the product.** No threads,
  no timers, no clock: callbacks fire synchronously, scripts replay identically, and the
  whole chain is testable without a network. The product's Null backend remains silent on
  purpose (AGENTS.md 19); task 019's mock *mode* is a different, built-on-top thing.
* **End to end is proven on the mock:** device callback → input gain → ring → submit →
  mock → translated audio → controller → jitter buffer → output gain → wire, with exact
  float values checked at the output. The mock's audio is deliberately sign-flipped and
  scaled so it can never be mistaken for loopback.
* **Typed text events (`TranslationTextEvent`) and `getCapabilities()` are deliberately
  absent** - they are tasks 013 and 011; the contract names the extension points instead
  of pre-inventing the types.

## The real OpenAI backend (task 009)

`src/Network/OpenAIRealtimeBackend` implements the task 007 contract against the OpenAI
realtime translation service, coding only to `docs/openai-realtime-protocol.md`. It is a
separate `lingoflow_network` target so the portable core stays free of the OS network
stack and of JSON parsing outside `Config` (the architecture audit now guards exactly
that: only `App` may include `Network`).

* **Transport: the Windows stack, not a new dependency.** `WinHttpTransport` uses
  `winhttp.dll` for TLS, the HTTP->WS upgrade and RFC 6455 framing; the browser is never
  involved. The seam is a 4-method `IWebSocketTransport`, so the whole protocol is tested
  offline against a scripted fake with production-mirroring cancel semantics.
* **Two threads, because the live service demanded it.** A pending synchronous
  `WinHttpWebSocketReceive` blocks indefinitely and is not released by receive timeouts
  (measured), so a dedicated **receiver** thread drains server events while a **sender**
  thread keeps the continuous 200 ms append cadence (with silence) that both translation
  quality and the service's keepalive need; closing the socket from the other thread is
  what bounds every wait, including the graceful `session.close` drain.
* **The 24 kHz wire contract lives here.** Input is float32 at the device rate and is
  converted to base64 PCM16 mono 24 kHz; delivered deltas are decoded, their optional
  `format`/`sample_rate`/`channels` fields validated as authoritative, and resampled to
  the requested output rate by `PcmResampler` (a halfband FIR cascade covering only
  24/48/96; 44.1k and friends are refused at configuration, so the session is refused
  rather than guessed at).
* **Faithful to the frozen protocol, honest about its limits.** Instructions are accepted,
  ignored and reported (the model has no custom prompting - AGENTS.md 19); `session.updated`
  is the usable trigger; in-session errors map to the five categories and leave the session
  open, only transport death is fatal; no `*.done` events exist, so transcript fragments
  arrive as partials and final text is left to task 013. The probe `lingoflow_openai_probe`
  performs the one live functional round-trip through this exact binary.

## Session recovery (task 010)

`src/Translation/ReconnectSupervisor` owns the "when" of reopening that the task 007
contract deliberately left to the application. It wraps any `ITranslationBackend` - to
the controller it is a backend, to the backend it is the sink - so recovery is provider
knowledge, not OpenAI knowledge, and cannot reach the audio device.

* **Retryable vs terminal comes from the product categories** (protocol doc section 9,
  decision recorded in section 10): `connection` and `protocol` get a fresh session with
  exponential backoff and are retried forever while the application runs; `rejectedRequest`
  (bad key, bad pair, billing), `audioFormat` and `internal` stop recovery and report
  `faulted` once - retrying cannot fix them, hammering hides defects. A service
  `Retry-After` hint crosses the seam as `TranslationError.retryAfterMs` and is honored
  as a floor.
* **The gap policy is drop-and-count**: audio submitted while reconnecting is refused at
  the seam (`gapRefusedFrames`) and never replayed late - the translation resumes aligned
  with the room, which is what a live audience needs (owner decision 2026-10-02).
* **Proactive reopen** fires before the measured one-hour provider ceiling (default
  55 minutes, four `translation.reconnect*` settings), so a long event never discovers
  expiry at runtime; metrics (attempts, recoveries, reopens, session duration) are
  exposed via `stats()` for SPEC "Diagnostics".
* **`closeSession()` is a full stop**: it joins the recovery thread's in-flight attempt
  and no callback reaches the application after it returns, which is what keeps the
  contract's shutdown guarantee intact.
* The 009 backend completed the hint chain for it: `WinHttpTransport` captures
  `Retry-After` and a capped refusal body, and the documented billing code now maps to
  the terminal category instead of a retryable `connection`.
* In production composition since task 012: the windowed application wraps the OpenAI
  backend in the supervisor at the composition root; `--smoke` stays on the Null backend
  so startup checks never open sockets.

## The mounted translation pipeline (task 012)

The windowed application now carries the whole chain the earlier tasks built:

```text
ASIO in -> engine input ring -> TranslationStreamer (worker thread) -> ReconnectSupervisor
        -> OpenAIRealtimeBackend -> ... -> controller sink -> output jitter -> ASIO out
```

* `src/App/TranslationStreamer` is the capture-side transport: it drains the engine's
  input ring and calls `submitAudio()` in bounded chunks on its own thread - the shape
  task 005's loopback proved, with the translation seam in place of the loopback worker.
  It is the executor of the gap policy on the input side: frames that arrive while the
  session is reconnecting are consumed and counted, never replayed late, so the
  translation stays aligned with the room. A backend that throws costs a worker and a
  critical log line, not the engine or the application.
* The streamer lives exactly as long as an open session: the controller creates it when
  `openSession()` succeeds and joins it before `closeSession()` returns - the same
  shutdown ordering the task 007 contract rules were built for.
* The composition root (`Main.cpp`) mounts `ReconnectSupervisor(OpenAIRealtimeBackend)`
  from the settings; the credential is read through the task 015 chain - Windows
  Credential Manager first, the environment variable as AGENTS.md 10's development
  fallback - and its value is never logged. Without a
  credential or a network the session refuses, is recorded, and the audio path keeps
  running (AGENTS.md 12). `--smoke` never mounts the real backend.
* The default `translation.jitterBufferMs` moved 120 -> 250: with the live wire facts
  (delta bursts to ~2x realtime, post-close drain ~4.7x - protocol doc section 15) the
  old pre-roll overflowed its own headroom on a long burst. This is arithmetic with a
  named source, not a measured latency: the rig sweep in `docs/rig-checklist.md` (its
  step 7) confirms or moves it.
* Real EN<->RU audio through a real SoundGrid path is the task's REQUIRED human
  checkpoint and stays open - `docs/rig-checklist.md` is the run sheet for it.

## Languages and the capability manifest (task 011)

`src/Translation/LanguageRegistry` is the only language list in the product (AGENTS.md 9):
`TranslationCapabilities` says what a backend can do, `LanguageDefinition` carries a code
and a human name, `LanguageRegistry` answers the one question the product asks —
`checkPair(input, output)`. The OpenAI set is the versioned frozen manifest
(`openAiManifest()`, provenance fields inside): the 13 target codes live-verified against
the real service plus the 70+ auto-detected sources enumerated by the translation docs;
the key has no `api.model.read` scope, so this frozen manifest is the **primary** capability
source, not a fallback (`docs/openai-realtime-protocol.md` sections 13/15). Behaviour, all
covered by tests:

* the pair gate lives where sessions are opened — `ApplicationController::startSession`
  and `OpenAIRealtimeBackend::openSession` refuse an unsupported pair before any transport
  exists, with an operator-readable sentence, and the refusal is offline-testable;
* Config validates only the *shape* of a stored language tag (module boundary: a config
  file must not know backend capabilities); supportability is the registry's answer;
* English <-> Russian, the MVP pair, is pinned in both directions by a test;
* `OpenAIRealtimeOptions::capabilities` is the seam where a dynamically fetched manifest
  would be mounted if the key ever gains model-read scope - no wiring change needed.

## The typed text pipeline (task 013)

`src/Translation/TextPipeline` is what the UI and the subtitles read - no provider event
ever reaches them (SPEC "Text"):

* the sink's contract strings become `TranslationTextEvent`s: kind (`partial`/`final`),
  text, a pipeline-assigned monotonic sequence and a steady-clock arrival time. The
  provider's `elapsed_ms` keys nothing - the protocol doc (section 8) says it is
  alignment metadata that may repeat, so it never crosses the seam as identity;
* `partial` means the whole line as it currently reads (a replacement, not a fragment).
  Assembling a provider's append-only fragments into that snapshot is backend work: the
  OpenAI backend concatenates verbatim under its own lock and is the only file that knows
  fragments exist;
* the wire has no line-end event (verified live: the transcript crossed two sentences
  without any reset), so line boundaries are documented PRODUCT policy - the
  `transcriptSettleMs` pause (default 2500 ms) closes a line from the sender's stream
  clock, and `closeSession()` flushes whatever line is open before the `closed` transition.
  Whether the settle threshold fits live speech is a rig observation (checklist step 9),
  not a number any test depends on;
* history is a fixed-capacity ring of final lines with counted eviction, and an
  overlong open draft is capped at 64 KiB (counted) - the task's FAIL criterion "grows
  unbounded" is structurally unreachable. A session that dies mid-sentence closes the
  line instead of losing it: the words that reached the wire go to history (and NDI -
  the stop order keeps subtitles up until the session's last words are out);
* text runs on backend worker threads only. It never touches the audio callback: the
  audio path and this pipeline share no lock, and the realtime audit stays green.
  Delivery to the listener rides the pipeline's own dispatch worker (code review P2,
  2026-10-05): ingest queues typed events under the lock and returns; one worker fires
  the listener without the lock, in FIFO order, on a bounded queue that drops the
  oldest pending event under sustained listener stall - counted in
  `droppedEvents`, exported as `text_events_dispatch_dropped`. A wedged listener
  stalls subtitles, never the receiver thread, and the listener may reenter the
  pipeline without deadlock;
* task 014's UI reads exactly one type here: `TextPipeline::snapshot()` - the open line,
  the bounded history and the counters that say what was ignored and why.

## The operator UI (task 014, information architecture by UI-01)

The window is thin by construction - three parts, each in the place that owns it:

* `src/App/UiModel.{h,cpp}` (portable core): `buildOperatorPanel(controller, note)` turns
  the controller's public reads into everything the operator screen shows, and
  `buildDiagnosticsPanel(controller)` builds the engineering surface from the same reads -
  two destinations, one source of truth, so the screens cannot tell two stories. The
  operator panel carries the live-event hierarchy (UI-01): system status, routing with
  **discrete channel choices** (`channelOptions`: the driver's own names once a device is
  open, honest "Channel N" numbering before that, never a fader for an index), paired
  input/output meters and gains, the language pair with its live text, and a four-fact
  health strip whose latency row says "estimated" out loud. The raw counter wall, the
  full 018 accounting and the text-pipeline summary moved to the diagnostics panel -
  relocated, never deleted. Tested headless against the real controller, including the
  refused device, refused pair, clamped gain and the "configured device not in the scan
  stays visible, labelled" honesty rule (its channel-list twin included).
* `src/App/OperatorWindow.{h,cpp}`, `src/App/SettingsWindow.{h,cpp}` and
  `src/App/DiagnosticsWindow.{h,cpp}` (JUCE app target; shared widget helpers in
  `src/App/UiWidgets.h`): widgets, layout and repaint timers over those panels. The
  operator screen is sectioned (SYSTEM STATUS / AUDIO / TRANSLATION / LIVE HEALTH) with
  Diagnostics behind its own button - which also owns the Export-diagnostics receipt -
  and the settings dialog is grouped into Audio, Credentials, Translation, Subtitles/NDI,
  Diagnostics and an Advanced band last: developer/test mode and the unsupported
  instructions field live there, away from everyday setup. It owns no logic: every
  control either calls one controller method (`updateSettings`, `setGainsLive`,
  `clearFault`, `refreshDevices`, `start`, `stop`...) or paints a panel value.
  Programmatic refresh is guarded so a tick can never echo back as an operator action,
  sliders commit at the end of a drag (no disk writes mid-glide), and ComboBoxes are
  rebuilt only when the list actually changed.
* controller seams added for the UI, all of them orchestrations of existing subsystems:
  the device-lister seam (the same pattern as the backend factory - platform knowledge
  arrives as a function, the core caches only its result), `clearFault` (retry is the
  operator's decision, the log keeps the original failure), live knob forwards, and
  `updateSettings` as the single funnel: validate → apply what applies live → persist,
  with the note naming what needs a Stop/Start.

The PASS criterion "UI does not do network/audio work" holds by construction: the window
contains no decisions - every value it paints and every branch it shows is `UiModel`
output, asserted headless against the real controller, and each control is one controller
method. "Freezes" is guarded by the same rule the meters were designed for: reads are
atomics and snapshots, and the poll cannot block on anything.

## Credentials and the Settings dialog (task 015)

The key's whole life story, in one paragraph: it is typed into a masked field, handed
straight from the UI to `security::WindowsCredentialStore` (one call:
`ApplicationController::storeApiSecret`), persisted by the OS encrypted at rest as the
generic credential `LingoFlow/openai_api_key`, read only by the translation backend at
session start, and named nowhere else - not in `config.json` (the struct has no such
field and the parser drops secret-shaped ones), not in the log (only operations and OS
error codes are written; notes quote the store's *name*, never a value), not on screen
(a successful store clears the field; the presence line reads `identifiers()`, names
only).

Storage choices worth saying out loud: the Windows Credential Manager is what AGENTS.md
10's "Windows secure storage" means concretely - per-user, OS-encrypted, restart-proof,
and inspectable by the operator in the Control Panel. `ChainedSecretStore` layers it
over the development environment variable: reads try the store first (a key entered in
Settings wins over any environment value), writes go only to the store (the application
never rewrites the developer's environment), removal never touches the fallback, and
nothing is faked - a refused or empty store answers refusal, and the Settings dialog's
presence line says so. Offline evidence for "survives restart" is the child-process
test: one process writes a marker credential, a second process reads it back - the
store is the OS's, not the process's memory. The actual reboot on the venue machine is
the task's REQUIRED human checkpoint.

The Settings dialog itself keeps task 014's rule: widgets only, every commit through
`updateSettings` as one atomic draft. The schema's own exported ranges back the controls
(the same single-source rule as every other selector), and the note names honestly what
applies now (gains, jitter, log level) versus what waits for Stop + Start (device,
languages, NDI) versus what waits for a full application restart (recovery policy is
mounted by the composition root at startup, and the log-file sink is chosen there too).

## The real NDI subtitle output (task 016)

Mode A of SPEC 38: caption snapshots travel as NDI metadata frames carrying TTML1
documents - every format/API fact is dated and cited in `docs/ndi-protocol.md`, nothing
is quoted from memory. The pieces:

* `NDI/NdiTimedText.{h,cpp}` (portable, tested): one complete snapshot document per
  publish - one root, no XML prolog, the W3C TTML1 namespace, the pipeline's own sequence
  as `xml:id`, total escaping for text content and removal of characters XML 1.0 forbids.
  A document a receiver cannot parse is the failure mode this file exists to prevent.
* `NDI/Real/` (a separate target, compiled against the SDK's headers): `NdiRuntime` loads
  `Processing.NDI.Lib.x64.dll` at run time through the SDK's own dynamic-load entry point
  - the license rule of docs/licensing.md made concrete: the import library is never
  linked. `NdiTimedTextOutput` implements `INdiOutput` with an honest state machine:
  `ready` while nothing consumes, `publishing` when `send_get_no_connections` says a
  receiver is there, and `stop()` answering "not started" to any early publish. The SDK's
  `send_send_metadata` returns void - so this product never claims "displayed", only
  "handed to the SDK"; delivery truth belongs to the venue receiver, which is precisely
  why the task carries a REQUIRED real-receiver checkpoint.
* `lingoflow_ndi_probe.exe`: `list` / `recv` / `send` / `selfcheck` - the same loader and
  the same document builder as the app, so a ten-second run on the venue network either
  receives the captions or says which step failed. (On the development laptop the send
  side runs green while discovery legitimately sees zero sources through the VPN stack -
  measured and recorded, not hidden, in docs/ndi-protocol.md.)
* Composition root: the real output is mounted only for the windowed run; `--smoke` keeps
  Null NDI (a startup check must not touch the LAN discovery stack either), and with no
  NDI runtime installed `start()` says so in one sentence while audio and translation
  keep running (AGENTS.md 12, the task's FAIL criterion kept out by construction).

## Structured diagnostics and the export (task 017)

Two layers, each with one job:

* **Counters** stay the callback's voice: the audio thread only increments relaxed
  atomics (the realtime gates prove the callback allocates and locks nothing, and
  the new `noteEvent`/ring calls are nowhere in `src/Audio` - narration comes from
  worker/UI threads). Levels, buffers, underruns/overruns, capture and delivery
  throughput, text and NDI counts - all already tracked by 012-016, now also
  **narrated**: `DiagnosticsManager` keeps a bounded event ring (256 entries,
  sequence + the log's own timestamp format via `log::timestampNow()`, oldest-first
  readout). `noteError` records into the ring at its existing call sites, so errors
  and narration cannot drift apart; overflow evicts the oldest and the eviction
  count is exported - a window is labelled as a window, never sold as history.
  Engine counters that previously lived only in the UI grid (ring drops, jitter
  fill, clip splits, malformed/oversized callbacks, non-finite input, gain
  clamps/rejects) now ship in the export too.
* **The export** (`Diagnostics/DiagnosticsExport` + `ApplicationController::exportDiagnostics`):
  key=value text sections (`[app] [audio] [translation] [ndi] [security] [settings]`)
  plus the event ring, written atomically (unique tmp + rename, parents created,
  failures said with reasons - no silent second path), with collision-suffixed
  filenames so two excited clicks produce two receipts. The renderer takes Config's
  *own* "looks like a secret" predicate as an injected function pointer - Diagnostics
  may not include Config (boundary audit), so the rule is passed in, never copied,
  and no second list of secret words exists. Any secret-shaped key ships
  `[redacted]` and the count rides in the operator's note. The security section
  carries the store's name and `key_present=yes/no`; the values never leave the
  store. (A test caught this design being *too* good at first: the obvious key name
  `api_key_present` is itself secret-shaped and got redacted - so presence fields
  are named `key_present`, a choice recorded here because a design lesson learned
  from one's own guard is worth keeping visible.)
* **Latency**: the buffer-delay arithmetic moved to the engine (`bufferBlockMs`,
  `pipelineBufferDelayMs`) with `estimateBufferDelay` falling back to settings when
  nothing runs - the UI line and the export read out of ONE arithmetic, and both
  label their source. Still explicitly not a measurement: mouth-to-ear is task 018's
  job, and the disclaimer travels inside the file itself.
* **UI**: one "Export diagnostics" button on the operator screen - one controller
  call, and the receipt note carries the written path. Default location:
  `%APPDATA%\LingoFlow\diagnostics\`, next to the settings, so an operator takes one
  folder home.

The PASS trio - metrics work (counters asserted through the e2e suites and in the
export contents), export contains no secrets (canary credential: absent from the
file, from the note, and impossible by structure; redaction pass separately tested
including its honest counting), callback unaffected (the allocation gates plus the
fact that no `src/Audio` path can even reach the ring) - is closed entirely
headless; the task declares no human checkpoint.

## Developer & mock mode (task 019)

The pack asks that the core be operable and testable without hardware or a provider
account, and that mock behaviour never leak into production. What exists for it:

* `LingoFlow.exe --dev` mounts a test tone as the "device" and an **echo translator**
  in place of the OpenAI chain: no ASIO device is opened, no key is read, no socket is
  created - while the windowed app runs its full pipeline (capture rings, streaming
  worker, sink checks, jitter pre-roll, meters, counters, the 018 accounting). Combined
  with `--smoke` it is the offline startup proof; the settings file is never rewritten
  by the flag, and `--dev` never enables loopback - routing a room's microphones to its
  own speakers stays a settings-level, two-intentional-clicks decision.
* The settings gain a `developer` section (round-tripped, repaired per field, absent =
  off): audio source `device|wav|tone`, WAV input and rehearsal-recording paths, mock
  echo delay, loopback. `App/DeveloperMode.h`'s `developerPlan()` is the ONE
  interpretation: the composition root mounts from it, the controller gates loopback
  and streaming through it, the badge and the export render it - and a default config
  plans nothing at all (asserted, not trusted).
* `Audio/Dev/SimulatedDeviceBackend` is a real `IAudioBackend`: its worker paces blocks
  at the configured block time and counts its own lateness honestly (a rehearsal drift
  is reported as simulator pacing, never claimed as audio timing). The WAV backend
  refuses a file whose rate disagrees with the settings instead of resampling silently.
* `Translation/Mock/MockTranslationBackend` honours the task 007 contract rules 1-6 -
  the same lifecycle tests the supervisor runs against the real seam apply - and every
  text event it emits begins with "mock" and says "no model ran", because a mock that
  could be mistaken for a translation is the one thing this product must never ship.
* Isolation visible everywhere: the operator screen carries a red DEVELOPER MODE band
  (loopback suffix reports the worker's real state, not the wish), the log opens with
  `DEVELOPER MODE: <badge>`, and the diagnostics export ships a `[developer]` section
  so any venue report can answer "was that a mock run?" from the file alone.
* The 018 in-flight backlog row gains a ground truth here: the mock's configured delay
  is known, so the "audio in the translator path" arithmetic can be verified offline
  against a deliberate latency.

## Honest latency accounting (task 018)

The deliverable is an accounting, not a number: **no measured latency value exists in
code** (AGENTS.md 19), and the ones that will exist after a venue visit live only in
`docs/latency-budget.md`, dated and attributed. What 018 built:

* `DeviceCapabilities` now carries the driver's own reported input/output latency in
  samples (`getInputLatencyInSamples`/`...Output...` via `JuceAsioBackend`), and the
  accounting names ASIO's honest ambiguity: a zero cannot be distinguished from "not
  reported", so it is rendered **not reported** and contributes a visible 0.0 - silence
  is never sold as speed.
* `latencyAccounting(engine, backend, diag, settings)` in `App/UiModel` is the single
  function rendering the component list - driver rows, capture block, **the translator
  path as ONE combined live-computed row** ("audio in flight" = frames submitted minus
  frames returned in any form, clamped at 0): the product has no provider-side
  timestamp, so network and model are not separated and the row says so in its own
  kind string; jitter fill and pre-roll target; playback block; and an "estimated
  total (labeled rows)" whose kind field spells out what it excludes.
* Every row = component + value + **kind** ("driver-reported", "arithmetic
  (live geometry)" / "(settings, not running)", "configuration", "live-computed",
  "not measured"). The operator screen shows the whole table under the headline;
  the diagnostics export ships the same rows plus a `limitations` line - the file
  cannot disagree with the screen because both call the one function (tested).
* Mouth-to-ear, per-sentence timing, session-open latency: the venue table in
  `docs/latency-budget.md` - blanks, with the rule that numbers enter that file and
  never the binary.

## Layout

```text
CMakeLists.txt          root project, JUCE discovery
src/CMakeLists.txt      lingoflow_core + LingoFlow targets
src/App/                ApplicationController (composition root), TranslationStreamer (capture worker), UiModel (headless-tested operator model), OperatorWindow + SettingsWindow (JUCE shells), JUCE entry point
src/Audio/              AudioEngine + pipeline (ring/jitter/gain/meters/loopback), IAudioBackend (+ DeviceRequest), ASIO model/policy, Null/ device
src/Translation/        ITranslationBackend contract (states, request, errors, sink), ReconnectSupervisor (010 recovery), LanguageRegistry (011 single language list + frozen OpenAI capability manifest), TextPipeline (013 typed events + bounded history), Null/ backend
src/Network/            OpenAI realtime backend (contract impl) + WinHTTP WebSocket transport + PCM resampler + base64, and the live lingoflow_openai_probe tool
src/NDI/                INdiOutput contract, NdiTimedText (TTML1, portable), Null/ output, Real/ (runtime-loaded sender + probe, never links the SDK import lib)
src/Platform/Asio/      JUCE ASIO discovery, JuceAsioBackend, lingoflow_asio_probe tool
src/Config/             AppConfig, ConfigSchema (validation + JSON text), ConfigStore (atomic file), ConfigManager
src/Security/           ISecretStore boundary + NullSecretStore + WindowsCredentialStore (015 production storage) + ChainedSecretStore (store-first, environment-fallback)
src/Diagnostics/        DiagnosticsManager (callback-safe counters + bounded event ring, 017), DiagnosticsExport (sections renderer + atomic writer, secret-shape predicate injected)
src/Utils/              logging skeleton
tests/                  Catch2 unit + contract tests, integration (mock end-to-end), tests/support/ deterministic mock backend + scripted WebSocket fake, realtime allocation suite, architecture audit, realtime safety audit, self-tests
docs/                   licensing, device defaults, architecture, environment report, verified OpenAI realtime protocol reference
third_party/            vendored JUCE 9.0.3 and ASIO SDK 2.3.4 (see third_party/README.md)
tasks/                  agent task files
```

Real-time rules, thread model and architectural boundaries are defined in
`AGENTS.md`, `docs/architecture.md` and `docs/threading.md`.

## Licensing

Binding decisions (`docs/licensing.md`): JUCE 9 under **AGPLv3**, ASIO SDK 2.3.4
under **GPLv3**. Consequences to respect in every later task:

* the product is AGPLv3: source offer, `LICENSE` + third-party notices, visible
  licence notice in the UI, no obfuscation - required before the release build;
* **NDI must not be link-time**: `Processing.NDI.Lib.*.lib` stays out of the link,
  NDI is reached by runtime loading behind `INdiOutput` (task 016);
* no ASIO logo/compatibility branding without Steinberg's separate trademark
  agreement;
* Waves SoundGrid driver and NDI runtime are prerequisites of the host system, not
  redistributed source.

`third_party/README.md` lists what is vendored, what is deliberately not, and the
rules for touching those trees.

### Architecture boundary audit

Two CTest entries with label `architecture` guard the dependency direction
(`docs/architecture.md`):

* `architecture_boundary_audit` - scans every `#include` under `src/` and rejects
  a module including a module it must not know, JUCE outside `src/App`/`src/Platform`,
  and any protocol/transport header (`openai`, `websocket`, `asio`, `curl`, `json` -
  with one registered exception: `nlohmann/json.hpp` inside `Config`).
* `architecture_boundary_audit_selftest` - copies `src/` and applies a dozen
  modifications: violations that must be rejected (each with its own `AUDIT_*` code)
  and documented exceptions that must stay accepted (JUCE in `Platform`,
  `nlohmann/json.hpp` in `Config`). A gate that stops working therefore fails CI
  instead of passing silently.

Run them alone: `ctest --test-dir D:\LingoFlow\build-debug -L architecture --output-on-failure`.

### ASIO discovery tool (task 004)

`lingoflow_asio_probe.exe` is built next to the app; it exercises the real JUCE ASIO path.
Two CTest entries with the label `device` use the safe part of it automatically.

| Command | What it does | Touches the driver? |
|---|---|---|
| `--list` | prints the enumerated ASIO devices | no (registry scan) |
| `--verify` | checks that enumeration matches `HKLM\SOFTWARE\ASIO` one-to-one | no |
| `--probe "<device>" [--start]` | opens the device, reports channels/rates/buffers/latencies, optionally one start/stop | yes |
| `--lifecycle "<device>" [--cycles N]` | N open/start/stop/close cycles through `AudioEngine` + `JuceAsioBackend`, asserting callbacks arrive and the backend ends `closed` | yes |
| `--loopback "<device>" [--seconds N] [--input M] [--output K] [--jitter MS] [--gain-in DB] [--gain-out DB]` | runs the device through the real pipeline (gain, ring, loopback, jitter, gain, meters) and prints measured input/output levels once per second | yes |

`--loopback` is how the task 005 and 006 hardware checks are done on a rig: patch a source
into `--input`, and the same audio must leave on `--output` roughly `--jitter` ms later, with
`in=` and `out=` levels moving together. `--gain-out -6` should then be visibly 6 dB quieter
on the `out=` line and `--gain-in` should move `in=` (which reads the post-gain signal),
while `appliedGain=` shows the level the callback is really using during a glide. If both
levels stay at digital silence the tool reports `LOOPBACK INCONCLUSIVE` and exits 1 - the
pipeline ran, but there was no signal to loop, and it says so instead of calling that a
pass. Measured on this PC (2026-10-02): the Waves driver refused with `input channels are
not available: the driver reported none at all`, because `SoundGrid QRec` and the
`SoundGrid Driver Control Panel` were running and an ASIO device is exclusive;
`docs/device-defaults.md` records both observations.

Exit codes: 0 ok, 1 verification failed / no callbacks / inconclusive silence, 2 the device
refused to open, 3 usage error. `--verify` and `--list` are the automated part
(CTest label `device`); `--probe`, `--lifecycle` and `--loopback` against a live SoundGrid
system stay a human check. What these commands measured on this machine:
`docs/device-defaults.md`.

### Realtime safety gates

Two CTest entries with the label `realtime` guard AGENTS.md 5 directly
(`docs/architecture.md` explains the design):

* `realtime_safety_audit` - extracts the body of every function reachable from the audio
  callback (12 of them: the engine, both buffers, the meter, both gain entry points and the
  JUCE bridge) and rejects allocations, locks, sleeping, filesystem, transport, UI and
  exception constructs in it;
* `realtime_safety_audit_selftest` - runs twelve checks: the clean tree must be accepted,
  ten injected violations (one per forbidden class, two of them inside the gain stage) must
  each be rejected, and a deleted realtime function must fail the run instead of shrinking
  the gate;
* `lingoflow_realtime_tests` (a separate binary that replaces global `operator new`)
  measures that the callback performs zero heap allocations over 5000 blocks, and that
  changing the gain while the callback runs costs zero too.

Run them alone: `ctest --test-dir D:\LingoFlow\build-debug -L realtime --output-on-failure`.

## Secrets

Never put an API key in the repository, in `config.json` or in logs.
Development: `OPENAI_API_KEY` environment variable. Production: Windows secure
credential storage. See `AGENTS.md` section 10.
