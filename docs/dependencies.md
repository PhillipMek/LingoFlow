# Dependencies

How every external component is declared, obtained and licensed. Nothing here is
a CI-only hack: this is the actual build architecture. Four categories, kept
apart on purpose: what ships inside the repository, what CMake downloads at
configure time, what must be installed on the machine, and what is a cloud
service the product talks to.

## 1. Vendored in this repository (a clone is complete)

| Component | Version | Licence | Location | Role |
|---|---|---|---|---|
| JUCE | 9.0.3 | AGPLv3 (optionally commercial) | `third_party/JUCE` | build + runtime (the app framework) |
| Steinberg ASIO SDK | 2.3.4 | Steinberg ASIO License **or** GPLv3 (used under GPLv3) | `third_party/asiosdk` | build (ASIO host headers; the JUCE ASIO path also carries the three interface headers) |

No download, no submodule: `add_subdirectory` / include paths point inside the
tree. `-DLIVEAI_JUCE_PATH=<dir>` can point at another JUCE checkout for
experiments. Provenance and the exact file subsets: `third_party/README.md`.

## 2. Fetched by CMake at configure time (FetchContent, pinned tags)

| Component | Declared in | Version pin | Needed for | Licence |
|---|---|---|---|---|
| nlohmann/json | root `CMakeLists.txt` | tag `v3.12.0`, shallow clone from the official repository | build (Config/Network JSON parse) | MIT |
| Catch2 | `tests/CMakeLists.txt` | tag `v3.16.0`, shallow clone from the official repository | tests only; never linked into the app | BSL-1.0 |

* A clean `git clone` + `cmake -S . -B build` is complete once the machine has
  network access for the two FetchContent clones (`FETCHCONTENT_BASE_DIR` may
  point at a warm cache; CI caches it keyed on the pinning CMakeLists).
* `-DLIVEAI_FETCH_NLOHMANN_JSON=OFF` / `-DLIVEAI_FETCH_CATCH2=OFF` switch to a
  locally installed package via `find_package` for offline environments.

## 3. External runtime prerequisites (installed on the machine, never vendored)

| Component | Required for | How to obtain | How the application locates it |
|---|---|---|---|
| Waves SoundGrid driver (ASIO driver + SoundGrid server reachable) | real production audio I/O | Waves install package (EULA). Not redistributable by us. | Standard ASIO registry enumeration (`HKLM\SOFTWARE\ASIO`); appears as a device in the settings UI. `lingoflow_asio_probe --verify` cross-checks registry vs JUCE enumeration without loading the driver. |
| any other ASIO driver | audio I/O without SoundGrid | driver vendor | same enumeration |
| NDI runtime (`Processing.NDI.Lib.x64.dll`) | subtitle output over NDI only | NDI Tools/Runtime install (NewTek EULA). Optional: without it the feature reports "unavailable" and translation audio is unaffected. | Dynamic load at run time (`LoadLibrary` + `GetProcAddress`) behind `INdiOutput`; never statically linked (AGPL-compatibility rule in `docs/licensing.md`). |
| NDI SDK (headers only) | rebuilding the optional NDI target | NDI SDK install | `-DLINGOFLOW_NDI_SDK_INCLUDE=<SDK>/Include` or the `NDI_SDK_DIR` environment variable; if neither is set the NDI real output is not compiled and the Null output remains, with tests adapting to the machine's runtime. |

## 4. Cloud service (talked to over TLS, nothing bundled)

| Service | Role | Access |
|---|---|---|
| OpenAI Realtime Translation API | the translation itself | Account + API key from platform.openai.com, entered in Settings (Windows Credential Manager) or `OPENAI_API_KEY` for development. The application reaches `api.openai.com` over TLS 1.2+; no OpenAI library is bundled. Protocol assumptions: `docs/openai-realtime-protocol.md`. |

## Licences, in one line each

JUCE AGPLv3 (vendored `LICENSE.md`), ASIO SDK dual (used under GPLv3),
nlohmann/json MIT, Catch2 BSL-1.0, NDI NewTek proprietary (runtime-loaded, not
linked), Waves EULA (external), OpenAI terms (network service). The project
itself is AGPLv3: `LICENSE`, verbatim third-party texts in `LICENSES/`, policy
decisions in `docs/licensing.md`, combined-work notice in
`THIRD_PARTY_NOTICES.md`.

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
