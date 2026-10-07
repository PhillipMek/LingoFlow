param(
    [string]$PackageDir = '',
    [string]$TargetDir  = '',
    [switch]$NoShortcut
)
$ErrorActionPreference = 'Stop'

# Design note - per-user installer, no administrator, no MSI engine, no new
# dependencies: a scripted copy + Start Menu shortcut. The venue machine is
# offline and locked down; the fewer moving parts and reboots, the better.
#
# It writes exactly two things: the program files under $TargetDir and one
# Start Menu shortcut. Application settings and credentials belong to the app
# itself (%APPDATA%\LingoFlow and the Windows credential store); the installer
# neither creates nor touches them.

if (-not $PackageDir) {
    $PackageDir = Join-Path $PSScriptRoot '..\out\release\LingoFlow-0.1.0'
}
if (-not (Test-Path (Join-Path $PackageDir 'LingoFlow.exe'))) {
    throw "not a LingoFlow package: $PackageDir (run release-package.ps1 first)"
}

if (-not $TargetDir) {
    $TargetDir = Join-Path $env:LOCALAPPDATA 'Programs\LingoFlow'
}

New-Item -ItemType Directory -Path $TargetDir -Force | Out-Null
Copy-Item (Join-Path $PackageDir 'LingoFlow.exe') (Join-Path $TargetDir 'LingoFlow.exe') -Force
if (Test-Path (Join-Path $PackageDir 'VERSION.txt')) {
    Copy-Item (Join-Path $PackageDir 'VERSION.txt') (Join-Path $TargetDir 'VERSION.txt') -Force
}
# the licence chain installs WITH the binary (the licensing policy forbids an
# installer that hides it); the package build already fails if these are absent.
foreach ($licence in @('LICENSE', 'THIRD_PARTY_NOTICES.md')) {
    if (Test-Path (Join-Path $PackageDir $licence)) {
        Copy-Item (Join-Path $PackageDir $licence) (Join-Path $TargetDir $licence) -Force
    }
}
if (Test-Path (Join-Path $PackageDir 'LICENSES')) {
    Copy-Item (Join-Path $PackageDir 'LICENSES') (Join-Path $TargetDir 'LICENSES') -Recurse -Force
}

if (-not $NoShortcut) {
    # PS 5.1/.NET Framework name for the per-user Start Menu Programs folder is
    # 'Programs' (%APPDATA%\Microsoft\Windows\Start Menu\Programs).
    $startMenu = [Environment]::GetFolderPath('Programs')
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut((Join-Path $startMenu 'LingoFlow.lnk'))
    $link.TargetPath = (Join-Path $TargetDir 'LingoFlow.exe')
    $link.WorkingDirectory = $TargetDir
    $link.Description = 'LingoFlow - live speech translation'
    $link.Save()
    Write-Output "SHORTCUT: $startMenu\LingoFlow.lnk"
}

Write-Output "INSTALLED: $TargetDir"
Write-Output ("installed exe: " + (Join-Path $TargetDir 'LingoFlow.exe'))
