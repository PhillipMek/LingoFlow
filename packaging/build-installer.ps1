#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$OutDir = '',
    [string]$ISCC = ''
)
$ErrorActionPreference = 'Stop'

<#
    Compile the Inno Setup installer for one already-staged release package.

    Inputs:  the package folder produced by packaging/release-package.ps1
             (LingoFlow.exe, VERSION.txt, licence chain), the release version
             (the Git tag without the v prefix).
    Output:  out\installer\LingoFlow-Setup-<version>-x64.exe (+ .sha256 sidecar)
             and nothing else - the installer is built from the explicit file
             list in packaging/LingoFlow.iss, not by copying the staging
             folder: the RelWithDebInfo crash symbols that travel with the
             portable package are deliberately excluded from the installer.

    Gates this script enforces before declaring success:
      - the staged package carries exactly the requested version
        (VERSION.txt content, exe FileVersion);
      - ISCC exists, is Inno Setup >= 6.3 (x64compatible / VersionInfoVersion
        need 6.3);
      - the compiled setup exe is non-trivial, named per the release rules
        and stamps the same version into its version resource;
      - the SHA-256 sidecar is written in sha256sum format (lowercase hex,
        two spaces, file name) for anyone verifying a download.
#>

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $repoRoot 'scripts\release\release-lib.ps1')

if (-not (Test-LingoFlowVersion $Version)) {
    throw "build-installer: -Version must be MAJOR.MINOR.PATCH (the release tag minus the v prefix), got '$Version'"
}
$installerName = Get-LingoFlowInstallerName $Version
$checksumName = Get-LingoFlowChecksumName $installerName

if (-not (Test-Path -LiteralPath $PackageDir)) {
    throw "build-installer: package dir not found: $PackageDir (run packaging/release-package.ps1 first)"
}
$PackageDir = [IO.Path]::GetFullPath($PackageDir)
if (-not $OutDir) { $OutDir = Join-Path $repoRoot 'out\installer' }
$OutDir = [IO.Path]::GetFullPath($OutDir)

# --- the staging gate: the package must carry the version we are releasing -----
foreach ($f in @('LingoFlow.exe', 'VERSION.txt', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'LICENSES')) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageDir $f))) {
        throw "build-installer: '$f' missing from $PackageDir - it is not a complete LingoFlow release package"
    }
}
$stamped = (Get-Content -LiteralPath (Join-Path $PackageDir 'VERSION.txt') -Raw).Trim()
$exeVersion = (Get-Item -LiteralPath (Join-Path $PackageDir 'LingoFlow.exe')).VersionInfo.FileVersion
$consistencyErrors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion $Version `
    -Sources @{ versiontxt = $stamped; exe = $exeVersion }
if (@($consistencyErrors).Count -gt 0) {
    throw ('build-installer: refusing to build an installer for ' + $Version + " -`n  " + ($consistencyErrors -join "`n  "))
}

# --- locate ISCC ------------------------------------------------------------------
function Find-Iscc {
    if ($ISCC) {
        if (-not (Test-Path -LiteralPath $ISCC)) { throw "build-installer: -ISCC path does not exist: $ISCC" }
        return (Get-Item -LiteralPath $ISCC).FullName
    }
    $cmd = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $roots = @()
    $pf86 = ${env:ProgramFiles(x86)}
    if ($pf86) { $roots += $pf86 }
    if ($env:ProgramFiles) { $roots += $env:ProgramFiles }
    if ($env:LOCALAPPDATA) { $roots += (Join-Path $env:LOCALAPPDATA 'Programs') }
    foreach ($root in $roots) {
        $cand = Join-Path $root 'Inno Setup 6\ISCC.exe'
        if (Test-Path -LiteralPath $cand) { return (Get-Item -LiteralPath $cand).FullName }
    }
    return ''
}

$isccPath = Find-Iscc
if (-not $isccPath) {
    throw 'build-installer: ISCC.exe (Inno Setup 6) was not found. Run scripts/release/ensure-inno-setup.ps1 (CI does this automatically) or install Inno Setup 6 >= 6.3.'
}
Write-Output "ISCC: $isccPath"

# --- stage only the approved file set (defence in depth against a polluted package)
$stage = Join-Path $OutDir 'stage'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($f in @('LingoFlow.exe', 'VERSION.txt', 'LICENSE', 'THIRD_PARTY_NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $PackageDir $f) -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $PackageDir 'LICENSES') -Destination (Join-Path $stage 'LICENSES') -Recurse

# --- compile -----------------------------------------------------------------------
# All /D values arrive through the environment (GetEnv in the .iss), so paths with
# spaces can never be mangled by command-line re-quoting.
$env:LINGOFLOW_APP_VERSION = $Version
$env:LINGOFLOW_STAGE_DIR = $stage
$env:LINGOFLOW_REPO_ROOT = $repoRoot
$env:LINGOFLOW_OUTPUT_DIR = $OutDir
$iss = Join-Path $PSScriptRoot 'LingoFlow.iss'

$compileLog = Join-Path $OutDir 'iscc-compile.log'
# ISCC writes errors to stderr. Windows PowerShell 5.1 turns redirected
# native stderr into a terminating error under EAP=Stop (PowerShell 7 does
# not) - since this script must run identically on both (local rehearsal on
# 5.1, CI on pwsh 7), the strictness is lifted only around this call. The
# exit code remains the authoritative gate below.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $compileOut = (& $isccPath $iss 2>&1 | ForEach-Object { $_.ToString() } | Tee-Object -FilePath $compileLog | Out-String)
    $isccExit = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $prevEap
}
Write-Output $compileOut
if ($isccExit -ne 0) {
    throw "build-installer: ISCC failed with exit code $isccExit - no installer was produced, nothing to publish (log: $compileLog)"
}

# The compiler identifies itself precisely only in real compile output (the
# binaries carry no readable FileVersion):
#   "Compiler engine version: Inno Setup 6.7.3"
# The .iss needs >= 6.3 (ArchitecturesAllowed=x64compatible, VersionInfoVersion).
$eng = [regex]::Match($compileOut, 'Compiler engine version: Inno Setup (\d+)\.(\d+)(?:\.(\d+))?')
if (-not $eng.Success) {
    throw 'build-installer: cannot determine the ISCC engine version from the compile output - refusing to trust an unknown compiler'
}
if ([int] $eng.Groups[1].Value -lt 6 -or ([int] $eng.Groups[1].Value -eq 6 -and [int] $eng.Groups[2].Value -lt 3)) {
    throw ("build-installer: Inno Setup >= 6.3 is required (x64compatible + VersionInfoVersion), " +
        "but the compiler reported '$($eng.Value)'. Update the compiler (ensure-inno-setup.ps1 pins 6.7.x).")
}
$engineVersion = $eng.Groups[0].Value -replace 'Compiler engine version: Inno Setup ', ''

$installer = Join-Path $OutDir $installerName
if (-not (Test-Path -LiteralPath $installer)) {
    throw "build-installer: ISCC reported success but $installerName is not in $OutDir"
}
if ([IO.Path]::GetFileName($installer) -ne $installerName) { throw 'internal naming error' }
$size = (Get-Item -LiteralPath $installer).Length
if ($size -lt 1MB) { throw "build-installer: the installer is only $size bytes - it cannot contain the release payload" }

$ifv = Get-LingoFlowNormalizedVersion (Get-Item -LiteralPath $installer).VersionInfo.FileVersion
$consistencyErrors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion $Version -Sources @{ installer = $ifv }
if (@($consistencyErrors).Count -gt 0) {
    throw ('build-installer: the compiled installer does not carry the release version: ' + ($consistencyErrors -join '; '))
}

# --- checksum + machine-readable summary -------------------------------------------
$sha256 = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath (Join-Path $OutDir $checksumName) -Value ("{0}  {1}" -f $sha256, $installerName) -Encoding Ascii
Remove-Item -LiteralPath $stage -Recurse -Force

Write-Output "INSTALLER: $installer"
Write-Output "SIZE: $size"
Write-Output "SHA256: $sha256"
Write-Output "BUILT: $installerName (Inno Setup $engineVersion, version $Version)"
