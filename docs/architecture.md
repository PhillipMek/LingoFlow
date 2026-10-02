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
Config         -> Utils                           settings, no persistence yet (task 003)
Diagnostics    -> Utils                           atomic counters + snapshot
Audio          -> Utils, Diagnostics              engine, IAudioBackend, Null backend
Translation    -> Utils, Diagnostics              ITranslationBackend contract, Null backend
NDI            -> Utils, Diagnostics              INdiOutput contract, Null output
App            -> all of the above                ApplicationController (composition root)
App/Main.cpp   -> App, Utils, JUCE                the only JUCE-dependent code
```

Rules that the audit enforces:

1. **No upward dependencies.** `Audio`, `Translation` and `NDI` never include each
   other and never include `App`. The controller wires them together; the engine
   knows only `IAudioBackend`, the backend knows only its own sink interface.
2. **JUCE only in `src/App`.** The core stays portable (SPEC: future macOS/Linux
   must not require rewriting the translation core).
3. **No protocol or transport headers under `src/` at all yet** (`openai`,
   `websocket`, `json`, `asio`, `curl`). When task 004/009 introduce ASIO and the
   OpenAI backend, they get their own module and are added to the allowed lists;
   the UI and the audio engine still do not see them.
4. **The audit itself is tested.** `ArchitectureBoundariesSelfTest.cmake` injects
   eight violations into a copy of `src/` and requires the audit to reject each
   one with the expected `AUDIT_*` code, so a broken gate fails CI instead of
   silently passing.

## Realtime boundary

`IAudioProcessor::processAudio()` is the only function on this list declared
`noexcept` and documented as realtime: it runs on the device thread and may not
allocate, lock, log, touch the network, disk or UI. Everything else in the
architecture is explicitly non-realtime:

| Call | Thread | Realtime-safe contract |
|---|---|---|
| `IAudioProcessor::processAudio` | audio callback | `noexcept`, fixed work, relaxed atomics only |
| `IAudioProcessor::onAudioConfigurationChanged` | non-realtime | after `open()`, before first block |
| `ITranslationSink::on*` | network/worker | enqueue only, never play directly |
| `INdiOutput::publish` | worker | drop on pressure, never block the producer |
| `DiagnosticsManager::count*` | any, incl. audio | relaxed atomics |
| `DiagnosticsManager::snapshot`, `note*` with strings | UI/worker | mutex-guarded, never from the audio thread |

`AudioEngine::processAudio` currently outputs silence on purpose: there is no
translated audio source yet (task 012), and silence is the only correct outcome
before it exists.

## Status path to the UI

The UI reads exactly one type, `AppStatus`, produced by
`ApplicationController::status()`, and formats nothing but the header line
(`describeStatus()` lives in the App module). New subsystem state reaches the
operator only by extending `AppStatus` - there is no path from a backend to the
UI.
