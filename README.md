# LingoFlow

Windows desktop application for simultaneous speech translation at live events:

```text
SoundGrid/ASIO in  ->  audio engine  ->  OpenAI Realtime translation  ->  audio engine  ->  SoundGrid/ASIO out
                                       ->  NDI subtitle out (optional)
```

Status: **tasks 000-006 complete**. The repository contains a JUCE/CMake
application, a portable core (`lingoflow_core`) with the module interfaces, a realtime
audio pipeline (lock-free ring buffer, output jitter buffer, input and output gain with
click-free gliding, level meters, clipping and underrun/overrun counters) with
input->output loopback, Null implementations of the translation and NDI boundaries,
versioned configuration with safe persistence, ASIO device discovery and device lifecycle
on top of JUCE, and 144 tests.
**No translated audio reaches a real device and no network request is sent yet** -
the OpenAI backend is task 009 and translated audio reaches the output in task 012. The
operator application does open the ASIO device named in settings, but until 012 the only
thing that can leave on the output is loopback audio, and that has to be started
explicitly.

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

144 CTest entries: Catch2 unit/integration suites (including the gain-stage suite), the
realtime allocation suite in its own binary, the architecture boundary audit plus its
self-test, the realtime safety audit plus its self-test, and the two device entries that
run the ASIO tool's driver-free modes.

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
parsing, reported and dropped. They belong to `security::ISecretStore` (task 015
implements it on Windows secure storage). The application must keep running when
no credential exists: only the translation backend becomes unavailable.

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
  full scale from the device (`inputClippedFrames`), what our own input trim produced on
  the way to the translator (`inputGainClippedFrames`) and what is heading at the audience
  (`outputGainClippedFrames`). Turning the gain down cannot hide the first one, which is
  the whole reason the meters read post-gain audio but the stage also counts pre-gain
  full-scale samples. A latching `takeInputClipIndicator()` / `takeOutputClipIndicator()`
  is what the UI lamp will use.
* **Non-finite samples become silence, counted.** One NaN from a driver silences one
  sample (`nonFiniteInputFrames`) instead of poisoning the translation stream or the room.
* **Levels survive a device restart.** The stages are not released by `deactivate()`, so a
  re-opened ASIO device starts with the same gains, the same mute state and the same
  clipping history.
* **`--smoke` logs the levels in effect**, so "my settings were applied" is distinguishable
  from "the defaults were used": `audio gains in effect: input -6.5 dB, output +3.0 dB,
  glide 20 ms`. The slider, the numeric box, the reset button and the mute button themselves
  are task 014; the engine API they will call already exists.

## Layout

```text
CMakeLists.txt          root project, JUCE discovery
src/CMakeLists.txt      lingoflow_core + LingoFlow targets
src/App/                ApplicationController (composition root), JUCE entry point
src/Audio/              AudioEngine + pipeline (ring/jitter/gain/meters/loopback), IAudioBackend (+ DeviceRequest), ASIO model/policy, Null/ device
src/Translation/        ITranslationBackend contract, Null/ backend
src/NDI/                INdiOutput contract, Null/ output
src/Platform/Asio/      JUCE ASIO discovery, JuceAsioBackend, lingoflow_asio_probe tool
src/Config/             AppConfig, ConfigSchema (validation + JSON text), ConfigStore (atomic file), ConfigManager
src/Security/           ISecretStore boundary + NullSecretStore (credentials never live in config)
src/Diagnostics/        DiagnosticsManager (atomic counters + snapshot)
src/Utils/              logging skeleton
tests/                  Catch2 unit + pipeline tests, realtime allocation suite, architecture audit, realtime safety audit, self-tests
docs/                   licensing, device defaults, architecture, environment report
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
