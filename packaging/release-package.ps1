param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build-release'),
    [string]$OutDir   = (Join-Path $PSScriptRoot '..\out\release'),
    [string]$Version  = '0.1.0',
    [string]$Dumpbin  = ''
)
$ErrorActionPreference = 'Stop'

# Design note - build the shippable release folder, and verify the one thing that
# makes it shippable: the executable imports SYSTEM dlls only. If a developer
# dependency ever comes back (dynamic CRT, a linked import library, anything
# that is not on a stock Windows), this script fails before the package exists.

$exe = Join-Path $BuildDir 'src\LingoFlow_artefacts\Release\LingoFlow.exe'
$pdb = Join-Path $BuildDir 'src\LingoFlow_artefacts\RelWithDebInfo\LingoFlow.pdb'

if (-not (Test-Path $exe)) { throw "Release exe not found: $exe (build --config Release first)" }

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
if (Test-Path (Join-Path $package 'LingoFlow-symbols.pdb')) {
    Write-Output ("  symbols.pdb         " + [Math]::Round((Get-Item (Join-Path $package 'LingoFlow-symbols.pdb')).Length / 1MB, 1) + " MB (for crash analysis, not installed)")
}
