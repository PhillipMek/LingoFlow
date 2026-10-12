#Requires -Version 5.1

<#
    LingoFlow release process logic.

    Pure functions only: no network, no filesystem side effects, no GitHub
    Actions contexts. This is the single place where release semantics live:

      - SemVer: tag format, latest release tag, next version by bump type;
      - release provenance: which CI run may be released and how it is checked;
      - tag claims: create / resume / abort decisions under races;
      - artifact naming and the version-consistency matrix
        (Git tag == CMake stamp == app exe == installer == VERSION.txt);
      - release notes assembly.

    Consumers:
      - .github/workflows/release.yml (validate / build / publish jobs)
      - packaging/build-installer.ps1, packaging/verify-installer.ps1,
        packaging/release-package.ps1, scripts/release/run-release-build.ps1
      - scripts/release/tests/release-process.Tests.ps1 - the gate registered
        in CI as job "release-logic" and in ctest as "release_process_logic".

    Must keep running on Windows PowerShell 5.1 AND PowerShell 7 (CI executes
    both shells; the release workflow steps use pwsh 7).
#>

# --- version / tag primitives --------------------------------------------------

function Test-LingoFlowVersion {
    # MAJOR.MINOR.PATCH, no v prefix, no leading zeros, no prerelease suffix.
    param([string]$Value)
    if ([string]::IsNullOrWhiteSpace($Value)) { return $false }
    return ($Value -cmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')
}

function Test-LingoFlowReleaseTag {
    # The one release tag format the project uses: vMAJOR.MINOR.PATCH.
    param([string]$Name)
    if ([string]::IsNullOrWhiteSpace($Name)) { return $false }
    return ($Name -cmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')
}

function Get-LingoFlowVersionParts {
    param([string]$Version)
    if (-not (Test-LingoFlowVersion $Version)) {
        throw "not a MAJOR.MINOR.PATCH version: '$Version'"
    }
    $m = [regex]::Match($Version, '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')
    return , @([int] $m.Groups[1].Value, [int] $m.Groups[2].Value, [int] $m.Groups[3].Value)
}

function Compare-LingoFlowVersion {
    # Numeric SemVer comparison (1.10.0 > 1.9.0); returns -1 / 0 / 1.
    param([string]$A, [string]$B)
    $pa = Get-LingoFlowVersionParts $A
    $pb = Get-LingoFlowVersionParts $B
    for ($i = 0; $i -lt 3; $i++) {
        if ($pa[$i] -gt $pb[$i]) { return 1 }
        if ($pa[$i] -lt $pb[$i]) { return -1 }
    }
    return 0
}

function Get-LingoFlowNormalizedVersion {
    # Map the shapes the real sources produce onto a bare MAJOR.MINOR.PATCH:
    # 'v1.4.3' (tag), '1.4.3' (VERSION.txt), '1.4.3.0' (Windows FileVersion
    # resource pads the fourth component). Anything else -> $null.
    param([string]$Value)
    if ($null -eq $Value) { return $null }
    $v = $Value.Trim()
    if ($v -cmatch '^v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(\.0)?$') {
        return ('{0}.{1}.{2}' -f [int] $Matches[1], [int] $Matches[2], [int] $Matches[3])
    }
    return $null
}

function Get-LingoFlowLatestReleaseTag {
    # Classify every Git tag name of the repository and return the highest
    # well-formed release tag. Prerelease-shaped tags (v1.2.3-rc1) and
    # anything else are reported but never used as the version base - the
    # project policy is stable releases only (docs/releasing.md).
    param([string[]]$TagNames)
    $result = [pscustomobject]@{
        Version    = $null   # '1.4.2' or $null when no valid release tag exists
        Tag        = $null   # 'v1.4.2' or $null
        Prerelease = @()     # v1.2.3-rc1-shaped names (ignored, reported)
        Ignored    = @()     # everything else (ignored, reported)
    }
    if ($null -eq $TagNames) { return $result }
    $releases = @()
    foreach ($name in $TagNames) {
        if ($null -eq $name -or $name -eq '') { continue }
        if (Test-LingoFlowReleaseTag $name) { $releases += $name.Substring(1); continue }
        if ($name -cmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)[-+].*$') {
            $result.Prerelease += $name
            continue
        }
        $result.Ignored += $name
    }
    if ($releases.Count -eq 0) { return $result }
    $best = $releases[0]
    foreach ($v in $releases) {
        if ((Compare-LingoFlowVersion $v $best) -gt 0) { $best = $v }
    }
    $result.Version = $best
    $result.Tag = "v$best"
    return $result
}

function Get-LingoFlowNextVersion {
    # Deterministic SemVer bump. v1.4.2: patch -> 1.4.3, minor -> 1.5.0,
    # major -> 2.0.0. Unknown bump -> hard error (no silent defaults here).
    param([string]$Version, [string]$Bump)
    $p = Get-LingoFlowVersionParts $Version
    switch ($Bump) {
        'patch' { return ('{0}.{1}.{2}' -f $p[0], $p[1], ($p[2] + 1)) }
        'minor' { return ('{0}.{1}.0' -f $p[0], ($p[1] + 1)) }
        'major' { return ('{0}.0.0' -f ($p[0] + 1)) }
        default { throw "bump type must be exactly one of patch|minor|major, got '$Bump'" }
    }
}

function Test-LingoFlowBumpType {
    param([string]$Value)
    # workflow_dispatch choice inputs are NOT enforced by the server (the API
    # accepts any string), so the release workflow applies this strict allowlist.
    return ($Value -ceq 'patch' -or $Value -ceq 'minor' -or $Value -ceq 'major')
}

function Test-LingoFlowRunId {
    param([string]$Value)
    # Positive integer, at most 19 decimal digits (fits [long]); no sign, no
    # spaces, no leading zeros - everything downstream fetches this id via
    # `gh api` and must never interpolate junk into a request.
    return ($Value -cmatch '^[1-9][0-9]{0,18}$')
}

function Get-LingoFlowRepoSeedVersion {
    # The documented initial version for a repository without release tags:
    # the CMakeLists default (one declaration, never a second copy).
    param([Parameter(Mandatory = $true)][string]$CMakeListsContent)
    $m = [regex]::Match($CMakeListsContent, 'set\(LINGOFLOW_VERSION_DEFAULT\s+"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"\)')
    if (-not $m.Success) {
        throw 'CMakeLists.txt must declare set(LINGOFLOW_VERSION_DEFAULT "MAJOR.MINOR.PATCH") - the documented seed version (docs/releasing.md)'
    }
    return ('{0}.{1}.{2}' -f $m.Groups[1].Value, $m.Groups[2].Value, $m.Groups[3].Value)
}

# --- artifact naming -------------------------------------------------------------

function Get-LingoFlowInstallerName {
    param([string]$Version)
    if (-not (Test-LingoFlowVersion $Version)) {
        throw "cannot name the installer: '$Version' is not a MAJOR.MINOR.PATCH version"
    }
    return "LingoFlow-Setup-$Version-x64.exe"
}

function Get-LingoFlowChecksumName {
    param([string]$InstallerName)
    if (-not ($InstallerName -cmatch '^LingoFlow-Setup-(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-x64\.exe$')) {
        throw "'$InstallerName' is not a LingoFlow installer artifact name"
    }
    return "$InstallerName.sha256"
}

function Test-Sha256Hex {
    param([string]$Value)
    return ($Value -cmatch '^[0-9a-f]{64}$')
}

function Test-GitCommitSha {
    param([string]$Value)
    return ($Value -cmatch '^[0-9a-f]{40}$')
}

# --- version consistency matrix ---------------------------------------------------

function Get-LingoFlowVersionConsistencyErrors {
    <#
        Compare the version sources that must never disagree:
          ExpectedVersion - the computed release version (authoritative here)
          Sources         - hashtable, any subset of these keys:
              tag        raw tag name ('v1.4.3')
              cmake      LINGOFLOW_VERSION as CMake reported it
              exe        Windows FileVersion of the app exe
              installer  Windows FileVersion of the compiled setup exe
              versiontxt VERSION.txt content
        A key that is ABSENT or explicitly $null is skipped (not part of this
        check); a string value is checked. An empty string means "the source
        exists but carries nothing" - an error. (Hashtable instead of [string]
        parameters on purpose: [string] coerces an omitted $null into '' and
        would turn "skipped" into five phantom errors.)
        Returns an array of human-readable errors; empty array == consistent.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$ExpectedVersion,
        [Parameter(Mandatory = $true)][hashtable]$Sources
    )
    if (-not (Test-LingoFlowVersion $ExpectedVersion)) {
        return , @("the computed release version '$ExpectedVersion' is not MAJOR.MINOR.PATCH")
    }
    $fields = [ordered]@{
        tag       = 'git tag'
        cmake     = 'cmake LINGOFLOW_VERSION'
        exe       = 'app exe FileVersion'
        installer = 'installer exe FileVersion'
        versiontxt = 'VERSION.txt'
    }
    $errors = @()
    foreach ($key in $fields.Keys) {
        if (-not $Sources.Contains($key)) { continue }
        $value = $Sources[$key]
        if ($null -eq $value) { continue }
        $raw = [string] $value
        if ($raw.Trim() -eq '') {
            $errors += "version consistency: $($fields[$key]) is empty - the value was expected to be '$ExpectedVersion'"
            continue
        }
        $norm = Get-LingoFlowNormalizedVersion $raw
        if ($null -eq $norm) {
            $errors += "version consistency: $($fields[$key]) carries '$raw', which is not a MAJOR.MINOR.PATCH version (a v prefix or a .0 FileVersion pad is tolerated)"
            continue
        }
        if ($norm -ne $ExpectedVersion) {
            $errors += "version consistency: $($fields[$key]) carries '$norm' but the release version is '$ExpectedVersion'"
        }
    }
    return , $errors
}

# --- CI run provenance -------------------------------------------------------------

function Get-LingoFlowCiRunProvenanceErrors {
    <#
        Validate the workflow run object fetched from the GitHub REST API
        (GET /repos/{owner}/{repo}/actions/runs/{run_id}) BEFORE anything is
        tagged or built. The user only supplies a run id; every claim checked
        here is read back from the API - never from the dispatch payload.

        Policy (docs/releasing.md):
          - the run must live in THIS repository;
          - it must be the project CI workflow (name + file path);
          - it must be finished with conclusion 'success';
          - it must be a branch run on an allowed release branch (main/master),
            not a pull_request run (PR refs are not publishable states);
          - its head_sha is the only commit the release may be built from and
            it must be a full lowercase commit id.
    #>
    param(
        [Parameter(Mandatory = $true)][psobject]$Run,
        [Parameter(Mandatory = $true)][string]$ExpectedRepositoryFullName,
        [Parameter(Mandatory = $true)][string]$ExpectedWorkflowPath,
        [Parameter(Mandatory = $true)][string]$ExpectedWorkflowName,
        [Parameter(Mandatory = $true)][string]$ExpectedRunId,
        [string[]]$AllowedEvents = @('push', 'workflow_dispatch'),
        [string[]]$AllowedHeadBranches = @('main', 'master')
    )
    $errors = @()

    if (-not (Test-LingoFlowRunId $ExpectedRunId)) {
        $errors += "provenance: requested run id '$ExpectedRunId' is not a positive integer"
    }
    if ([string] $Run.id -ne [string] $ExpectedRunId) {
        $errors += "provenance: the run id returned by the API ($($Run.id)) does not match the requested run id ($ExpectedRunId)"
    }
    if ([string] $Run.repository.full_name -ne $ExpectedRepositoryFullName) {
        $errors += "provenance: the run belongs to repository '$($Run.repository.full_name)', not $ExpectedRepositoryFullName"
    }
    if ([string] $Run.path -ne ('{0}: {1}' -f $ExpectedRepositoryFullName, $ExpectedWorkflowPath)) {
        $errors += "provenance: the run was not produced by '$ExpectedWorkflowPath' in $ExpectedRepositoryFullName (run path: '$($Run.path)')"
    }
    if ([string] $Run.name -ne $ExpectedWorkflowName) {
        $errors += "provenance: the run workflow is named '$($Run.name)', expected '$ExpectedWorkflowName'"
    }
    if ([string] $Run.status -ne 'completed') {
        $errors += "provenance: the run is not finished (status '$($Run.status)'); releases need a completed run"
    }
    if ([string] $Run.conclusion -ne 'success') {
        $errors += "provenance: the run conclusion is '$($Run.conclusion)', not 'success'; an unsuccessful CI run is never released"
    }
    if (@($AllowedEvents) -notcontains [string] $Run.event) {
        $errors += "provenance: the run event '$($Run.event)' is not a release-eligible event (allowed: $($AllowedEvents -join ', '))"
    }
    if (@($AllowedHeadBranches) -notcontains [string] $Run.head_branch) {
        $errors += "provenance: the run branch '$($Run.head_branch)' is not a release branch (allowed: $($AllowedHeadBranches -join ', '))"
    }
    if (-not (Test-GitCommitSha ([string] $Run.head_sha))) {
        $errors += "provenance: the run head_sha '$($Run.head_sha)' is not a 40-hex commit id"
    }

    return , $errors
}

# --- tag claim / concurrency ---------------------------------------------------------

function Resolve-LingoFlowTagClaim {
    <#
        Decide what the publish step may do about the version tag, given the
        CURRENT server state (re-read immediately before the decision, so two
        concurrent runs cannot both claim it - ref creation itself is atomic).

        Returns @{ Action = 'Create' | 'Resume' | 'Abort'; Reason = '...' }

          Create - the tag is free: create it on the proven commit now.
          Resume - an earlier run created this exact tag on this exact commit
                   but failed before publishing the release: completing the
                   publication for that tag is safe and idempotent.
          Abort  - the tag exists elsewhere or a release already exists for
                   it: published history is never moved or overwritten.
    #>
    param(
        [bool]$TagRefExists,
        [string]$TagTargetSha,
        [bool]$ReleaseExists,
        [string]$ExpectedSha
    )
    if (-not (Test-GitCommitSha $ExpectedSha)) {
        return @{ Action = 'Abort'; Reason = "the proven commit id '$ExpectedSha' is not a 40-hex sha; refusing to touch tags" }
    }
    if (-not $TagRefExists) {
        if ($ReleaseExists) {
            return @{ Action = 'Abort'; Reason = 'a GitHub Release exists for this tag while the tag ref itself is missing - inspect the repository state manually; the workflow will not touch either' }
        }
        return @{ Action = 'Create'; Reason = 'the tag is free; it will be created on the proven commit' }
    }
    if (-not (Test-GitCommitSha $TagTargetSha)) {
        return @{ Action = 'Abort'; Reason = "the tag ref exists but its target commit could not be resolved ('$TagTargetSha'); refusing to move a tag whose state is unknown" }
    }
    if ($TagTargetSha -ne $ExpectedSha) {
        return @{ Action = 'Abort'; Reason = "the tag already exists and points to $TagTargetSha while this run would release $ExpectedSha; published tags are never moved" }
    }
    if ($ReleaseExists) {
        return @{ Action = 'Abort'; Reason = 'this version was already published as a GitHub Release for this exact commit; repeat publishing is refused, the existing release is untouched' }
    }
    return @{ Action = 'Resume'; Reason = 'the tag was created by an earlier run on this exact commit but no release exists for it; completing the publication without moving the tag' }
}

# --- release notes ----------------------------------------------------------------------

function New-LingoFlowReleaseNotes {
    <#
        Assemble the GitHub Release body from verified facts only: the version,
        the proven commit, the CI run, the installer checksum, and (when a
        previous release tag exists) the commit list the compare API returned.
        No invented prose about features.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$CommitSha,
        [Parameter(Mandatory = $true)][string]$CiRunUrl,
        [Parameter(Mandatory = $true)][string]$RepositoryFullName,
        [Parameter(Mandatory = $true)][string]$InstallerName,
        [Parameter(Mandatory = $true)][string]$InstallerSha256,
        [AllowNull()][string]$PreviousTag,
        [string[]]$CommitLines = @()
    )
    if (-not (Test-LingoFlowVersion $Version)) { throw "release notes: bad version '$Version'" }
    if (-not (Test-GitCommitSha $CommitSha)) { throw "release notes: bad commit sha '$CommitSha'" }
    if (-not (Test-Sha256Hex $InstallerSha256)) { throw "release notes: installer sha256 must be 64 lowercase hex chars, got '$InstallerSha256'" }
    $expectedInstaller = Get-LingoFlowInstallerName $Version
    if ($InstallerName -ne $expectedInstaller) { throw "release notes: installer '$InstallerName' does not match the expected '$expectedInstaller'" }
    if ($PreviousTag -and -not (Test-LingoFlowReleaseTag $PreviousTag)) { throw "release notes: previous tag '$PreviousTag' is not a release tag" }

    $tag = "v$Version"
    $lines = @()
    $lines += "# LingoFlow $tag"
    $lines += ''
    $lines += 'Windows x64 release, built and verified by the GitHub Actions Release workflow from a CI run that passed on this exact commit.'
    $lines += ''
    $lines += '| | |'
    $lines += '|---|---|'
    $lines += "| Version | ``$Version`` (Git tag ``$tag``) |"
    $lines += "| Commit | ``$CommitSha`` |"
    $lines += "| Source CI run | [$CiRunUrl]($CiRunUrl) |"
    $lines += ''
    $lines += '## Downloads'
    $lines += ''
    $lines += "- **[$InstallerName](https://github.com/$RepositoryFullName/releases/download/$tag/$InstallerName)** - Windows 10/11 x64 installer (per-user install, no administrator required)."
    $lines += "- Checksum: ``$InstallerSha256`` (`$InstallerName.sha256`, sha256sum format, published next to the asset)."
    $lines += ''
    $lines += 'The installer contains the application executable, `VERSION.txt` and the AGPLv3 licence chain (`LICENSE`, `THIRD_PARTY_NOTICES.md`, `LICENSES/`). The build links the static CRT, so no Visual C++ redistributable is needed. Debug symbols are not included.'
    $lines += ''
    $lines += 'Not bundled by design (see `docs/release.md` and `docs/licensing.md`): the NDI runtime (NewTek EULA - install it separately when subtitle output is used), ASIO / Waves SoundGrid drivers (hardware-provided), and the OpenAI API key (entered in the app; stored in the Windows Credential Manager).'
    $lines += ''
    if ($PreviousTag) {
        $lines += "## Changes since $PreviousTag"
        $lines += ''
        if (@($CommitLines).Count -gt 0) {
            foreach ($c in @($CommitLines)) { $lines += "- $c" }
        }
        else {
            $lines += '- (the compare API returned no commit list; nothing is invented here)'
        }
    }
    else {
        $lines += '## Changes'
        $lines += ''
        $lines += '- First tagged release: no previous `v*` release tag exists to diff against, so no change list is claimed.'
    }
    $lines += ''
    $lines += '## Before running it at a venue'
    $lines += ''
    $lines += '- The installer is not code-signed: Windows SmartScreen may warn on first run (documented in `docs/releasing.md`).'
    $lines += '- Real SoundGrid/ASIO routing, a live OpenAI session, a real NDI receiver and the clean-machine check remain REQUIRED human checkpoints: `docs/rig-checklist.md`, `docs/release.md`.'
    return ($lines -join "`n")
}
