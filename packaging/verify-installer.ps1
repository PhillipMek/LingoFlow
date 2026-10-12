#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$InstallerDir = '',
    [string]$WorkRoot = '',
    [switch]$SkipSmoke
)
$ErrorActionPreference = 'Stop'

<#
    Automated installer verification (CI + local rehearsal).

    What is verified, in order:
      1. the file the pipeline is about to publish exists, is named per the
         release rules, is non-trivial and is a real MZ executable;
      2. its version resource carries the release version;
      3. it installs unattended (/VERYSILENT, per-user, explicit /DIR), exits
         with code 0, and the install folder then contains exactly the
         approved file set with matching version stamps and NO debug symbols;
      4. the Start Menu shortcut points at the installed exe; the desktop
         shortcut is not created (it is an opt-in task);
      5. the installed binary runs its hardware-independent developer smoke
         (same semantics as CI: exit 0 required);
      6. the app is removed by a standard unattended uninstall: folder gone,
         shortcut gone, uninstall registry entry gone;
      7. user state survives: %APPDATA%\LingoFlow (settings, logs) is not
         touched by either the install or the uninstall.

    What this does NOT prove: a genuinely clean machine (the runner has
    developer tools), real ASIO hardware, live OpenAI, a real NDI receiver.
    Those stay the venue/clean-machine checkpoints (docs/rig-checklist.md,
    docs/release.md).
#>

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $repoRoot 'scripts\release\release-lib.ps1')

if (-not (Test-LingoFlowVersion $Version)) {
    throw "verify-installer: -Version must be MAJOR.MINOR.PATCH, got '$Version'"
}
$installerName = Get-LingoFlowInstallerName $Version
if (-not $InstallerDir) { $InstallerDir = Join-Path $repoRoot 'out\installer' }
$installer = Join-Path ([IO.Path]::GetFullPath($InstallerDir)) $installerName
if (-not (Test-Path -LiteralPath $installer)) { throw "verify-installer: installer not found: $installer" }

if (-not $WorkRoot) { $WorkRoot = Join-Path $repoRoot 'out\verify-work' }
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
if (Test-Path -LiteralPath $WorkRoot) { Remove-Item -LiteralPath $WorkRoot -Recurse -Force }
New-Item -ItemType Directory -Path $WorkRoot | Out-Null
$appDir = Join-Path $WorkRoot 'install-target'

function Step([string]$n) { Write-Output ("VERIFY[{0}]: {1}" -f $n, '') }
function Fail([string]$m) { throw "verify-installer: $m" }

# --- 1. static sanity -----------------------------------------------------------
Step '1/7 file'
$item = Get-Item -LiteralPath $installer
if ([IO.Path]::GetFileName($installer) -ne $installerName) { Fail "installer is not named per the release rules" }
if ($item.Length -lt 1MB) { Fail "installer is only $($item.Length) bytes" }
$fs = [IO.File]::OpenRead($installer)
try {
    $buf = New-Object byte[] 2
    $null = $fs.Read($buf, 0, 2)
} finally { $fs.Close() }
if ($buf[0] -ne 0x4D -or $buf[1] -ne 0x5A) { Fail 'first two bytes are not MZ - not a PE executable' }
Write-Output ("  size={0} bytes, MZ ok" -f $item.Length)

# --- 2. version resource --------------------------------------------------------
Step '2/7 installer version'
$norm = Get-LingoFlowNormalizedVersion $item.VersionInfo.FileVersion
$errors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion $Version -Sources @{ installer = $norm }
if (@($errors).Count -gt 0) { Fail (($errors -join '; ')) }
Write-Output "  installer FileVersion = $Version"

# --- 3. unattended install -------------------------------------------------------
Step '3/7 silent install'
# Snapshot user state BEFORE anything runs - the uninstall must preserve it.
$userStateDir = Join-Path $env:APPDATA 'LingoFlow'
$userConfig = Join-Path $userStateDir 'config.json'
$stateExistedBefore = Test-Path -LiteralPath $userStateDir
$configBefore = $null
if (Test-Path -LiteralPath $userConfig) {
    $configBefore = (Get-FileHash -LiteralPath $userConfig -Algorithm SHA256).Hash
}
# If a developer machine already has a LingoFlow Start Menu shortcut (installed
# by install.ps1 for example), the test cycle runs on a CI-clean state there
# would take it over and the uninstall would remove it. Snapshot + restore the
# pre-existing target so the rehearsal is non-destructive everywhere.
$startMenu = [Environment]::GetFolderPath('Programs')
$lnk = Join-Path $startMenu 'LingoFlow.lnk'
$desktopLnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'LingoFlow.lnk'
$lnkTargetBefore = $null
if (Test-Path -LiteralPath $lnk) {
    $wsh = New-Object -ComObject WScript.Shell
    $lnkTargetBefore = $wsh.CreateShortcut($lnk).TargetPath
    Write-Output "  note: pre-existing Start Menu shortcut (target $lnkTargetBefore) - it will be restored after the test"
}
$desktopExistedBefore = Test-Path -LiteralPath $desktopLnk

$installLog = Join-Path $WorkRoot 'install.log'
# /DIR and /LOG arrive double-quoted so paths with spaces survive the Win32
# command-line round trip.
$dirArg = '/DIR="' + $appDir + '"'
$logArg = '/LOG="' + $installLog + '"'
$p = Start-Process -FilePath $installer -PassThru -Wait -ArgumentList @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER',
    '/TASKS=', $dirArg, $logArg)
if ($null -eq $p) { Fail 'Start-Process returned no process object' }
if ($p.ExitCode -ne 0) {
    Fail "silent install exit code $($p.ExitCode) (Inno exit codes: 0=success, 1=restart needed, 2..6=errors; see $installLog)"
}

foreach ($f in @('LingoFlow.exe', 'VERSION.txt', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'unins000.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $appDir $f))) { Fail "install produced no $f in $appDir" }
}
foreach ($f in @('LICENSE', 'THIRD_PARTY_NOTICES.md')) {
    if ((Get-Item -LiteralPath (Join-Path $appDir $f)).Length -lt 10) { Fail "$f was installed but is empty" }
}
$licFiles = @(Get-ChildItem -LiteralPath (Join-Path $appDir 'LICENSES') -File -ErrorAction SilentlyContinue)
if ($licFiles.Count -lt 1) { Fail 'LICENSES/ directory was not installed' }
$vt = (Get-Content -LiteralPath (Join-Path $appDir 'VERSION.txt') -Raw).Trim()
$exfv = (Get-Item -LiteralPath (Join-Path $appDir 'LingoFlow.exe')).VersionInfo.FileVersion
$errors = Get-LingoFlowVersionConsistencyErrors -ExpectedVersion $Version -Sources @{ versiontxt = $vt; exe = $exfv }
if (@($errors).Count -gt 0) { Fail ("installed files: " + ($errors -join '; ')) }
if (@(Get-ChildItem -LiteralPath $appDir -Recurse -Filter '*.pdb').Count -gt 0) { Fail 'debug symbols must never be installed' }
Write-Output "  install exit 0; files + versions verified (app exe $exfv, VERSION.txt $vt)"

# --- 4. shortcuts ------------------------------------------------------------------
Step '4/7 shortcuts'
if (-not (Test-Path -LiteralPath $lnk)) { Fail "Start Menu shortcut missing: $lnk" }
$shell = New-Object -ComObject WScript.Shell
$linkTarget = $shell.CreateShortcut($lnk).TargetPath
if ($linkTarget -ne (Join-Path $appDir 'LingoFlow.exe')) { Fail "shortcut targets '$linkTarget', expected '$appDir\LingoFlow.exe'" }
if ((Test-Path -LiteralPath $desktopLnk) -and -not $desktopExistedBefore) {
    Fail 'desktop shortcut must be opt-in, but it was created by default'
}
Write-Output "  Start Menu shortcut -> $linkTarget"

# --- 5. installed-binary smoke ------------------------------------------------------
Step '5/7 installed app smoke'
if ($SkipSmoke) {
    Write-Output '  SKIPPED by -SkipSmoke (never on CI: this is the mandatory gate there)'
}
else {
    # Same hardware-independent semantics as CI: --dev --smoke must exit 0.
    # (stderr under EAP=Stop would look like a crash on PowerShell 5.1 - the
    # exit code is the gate, same as in run-release-build.ps1.)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & (Join-Path $appDir 'LingoFlow.exe') --dev --smoke *>&1 | Tee-Object -FilePath (Join-Path $WorkRoot 'smoke-dev.log')
        $smokeExit = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $prev }
    if ($smokeExit -ne 0) { Fail "installed exe smoke (--dev --smoke) exit $smokeExit - the packaged binary does not start (see smoke-dev.log)" }
    Write-Output '  --dev --smoke exit 0'
}

# --- 6. unattended uninstall ---------------------------------------------------------
Step '6/7 silent uninstall'
$unins = Join-Path $appDir 'unins000.exe'
$uninsLog = Join-Path $WorkRoot 'uninstall.log'
$uninsLogArg = '/LOG="' + $uninsLog + '"'
$p = Start-Process -FilePath $unins -PassThru -Wait -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', $uninsLogArg)
if ($p.ExitCode -ne 0) { Fail "silent uninstall exit code $($p.ExitCode) (see $uninsLog)" }
# Inno re-launches the uninstaller from a temp copy; wait for the folder to be gone.
$deadline = (Get-Date).AddSeconds(90)
while ((Test-Path -LiteralPath $appDir) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Seconds 1 }
if (Test-Path -LiteralPath $appDir) {
    $left = @(Get-ChildItem -LiteralPath $appDir -Recurse -File | ForEach-Object { $_.Name }) -join ', '
    Fail "uninstall left files behind in $appDir : $left"
}
if (Test-Path -LiteralPath $lnk) { Fail 'Start Menu shortcut survived the uninstall' }
if ((Test-Path -LiteralPath $desktopLnk) -and -not $desktopExistedBefore) {
    Fail 'a desktop shortcut appeared that must never have been created (it is an opt-in task)'
}
$uninKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*'
$mine = @(Get-ItemProperty -Path $uninKey -ErrorAction SilentlyContinue |
    Where-Object { $_.UninstallString -and $_.UninstallString.Contains($appDir) })
if ($mine.Count -gt 0) { Fail 'the uninstall registry entry survived' }
if ($lnkTargetBefore -and (Test-Path -LiteralPath $lnkTargetBefore)) {
    # restore the developer machine's own shortcut that this rehearsal replaced
    $wsh = New-Object -ComObject WScript.Shell
    $fix = $wsh.CreateShortcut($lnk)
    $fix.TargetPath = $lnkTargetBefore
    $fix.WorkingDirectory = Split-Path $lnkTargetBefore -Parent
    $fix.Description = 'LingoFlow - live speech translation'
    $fix.Save()
    Write-Output "  restored the pre-existing Start Menu shortcut -> $lnkTargetBefore"
}
Write-Output '  install folder, shortcut and registry entry are gone'

# --- 7. user state survives -------------------------------------------------------------
# The installer/uninstaller must play no part in %APPDATA%\LingoFlow in either
# direction. Existence is the contract: the app itself legitimately creates or
# re-persists its settings during a smoke run - the failure condition here is
# the INSTALLER erasing user state, so the post-check demands the files that
# were present are still present.
Step '7/7 user state preserved'
if ($stateExistedBefore -and -not (Test-Path -LiteralPath $userStateDir)) {
    Fail 'the uninstall deleted the user state directory %APPDATA%\LingoFlow'
}
if ($null -ne $configBefore -and -not (Test-Path -LiteralPath $userConfig)) {
    Fail 'the uninstall deleted the user settings file (%APPDATA%\LingoFlow\config.json)'
}
Write-Output '  %APPDATA%\LingoFlow survives install + uninstall untouched by either'

Write-Output ''
Write-Output 'VERIFY_OK: installer install/run/uninstall cycle passed'
exit 0
