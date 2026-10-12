#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDir = '',
    [string]$FetchBase = '',
    [string]$LogDir = ''
)
$ErrorActionPreference = 'Stop'

<#
    The Release-build leg of the GitHub Release pipeline: configure, stamp-verify,
    build, run the FULL registered ctest suite, and run the hardware-independent
    smoke - all against the proven commit, never against a Debug artifact.

    Same command sequence as .github/workflows/ci.yml documents in docs/CI.md
    ("reproduce CI locally"), plus one release-only difference: the CMake
    version stamp is forced to the release tag version (-DLINGOFLOW_VERSION),
    and the stamp is then verified where it is observable (configure cache,
    and after linking, the exe's FileVersion). Every step checks its exit code
    and the process stops on the first failure; nothing partial gets published.

    Smoke semantics are the product's (src/App/Main.cpp), identical to CI:
      --dev --smoke  -> must exit 0 on any machine (no device, no key, no net)
      --smoke        -> 0 = PASS; 2 = EXPECTED-HARDWARE-SKIP (a runner without
                        ASIO hardware; documented, surfaced, never swallowed);
                        anything else = FAIL.
#>

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
. (Join-Path $repoRoot 'scripts\release\release-lib.ps1')

if (-not (Test-LingoFlowVersion $Version)) {
    throw "run-release-build: -Version must be MAJOR.MINOR.PATCH, got '$Version'"
}
if (-not $BuildDir) { $BuildDir = 'build-release' }
if (-not $LogDir) { $LogDir = 'release-logs' }
$BuildDirFull = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
$LogDirFull = [IO.Path]::GetFullPath((Join-Path $repoRoot $LogDir))
New-Item -ItemType Directory -Force $LogDirFull | Out-Null

function Invoke-Checked {
    param([string]$Name, [string]$LogFile, [scriptblock]$Body)
    Write-Output "--- $Name"
    # cmake/ctest report diagnostics on stderr; Windows PowerShell 5.1 turns
    # redirected native stderr into a terminating error under EAP=Stop (pwsh 7
    # does not). The strictness is relaxed around the call for cross-shell
    # parity; the exit code below is the authoritative gate.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Body *>&1 | Tee-Object -FilePath $LogFile | Write-Output; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $prev }
    if ($code -ne 0) {
        throw "$Name failed with exit code $code (full log: $LogFile)"
    }
}

# --- configure (Release, stamped version) ------------------------------------------
# The version travels through the ENVIRONMENT (see CMakeLists.txt), never through a
# cache entry, and the proof that it landed is CMake's own configure banner:
# "LingoFlow <PROJECT_VERSION>: configure done" (the last configure status line).
$env:LINGOFLOW_VERSION = $Version
$configureArgs = @('-S', $repoRoot, '-B', $BuildDirFull)
if ($FetchBase) {
    $configureArgs += @('-DFETCHCONTENT_BASE_DIR=' + [IO.Path]::GetFullPath($FetchBase))
}
Invoke-Checked 'Configure (Release, version-stamped)' (Join-Path $LogDirFull 'configure.log') {
    & cmake @configureArgs
}

$configureLog = Get-Content -LiteralPath (Join-Path $LogDirFull 'configure.log') -Raw
if ($configureLog -notmatch ('-- LingoFlow ' + [regex]::Escape($Version) + ': configure done')) {
    throw "run-release-build: CMake configured a version other than the release stamp '$Version' (see configure.log)"
}
if ($configureLog -notmatch 'version override from the release workflow') {
    throw 'run-release-build: the release version override was not picked up by CMake'
}
Write-Output "VERSION STAMP OK: PROJECT_VERSION = $Version"

# --- build -----------------------------------------------------------------------------
Invoke-Checked 'Build (Release)' (Join-Path $LogDirFull 'build-release.log') {
    & cmake --build $BuildDirFull --config Release --parallel
}
$exe = Join-Path $BuildDirFull 'src\LingoFlow_artefacts\Release\LingoFlow.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "run-release-build: the Release exe was not produced at $exe" }
$exeVersion = Get-LingoFlowNormalizedVersion (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
$errors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion $Version -Sources @{ exe = $exeVersion }
if (@($errors).Count -gt 0) { throw ('run-release-build: the built binary does not carry the release version: ' + ($errors -join '; ')) }

# --- tests: the FULL registered suite, no filter (the CI contract) ----------------------
Invoke-Checked 'Tests (Release - full registered suite)' (Join-Path $LogDirFull 'ctest-release.log') {
    & ctest --test-dir $BuildDirFull -C Release --output-on-failure
}

# --- smoke --------------------------------------------------------------------------------
Invoke-Checked 'Smoke (--dev --smoke - hardware-independent, must pass)' (Join-Path $LogDirFull 'smoke-dev.log') {
    & $exe --dev --smoke
}
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try { & $exe --smoke *>&1 | Tee-Object -FilePath (Join-Path $LogDirFull 'smoke-plain.log') | Write-Output; $smokeCode = $LASTEXITCODE }
finally { $ErrorActionPreference = $prev }
switch ($smokeCode) {
    0 { Write-Output 'SMOKE: PASS' }
    2 { Write-Output 'SMOKE: EXPECTED-HARDWARE-SKIP (exit 2 - a runner without ASIO devices; the production smoke is a venue check, docs/rig-checklist.md)' }
    default { throw "run-release-build: unexpected --smoke exit code $smokeCode (not 0, not the documented 2) - see smoke-plain.log" }
}

Write-Output ''
Write-Output "BUILD_OK: Release $Version built, tested and smoke-verified in $BuildDirFull"
exit 0
