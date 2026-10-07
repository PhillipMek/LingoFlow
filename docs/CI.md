# CI - what runs, what does not, how to reproduce locally

Workflow: `.github/workflows/ci.yml` (name: **CI**, badge in README).
Runner: `windows-latest`. Trigger: push to `main`/`master`, any pull request,
manual `workflow_dispatch`. Permissions: `contents: read` only.

## What the CI runs (all of it hardware- and secret-free)

1. **Configure + build, Debug** - the project's own CMake flow (Visual Studio
   generator, the default on the image; same multi-config model as the local
   build tree). `/W4` warnings from the project's own flags are counted and
   printed in the job summary (surfaced, not silently raised to errors).
2. **Configure + build, Release** - mandatory; failure reddens CI.
3. **The FULL registered ctest suite in both configurations** - no filters:
   - Catch2 unit + integration (`lingoflow_tests`, discovered automatically at
     build time via `catch_discover_tests`): audio engine, rings/jitter, gain,
     resampler, ASIO channel forwarding/mapping, config + schema + store,
     language registry, OpenAI protocol/backend against `FakeWebSocketTransport`
     (no network), text pipeline, NDI dispatch, latency accounting, security
     secret stores, diagnostics export, UI model (headless), fault injection
     suite, performance headroom bounds;
   - the realtime allocation gate binary (`lingoflow_realtime_tests`);
   - the static gates and their self-tests, which are exactly the project's
     realtime-safety regression protection: `realtime_safety_audit(+selftest)`,
     `architecture_boundary_audit(+selftest)`;
   - `asio_discovery_verify`/`asio_discovery_list` - registry enumeration only,
     never loads a driver.
   The job summary prints the discovered count per configuration; the suite
   currently registers 359 tests. A hard-coded count guard was deliberately
   NOT added (test renames legitimately change it); the guard that matters is
   that CI runs every registered test with no exclusion filter.
4. **Smoke modes** (Release executable):
   - `--dev --smoke` must exit 0. This is the hardware-independent proof that
     the production wiring starts end to end without device/key/network
     (the built-in `--dev` mode).
   - `--smoke` (plain) is classified, never swallowed: exit 0 = PASS,
     exit 2 = **EXPECTED-HARDWARE-SKIP** - the documented semantics of 2 in
     `src/App/Main.cpp` is "a subsystem refused to start", and on a runner with
     no ASIO device that refusal is the correct application behavior, recorded
     as a notice in the summary. Any other exit code (a crash would look like
     that) fails CI. CI does not change these semantics; they are the product's.

## What CI intentionally does NOT run (and why)

| Left to | Because |
|---|---|
| opening a real ASIO device (Waves SoundGrid) | no hardware on runners; `docs/rig-checklist.md` |
| a real OpenAI session | no API key by design - and none needed: the protocol surface is tested against scripted transports (the project rules); live round trip = the venue translation checkpoint |
| a real NDI receiver | needs a receiver on the network; transport failure paths are injected in tests; live = the venue NDI checkpoint |
| long-run 1h/4h/8h+ | time + real hardware; `docs/performance.md` protocol, the long-run venue checkpoint |
| clean-machine install test | the runner is neither clean nor the installed target; `docs/release.md` four-step protocol, the clean-machine checkpoint |
| branch protection configuration | GitHub setting, owner's decision, not a repository file |

A green CI run means: everything deterministic without hardware and secrets
passed. It never means the venue check happened.

## Secrets policy

The workflow references **no secrets at all**: nothing to echo, nothing to leak
(no `set -x`/`Set-PSDebug` tracing anywhere, no secrets interpolated into run
scripts; `${{ ... }}` appears only in trusted metadata contexts - triggers,
cache keys, concurrency groups - and in the summary from values the job itself
computed). Credentials tests use the Credential Manager of the ephemeral runner
with unique `unit-<pid>-*` names and remove them themselves.

## Caching, honestly

Only the FetchContent download base (`.fc-cache`, pinned-tag sources for Catch2
and nlohmann/json) is cached; build trees are never reused across runs, so a
cached binary cannot hide a compilation failure. The key hashes the CMakeLists
files that declare the pinned versions, so a dependency change invalidates it.
JUCE is vendored in `third_party/` and needs no cache. No third-party installers
are downloaded at any step.

## Reproduce CI locally (same commands, same order)

From the repository root, in PowerShell:

```powershell
# Debug
cmake -S . -B build-debug -DFETCHCONTENT_BASE_DIR="$PWD\.fc-cache"
cmake --build build-debug --config Debug --parallel
ctest --test-dir build-debug -C Debug --output-on-failure

# Release
cmake -S . -B build-release -DFETCHCONTENT_BASE_DIR="$PWD\.fc-cache"
cmake --build build-release --config Release --parallel
ctest --test-dir build-release -C Release --output-on-failure

# smoke classification (identical logic to the workflow)
& build-release/src/LingoFlow_artefacts/Release/LingoFlow.exe --dev --smoke;   $LASTEXITCODE  # expect 0
& build-release/src/LingoFlow_artefacts/Release/LingoFlow.exe --smoke;         $LASTEXITCODE  # 0 = hardware present and worked, 2 = EXPECTED-HARDWARE-SKIP
```

The single-build-tree alternative (`build/`, as the local developer workflow
uses it) is equivalent for developers:

```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel
cmake --build build --config Release --parallel
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build -C Release --output-on-failure
```

Notes for local runs: close any running LingoFlow GUI before testing (the
shared %APPDATA% state is the one environment flake the suite has, and the
Release exe would be file-locked); stop external ASIO hosts if you want plain
`--smoke` to exercise the success path.

## Artifacts

- `ci-diagnostics` (always, 7 days): build logs, ctest output, smoke logs,
  and the app's own log file from the run under `appdata/` (secret-free by
  design; the settings file is not collected).
- `LingoFlow-Windows-Release` (success only, 14 days): the Release executable
  as built by CI. CI is not a release pipeline - nothing is published.
