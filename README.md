# Live AI Interpreter

Windows desktop application for simultaneous speech translation at live events:

```text
SoundGrid/ASIO in  ->  audio engine  ->  OpenAI Realtime translation  ->  audio engine  ->  SoundGrid/ASIO out
                                       ->  NDI subtitle out (optional)
```

Status: **tasks 000-003 complete**. The repository contains a JUCE/CMake
application, a portable core (`liveai_core`) with the module interfaces, Null
implementations of the audio, translation and NDI boundaries, versioned
configuration with safe persistence, and unit tests.
**No real audio device is opened and no network request is sent yet** - the ASIO
backend is task 004/005, the OpenAI backend is task 009.

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
$dbg = 'D:\LiveAI\build-debug'
$rel = 'D:\LiveAI\build-release'

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

```powershell
cmd /c "call $vccmd && `"$cmake`" --test-dir $dbg --output-on-failure"
cmd /c "call $vccmd && `"$cmake`" --test-dir $rel --output-on-failure"
```

(`ctest.exe` sits next to the bundled `cmake.exe`; if it is on `PATH`, plain
`ctest --test-dir D:\LiveAI\build-debug --output-on-failure` works too.)

## Run the application

The executable name comes from `PRODUCT_NAME`, so it contains spaces:

```powershell
$exe = "$dbg\src\LiveAIInterpreter_artefacts\Debug\Live AI Interpreter.exe"

& $exe --smoke   # headless start/stop check: exit code 0, log file written
& $exe           # operator window
```

`--smoke` starts the application lifecycle, stops it and exits without creating
a window. It is a startup/logging check only — it does not verify audio,
translation quality or the operator-visible UI.

The application writes a log file to
`%APPDATA%\Live AI Interpreter\logs\liveai.log` (JUCE's
`File::userApplicationDataDirectory`, which is the Roaming profile folder on
Windows).

### Build tree must be on an ASCII path

The repository lives under `D:\Рабочий\...`. CMake, Ninja and MSVC handle that
path correctly, but JUCE's helper tool `juceaide` crashes ("Unhandled
exception") as soon as a **non-ASCII path appears in its command line**, which
is what generates `*_resources.rc`. Keep the build directory on an ASCII path:

```powershell
cmake -S . -B D:/LiveAI/build-debug ...     # works
cmake -S . -B build-debug ...               # juceaide fails: rcfile step
```

Verified by running `juceaide rcfile` directly with ASCII and non-ASCII
arguments: ASCII → exit 0, non-ASCII → exit 1.

## Configuration

Settings live in one file, `config.json`, next to the log:

```text
%APPDATA%\Live AI Interpreter\config.json          (Windows)
~/.liveai/config.json                              (other platforms)
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

This development PC has the Waves SoundGrid **driver** installed but will never have
a SoundGrid **server** reachable, so opening the device, channel names and real
latency cannot be verified here at all. Consequences and the exact list of deferred
hardware checks: `docs/device-defaults.md`.

The defaults that follow from it: empty device identifiers (nothing is selected or
hardcoded), 48 kHz / 480 frames / mono in+out / 0 dB gain, `ndi.enabled = false`.
They live in `config.json`, so real hardware later changes settings, not code. The UI
must never present them as measurements.

## Layout

```text
CMakeLists.txt          root project, JUCE discovery
src/CMakeLists.txt      liveai_core + LiveAIInterpreter targets
src/App/                ApplicationController (composition root), JUCE entry point
src/Audio/              AudioEngine shell, IAudioBackend, Null/ device
src/Translation/        ITranslationBackend contract, Null/ backend
src/NDI/                INdiOutput contract, Null/ output
src/Config/             AppConfig, ConfigSchema (validation + JSON text), ConfigStore (atomic file), ConfigManager
src/Security/           ISecretStore boundary + NullSecretStore (credentials never live in config)
src/Diagnostics/        DiagnosticsManager (atomic counters + snapshot)
src/Utils/              logging skeleton
tests/                  Catch2 unit tests + architecture boundary audit + self-test
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
  a module including a module it must not know, JUCE outside `src/App`, and any
  protocol/transport header (`openai`, `websocket`, `asio`, `curl`, `json` - with
  one registered exception: `nlohmann/json.hpp` inside `Config`).
* `architecture_boundary_audit_selftest` - copies `src/`, applies 15 modifications
  (8 violations, 2 documented exceptions that must still be accepted, structural
  errors) and asserts the audit answers each one correctly, so a broken gate fails
  instead of silently passing.

Run them alone: `ctest --test-dir D:\LiveAI\build-debug -L architecture --output-on-failure`.

## Secrets

Never put an API key in the repository, in `config.json` or in logs.
Development: `OPENAI_API_KEY` environment variable. Production: Windows secure
credential storage. See `AGENTS.md` section 10.
