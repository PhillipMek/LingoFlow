# The ONE local build entry point: every configuration goes into <repo>\build.
# JUCE keeps its own artefacts folder inside that tree, so the operator exe is
# always build\src\LingoFlow_artefacts\<Config>\LingoFlow.exe - there is no
# second tree (build-debug / build-release / out\build) to look in.
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string] $Config = 'Release',
    [switch] $Test,
    [switch] $Smoke
)

$ErrorActionPreference = 'Stop'

# Visual Studio ships CMake but does not always put it on PATH.
if (-not (Get-Command cmake -ErrorAction SilentlyContinue))
{
    $cmakeExe = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -ErrorAction SilentlyContinue |
                Select-Object -First 1
    if ($cmakeExe) { Set-Item -Path Env:PATH -Value "$($cmakeExe.DirectoryName);$Env:PATH" }
}

$repo   = Split-Path -Parent $PSScriptRoot
$build  = Join-Path $repo 'build'
$exe    = Join-Path $build "src\LingoFlow_artefacts\$Config\LingoFlow.exe"

cmake -S $repo -B $build
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

cmake --build $build --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

if ($Test)
{
    ctest --test-dir $build -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }
}

if ($Smoke)
{
    & $exe --dev --smoke
    if ($LASTEXITCODE -ne 0) { throw "Offline smoke failed ($LASTEXITCODE)" }
}

if (-not (Test-Path $exe)) { throw "Expected exe not found: $exe" }
Write-Output "EXE: $exe"
