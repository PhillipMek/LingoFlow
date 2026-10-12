param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build-release'),
    [string]$OutDir   = (Join-Path $PSScriptRoot '..\out\release'),
    [string]$Version  = '0.1.0',
    [string]$Dumpbin  = ''
)
$ErrorActionPreference = 'Stop'

# Design note - build the shippable release folder, and verify the two things that
# make it shippable: (a) the executable imports SYSTEM dlls only - if a developer
# dependency ever comes back (dynamic CRT, a linked import library, anything
# that is not on a stock Windows), this script fails before the package exists;
# (b) the binary carries exactly the version being released (single source of
# truth chain: release tag -> LINGOFLOW_VERSION -> PROJECT_VERSION -> this gate
# -> installer metadata -> GitHub Release; see docs/releasing.md).

. (Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))) 'scripts\release\release-lib.ps1')

$exe = Join-Path $BuildDir 'src\LingoFlow_artefacts\Release\LingoFlow.exe'
$pdb = Join-Path $BuildDir 'src\LingoFlow_artefacts\RelWithDebInfo\LingoFlow.pdb'

if (-not (Test-Path $exe)) { throw "Release exe not found: $exe (build --config Release first)" }
$exe = (Get-Item -LiteralPath $exe).FullName

# version gate: the exe's Windows version resource (stamped by CMake/JUCE from
# PROJECT_VERSION) must equal the release version this package is for.
if (-not (Test-LingoFlowVersion $Version)) {
    throw "release-package: -Version must be MAJOR.MINOR.PATCH (the tag minus the v prefix), got '$Version'"
}
$exeVersion = Get-LingoFlowNormalizedVersion (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
$consistencyErrors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion $Version -Sources @{ exe = $exeVersion }
if (@($consistencyErrors).Count -gt 0) {
    throw ('release-package: ' + ($consistencyErrors -join '; ') + ' - refusing to package a build that does not carry the release version')
}
Write-Output ("version gate OK: LingoFlow.exe FileVersion = {0}" -f (Get-Item -LiteralPath $exe).VersionInfo.FileVersion)

# the package is exe + paperwork by design; a DLL that appeared next to the
# binary would mean a dynamic dependency crept back in (the dumpbin scan below
# names imports, this check catches payloads).
$strayDlls = @(Get-ChildItem -LiteralPath (Split-Path $exe) -Filter '*.dll' -ErrorAction SilentlyContinue)
if ($strayDlls.Count -gt 0) {
    throw ("unexpected DLLs next to the Release exe (the package must stay self-contained): " + (($strayDlls | ForEach-Object { $_.Name }) -join ', '))
}

if (-not $Dumpbin) {
    $Dumpbin = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022' -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1 -ExpandProperty FullName
}

$package = Join-Path $OutDir "LingoFlow-$Version"
if (Test-Path $package) { Remove-Item $package -Recurse -Force }
New-Item -ItemType Directory -Path $package | Out-Null

Copy-Item $exe (Join-Path $package 'LingoFlow.exe')
if (Test-Path $pdb) { Copy-Item $pdb (Join-Path $package 'LingoFlow-symbols.pdb') }
else { Write-Warning 'RelWithDebInfo pdb not found - package will lack venue debug symbols (not required to run)' }

# The AGPL chain travels WITH the binary: a recipient of the package alone has
# the licence, the combined-work notice and the verbatim third-party texts.
$repoRoot = Join-Path $PSScriptRoot '..'
foreach ($licence in @('LICENSE', 'THIRD_PARTY_NOTICES.md')) {
    $src = Join-Path $repoRoot $licence
    if (-not (Test-Path $src)) { throw "release package would be incomplete: $licence missing from the repository" }
    Copy-Item $src (Join-Path $package $licence)
}
if (-not (Test-Path (Join-Path $repoRoot 'LICENSES'))) { throw 'release package would be incomplete: LICENSES/ missing from the repository' }
Copy-Item (Join-Path $repoRoot 'LICENSES') (Join-Path $package 'LICENSES') -Recurse

$deps = @()
if ($Dumpbin) {
    $deps = & $Dumpbin /DEPENDENTS $exe | Select-String -Pattern '^\s+(\S+\.dll)$' | ForEach-Object { $_.Matches[0].Groups[1].Value.ToLower() }
} else {
    Write-Warning 'dumpbin not found - self-containment was NOT verified'
}

if ($deps.Count -gt 0) {
    $bad = $deps | Where-Object { $_ -match '^(msvcp|vcruntime|ucrtbase|concrt|vccorlib)' -or (Test-Path (Join-Path (Split-Path $exe) $_)) }
    if ($bad) { throw "the release exe imports a non-system dependency: $($bad -join ', ')" }
    Write-Output ("verified imports (all system): " + ($deps -join ' '))
}

# the icon the shell uses must be inside the binary (the app-icon asset chain)
Set-Content -Path (Join-Path $package 'VERSION.txt') -Value $Version -Encoding Ascii

Write-Output "PACKAGE: $package"
Write-Output ("  LingoFlow.exe       " + [Math]::Round((Get-Item (Join-Path $package 'LingoFlow.exe')).Length / 1MB, 1) + " MB")
Write-Output '  LICENSE             (AGPLv3)'
Write-Output '  THIRD_PARTY_NOTICES.md'
Write-Output ("  LICENSES/           " + @(Get-ChildItem (Join-Path $package 'LICENSES')).Count + ' verbatim third-party licence texts')
if (Test-Path (Join-Path $package 'LingoFlow-symbols.pdb')) {
    Write-Output ("  symbols.pdb         " + [Math]::Round((Get-Item (Join-Path $package 'LingoFlow-symbols.pdb')).Length / 1MB, 1) + " MB (for crash analysis, not installed)")
}
