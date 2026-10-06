# Contributing to LingoFlow

## Getting the code building

```powershell
git clone https://github.com/PhillipMek/LingoFlow.git
cd LingoFlow
cmake -S . -B build
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Requirements: Windows 10/11 x64, Visual Studio 2022 (MSVC), CMake >= 3.25. JUCE
and the ASIO SDK are vendored under `third_party/`; nlohmann/json and Catch2 are
fetched by CMake from their official repositories (pinned tags). See
`docs/dependencies.md` for the full dependency picture and `docs/CI.md` for
exact commands CI runs.

## Ground rules for changes

* **The audio callback is sacred.** Nothing in it may allocate, lock, wait,
  sleep, touch the filesystem, the network, JSON or the GUI. This is enforced by
  three independent mechanisms (a lexical audit gate, a runtime allocation
  counter, and behavioral tests) - all run in CI. If a change breaks a gate, the
  gate is right until proven otherwise: read `tests/RealtimeSafetyAudit.cmake`.
* **Architecture boundaries** are enforced by `tests/ArchitectureBoundaries.cmake`
  (module include rules, e.g. only Config/Network parse JSON). Keep new code on
  the existing seams: `IAudioBackend`, `ITranslationBackend`, `INdiOutput`,
  `ISecretStore`.
* **No secrets anywhere in the repository.** API keys live in the Windows
  Credential Manager (production) or an environment variable (development);
  never in config files, logs, diagnostics exports, tests or commit messages.
  Diagnostic exports are value-redacted and tested with canary keys.
* **Provider facts come from the official documentation**
  (`docs/openai-realtime-protocol.md` is the normative file). Do not invent
  endpoints, event names or capabilities.
* **Do not fabricate measurements.** Latency numbers shown to operators must be
  measured or clearly labelled as estimates (the accounting enforces the kinds).
* Keep changes focused: one logical change per commit; do not mix refactors,
  formatting and behavior.

## Testing your change

Run the full suite in Debug and Release (`ctest --output-on-failure`). CI is
identical (see `docs/CI.md`); hardware, provider and NDI-receiver behavior are
covered by the venue protocol (`docs/rig-checklist.md`, `docs/release.md`) and
cannot be asserted from a CI run.

## Code style

C++20, exceptions at non-realtime boundaries, `noexcept` where the contract
promises it; comments say *why*, not *what*; all warnings visible (`/W4`), zero
warnings policy.
