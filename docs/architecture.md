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
Audio          -> Utils, Diagnostics              engine, IAudioBackend, ASIO model/policy, Null backend
Translation    -> Utils, Diagnostics              ITranslationBackend contract, Null backend
NDI            -> Utils, Diagnostics              INdiOutput contract, Null output
Platform       -> Audio, Utils, Diagnostics       JUCE adapters (ASIO device discovery/lifecycle)
App            -> all of the above                ApplicationController (composition root)
App/Main.cpp   -> App, Utils, JUCE                the only UI code
```

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
| `Platform::JuceAsioBackend::Callback::audioDeviceIOCallbackWithContext` | audio callback | forwards driver pointers into `processAudio`, views preallocated in `aboutToStart`, `audioDeviceError` sets a flag only |
| `IAudioProcessor::onAudioConfigurationChanged` | non-realtime | after `open()`, before first block |
| `ITranslationSink::on*` | network/worker | enqueue only, never play directly |
| `INdiOutput::publish` | worker | drop on pressure, never block the producer |
| `DiagnosticsManager::count*` | any, incl. audio | relaxed atomics |
| `DiagnosticsManager::snapshot`, `note*` with strings | UI/worker | mutex-guarded, never from the audio thread |
| `IAudioBackend::open/start/stop/close`, `AsioDiscovery::*` | control thread | may allocate and log; never called from the callback |

`AudioEngine::processAudio` currently outputs silence on purpose: there is no
translated audio source yet (task 012), and silence is the only correct outcome
before it exists. The probe tool drives that same engine, so what it proves about a
real device is callback delivery and clean shutdown - not signal content, which stays
a human check (docs/device-defaults.md).

## Status path to the UI

The UI reads exactly one type, `AppStatus`, produced by
`ApplicationController::status()`, and formats nothing but the header line
(`describeStatus()` lives in the App module). New subsystem state reaches the
operator only by extending `AppStatus` - there is no path from a backend to the
UI.
