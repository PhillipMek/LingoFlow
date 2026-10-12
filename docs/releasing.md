# Releasing LingoFlow via GitHub Actions

Release pipeline: `.github/workflows/release.yml` (name: **Release**), manual
only. CI (`.github/workflows/ci.yml`, name: **CI**) verifies and builds but
never publishes anything; a green CI run on `main`/`master` is the entry
ticket for a release.

## 1. How to publish a release

1. Make sure CI is green on `main`/`master` for the commit you want to ship
   (Actions -> CI -> the run). Note the **run ID** - it is the number at the
   end of the run URL (`.../actions/runs/<ID>`), and it is printed in the
   run's Job summary ("To release this verified run").
2. Actions -> **Release** -> **Run workflow** -> branch `main`/`master`:
   - `ci_run_id` - the ID from step 1;
   - `bump_type` - `patch` | `minor` | `major` (default `patch`).
   CLI equivalent: `gh workflow run release.yml -M master -f ci_run_id=<ID> -f bump_type=patch`
3. Wait. The workflow validates the run, rebuilds the exact commit, verifies
   the installer, and only then tags and publishes. The publish job's summary
   contains the links: GitHub Release, tag, asset download, SHA-256.

There is deliberately no "publish" button on the CI run itself - GitHub does
not let a run dispatch a workflow with prefilled inputs, and a fake button is
worse than none. The summary hands you the exact ID and the exact commands.

## 2. What the release is allowed to be built from

The only trusted input is the run ID; everything else is read back from the
GitHub API (`GET /repos/{owner}/{repo}/actions/runs/{id}`). The release is
refused unless ALL of the following hold:

- the run lives in this repository (forks are irrelevant and cannot publish);
- the run is the **CI** workflow (`.github/workflows/ci.yml`, name `CI`);
- it is completed with conclusion `success`;
- its event is `push` or `workflow_dispatch` (never a pull-request run);
- its branch is `main` or `master`;
- the source code is checked out by that run's `head_sha` and the working
  tree is re-locked to exactly that SHA (`git rev-parse HEAD` gate). Never
  "whatever the branch is now", never the Debug artifacts of another run:
  the Release workflow compiles a fresh Release build from the proven commit.

Inputs themselves are validated with strict allowlists (`bump_type` is
re-checked, because the API can pass arbitrary strings into `choice` inputs;
`ci_run_id` must be a positive integer). The dispatch ref must also be a
release branch - the pipeline logic must come from trunk, not from a feature
branch copy of the workflow.

## 3. Versioning (SemVer)

- Format: `vMAJOR.MINOR.PATCH` (e.g. `v1.4.2`). That is the only tag format
  treated as a release tag; anything else (`v1.4`, `pre-history-rewrite-*`,
  prerelease-shaped `v1.4.3-rc1`) is listed in the workflow log and IGNORED -
  prerelease tags have no role in this project's policy today.
- Current version = the numerically highest valid `v*` tag. The next version
  is `bump_type` applied to it: `1.4.2`+patch=`1.4.3`, +minor=`1.5.0`
  (patch reset), +major=`2.0.0` (minor and patch reset).
- **No tags at all** (this is the repository's current state): the documented
  seed version is used - `LINGOFLOW_VERSION_DEFAULT` from `CMakeLists.txt`
  (currently `0.1.0`), so the very first release will be `0.1.1` (patch),
  `0.2.0` (minor) or `1.0.0` (major). No guessing, no timestamps.
- If the version cannot be derived unambiguously, the run fails BEFORE any
  tag or release exists.

### Single source of version truth

The release version flows from ONE value into everything:

    Git tag vX.Y.Z
      -> CMake via the LINGOFLOW_VERSION environment variable (process-scoped;
         never a cache entry, so it cannot leak into the next local build)
      -> PROJECT_VERSION -> the exe's Windows version resource +
         JUCE_APPLICATION_VERSION_STRING (the version shown in the app)
      -> packaging/release-package.ps1 writes VERSION.txt and gates the exe's
         FileVersion against the tag version
      -> packaging/build-installer.ps1 stamps it into the installer
         (AppVersion, VersionInfoVersion, file name) and gates it
      -> packaging/verify-installer.ps1 checks tag == CMake banner ==
         app FileVersion == installer FileVersion == VERSION.txt
      -> the GitHub Release name/notes carry the same version.

A mismatch at any of these gates stops the pipeline before publication.
Local (non-release) builds keep using `LINGOFLOW_VERSION_DEFAULT`.

## 4. Tags, races, repeat runs

- Workflow-level `concurrency` serializes release pipelines: a second
  dispatch queues, it does not run in parallel.
- Immediately before touching Git state, the publish job re-reads the tag ref
  and the release-for-tag from the API and applies the decision table
  (`Resolve-LingoFlowTagClaim`, unit-tested):
  - tag free -> create it atomically via the Git refs API pointing at the
    proven `head_sha` (not the branch tip);
  - tag exists on the SAME proven commit and no release -> **Resume**
    (complete a publication that an earlier run created the tag for but did
    not finish);
  - tag exists on a DIFFERENT commit -> fail; tags are never moved;
  - release already published -> fail; releases are never overwritten.
- If ref creation loses a race (422/409), the run fails without modifying
  anything.
- After a failure where the tag was already created and the build+verify had
  passed (i.e. a publish-step crash), the safe recovery IS the normal retry:
  same `ci_run_id`, same `bump_type` -> Resume. If the tag was created but
  you have decided NOT to publish that version: delete the unused tag in
  Settings/UI or with `gh api -X DELETE repos/{owner}/{repo}/git/refs/tags/vX.Y.Z`
  (requires contents:write; confirm nobody published from it first).
- A release run that failed BEFORE the publish job never leaves Git state
  changed at all - just fix and re-dispatch.

## 5. What the build job actually runs

On `windows-latest` (same runner family as CI, pinned action SHAs shared):

1. checkout, `git fetch --depth 1 origin <proven-sha>` + detach + HEAD eq-gate;
2. `scripts/release/run-release-build.ps1`:
   - CMake Release configure with the version stamp (the stamped
     `PROJECT_VERSION` is verified from the configure banner);
   - `cmake --build ... --config Release --parallel` (never a Debug artifact);
   - the FULL registered ctest suite (`ctest -C Release`), same contract as
     CI, now including the `release_process_logic` gate itself;
   - `--dev --smoke` must exit 0; plain `--smoke` is classified with the
     product's documented semantics (0 PASS, 2 EXPECTED-HARDWARE-SKIP -
     runners have no ASIO devices - anything else FAILS the release);
3. `packaging/release-package.ps1` - licence-chain completeness gates, the
   exe FileVersion == tag gate, stray-DLL gate and the `dumpbin /DEPENDENTS`
   system-DLL-only gate (static CRT - see docs/release.md);
4. `scripts/release/ensure-inno-setup.ps1` - the Inno Setup 6 compiler
   (pre-installed on the runner; otherwise the pinned official artifact with
   SHA-256 + Authenticode-publisher verification, see section 8);
5. `packaging/build-installer.ps1` - stages ONLY the approved file list,
   compiles with ISCC, asserts the engine version (>= 6.3), name, size and
   version resource, writes the SHA-256 sidecar;
6. `packaging/verify-installer.ps1` - the automated installer verification
   (see section 6).

Only when 1-6 are green does the publish job touch the repository.

## 6. What is verified before publication, and what is NOT

Verified automatically, every release run:

- installer exists, is an MZ PE executable, is named
  `LingoFlow-Setup-<version>-x64.exe`, is > 1 MB, its version resource matches
  the tag;
- unattended install (`/VERYSILENT /CURRENTUSER /DIR=<test dir>`) exits 0 and
  produces exactly the approved content: `LingoFlow.exe`, `VERSION.txt`,
  `LICENSE`, `THIRD_PARTY_NOTICES.md`, `LICENSES/*` - no `.pdb`, no stray DLLs;
- Start Menu shortcut points at the installed exe; the desktop shortcut is
  NOT created by default (it is an opt-in task);
- the INSTALLED binary passes `--dev --smoke` (exit 0) - the
  hardware-independent chain from the app through mocks, without a device,
  key or network;
- unattended uninstall exits 0, removes the folder, the shortcut and the
  registry entry;
- user state survives: `%APPDATA%\LingoFlow` (settings, logs) and the Windows
  credential store are not touched by the installer in either direction;
- the bytes downloaded by the publish job hash (SHA-256) to exactly what the
  build job verified - the asset published is the asset tested, not a
  rebuilt or substituted one;
- the proven commit: the publish job's tree HEAD equals the CI-run SHA.

NOT verified automatically (honest limits):

- the verification runs on a GitHub runner (a developer-ish machine with
  Visual Studio etc.) - it is NOT the clean-machine test. The clean Windows
  10/11 x64 profile check remains a REQUIRED human checkpoint (docs/release.md,
  section "Clean-machine protocol");
- real ASIO/SoundGrid start, live OpenAI translation, a real NDI receiver,
  long-run stability - venue checkpoints (docs/rig-checklist.md);
- the workflow files themselves (release.yml) are static-checked by
  actionlint-equivalent rules + the `release-logic` CI job on every push, but
  the first true end-to-end run is the first real release.

## 7. Where the installer is

GitHub -> repository -> Releases -> `vX.Y.Z` -> `LingoFlow-Setup-X.Y.Z-x64.exe`
(+ `...exe.sha256`). The download page and the asset link are printed in the
release run's summary. Verify: `Get-FileHash .\LingoFlow-Setup-X.Y.Z-x64.exe
-Alg SHA256` must equal the sidecar/notes value.

Installer behaviour: per-user (no administrator) install into
`%LOCALAPPDATA%\Programs\LingoFlow` (same folder the scripted portable
install used, so upgrades are stable; the dialog lets a machine-wide install
be chosen explicitly), Start Menu shortcut, optional desktop icon, standard
Apps&Features uninstall. Settings, logs and the stored API key are separate
user state and survive installation and removal unchanged.

## 8. Runtime dependencies of the installer

Included by right: the app exe (static CRT - no Visual C++ redistributable
needed), VERSION.txt, the AGPLv3 licence chain. Inno Setup's license permits
distributing compiled installers; the compiler itself is a build-machine tool
and is not shipped. The vendor asks commercial users to purchase an Inno
Setup license (jrsoftware.org/isorder.php); using it without one is permitted
by LICENSE.TXT, the compiled setups are not marked, and CI installers are
built from the pinned artifact
`innosetup-6.7.3.exe` (immutable GitHub release, SHA-256
`9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732`,
Authenticode publisher "Pyrsys B.V.", verified at runtime when the fallback
path is used; the runner's pre-installed 6.x is verified by banner + the
`>= 6.3` engine-version gate on the real compile). Purchasing for a commercial
venue deployment is the product owner's decision - HUMAN CHECKPOINT.

NOT included by design (prerequisites documented at
docs/release.md "External driver prerequisites"):

- Waves SoundGrid / any ASIO driver - hardware-provided, vendor EULA forbids
  us shipping it; the app reports an honest device error without it and
  `--dev` works fully;
- NDI runtime (NewTek EULA) - loaded dynamically when present; subtitle
  output degrades gracefully without it; install the NDI runtime separately
  on a venue machine that needs subtitles;
- OpenAI API key - entered by the operator into the app (Windows credential
  store), never part of any package.

Debug symbols, PDBs, build tools, test binaries, user configs, tokens, logs:
never in the installer.

## 9. Required permissions and repository settings

Jobs use the default `GITHUB_TOKEN`; no PAT or custom secrets exist or are
needed anywhere in the pipeline.

| job | permissions | why |
|---|---|---|
| validate | `contents: read`, `actions: read` | read the run/tags/ref state via the API |
| build | `contents: read` | checkout the proven commit |
| publish | `contents: write`, `actions: read` | create the tag ref + release + upload assets; download this run's artifact |
| all CI jobs | `contents: read` | verify only |

Repository settings (owner):

- Settings -> Actions -> General -> Workflow permissions: **"Read repository
  contents and packages permissions"** (the per-job `contents: write`
  override for publish then applies; "Read and write" as the default also
  works but is broader than needed);
- the same page must allow Actions workflows (default) - the pipeline calls
  only same-repository endpoints with its own token;
- tag protection rules: none configured - if branch/tag protection is
  introduced later, allow the Release workflow to create tags or releases
  will start failing (403) with a clear error;
- releases from forks: impossible by construction (forks cannot dispatch
  workflows into the upstream repository, and the provenance check requires
  the run to be in this repository on a release branch).

## 10. After a failure - playbook

- red validate/build: no tag, no release, no Git state changed. Read the
  error, fix, re-dispatch. The tag-claim preview in the validate summary
  already tells you which version the run would take.
- red publish AFTER "Create the version tag" succeeded: the summary shows
  the recovery ladder. If the tag points at the proven commit and no release
  exists: re-dispatch the same run ID + bump -> Resume completes it.
  Never edit a tag by hand to "retry differently".
- a published version must never be republished or moved: to fix a shipped
  release, publish the NEXT patch version. Delete/rollback of published
  releases is outside the workflow and is a deliberate human action.
- release created but its assets are missing/unverified (the publish step
  says so in the run summary): the workflow will not touch that release.
  Attach the two files from the run's Artifacts (`release-installer`: the
  setup exe + checksum, already hash-verified in the publish job) manually
  on the release edit page, or leave it and ship the next patch. Which you
  pick is a human decision, never an automatic rewrite.
- concurrent same-version dispatches: they queue; the second one either
  aborts with "already published/existing tag on a different commit" or, for
  a bump recomputed against the now-higher tag, releases the next version.

## 11. Where release logic is tested

`scripts/release/tests/release-process.Tests.ps1` (pure PowerShell, no
network): 59+ checks over SemVer rules, dispatch-input allowlists, the
provenance table, claim/race decisions, naming, the version-consistency
matrix, notes assembly, and static invariants of both workflows and the
installer script (manual-only triggers, SHA-pinned actions, single
`contents: write`, no secrets, no hardcoded version in the .iss).

Runs: CI job `release-logic` (on both PowerShell 5.1 and 7), ctest entry
`release_process_logic` (Debug+Release suites), and the Release workflow
itself re-runs the suite from the proven commit before computing any version.
