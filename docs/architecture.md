# Architecture

```text
GUI
  |
  v
ApplicationController
  |
  +-- AudioEngine -- IAudioBackend -- JuceAsioAudioBackend
  |       +-- Input Ring Buffer
  |       +-- Output/Jitter Buffer
  |       +-- DSP
  |
  +-- ITranslationBackend -- OpenAIRealtimeTranslateBackend
  |       +-- Audio -> AudioEngine
  |       +-- Text -> TextPipeline
  |
  +-- INdiOutput -- NdiSubtitleOutput
  +-- ConfigManager
  +-- DiagnosticsManager
  +-- CredentialStore
```

Audio callback is isolated from network/UI/file operations.

## Module layout and dependency direction (task 002)

Source directories under `src/` are the modules. An arrow means "may include";
anything not listed is forbidden and is rejected by
`tests/ArchitectureBoundaries.cmake`, which runs as two CTest entries:

```text
Utils          -> (nothing)                       logging, no JUCE
Config         -> Utils                           settings: schema, validation, atomic file I/O
Security       -> Utils                           credential store boundary, no config access
Diagnostics    -> Utils                           atomic counters + snapshot
Audio          -> Utils, Diagnostics              engine, ring/jitter buffers, gain stages,
                                                  meters, loopback, IAudioBackend,
                                                  ASIO model/policy, Null backend
Translation    -> Utils, Diagnostics              ITranslationBackend contract, Null backend
NDI            -> Utils, Diagnostics              INdiOutput contract, Null output
Platform       -> Audio, Utils, Diagnostics       JUCE adapters (ASIO device discovery/lifecycle)
App            -> all of the above                ApplicationController (composition root)
App/Main.cpp   -> App, Platform, Utils, JUCE      UI + installation of the device backend factory
```

The composition root is the only place that turns a device name from `config.json` into a
backend: `ApplicationController` takes an `AudioBackendFactory` callback, and `Main.cpp`
installs one that builds `platform::JuceAsioBackend`. That keeps the portable core free of
JUCE while the product still opens the operator's chosen ASIO device, and it makes the
"device selected but no backend available" case an explicit error instead of a silent fall
back to the null device.

`Platform` is the second and last place allowed to include JUCE. It exists so that
device adapters can use JUCE without leaking it into the portable core: `Audio`
declares `IAudioBackend`, `Platform` implements it. Nothing in `Audio`, `Translation`,
`NDI`, `Config`, `Diagnostics` or `Utils` may know JUCE, and the direction
`Audio -> Platform` is forbidden (the engine depends on the interface, never on the
adapter).

One more dependency exception is registered in the audit: `nlohmann/json.hpp` may be
included by `Config` only, and only in a `.cpp` - no header in the project exposes a
JSON type, so the UI and the audio path cannot start parsing configuration or wire text.

Rules that the audit enforces:

1. **No upward dependencies.** `Audio`, `Translation` and `NDI` never include each
   other and never include `App`. The controller wires them together; the engine
   knows only `IAudioBackend`, the backend knows only its own sink interface.
2. **JUCE only in `src/App` and `src/Platform`.** `Audio` may not include `Platform`:
   the engine depends on the interface, the adapter depends on the engine's interface.
   The core stays portable (SPEC: future macOS/Linux must not require rewriting the
   translation core).
3. **Protocol/transport headers only inside their owning module** (`openai`,
   `websocket`, `json`, `asio`, `curl`). Today that means one exception:
   `nlohmann/json.hpp` in `Config`. Task 009 will register the OpenAI backend the same
   way; the UI and the audio engine still will not see protocol code. ASIO is reached
   through JUCE, which carries its own interface headers, so no ASIO SDK header is
   included from `src/`.
4. **The audit itself is tested.** `ArchitectureBoundariesSelfTest.cmake` applies a
   dozen modifications to a copy of `src/`: violations that must be rejected (with the
   expected `AUDIT_*` code) and documented exceptions that must stay accepted, so a
   gate that stopped working fails CI instead of passing silently.

## Realtime boundary

`IAudioProcessor::processAudio()` is the only function on this list declared
`noexcept` and documented as realtime: it runs on the device thread and may not
allocate, lock, log, touch the network, disk or UI. Everything else in the
architecture is explicitly non-realtime:

| Call | Thread | Realtime-safe contract |
|---|---|---|
| `IAudioProcessor::processAudio` | audio callback | `noexcept`, fixed work, relaxed atomics only |
| `AudioEngine::processAudio` | audio callback | meters, `memcpy` into the input ring, `memcpy` out of the jitter buffer, relaxed atomic counters; never allocates or blocks |
| `AudioRingBuffer::write / read / readOrSilence` | either side | lock-free SPSC: two atomic positions, `memcpy`, relaxed drop/underrun counters |
| `AudioJitterBuffer::write / readOrSilence / setTargetFrames` | either side | same primitives plus an atomic pre-roll flag; overflow drops incoming frames, underflow plays silence |
| `LevelMeter::measure` | audio callback | one pass over the block, one `sqrt`, atomic publish; non-finite samples counted as out-of-range |
| `GainStage::process` | audio callback | per sample: one finite check, one multiply, two magnitude compares; the coefficient is a plain float advanced toward an atomic target |
| `GainStage::dbToLinear` | audio callback (once per block) | `exp2` against a documented constant, no libm path that can allocate |
| `Platform::JuceAsioBackend::Callback::audioDeviceIOCallbackWithContext` | audio callback | forwards driver pointers into `processAudio`, views preallocated in `aboutToStart`, `audioDeviceError` sets a flag only |
| `AudioLoopback::run` | worker | consumer/producer of the lock-free buffers; may sleep between polls; never called by the audio thread |
| `IAudioProcessor::onAudioConfigurationChanged` | non-realtime | after `open()`, before first block |
| `AudioEngine::activate / configure / deactivate` | control thread | the only place pipeline memory is allocated or released |
| `AudioEngine::setInputGainDb / setOutputGainDb / setInputMuted / setOutputMuted / setGainRampMs` | any thread, UI in particular | relaxed atomic publish; the callback picks the new target up at the start of its next block. No lock, no allocation, no wait |
| `ITranslationSink::on*` (audio, text, state, error) | network/worker | enqueue only, never play directly; delivered audio goes to the engine's jitter buffer, wrong-rate blocks are rejected and counted, never resampled in silence |
| `INdiOutput::publish` | worker | drop on pressure, never block the producer |
| `DiagnosticsManager::count*` | any, incl. audio | relaxed atomics |
| `DiagnosticsManager::snapshot`, `note*` with strings | UI/worker | mutex-guarded, never from the audio thread |
| `IAudioBackend::open/start/stop/close`, `AsioDiscovery::*` | control thread | may allocate and log; never called from the callback |

## Realtime pipeline (tasks 005 and 006)

The pipeline is the one from SPEC "Audio Pipeline", with the two translation stages
still to be filled in:

```text
ASIO input -> AudioEngine::processAudio
                  -> GainStage (input)       -6 dB .. +24 dB, mute, click-free glide
                  -> LevelMeter (input)      reads the post-gain block
                  -> AudioRingBuffer        (producer = audio thread)
                        |
                  [loopback worker now (AudioLoopback); OpenAI streaming worker in task 012]
                        |
                  AudioJitterBuffer         (consumer = audio thread)
                  -> GainStage (output)      separate trim, separate mute
                  -> LevelMeter (output)
                  -> ASIO output
```

Design decisions, and why:

* **`AudioRingBuffer`** is single-producer/single-consumer with monotonic atomic
  positions and a power-of-two capacity (mask instead of modulo). The audio thread is
  always the producer on the input side and the consumer on the output side. Both
  directions degrade the same way: overflow drops the newest frames and counts them,
  underflow yields silence and counts the shortfall. Nothing in either path waits for
  the other peer, which is what makes "the network must never be inside the callback"
  a structural property rather than a convention.
* **`AudioJitterBuffer`** is the ring plus an operator-configurable pre-roll
  (`translation.jitterBufferMs`, SPEC range 20-500 ms, clamped to 500 by the engine).
  Playback does not start until the pre-roll is buffered; a mid-stream drain counts one
  underrun event and re-primes instead of streaming from an almost empty buffer, which
  is what turns a single network hiccup into a burst of audible glitches.
* **The output has exactly one source.** Only `write()` on the jitter buffer can put
  audio on the wire, and that call belongs to the transport side (loopback today, task
  012 later). Microphone audio therefore cannot reach the audience by a missing
  underrun branch - the silence path is the default, not an extra case.
* **`LevelMeter`** publishes peak and RMS of the last block plus cumulative clipping
  and signalled-block counters, and it reads the *post-gain* signal on both sides: a knob
  that does not move the meter looks broken to an operator. What arrived at full scale
  before the application's own trim is counted separately, by the gain stage, so turning
  the level down cannot hide a damaged console feed.
* **Counters are engine-level, not buffer-level.** `deactivate()` releases the buffers,
  so dropped/underrun/overrun totals live in `AudioEngine`'s own atomics and stay
  readable after shutdown - that is where the operator's diagnostics (task 017) and the
  long-run test (task 024) will read them.
* **Loopback is never implicit.** `AudioEngine` outputs silence until something writes
  to the jitter buffer, and the loopback worker has to be started on purpose (the probe
  tool today; developer mode in task 019). Routing live microphone audio to the audience
  is an operator action.

### Gain, mute and clipping (task 006)

`GainStage` is one class used in both positions of the SPEC pipeline, because both
positions need the same three things: a dB value the operator sets, a mute, and the
guarantee that changing either makes no click.

* **The glide is the point.** The requested coefficient is published as a relaxed atomic
  and the callback walks its own coefficient toward it over `kDefaultRampMs` (20 ms, = 960
  samples at 48 kHz), landing exactly on the target rather than drifting near it. A test
  measures the consequence: cutting a 500 Hz sine at its peak and dropping the level by
  12 dB produces a 0.37 sample step without the glide and nothing above the wave's own
  slew (~0.033) with it. That is what PASS criterion "no clicks" means in numbers.
* **`configure()` lands instead of gliding.** At `activate()` the stage is set directly to
  the level the settings already show: an operator who set -6 dB must not get 20 ms of
  audio at 0 dB at the start of every run.
* **Gain values are validated twice, in different ways.** `ConfigSchema` refuses a
  hand-edited file outside its window, while `GainStage` clamps anything handed to it at
  run time and counts it (`gainRequestsClamped()`), and refuses non-finite values,
  keeping the previous level instead of inventing one (`gainRequestsRejected()`). A
  refused request is never silent.
* **Three clipping counts, because they are three different questions.**
  `inputClippedFrames()` (the device fed us full scale), `inputGainClippedFrames()` (our
  own trim created it on the way to the translator) and `outputGainClippedFrames()` (what
  is heading at the audience). A latching `takeInputClipIndicator()` /
  `takeOutputClipIndicator()` exists for the UI lamp, so the interface does not have to
  diff counters between frames.
* **This stage does not limit.** SPEC puts a limiter under "Future architecture", so a
  sample above full scale is reported and passed through untouched. There is one
  exception, and it is a validity guard rather than dynamics: a product that overflows
  to infinity becomes full scale, because no converter can reproduce infinity, and every
  such sample is counted. A test pins the rule so a future limiter cannot arrive quietly.
* **Non-finite samples become silence, counted.** NaN from a driver is not audio; one
  poisoned sample silences one sample (`nonFiniteInputFrames()`) instead of propagating
  into the translation stream or the room.
* **Input mute and output mute are different controls.** Muting the input stops feeding
  the translator; muting the output stops the audience hearing anything. One shared
  "mute" would be the wrong control on a live show, so the engine keeps them apart and a
  test proves the difference in both directions.
* **Gain settings and their history belong to the operator, not the device.** The stages
  are not released by `deactivate()`: levels, mute state and the clipping counts survive a
  device restart, and a re-`activate()` re-applies them. A room does not lose its levels
  because ASIO was re-opened.
* **Oversized blocks are chunked, not overrun.** The input side needs scratch because the
  device hands the callback a const pointer, so `kGainChunkFrames` (4096) is preallocated
  per channel; a block larger than that is processed in chunks, still gained in full, and
  counted in `oversizedCallbacks()`. Task 005 learned this the hard way, when a test's
  badly sized buffer wrote past an array.

### How the realtime rule is enforced

Two independent gates, both in CTest (label `realtime`):

1. `tests/RealtimeSafetyAudit.cmake` extracts the body of every function reachable from
   the audio callback - today twelve: the engine, both buffers, the meter, `GainStage::process`
   and `GainStage::dbToLinear`, and the JUCE bridge - and rejects a list of forbidden
   constructs: allocation (`new`, `make_unique`, `malloc`, container growth), locks and
   waits, sleeping, filesystem/registry, transport (`json`, `websocket`, sockets), UI,
   exceptions. `...SelfTest.cmake` runs twelve checks: the clean tree must be accepted, ten
   injected violations (one per forbidden class, two of them inside the gain stage, because a
   newly audited function is only really guarded once something proves the gate reads its
   body) must each be rejected, and a deleted realtime function must fail the gate instead of
   shrinking it silently.
2. `tests/realtime/TestRealtimeAllocations.cpp` replaces global `operator new` in its
   own test binary and counts heap allocations during 5000 callbacks with the loopback
   worker running: the count must be 0. A third case there does the same while the gain and
   mute are being changed every block, because the SPEC allows gain state to be set from
   another thread only if doing so cannot allocate. It is a separate executable so the
   substitution cannot influence any other suite.

Both are behavioural *and* textual: the audit cannot see a violation hidden behind a
macro, the allocation counter cannot see a lock. Together with the stress test in
`tests/unit/TestAudioPipeline.cpp` (5000 blocks with a live consumer thread, frame
accounting required to close) they cover the three failure modes that matter: allocating,
blocking, and losing audio without counting it.

`AudioEngine::processAudio` outputs silence while nothing feeds the jitter buffer, and
that is still deliberate: the only sources are loopback (probe today, developer mode in
task 019) and translated audio delivered through the task 007 contract. The probe tool
adds `--loopback`, which turns the same pipeline into a measurable end-to-end path on
a real device.

## Translation contract (task 007)

`Translation/ITranslationBackend.h` is the whole seam between the product and any
translation provider. It is a contract, not a skeleton: six lifecycle rules are stated
in the header and enforced by the reference implementation the tests run against, so
task 009 is written against behaviour that already has proof, not against comments.

* **The shape** (SPEC "Translation Provider Interface"): `openSession(SessionRequest)` /
  `submitAudio(float mono)` / `closeSession()` on the control side; five sink callbacks
  on the delivery side - translated audio, partial text, final text, session state and
  errors. Audio and text are separate methods on purpose (SPEC: "The audio callback and
  text callback must be independent") and the contract tests prove the sink sees one
  without the other.
* **`SessionRequest` carries rates and an opaque model string.** The backend cannot
  translate what it does not know the rate of, and the application cannot play what it
  did not ask for; both numbers are in the request, and the delivered block repeats its
  actual rate in every call so the receiver never has to trust the setup. `model` is an
  opaque string - empty means "backend default", the set of legal values comes from
  task 008's documentation research, not from the application's imagination (AGENTS.md 8).
* **`TranslationError`** has five categories (`connection`, `rejectedRequest`,
  `audioFormat`, `protocol`, `internal`) and a `fatal` flag. Categories are vocabulary
  the product owns; task 009 maps provider-specific failures onto them, so no OpenAI
  error name crosses this line. Fatal means "this session is over" - it never means
  "stop anything": the controller logs, counts and records it, and AGENTS.md 12 holds
  the audio path running either way.
* **Every transition reports exactly once, and a refusal that changes nothing reports
  nothing.** That is what makes the state trace assertable, which is what makes the
  reconnect logic of task 010 testable.
* **`closeSession()` guarantees no sink callbacks after it returns.** For a real
  backend that means joining or draining its worker inside close. This guarantee is
  load-bearing for shutdown order: `ApplicationController::stop()` closes the session
  before deactivating the device, so a late network callback can never write into a
  destroyed pipeline. The controller's own guards (checked drop into `outputJitter`,
  counted in `rejectedAudioFrames`) are the second layer, for the callbacks task 010
  will one day race against a restart.
* **Translated audio has exactly one delivery route.** The controller writes accepted
  blocks into the engine's output jitter buffer - the one source of audible audio from
  task 005. Wrong-rate blocks are rejected and counted, never silently resampled or
  played fast: a resampler does not exist yet and inventing one outside task 012/018
  would change measured latency without telling anyone.
* **Diagnostics answer three questions separately**: `translatedAudioFrames` (what was
  accepted), `rejectedAudioFrames` (what arrived unusable - rate, null, empty) and
  `translatedAudioDroppedFrames` (what our own full buffer dropped). "The translator
  stopped delivering" and "delivery went somewhere wrong" are different operator
  problems and get different numbers.
* **Deliberately not here yet**: typed text events (`TranslationTextEvent`,
  sequence numbers, timestamps) belong to task 013, and `getCapabilities()` to task 011
  - both extension points are named in the header, so neither can be quietly invented
  by a task that was not its owner.

### Where the mock lives, and why

`tests/support/MockTranslationBackend.{h,cpp}` is the deterministic reference
implementation: no threads, no timers, no clock - callbacks fire synchronously inside
the call that produced them, and per-session state resets on reopen. Determinism is a
contract obligation, not a convenience: the tests run the identical script twice and
compare whole callback traces, and the end-to-end suite asserts exact float values
(0.4 in, -0.2 out) at the device output.

It lives in the test tree because task 019 makes "mock behaviour leaking into
production" a FAIL criterion, and `src/` keeps only the Null backend - a shell that
produces nothing, precisely so that no subsystem can mistake it for translation
(AGENTS.md 19). Task 019's interactive mock is a different thing built on top of this
contract.

### End to end without a provider

The integration suite (`tests/integration/TestTranslationEndToEnd.cpp`) runs the whole
chain on the Null audio device and the mock: device callback → input gain → ring →
submit → mock → translated audio → controller → jitter buffer → output gain → wire,
and proves four operator-relevant properties: buffered translation is what plays (not
the microphone), a wrong-rate delivery silences nothing else and stops nothing else
(text keeps flowing to NDI and history counters), a fatal error leaves
`status().audio == running` and puts the reason into `status().detail`, and restart
opens a fresh session whose first submit behaves like a first submit again.

## Status path to the UI

The UI reads exactly one type, `AppStatus`, produced by
`ApplicationController::status()`, and formats nothing but the header line
(`describeStatus()` lives in the App module). New subsystem state reaches the
operator only by extending `AppStatus` - there is no path from a backend to the
UI.
