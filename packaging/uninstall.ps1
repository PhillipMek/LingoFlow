param(
    [string]$TargetDir = ''
)
$ErrorActionPreference = 'Stop'

# Design note - uninstall = remove what install.ps1 put: the program files and the
# Start Menu shortcut. Deliberately does NOT touch %APPDATA%\LingoFlow (the
# operator's settings and logs) or the Windows credential store (the key):
# uninstalling the program must not erase a venue's setup or anyone's secret.

if (-not $TargetDir) {
    $TargetDir = Join-Path $env:LOCALAPPDATA 'Programs\LingoFlow'
}

$startMenu = [Environment]::GetFolderPath('Programs')
$lnk = Join-Path $startMenu 'LingoFlow.lnk'
if (Test-Path $lnk) { Remove-Item $lnk -Force; Write-Output "removed shortcut: $lnk" }

if (Test-Path $TargetDir) {
    if (-not (Test-Path (Join-Path $TargetDir 'LingoFlow.exe'))) {
        throw "refusing to delete '$TargetDir': no LingoFlow.exe inside (not our folder)"
    }
    Remove-Item $TargetDir -Recurse -Force
    Write-Output "removed program folder: $TargetDir"
} else {
    Write-Output "not installed: $TargetDir"
}

Write-Output 'settings and credentials were left alone on purpose (%APPDATA%\LingoFlow, Windows credential store)'
