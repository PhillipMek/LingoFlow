#Requires -Version 5.1

<#
    Release process logic tests (task: secure CI-driven releases).

    This is the gate that protects the release decisions themselves: SemVer
    derivation, dispatch-input validation, CI run provenance rules, tag claim
    / concurrent publish decisions, artifact naming, the version consistency
    matrix, release notes assembly, and static invariants of the workflow
    files and installer script (manual-only publishing, SHA-pinned actions,
    no secrets, no hardcoded version in the installer source).

    Pure PowerShell against fixture data: no network, no GitHub, no build.
    Runs on Windows PowerShell 5.1 and PowerShell 7 alike.

    Registered in three places on purpose:
      - .github/workflows/ci.yml            job "release-logic" (both shells)
      - tests/CMakeLists.txt                ctest "release_process_logic"
      - .github/workflows/release.yml       "validate" job re-runs it from
                                             the proven commit before tagging
#>

param()

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $repoRoot 'scripts/release/release-lib.ps1')

$script:passCount = 0
$script:failures = @()
$script:index = 0

function Invoke-Check {
    param([string]$Name, [scriptblock]$Body)
    $script:index++
    try {
        $null = & $Body
        $script:passCount++
        Write-Host ('ok   {0:D3}  {1}' -f $script:index, $Name)
    }
    catch {
        $script:failures += ('{0:D3} {1}: {2}' -f $script:index, $Name, $_.Exception.Message)
        Write-Host ('FAIL {0:D3}  {1}: {2}' -f $script:index, $Name, $_.Exception.Message)
    }
}

function Assert-True {
    param($Condition, [string]$Message = 'condition is false')
    if (-not $Condition) { throw $Message }
}

function Assert-Equal {
    param($Expected, $Actual, [string]$Message)
    if ($Expected -ne $Actual) { throw "$Message (expected '$Expected', got '$Actual')" }
}

function Assert-Throws {
    param([scriptblock]$Body, [string]$Message = 'expected a terminating error')
    try { $null = & $Body } catch { return }
    throw $Message
}

function Read-RepoFile {
    param([string]$RelativePath)
    $full = Join-Path $repoRoot $RelativePath
    if (-not (Test-Path $full)) { throw "required repository file is missing: $RelativePath" }
    return (Get-Content -Path $full -Raw)
}

$goodSha = '0123456789abcdef0123456789abcdef01234567'
$otherSha = '9999990000001111222233334444555566667777'

function New-FakeCiRun {
    param(
        [string]$Id = '12345678901',
        [string]$Name = 'CI',
        [string]$Path = 'PhillipMek/LingoFlow: .github/workflows/ci.yml',
        [string]$Status = 'completed',
        [string]$Conclusion = 'success',
        [string]$Event = 'push',
        [string]$Branch = 'master',
        [string]$Sha = '0123456789abcdef0123456789abcdef01234567',
        [string]$Repo = 'PhillipMek/LingoFlow'
    )
    $template = @"
{
  "id": __ID__,
  "name": "__NAME__",
  "path": "__PATH__",
  "status": "__STATUS__",
  "conclusion": "__CONCLUSION__",
  "event": "__EVENT__",
  "head_branch": "__BRANCH__",
  "head_sha": "__SHA__",
  "html_url": "https://github.com/__REPO__/actions/runs/__ID__",
  "repository": { "full_name": "__REPO__" }
}
"@
    $json = $template.Replace('__ID__', $Id).Replace('__NAME__', $Name).Replace('__PATH__', $Path)
    $json = $json.Replace('__STATUS__', $Status).Replace('__CONCLUSION__', $Conclusion).Replace('__EVENT__', $Event)
    $json = $json.Replace('__BRANCH__', $Branch).Replace('__SHA__', $Sha).Replace('__REPO__', $Repo)
    return ($json | ConvertFrom-Json)
}

function Get-FakeProvenanceErrors {
    param([psobject]$Run, [string]$ExpectedRunId = '12345678901')
    return (Get-LingoFlowCiRunProvenanceErrors -Run $Run `
        -ExpectedRepositoryFullName 'PhillipMek/LingoFlow' `
        -ExpectedWorkflowPath '.github/workflows/ci.yml' `
        -ExpectedWorkflowName 'CI' `
        -ExpectedRunId $ExpectedRunId)
}

Write-Host ("release-process logic tests (PowerShell {0})" -f $PSVersionTable.PSVersion)
Write-Host ''

# --- 1. function inventory (the workflows dot-source exactly these) --------------

Invoke-Check 'release-lib exposes the documented function set' {
    foreach ($name in @(
            'Test-LingoFlowVersion', 'Test-LingoFlowReleaseTag', 'Get-LingoFlowVersionParts',
            'Compare-LingoFlowVersion', 'Get-LingoFlowNormalizedVersion', 'Get-LingoFlowLatestReleaseTag',
            'Get-LingoFlowNextVersion', 'Test-LingoFlowBumpType', 'Test-LingoFlowRunId',
            'Get-LingoFlowRepoSeedVersion', 'Get-LingoFlowInstallerName', 'Get-LingoFlowChecksumName',
            'Test-Sha256Hex', 'Test-GitCommitSha', 'Get-LingoFlowVersionConsistencyErrors',
            'Get-LingoFlowCiRunProvenanceErrors', 'Resolve-LingoFlowTagClaim', 'New-LingoFlowReleaseNotes')) {
        Assert-True (Get-Command $name -ErrorAction SilentlyContinue) "missing function: $name"
    }
}

# --- 2. SemVer: bump types against the task's examples ----------------------------

Invoke-Check 'patch bump v1.4.2 -> 1.4.3' { Assert-Equal '1.4.3' (Get-LingoFlowNextVersion '1.4.2' 'patch') 'patch bump' }
Invoke-Check 'minor bump v1.4.2 -> 1.5.0' { Assert-Equal '1.5.0' (Get-LingoFlowNextVersion '1.4.2' 'minor') 'minor bump' }
Invoke-Check 'major bump v1.4.2 -> 2.0.0' { Assert-Equal '2.0.0' (Get-LingoFlowNextVersion '1.4.2' 'major') 'major bump' }
Invoke-Check 'minor bump is numeric, not lexical (1.9.9 -> 1.10.0)' { Assert-Equal '1.10.0' (Get-LingoFlowNextVersion '1.9.9' 'minor') 'numeric minor' }
Invoke-Check 'major bump resets minor and patch (0.9.9 -> 1.0.0)' { Assert-Equal '1.0.0' (Get-LingoFlowNextVersion '0.9.9' 'major') 'numeric major' }
Invoke-Check 'patch bump 0.0.0 -> 0.0.1' { Assert-Equal '0.0.1' (Get-LingoFlowNextVersion '0.0.0' 'patch') 'zero base' }
Invoke-Check 'bump rejects a malformed base version' { Assert-Throws { Get-LingoFlowNextVersion '1.4' 'patch' } }
Invoke-Check 'bump rejects a prerelease base version' { Assert-Throws { Get-LingoFlowNextVersion '1.4.3-rc1' 'patch' } }
Invoke-Check 'bump rejects an unknown bump type' { Assert-Throws { Get-LingoFlowNextVersion '1.4.2' 'trivial' } }

# --- 3. tag classification ---------------------------------------------------------

Invoke-Check 'release tag format is strict vMAJOR.MINOR.PATCH' {
    Assert-True (Test-LingoFlowReleaseTag 'v1.4.2') 'v1.4.2 is valid'
    Assert-True (-not (Test-LingoFlowReleaseTag '1.4.2')) 'no v prefix is not a release tag'
    Assert-True (-not (Test-LingoFlowReleaseTag 'v1.4')) 'two components are not a release tag'
    Assert-True (-not (Test-LingoFlowReleaseTag 'v1.4.2.1')) 'four components are not a release tag'
    Assert-True (-not (Test-LingoFlowReleaseTag 'v01.4.2')) 'leading zeros are not a release tag'
    Assert-True (-not (Test-LingoFlowReleaseTag 'v1.4.3-rc1')) 'prerelease is not a stable release tag'
}

Invoke-Check 'latest tag picks the highest valid release tag numerically' {
    $r = Get-LingoFlowLatestReleaseTag @('v0.9.0', 'v1.9.0', 'v1.10.0', 'v1.4.2')
    Assert-Equal '1.10.0' $r.Version 'highest is numeric'
    Assert-Equal 'v1.10.0' $r.Tag 'tag form'
}

Invoke-Check 'invalid and prerelease tags are ignored and reported, never used' {
    $r = Get-LingoFlowLatestReleaseTag @(
        'pre-history-rewrite-2026-10-06',   # the only tag that exists today
        'v1.4.2', 'v1.4', '1.4.2', 'v1.4.2.1', 'release-2', 'v01.4.2',
        'v1.4.3-rc1', 'v2.0.0-alpha+meta')
    Assert-Equal '1.4.2' $r.Version 'only valid release tags count'
    Assert-True (@($r.Prerelease) -contains 'v1.4.3-rc1') 'prerelease is reported'
    Assert-True (@($r.Prerelease) -contains 'v2.0.0-alpha+meta') 'build-metadata prerelease is reported'
    Assert-True (@($r.Ignored) -contains 'pre-history-rewrite-2026-10-06') 'legacy tag is ignored, reported'
    Assert-True (@($r.Ignored) -contains 'v1.4') 'malformed tag is ignored, reported'
}

Invoke-Check 'no tags at all yields no version (caller must fall back to the seed)' {
    $r = Get-LingoFlowLatestReleaseTag @()
    Assert-Equal $null $r.Version 'no version'
    $r2 = Get-LingoFlowLatestReleaseTag @('nonsense', 'v1.4')
    Assert-Equal $null $r2.Version 'only invalid tags -> still no version'
}

Invoke-Check 'duplicate tag names resolve deterministically' {
    $r = Get-LingoFlowLatestReleaseTag @('v1.4.2', 'v1.4.2', 'v1.3.0')
    Assert-Equal '1.4.2' $r.Version 'dedup by value is fine'
}

# --- 4. seed version from CMakeLists (single declaration, no guessing) -------------

Invoke-Check 'the repository seed version parses out of CMakeLists.txt' {
    $seed = Get-LingoFlowRepoSeedVersion (Read-RepoFile 'CMakeLists.txt')
    Assert-True (Test-LingoFlowVersion $seed) "seed '$seed' must be MAJOR.MINOR.PATCH"
    Assert-Equal '0.1.0' $seed 'the documented seed for the current project state'
}

Invoke-Check 'a CMakeLists without the seed declaration is a hard error' {
    Assert-Throws { Get-LingoFlowRepoSeedVersion 'project(LingoFlow VERSION 0.1.0)' } 'no seed declaration -> throw'
}

# --- 5. dispatch input validation (strict allowlists) ------------------------------

Invoke-Check 'bump type allowlist is exact and case sensitive' {
    Assert-True (Test-LingoFlowBumpType 'patch') 'patch'
    Assert-True (Test-LingoFlowBumpType 'minor') 'minor'
    Assert-True (Test-LingoFlowBumpType 'major') 'major'
    Assert-True (-not (Test-LingoFlowBumpType 'PATCH')) 'uppercase rejected (workflow_choice is not enforced by the server)'
    Assert-True (-not (Test-LingoFlowBumpType 'patch ')) 'trailing space rejected'
    Assert-True (-not (Test-LingoFlowBumpType '')) 'empty rejected'
}

Invoke-Check 'run id validation is a strict positive integer' {
    Assert-True (Test-LingoFlowRunId '1') 'smallest'
    Assert-True (Test-LingoFlowRunId '1234567890123456789') 'largest 19-digit value'
    foreach ($bad in @('', '0', '01', '-7', '1e3', '12 ', ' 12', '1.5', 'abc', '123456789012345678901')) {
        Assert-True (-not (Test-LingoFlowRunId $bad)) "must reject: '$bad'"
    }
}

# --- 6. CI run provenance ------------------------------------------------------------

Invoke-Check 'provenance: a green CI run on master passes all gates' {
    $errors = Get-FakeProvenanceErrors (New-FakeCiRun)
    Assert-Equal 0 @($errors).Count 'no errors expected, got:' + ($errors -join '; ')
}

Invoke-Check 'provenance: a workflow_dispatch run on main is equally eligible' {
    $errors = Get-FakeProvenanceErrors (New-FakeCiRun -Event 'workflow_dispatch' -Branch 'main')
    Assert-Equal 0 @($errors).Count 'dispatch runs on the trunk are eligible'
}

Invoke-Check 'provenance: an unsuccessful run is rejected' {
    $errors = Get-FakeProvenanceErrors (New-FakeCiRun -Conclusion 'failure')
    Assert-True (@($errors | Where-Object { $_ -match 'not .success.' }).Count -ge 1) 'conclusion failure must error'
}

Invoke-Check 'provenance: an unfinished run is rejected' {
    $errors = Get-FakeProvenanceErrors (New-FakeCiRun -Status 'in_progress' -Conclusion '')
    Assert-True (@($errors | Where-Object { $_ -match 'not finished' }).Count -ge 1) 'in_progress must error'
}

Invoke-Check 'provenance: a pull_request run is rejected (not a publishable state)' {
    $errors = Get-FakeProvenanceErrors (New-FakeCiRun -Event 'pull_request' -Branch 'feature/ndi')
    Assert-True (@($errors | Where-Object { $_ -match 'not a release-eligible event' }).Count -ge 1) 'PR event must error'
    Assert-True (@($errors | Where-Object { $_ -match 'not a release branch' }).Count -ge 1) 'PR branch must error'
}

Invoke-Check 'provenance: a run from another workflow is rejected' {
    $errors = Get-FakeProvenanceErrors (New-FakeCiRun -Path 'PhillipMek/LingoFlow: .github/workflows/release.yml' -Name 'Release')
    Assert-True (@($errors | Where-Object { $_ -match 'not produced by' }).Count -ge 1) 'foreign workflow path must error'
    Assert-True (@($errors | Where-Object { $_ -match 'expected .CI.' }).Count -ge 1) 'foreign workflow name must error'
}

Invoke-Check 'provenance: a run from another repository is rejected' {
    $errors = Get-FakeProvenanceErrors (New-FakeCiRun -Repo 'someone-else/LingoFlow-fork' -Path 'someone-else/LingoFlow-fork: .github/workflows/ci.yml')
    Assert-True (@($errors | Where-Object { $_ -match 'belongs to repository' }).Count -ge 1) 'fork repository must error'
}

Invoke-Check 'provenance: a bogus head_sha or an id mismatch is rejected' {
    $errors = Get-FakeProvenanceErrors (New-FakeCiRun -Sha 'not-a-sha')
    Assert-True (@($errors | Where-Object { $_ -match 'not a 40-hex' }).Count -ge 1) 'malformed sha must error'
    $errors = Get-FakeProvenanceErrors (New-FakeCiRun -Id '777')
    Assert-True (@($errors | Where-Object { $_ -match 'does not match the requested' }).Count -ge 1) 'id mismatch must error'
    $errors = Get-FakeProvenanceErrors (New-FakeCiRun) 'not-a-number'
    Assert-True (@($errors | Where-Object { $_ -match 'not a positive integer' }).Count -ge 1) 'bad requested id must error'
}

Invoke-Check 'provenance: an uppercase (non-canonical) sha is rejected' {
    $errors = Get-FakeProvenanceErrors (New-FakeCiRun -Sha $goodSha.ToUpperInvariant())
    Assert-True (@($errors | Where-Object { $_ -match 'not a 40-hex' }).Count -ge 1) 'uppercase sha must error'
}

# --- 7. tag claim / concurrency decision table ---------------------------------------

Invoke-Check 'claim: free tag + no release -> Create' {
    $d = Resolve-LingoFlowTagClaim -TagRefExists $false -TagTargetSha '' -ReleaseExists $false -ExpectedSha $goodSha
    Assert-Equal 'Create' $d.Action 'action'
}
Invoke-Check 'claim: tag exists on the same commit, no release -> Resume (partial-failure recovery)' {
    $d = Resolve-LingoFlowTagClaim -TagRefExists $true -TagTargetSha $goodSha -ReleaseExists $false -ExpectedSha $goodSha
    Assert-Equal 'Resume' $d.Action 'action'
}
Invoke-Check 'claim: tag exists on a different commit -> Abort (never move a tag)' {
    $d = Resolve-LingoFlowTagClaim -TagRefExists $true -TagTargetSha $otherSha -ReleaseExists $false -ExpectedSha $goodSha
    Assert-Equal 'Abort' $d.Action 'action'
    Assert-True ($d.Reason -match 'never moved') 'reason'
}
Invoke-Check 'claim: released already -> Abort (never republish or overwrite)' {
    $d = Resolve-LingoFlowTagClaim -TagRefExists $true -TagTargetSha $goodSha -ReleaseExists $true -ExpectedSha $goodSha
    Assert-Equal 'Abort' $d.Action 'action'
}
Invoke-Check 'claim: release without a tag ref -> Abort (anomalous state)' {
    $d = Resolve-LingoFlowTagClaim -TagRefExists $false -TagTargetSha '' -ReleaseExists $true -ExpectedSha $goodSha
    Assert-Equal 'Abort' $d.Action 'action'
}
Invoke-Check 'claim: a concurrent run that grabbed the tag mid-flight turns Create into Abort' {
    # simulate: the pre-check saw no tag, the atomic create lost the race, the
    # publish step re-reads state -> now the tag exists on someone else's sha.
    $d = Resolve-LingoFlowTagClaim -TagRefExists $true -TagTargetSha $otherSha -ReleaseExists $true -ExpectedSha $goodSha
    Assert-Equal 'Abort' $d.Action 'race lost -> abort, not overwrite'
}
Invoke-Check 'claim: an unresolvable tag target -> Abort' {
    $d = Resolve-LingoFlowTagClaim -TagRefExists $true -TagTargetSha '' -ReleaseExists $false -ExpectedSha $goodSha
    Assert-Equal 'Abort' $d.Action 'unknown target must abort'
}
Invoke-Check 'claim: an invalid expected sha poisons every branch into Abort' {
    foreach ($t in @(@($false, ''), @($true, $goodSha))) {
        $d = Resolve-LingoFlowTagClaim -TagRefExists $t[0] -TagTargetSha $t[1] -ReleaseExists $false -ExpectedSha 'nope'
        Assert-Equal 'Abort' $d.Action ("invalid ExpectedSha with tagref=$($t[0])")
    }
}

# --- 8. artifact naming ---------------------------------------------------------------

Invoke-Check 'installer name carries product, version and architecture' {
    Assert-Equal 'LingoFlow-Setup-1.4.3-x64.exe' (Get-LingoFlowInstallerName '1.4.3') 'name'
    Assert-Equal 'LingoFlow-Setup-1.4.3-x64.exe.sha256' (Get-LingoFlowChecksumName 'LingoFlow-Setup-1.4.3-x64.exe') 'sidecar'
}
Invoke-Check 'naming rejects malformed versions and foreign names' {
    Assert-Throws { Get-LingoFlowInstallerName '1.4' } '1.4 must throw'
    Assert-Throws { Get-LingoFlowInstallerName 'v1.4.3' } 'v prefix must throw'
    Assert-Throws { Get-LingoFlowChecksumName 'NotLingoFlow.exe' } 'foreign name must throw'
}
Invoke-Check 'sha256 and commit sha validators are strict' {
    Assert-True (Test-Sha256Hex ('ab' * 32)) '64 lowercase hex'
    Assert-True (-not (Test-Sha256Hex ('AB' * 32))) 'uppercase rejected'
    Assert-True (Test-GitCommitSha $goodSha) '40 hex'
    Assert-True (-not (Test-GitCommitSha $goodSha.Substring(0, 39))) '39 hex rejected'
}

# --- 9. version consistency matrix ------------------------------------------------------

Invoke-Check 'consistency: all five sources agree (real-world shapes)' {
    $errors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion '1.4.3' -Sources @{
        tag = 'v1.4.3'; cmake = '1.4.3'; exe = '1.4.3.0'; installer = '1.4.3.0'; versiontxt = '1.4.3'
    }
    Assert-Equal 0 @($errors).Count ('must be consistent, got: ' + ($errors -join '; '))
}
Invoke-Check 'consistency: app exe lagging the tag is caught (the four-way drift case)' {
    $errors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion '1.4.3' -Sources @{ tag = 'v1.4.3'; exe = '0.1.0' }
    Assert-Equal 1 @($errors).Count 'one error'
    Assert-True ($errors[0] -match 'app exe') 'names the source'
}
Invoke-Check 'consistency: empty source is an error, absent/$null source is skipped' {
    $errors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion '1.4.3' -Sources @{ versiontxt = '' }
    Assert-True (@($errors | Where-Object { $_ -match 'VERSION\.txt is empty' }).Count -eq 1) 'empty VERSION.txt errors'
    $errors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion '1.4.3' -Sources @{}
    Assert-Equal 0 @($errors).Count 'no sources -> no errors'
    $errors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion '1.4.3' -Sources @{ exe = $null; versiontxt = '1.4.3' }
    Assert-Equal 0 @($errors).Count 'explicit $null skipped, real value checked'
}
Invoke-Check 'consistency: garbage version shapes are refused, not coerced' {
    $errors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion '1.4.3' -Sources @{ exe = '1.4.3-rc1' }
    Assert-True (@($errors | Where-Object { $_ -match 'not a MAJOR\.MINOR\.PATCH' }).Count -eq 1) 'prerelease exe stamp refused'
    $errors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion '1.4.3' -Sources @{ tag = 'v1.4.3.1' }
    Assert-Equal 1 @($errors).Count 'four-component tag refused'
}
Invoke-Check 'consistency: an unparsable expected version short-circuits with one clear error' {
    $errors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion '1.4' -Sources @{ exe = '1.4' }
    Assert-Equal 1 @($errors).Count 'single error'
}

# --- 10. release notes assembly ------------------------------------------------------------

Invoke-Check 'notes: contain only verified facts, all present' {
    $body = New-LingoFlowReleaseNotes -Version '1.4.3' -CommitSha $goodSha `
        -CiRunUrl 'https://github.com/PhillipMek/LingoFlow/actions/runs/12345678901' `
        -RepositoryFullName 'PhillipMek/LingoFlow' `
        -InstallerName 'LingoFlow-Setup-1.4.3-x64.exe' -InstallerSha256 ('ab' * 32) `
        -PreviousTag 'v1.4.2' -CommitLines @('abc1234 - fix: audio session lifecycle', 'def5678 - feat: NDI subtitles')
    Assert-True ($body -match '# LingoFlow v1.4.3') 'title'
    Assert-True ($body -match [regex]::Escape('12345678901')) 'run reference'
    Assert-True ($body -match $goodSha.Substring(0, 10)) 'commit reference'
    Assert-True ($body -match 'Changes since v1\.4\.2') 'diff header'
    Assert-True ($body -match 'fix: audio session lifecycle') 'commit line from compare API'
    Assert-True ($body -match 'LingoFlow-Setup-1\.4\.3-x64\.exe') 'asset name'
    Assert-True ($body -match ('ab' * 32)) 'checksum'
    Assert-True ($body -notmatch 'First tagged release') 'a diff exists here, the first-release wording must not appear'
}

Invoke-Check 'notes: first release (no previous tag) states there is no diff - and does not invent one' {
    $body = New-LingoFlowReleaseNotes -Version '0.1.1' -CommitSha $goodSha `
        -CiRunUrl 'https://github.com/PhillipMek/LingoFlow/actions/runs/1' `
        -RepositoryFullName 'PhillipMek/LingoFlow' `
        -InstallerName 'LingoFlow-Setup-0.1.1-x64.exe' -InstallerSha256 ('01' * 32) `
        -PreviousTag '' -CommitLines @()
    Assert-True ($body -match 'First tagged release') 'first-release wording'
    Assert-True ($body -notmatch '## Changes since') 'no fake diff header'
}

Invoke-Check 'notes: garbage in is refused (no mismatched versions reach a release body)' {
    Assert-Throws {
        New-LingoFlowReleaseNotes -Version 'v1.4.3' -CommitSha $goodSha -CiRunUrl 'x' -RepositoryFullName 'a/b' `
            -InstallerName 'LingoFlow-Setup-1.4.3-x64.exe' -InstallerSha256 ('ab' * 32)
    } 'version with v prefix'
    Assert-Throws {
        New-LingoFlowReleaseNotes -Version '1.4.3' -CommitSha 'nope' -CiRunUrl 'x' -RepositoryFullName 'a/b' `
            -InstallerName 'LingoFlow-Setup-1.4.3-x64.exe' -InstallerSha256 ('ab' * 32)
    } 'bad sha'
    Assert-Throws {
        New-LingoFlowReleaseNotes -Version '1.4.3' -CommitSha $goodSha -CiRunUrl 'x' -RepositoryFullName 'a/b' `
            -InstallerName 'LingoFlow-Setup-9.9.9-x64.exe' -InstallerSha256 ('ab' * 32)
    } 'installer name from another version'
    Assert-Throws {
        New-LingoFlowReleaseNotes -Version '1.4.3' -CommitSha $goodSha -CiRunUrl 'x' -RepositoryFullName 'a/b' `
            -InstallerName 'LingoFlow-Setup-1.4.3-x64.exe' -InstallerSha256 'deadbeef'
    } 'truncated checksum'
    Assert-Throws {
        New-LingoFlowReleaseNotes -Version '1.4.3' -CommitSha $goodSha -CiRunUrl 'x' -RepositoryFullName 'a/b' `
            -InstallerName 'LingoFlow-Setup-1.4.3-x64.exe' -InstallerSha256 ('ab' * 32) -PreviousTag '1.4.2'
    } 'previous tag without the v prefix'
}

# --- 11. static invariants of the workflow files -----------------------------------------

Invoke-Check 'release workflow exists and is manual-only' {
    $yml = Read-RepoFile '.github/workflows/release.yml'
    Assert-True ($yml -match '(?m)^on:\s*$') 'has an on: block'
    Assert-True ($yml -match 'workflow_dispatch') 'triggered by workflow_dispatch'
    # no automatic publishing paths at all (trigger KEYS anchored to line start;
    # note the JS in the publish job legitimately contains 'prerelease: false')
    Assert-True ($yml -notmatch '(?m)^\s*push:') 'no push trigger'
    Assert-True ($yml -notmatch '(?m)^\s*release:') 'no release trigger (would auto-publish on tags)'
    Assert-True ($yml -notmatch '(?m)^\s*pull_request') 'no pull request trigger (and no pull_request_target)'
    Assert-True ($yml -notmatch 'workflow_run') 'no workflow_run trigger (trusted-code risk documented in docs/releasing.md)'
}

Invoke-Check 'release workflow uses only SHA-pinned actions and declares no secrets' {
    $yml = Read-RepoFile '.github/workflows/release.yml'
    $uses = @([regex]::Matches($yml, '(?m)^\s*uses:\s*(\S+)').ForEach({ $_.Groups[1].Value }))
    Assert-True ($uses.Count -ge 5) "expected pinned action usages, found $($uses.Count)"
    foreach ($u in $uses) {
        Assert-True ($u -match '@[0-9a-f]{40}$') "action reference not pinned to a full commit sha: $u"
    }
    Assert-True ($yml -notmatch 'secrets\.') 'the release workflow must reference no repository secret'
    Assert-True (-not ($yml -cmatch 'PAT[ -]|github_pat')) 'no personal access token patterns'
}

Invoke-Check 'release workflow: write permissions scoped to the publish job only' {
    $yml = Read-RepoFile '.github/workflows/release.yml'
    $writeCount = @([regex]::Matches($yml, '(?m)^\s*contents:\s*write\s*$')).Count
    Assert-Equal 1 $writeCount 'exactly one job gets repository write access (tag + release creation)'
    Assert-True ($yml -match '(?m)^\s*concurrency:') 'concurrency guard present'
    Assert-True ($yml -match 'cancel-in-progress:\s*false') 'release runs queue instead of cancelling each other'
}

Invoke-Check 'ci workflow stays a verifier: read-only, manual release pointer present' {
    $ci = Read-RepoFile '.github/workflows/ci.yml'
    Assert-True ($ci -notmatch '(?m)^\s*contents:\s*write') 'CI has no write permissions'
    Assert-True ($ci -notmatch 'secrets\.') 'CI references no secrets'
    Assert-True ($ci -match '(?m)^\s*release-logic:') 'CI runs the release logic job'
    Assert-True ($ci -match 'docs/releasing.md') 'CI points to the release runbook'
    Assert-True ($ci -match 'github.run_id') 'CI summary hands the operator this run id for the release dispatch'
}

Invoke-Check 'the two workflows share the same pinned action versions' {
    $ci = Read-RepoFile '.github/workflows/ci.yml'
    $rel = Read-RepoFile '.github/workflows/release.yml'
    $ciPins = @([regex]::Matches($ci, '(?m)^\s*uses:\s*(\S+)').ForEach({ $_.Groups[1].Value })) | Sort-Object -Unique
    foreach ($u in @([regex]::Matches($rel, '(?m)^\s*uses:\s*(\S+)').ForEach({ $_.Groups[1].Value })) | Sort-Object -Unique) {
        $repo2 = ($u -split '@')[0]
        $inCi = @($ciPins | Where-Object { ($_ -split '@')[0] -eq $repo2 })
        if ($inCi.Count -gt 0) {
            # the same action must be pinned to the same sha in both workflows
            Assert-True (@($inCi) -contains $u) "action $u in release.yml diverges from ci.yml pin(s) $($inCi -join ', ')"
        }
    }
}

Invoke-Check 'ctest registers the release logic gate' {
    $t = Read-RepoFile 'tests/CMakeLists.txt'
    Assert-True ($t -match 'release_process_logic') 'add_test entry present'
    Assert-True ($t -match 'release-process\.Tests\.ps1') 'points at this file'
}

# --- 12. installer source static invariants -----------------------------------------------

Invoke-Check 'installer script: version-free by construction' {
    $iss = Read-RepoFile 'packaging/LingoFlow.iss'
    Assert-True ($iss -notmatch '0\.1\.0') 'the iss must not hardcode a version - the only versions allowed are GetEnv defines'
    Assert-True ($iss -notmatch 'Setup-\d') 'no version literal in the output file name pattern either'
    Assert-True ($iss -match "GetEnv\('LINGOFLOW_APP_VERSION'\)") 'version comes from the environment define'
    Assert-True ($iss -match '#error') 'missing defines must break the compile, not silently default'
}

Invoke-Check 'installer script: per-user, x64-only, start menu + optional desktop icon' {
    $iss = Read-RepoFile 'packaging/LingoFlow.iss'
    Assert-True ($iss -match 'PrivilegesRequired=lowest') 'per-user install without administrator'
    Assert-True ($iss -match 'PrivilegesRequiredOverridesAllowed=dialog') 'the user can opt into a machine install explicitly'
    Assert-True ($iss -match 'ArchitecturesAllowed=x64compatible') 'x64 only'
    Assert-True ($iss -match 'ArchitecturesInstallIn64BitMode=x64compatible') '64-bit install mode'
    Assert-True ($iss -match 'MinVersion=10\.0') 'Windows 10/11 only'
    Assert-True ($iss -match '\{autoprograms\}') 'Start Menu shortcut'
    Assert-True ($iss -match 'Tasks:\s*desktopicon') 'desktop shortcut is an opt-in task'
    Assert-True ($iss -match 'VersionInfoVersion=\{#MyAppVersion\}') 'setup exe version resource stamped'
    Assert-True ($iss -match 'OutputBaseFilename=LingoFlow-Setup-\{#MyAppVersion\}-x64') 'artifact naming follows the version define'
    Assert-True ($iss -match 'SetupLogging=yes') '/LOG usable for the automated verification'
}

Invoke-Check 'installer script: ships exactly the approved file set, never user state or symbols' {
    $iss = Read-RepoFile 'packaging/LingoFlow.iss'
    foreach ($f in @('LingoFlow\.exe', 'VERSION\.txt', 'LICENSE', 'THIRD_PARTY_NOTICES\.md', 'LICENSES')) {
        Assert-True ($iss -match "Source: .*\\$f") "the file list must include $f"
    }
    Assert-True ($iss -notmatch '\.pdb') 'debug symbols never ship in the installer'
    Assert-True ($iss -notmatch 'userappdata') 'user settings (%APPDATA%\LingoFlow) are not an installer target in either direction'
    Assert-True ($iss -notmatch 'UninstallDelete') 'the uninstaller deletes nothing beyond what it installed'
    Assert-True ($iss -match 'AppId=\{\{') 'stable AppId so an upgrade does not orphan an old install'
}

# --- 13. every released script parses on this shell ----------------------------------------

Invoke-Check 'all release scripts parse cleanly' {
    foreach ($rel in @(
            'scripts/release/release-lib.ps1',
            'scripts/release/run-release-build.ps1',
            'scripts/release/ensure-inno-setup.ps1',
            'scripts/release/tests/release-process.Tests.ps1',
            'packaging/release-package.ps1',
            'packaging/build-installer.ps1',
            'packaging/verify-installer.ps1',
            'packaging/install.ps1',
            'packaging/uninstall.ps1')) {
        $full = Join-Path $repoRoot $rel
        if (-not (Test-Path $full)) { throw "missing script: $rel" }
        $tokens = $null
        $parseErrors = $null
        [void] [System.Management.Automation.Language.Parser]::ParseFile($full, [ref] $tokens, [ref] $parseErrors)
        if (@($parseErrors).Count -gt 0) {
            throw ("{0}: {1}" -f $rel, (($parseErrors | ForEach-Object { $_.Message }) -join ' | '))
        }
    }
}

# --- 14. secret hygiene in the release tooling itself -----------------------------------------

Invoke-Check 'no credential-shaped strings in the release tooling' {
    # This scanner's own file is excluded (its pattern list contains the
    # very substrings it hunts in the other files); every release pipeline
    # input - scripts, installer source, both workflows - is scanned.
    $files = @(
        'scripts/release/release-lib.ps1',
        'scripts/release/run-release-build.ps1',
        'scripts/release/ensure-inno-setup.ps1',
        'packaging/build-installer.ps1',
        'packaging/verify-installer.ps1',
        'packaging/LingoFlow.iss',
        '.github/workflows/release.yml',
        '.github/workflows/ci.yml')
    $patterns = @(
        'sk-[A-Za-z0-9]{20,}',          # OpenAI-style key
        'ghp_[A-Za-z0-9]{36}',          # GitHub PAT
        'github_pat_[A-Za-z0-9_]{20,}', # fine-grained PAT
        'BEGIN.{0,20}PRIVATE KEY',
        'Authorization:\s*[Bb]earer\s+[A-Za-z0-9]') # token interpolated as a literal
    foreach ($rel in $files) {
        $content = Read-RepoFile $rel
        foreach ($p in $patterns) {
            if ($content -match $p) {
                throw "secret-shaped pattern '$p' found in $rel"
            }
        }
    }
}

Invoke-Check 'ensure-inno-setup pins the official artifact by sha256 and publisher' {
    $e = Read-RepoFile 'scripts/release/ensure-inno-setup.ps1'
    Assert-True ($e -match 'innosetup-6\.7\.3\.exe') 'pinned version 6.7.3'
    Assert-True ($e -match 'jrsoftware/issrc/releases/download/is-6_7_3') 'downloaded from the official immutable GitHub release'
    Assert-True ($e -match ('[0-9a-f]{64}')) 'a sha256 is pinned'
    Assert-True ($e -match 'Pyrsys B\.V\.') 'Authenticode publisher is checked'
}

# --- result -------------------------------------------------------------------------------------

Write-Host ''
Write-Host ('release-process logic: {0} passed, {1} failed' -f $script:passCount, @($script:failures).Count)
if (@($script:failures).Count -gt 0) {
    foreach ($f in $script:failures) { Write-Host "  FAILED $f" }
    exit 1
}
exit 0
