# Live AI Interpreter

Windows desktop application for simultaneous speech translation at live events:

```text
SoundGrid/ASIO in  ->  audio engine  ->  OpenAI Realtime translation  ->  audio engine  ->  SoundGrid/ASIO out
                                       ->  NDI subtitle out (optional)
```

Status: **tasks 000-002 complete**. The repository contains a JUCE/CMake
application, a portable core (`liveai_core`) with the module interfaces and Null
implementations of the audio, translation and NDI boundaries, and unit tests.
**No real audio device is opened and no network request is sent yet** - the ASIO
backend is task 004/005, the OpenAI backend is task 009.

## Requirements

Verified on this machine (`docs/environment-report.md` has the full evidence):

| Component | Version | How it is obtained |
|---|---|---|
| Windows | 10 x64 | host |
| MSVC (Visual Studio 2022) | 19.44.35229 | local install |
| CMake | 3.31.6-msvc6 | bundled in Visual Studio, **not on PATH** |
| Ninja | 1.12.1 | bundled in Visual Studio, **not on PATH** |
| JUCE | 9.0.3 | local source tree, path passed with `-DLIVEAI_JUCE_PATH=...` |
| ASIO SDK | 2.3.4 | local source tree (used from task 004 on) |
| Waves SoundGrid ASIO driver | 16.5.197.301 | installed product |
| NDI | 6 Tools 6.3.2.0 runtime | installed product (SDK headers still required for task 016) |
| Catch2 | v3.16.0 | CMake `FetchContent` (needs network on first configure) |

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

If JUCE is not in `../juce-9.0.3-windows/JUCE`, add
`-DLIVEAI_JUCE_PATH=C:/path/to/JUCE`.

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

## Layout

```text
CMakeLists.txt          root project, JUCE discovery
src/CMakeLists.txt      liveai_core + LiveAIInterpreter targets
src/App/                ApplicationController (composition root), JUCE entry point
src/Audio/              AudioEngine shell, IAudioBackend, Null/ device
src/Translation/        ITranslationBackend contract, Null/ backend
src/NDI/                INdiOutput contract, Null/ output
src/Config/             AppConfig + ConfigManager (validation only so far)
src/Diagnostics/        DiagnosticsManager (atomic counters + snapshot)
src/Utils/              logging skeleton
tests/                  Catch2 unit tests + architecture boundary audit
docs/                   specification support documents, environment report
tasks/                  agent task files
```

Real-time rules, thread model and architectural boundaries are defined in
`AGENTS.md`, `docs/architecture.md` and `docs/threading.md`.

### Architecture boundary audit

Two CTest entries with label `architecture` guard the dependency direction
(`docs/architecture.md`):

* `architecture_boundary_audit` - scans every `#include` under `src/` and rejects
  a module including a module it must not know, JUCE outside `src/App`, and any
  protocol/transport header (`openai`, `websocket`, `json`, `asio`, `curl`).
* `architecture_boundary_audit_selftest` - copies `src/`, injects 8 violations and
  requires the audit to reject each with the expected `AUDIT_*` code, so a broken
  gate fails instead of silently passing.

Run them alone: `ctest --test-dir D:\LiveAI\build-debug -L architecture --output-on-failure`.

## Secrets

Never put an API key in the repository, in `config.json` or in logs.
Development: `OPENAI_API_KEY` environment variable. Production: Windows secure
credential storage. See `AGENTS.md` section 10.
