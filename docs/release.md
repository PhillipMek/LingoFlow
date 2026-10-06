# Release Build and Installer (task 026)

Date: 2026-10-06. Version: 0.1.0 (the CMake project version, stamped into the
binary's JUCE version string and shown in the app, the log line and the
diagnostics export).

## What ships

A single executable. `packaging/release-package.ps1` assembles
`out/release/LingoFlow-0.1.0/`:

| File | Role |
|---|---|
| `LingoFlow.exe` (8.2 MB) | the product - icon, version info and manifest compiled in |
| `LingoFlow-symbols.pdb` (85 MB, RelWithDebInfo) | crash-analysis symbols; carried to a venue in a folder, not installed |
| `VERSION.txt` | the version stamp next to the binary |

## Verified runtime dependencies (the FAIL criterion: no developer-only dependencies)

`dumpbin /DEPENDENTS` of the shipping exe, recorded by the packaging script,
which FAILS the package if a non-system import ever appears:

    advapi32, winhttp, kernel32, user32, gdi32, shell32, ole32, oleaut32,
    comdlg32, wininet, ws2_32, version, shlwapi, winmm, cfgmgr32, dbghelp,
    api-ms-win-shcore-scaling-l1-1-1, imm32, comctl32, dwmapi, dwrite, d2d1,
    dxgi, d3d11, dcomp

- **The Microsoft Visual C++ redistributable is NOT required.** The first
  dependency scan of this task measured imports of `MSVCP140.dll`,
  `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` - i.e. a developer-machine component
  on the clean-machine path. The build now sets
  `CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded` (static CRT); the imports above
  are the proof, and the packaging script re-checks them.
- Every remaining import is a stock Windows 10/11 x64 component (the
  `api-ms-win-*` set is the OS universal CRT; the rest are Win32/COM/GDI/DirectX
  system DLLs used by JUCE).
- Nothing from the build tree is loaded at run time: the NDI SDK is compile-time
  headers only (the runtime DLL, if present on the machine, is located by name -
  task 016's design; no NDI installed is a supported, honest state), and the
  ASIO SDK is compiled in (the host loads driver DLLs via the registry at run
  time, which is the ASIO contract).

## External driver prerequisites (what a machine must have installed)

| Prerequisite | When needed | Absent = |
|---|---|---|
| Windows 10/11 x64 | always | the app does not run |
| Waves SoundGrid driver (+ server configured & reachable) | real audio in/out | Start reports the honest device error (faulted screen + Retry); developer `--dev` mode works fully without it |
| any ASIO driver | non-SoundGrid audio | same as above |
| NDI runtime (NewTek/NDI install) | subtitle output over NDI | subtitles off with the "NDI unavailable" state; translation audio unaffected |
| OpenAI account + API key | real translation | translation refuses until a key is stored in Settings (Credential Manager); developer mode needs none |
| VC++ redistributable | NEVER | - (static CRT) |

## Installing

Per-user, no administrator, no MSI engine (the venue machines are offline and
locked down - fewer moving parts, no reboots):

```
powershell -ExecutionPolicy Bypass -File packaging\release-package.ps1
powershell -ExecutionPolicy Bypass -File packaging\install.ps1
#   -> %LOCALAPPDATA%\Programs\LingoFlow\LingoFlow.exe
#   -> Start Menu shortcut "LingoFlow"
powershell -ExecutionPolicy Bypass -File packaging\uninstall.ps1
#   removes exactly those two; deliberately does NOT touch %APPDATA%\LingoFlow
#   (settings/logs) or the Windows credential store (the key) - a program
#   uninstall must not erase a venue's setup or anyone's secret.
```

Both scripts accept `-TargetDir`/`-PackageDir` for test runs; the full cycle was
exercised on this machine (package -> install -> launched the installed exe,
`--dev --smoke` exit 0 -> uninstall -> folder and shortcut gone, user state
untouched).

## Build configurations (the task's "Release/RelWithDebInfo")

- **Release** - ships (`ctest -C Release`: 359/359).
- **RelWithDebInfo** - builds clean, `ctest -C RelWithDebInfo`: 359/359; its pdb
  is the crash-analysis artifact in the package.
- Debug - development and the pessimistic performance budget (docs/performance.md).

## Clean-machine protocol (the REQUIRED human checkpoint)

The task declares `HUMAN_CHECKPOINT: REQUIRED: clean Windows test`. Everything
above is verified on the build machine; a genuinely clean machine (no Visual
Studio, no NDI SDK, no Waves driver, untouched %APPDATA%) is the one thing a
developer machine cannot simulate - its credentials are the venue visit, and the
protocol is short:

1. copy `LingoFlow.exe` (or run `install.ps1` + the package folder) on a fresh
   Windows 10/11 x64 profile;
2. launch - expect the window with the honest device-error/faulted state until a
   SoundGrid driver is installed (this IS the correct clean-machine answer);
3. run `LingoFlow.exe --dev --smoke` - expect exit 0 (the whole chain without a
   device or a key);
4. open Settings, store an API key - expect "stored securely in Windows
   credential store", expect the key NOWHERE in `%APPDATA%\LingoFlow\*`;
5. with the driver present: select the device, Start a mock or live session.

Rows 2-4 close the checkpoint; the venue steps (real device + live provider +
NDI receiver) are already the open checkpoints of tasks 012/015/016.

## Status

Automated scope: **done and verified here**. Task status: **PARTIAL** - the
REQUIRED clean-Windows test remains for the owner. No product code changed
beyond the CRT linkage decision (measured dependency -> documented fix); no new
dependencies were introduced (the installer is PowerShell + Shell COM, both
always present).
