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
Audio          -> Utils, Diagnostics              engine, ring/jitter buffers, meters, loopback,
                                                  IAudioBackend, ASIO model/policy, Null backend
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
| `Platform::JuceAsioBackend::Callback::audioDeviceIOCallbackWithContext` | audio callback | forwards driver pointers into `processAudio`, views preallocated in `aboutToStart`, `audioDeviceError` sets a flag only |
| `AudioLoopback::run` | worker | consumer/producer of the lock-free buffers; may sleep between polls; never called by the audio thread |
| `IAudioProcessor::onAudioConfigurationChanged` | non-realtime | after `open()`, before first block |
| `AudioEngine::activate / configure / deactivate` | control thread | the only place pipeline memory is allocated or released |
| `ITranslationSink::on*` | network/worker | enqueue only, never play directly |
| `INdiOutput::publish` | worker | drop on pressure, never block the producer |
| `DiagnosticsManager::count*` | any, incl. audio | relaxed atomics |
| `DiagnosticsManager::snapshot`, `note*` with strings | UI/worker | mutex-guarded, never from the audio thread |
| `IAudioBackend::open/start/stop/close`, `AsioDiscovery::*` | control thread | may allocate and log; never called from the callback |

## Realtime pipeline (task 005)

The pipeline is the one from SPEC "Audio Pipeline", with the two translation stages
still to be filled in:

```text
ASIO input -> AudioEngine::processAudio
                  -> LevelMeter (input)
                  -> AudioRingBuffer        (producer = audio thread)
                        |
                  [loopback worker now (AudioLoopback); OpenAI streaming worker in task 012]
                        |
                  AudioJitterBuffer         (consumer = audio thread)
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
  and signalled-block counters. Task 006 acts on these numbers with gain; reading them
  from the UI is safe at any time.
* **Counters are engine-level, not buffer-level.** `deactivate()` releases the buffers,
  so dropped/underrun/overrun totals live in `AudioEngine`'s own atomics and stay
  readable after shutdown - that is where the operator's diagnostics (task 017) and the
  long-run test (task 024) will read them.
* **Loopback is never implicit.** `AudioEngine` outputs silence until something writes
  to the jitter buffer, and the loopback worker has to be started on purpose (the probe
  tool today; developer mode in task 019). Routing live microphone audio to the audience
  is an operator action.

### How the realtime rule is enforced

Two independent gates, both in CTest (label `realtime`):

1. `tests/RealtimeSafetyAudit.cmake` extracts the body of every function reachable from
   the audio callback (engine, both buffers, meter, JUCE bridge) and rejects a list of
   forbidden constructs: allocation (`new`, `make_unique`, `malloc`, container growth),
   locks and waits, sleeping, filesystem/registry, transport (`json`, `websocket`,
   sockets), UI, exceptions. `...SelfTest.cmake` injects one violation of each class
   into a copy of `src/` and asserts the gate rejects it, and that a vanished function
   fails the gate instead of shrinking it silently.
2. `tests/realtime/TestRealtimeAllocations.cpp` replaces global `operator new` in its
   own test binary and counts heap allocations during 5000 callbacks with the loopback
   worker running: the count must be 0. It is a separate executable so the substitution
   cannot influence any other suite.

Both are behavioural *and* textual: the audit cannot see a violation hidden behind a
macro, the allocation counter cannot see a lock. Together with the stress test in
`tests/unit/TestAudioPipeline.cpp` (5000 blocks with a live consumer thread, frame
accounting required to close) they cover the three failure modes that matter: allocating,
blocking, and losing audio without counting it.

`AudioEngine::processAudio` outputs silence while nothing feeds the jitter buffer, and
that is still deliberate: there is no translated audio source before task 012. The probe
tool adds `--loopback`, which turns the same pipeline into a measurable end-to-end path on
a real device.

## Status path to the UI

The UI reads exactly one type, `AppStatus`, produced by
`ApplicationController::status()`, and formats nothing but the header line
(`describeStatus()` lives in the App module). New subsystem state reaches the
operator only by extending `AppStatus` - there is no path from a backend to the
UI.
