# Vendored third-party sources

Decision 2026-10-01: the GPL-family dependencies live **inside** this repository,
so a fresh clone builds without hunting for absolute paths outside it. Licences are
recorded in `docs/licensing.md`.

| Component | Version | Licence (chosen) | Location | Size in repo |
|---|---|---|---|---|
| JUCE | 9.0.3 | AGPLv3 | `third_party/JUCE/` | 3885 files, 53.6 MB |
| Steinberg ASIO SDK | 2.3.4 | GPLv3 | `third_party/asiosdk/` | 27 files, 0.4 MB |

Copied verbatim from the official upstream release archives (JUCE 9.0.3 source,
Steinberg ASIO SDK 2.3.4) after verifying the trees against them; the local
copies used for that verification are not build inputs: `CMakeLists.txt`
now defaults `LIVEAI_JUCE_PATH` to `third_party/JUCE`.

## What is included and why

`third_party/JUCE/` - only what the CMake build reads:

* `CMakeLists.txt` (it includes `extras/Build/CMake/*.cmake` and adds `modules`),
* `modules/` - all JUCE modules,
* `extras/Build/` - `juceaide` and the CMake helpers (`juce_add_gui_app`, ...),
* `LICENSE.md`, `CHANGE_LIST.md`, `README.md` - licence and provenance.

Deliberately excluded: `examples/` (13.6 MB), `Projucer.exe` (10.7 MB),
`DemoRunner.exe` (17.9 MB), `.github/`. Prebuilt binaries do not belong in a source
repository; Projucer can be built from `extras/` if it is ever needed.

`third_party/asiosdk/` - `common/`, `host/`, `LICENSE.txt`, `changes.txt`,
`README.md`, `Steinberg ASIO Licensing Agreement.pdf`. Excluded: the sample driver,
the ~10 MB of Steinberg PDFs beyond the licence itself, and the ASIO logo artwork,
which is covered by separate trademark rules (see `docs/licensing.md`: no "ASIO
compatible" branding without that agreement).

Note: JUCE 9 already ships the three ASIO interface headers inside
`modules/juce_audio_devices/native/asio/`, so the vendored SDK is not needed for the
JUCE ASIO build. It is kept because driver-side work, `host/asiodrivers.cpp`
reference behaviour and the licence text itself belong to the project.

## `.gitignore` warning (already hit once)

An unanchored `build/` pattern matched `third_party/JUCE/extras/Build/` on Windows
(case-insensitive filesystem) and silently excluded 47 CMake support files - the
clone would not have configured. Build patterns are repo-root anchored
(`/build/`, `/build-*/`) and `.gitattributes` marks `third_party/**` as `-text`.
Do not relax either of these.

## Not vendored on purpose

* **NDI SDK** (`C:\Program Files\NDI\NDI 6 SDK`, discovered via the machine-level
  `NDI_SDK_DIR`) - proprietary NewTek licence. Additionally, with JUCE under AGPLv3,
  linking `Processing.NDI.Lib.x64.lib` into the product would add a restriction the
  AGPL does not allow, so NDI is reached by runtime loading of
  `Processing.NDI.Lib.x64.dll` behind `INdiOutput` (see `docs/licensing.md`).
* **Waves SoundGrid driver** - a system-installed product; never copied in.
* **OpenAI Realtime** - a network service; the protocol code lives in
  `src/Network/`.
* **Catch2, nlohmann/json** - fetched by CMake (`FetchContent`), with
  `-DLIVEAI_FETCH_CATCH2=OFF` / `-DLIVEAI_FETCH_NLOHMANN_JSON=OFF` to use installed
  copies offline.

## Rules

1. Never edit vendored third-party sources. Fix things in our own code; a local
   modification makes the whole tree a derived work and must be recorded here.
2. Upgrades replace the same subset listed above, then update the table here and
   `docs/licensing.md`.
3. No prebuilt binaries from these vendors in git.
4. `.gitattributes` keeps `third_party/**` byte-identical to upstream (no line-ending
   rewriting).
