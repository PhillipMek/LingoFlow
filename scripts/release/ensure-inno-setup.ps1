#Requires -Version 5.1
param(
    [string]$SourceFile = '',
    [ValidateSet('machine', 'user')][string]$InstallScope = 'machine'
)
$ErrorActionPreference = 'Stop'

<#
    Make the Inno Setup compiler available on the build machine (CI step before
    packaging/build-installer.ps1).

    Normal path on GitHub windows runners: Inno Setup is pre-installed on the
    image (windows-2025 currently ships 6.7.x) - the script finds it, verifies
    it is the Inno Setup 6 compiler and returns without touching the network.
    The precise engine version (>= 6.3) is asserted where the compiler is
    actually invoked (packaging/build-installer.ps1 parses the real
    "Compiler engine version" line from the compile output - ISCC binaries do
    not carry a readable FileVersion).

    Fallback path (self-hosted runners or an image that ever removes it again -
    this happened during the 2025 windows-latest migration): download the
    OFFICIAL, Authenticode-signed installer of the pinned version from its
    immutable GitHub release, verify the publisher name and the pinned
    SHA-256, and install it silently. If the hash or the signature does not
    match the pinned values, the script fails - a silently different compiler
    is not acceptable for a release pipeline.

    Inno Setup itself is a build tool only: nothing of it is linked into the
    app, and the compiled setup executable is ours to distribute under the
    Inno Setup license (free use incl. commercial redistribution of compiled
    installers; the compiler is fetched, never vendored).

    -SourceFile exists for local rehearsal (feed a previously downloaded,
    hash-pinned artifact); CI never uses it. The Authenticode and SHA-256
    checks run either way.
#>

$pinnedUrl = 'https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe'
$pinnedFileName = 'innosetup-6.7.3.exe'
# SHA-256 measured on the official artifact on 2026-10-12 (docs/releasing.md,
# "toolchain pins"). The release is immutable; the publisher is Pyrsys B.V.
# (Inno Setup 6 signing entity), verified at runtime below.
$pinnedSha256 = '9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732'
$expectedPublisher = 'Pyrsys B.V.'

function Find-Iscc {
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

function Test-Iscc6([string]$isccPath) {
    # ISCC embeds no usable FileVersion - identify it by its own banner. The
    # exact engine version (>= 6.3 for x64compatible/VersionInfoVersion) is
    # asserted by packaging/build-installer.ps1 on the real compile output.
    # EAP is relaxed because ISCC prints its usage text on stderr, and
    # Windows PowerShell 5.1 would escalate that into a terminating error.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $out = & $isccPath '/?' 2>&1 | Out-String }
    finally { $ErrorActionPreference = $prev }
    return ($out -match 'Inno Setup 6 Command-Line Compiler')
}

$found = Find-Iscc
if ($found) {
    if (Test-Iscc6 $found) {
        Write-Output "ISCC=$found"
        Write-Output 'ISCC=verified Inno Setup 6 compiler (pre-installed; nothing downloaded)'
        exit 0
    }
    throw "found an ISCC at $found that is not the Inno Setup 6 command-line compiler - refusing to use it"
}

Write-Output 'Inno Setup not found on this machine - using the pinned official fallback artifact.'

$target = $SourceFile
if ($target) {
    if (-not (Test-Path -LiteralPath $target)) { throw "rehearsal -SourceFile does not exist: $target" }
    $target = (Get-Item -LiteralPath $target).FullName
    if ([IO.Path]::GetFileName($target) -ne $pinnedFileName) { throw "-SourceFile must be the pinned $pinnedFileName" }
}
else {
    $target = Join-Path $env:TEMP $pinnedFileName
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072
    Invoke-WebRequest -Uri $pinnedUrl -OutFile $target -UseBasicParsing
}

# Integrity gates: pinned hash first, then Authenticode publisher + chain status.
$actual = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $pinnedSha256) {
    Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue
    throw "sha256 of $pinnedFileName is $actual, pinned $pinnedSha256 - refusing to run a mismatched compiler installer"
}
$sig = Get-AuthenticodeSignature $target
$subject = if ($sig.SignerCertificate) { [string] $sig.SignerCertificate.Subject } else { '' }
if ($sig.Status -ne 'Valid' -or $subject -notmatch [regex]::Escape($expectedPublisher)) {
    Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue
    throw "Authenticode check failed (status '$($sig.Status)', publisher '$subject', expected '$expectedPublisher') - refusing to run an unverifiable installer"
}
Write-Output "signature OK: $pinnedFileName, sha256 $actual, publisher $expectedPublisher"

$installerArgs = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/NOICONS')
if ($InstallScope -eq 'user') { $installerArgs += '/CURRENTUSER' }
$p = Start-Process -FilePath $target -Wait -PassThru -ArgumentList $installerArgs
if ($p.ExitCode -ne 0) { throw "Inno Setup installer exit code $($p.ExitCode)" }

$found = Find-Iscc
if (-not $found) { throw 'the fallback install reported success but ISCC.exe is still not findable' }
if (-not (Test-Iscc6 $found)) { throw "the fallback install produced an ISCC that does not identify as Inno Setup 6: $found" }
Write-Output "ISCC=$found"
Write-Output 'ISCC=verified Inno Setup 6 compiler (pinned fallback install)'
exit 0
