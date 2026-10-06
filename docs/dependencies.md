# Dependencies

How every external component is declared, obtained and licensed. Nothing here is
a CI-only hack: this is the actual build architecture.

## Managed by CMake (automatic on a clean machine)

| Component | Declared in | Acquisition | Version pin | Needed for |
|---|---|---|---|---|
| JUCE | root `CMakeLists.txt` (`add_subdirectory`) | **Vendored** in `third_party/JUCE` - a normal clone carries it; nothing is downloaded | 9.0.3 | build + runtime (the app framework) |
| Steinberg ASIO SDK | `src/CMakeLists.txt` include paths | **Vendored** in `third_party/asiosdk` | 2.3.4 | build (ASIO host adapter) |
| nlohmann/json | root `CMakeLists.txt` (`FetchContent`) | Downloaded at configure time from the official repository (`GIT_REPOSITORY github.com/nlohmann/json`) | tag `v3.12.0`, shallow | build (Config/Network parse JSON) |
| Catch2 | `tests/CMakeLists.txt` (`FetchContent`) | Downloaded at configure time from the official repository | tag `v3.16.0`, shallow | tests only; never linked into the app |

Consequences:

* **No git submodules.** A clean `git clone` + `cmake -S . -B build` is complete
  once the machine has network access for the two FetchContent downloads (the
  pinned tags; `FETCHCONTENT_BASE_DIR` may point at a warm cache).
* `-DLIVEAI_FETCH_NLOHMANN_JSON=OFF` / `-DLIVEAI_FETCH_CATCH2=OFF` switch to a
  locally installed package via `find_package` for offline environments.
* `-DLIVEAI_JUCE_PATH=<dir>` can point at another JUCE checkout for experiments.

Licences: JUCE AGPLv3 (vendored `LICENSE.md`), ASIO SDK dual
(Steinberg ASIO License / GPLv3, used under GPLv3), nlohmann/json MIT, Catch2
BSL-1.0. The project itself is AGPLv3 (`LICENSE`, policy decisions in
`docs/licensing.md`, combined-work notice in `THIRD_PARTY_NOTICES.md`).

## Required externally, at the venue/target machine (never vendored)

| Component | Required for | How to obtain | How the application locates it |
|---|---|---|---|
| Waves SoundGrid driver (ASIO driver + SoundGrid server reachable) | real production audio I/O | Waves install package (EULA). Not redistributable by us. | Standard ASIO registry enumeration (`HKLM\SOFTWARE\ASIO`); appears as a device in the settings UI. `lingoflow_asio_probe --verify` cross-checks registry vs JUCE enumeration without loading the driver. |
| NDI runtime (`Processing.NDI.Lib.x64.dll`) | subtitle output over NDI only | NDI Tools/Runtime install (NewTek EULA). Optional: without it the feature reports "unavailable" and translation audio is unaffected. | Dynamic load at run time (`LoadLibrary` + `GetProcAddress`) behind `INdiOutput`; never statically linked (AGPL-compatibility rule in `docs/licensing.md`). |
| NDI SDK (headers only) | rebuilding the optional NDI target | NDI SDK install | `-DLINGOFLOW_NDI_SDK_INCLUDE=<SDK>/Include` or the `NDI_SDK_DIR` environment variable; if neither is set the NDI real output is not compiled and the Null output remains, with tests adapting to the machine's runtime. |
| OpenAI account/API key | real translation | platform.openai.com | Entered in Settings (Windows Credential Manager) or `OPENAI_API_KEY` environment variable for development. The application reaches `api.openai.com` over TLS 1.2+; no OpenAI library is bundled. |

## Toolchain

| Tool | Minimum | Notes |
|---|---|---|
| Windows | 10/11 x64 | the product target |
| Visual Studio 2022 (MSVC) + C++20 | any recent toolset | the CI image works as-is |
| CMake | 3.25 | FetchContent + presets-free flow |
| Git | any | two FetchContent clones at configure time |

### Known build-environment constraints (measured, not folklore)

* **The build tree must live on an ASCII path.** The JUCE resource helper
  (`juceaide`) fails on non-ASCII arguments; sources may sit on a Unicode path,
  the build directory may not.
* Generated build files must be written as UTF-8 (an ASCII re-write silently
  corrupts non-ASCII paths into broken include flags).
* The generator is Visual Studio (multi-config): Debug and Release coexist in
  one tree; `-C Debug/-C Release` selects per command.

## What must NEVER reappear

* No developer-machine absolute paths in scripts, docs or CMake (defaults are
  `$PSScriptRoot`-relative or CMake variables).
* No credentials of any kind, ever - see `SECURITY.md`.
* No linked NDI import library, no copied Waves driver files.
