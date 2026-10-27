#Requires -Version 5.1
<#
.SYNOPSIS
    Run every unit test (Windows counterpart of scripts/test, ADR 0005).

.DESCRIPTION
    Runs the C/C++ tests through CTest and the Python tests through pytest. Both suites always
    run; the exit status is non-zero if either fails.

      C/C++   cmake --workflow --preset <preset>: configure, build, then ctest (ADR 0004).
              On Windows this needs the Visual Studio x64 developer environment (README).
      Python  uv run --locked --group test pytest, in build/python-env/<os>-<cpu> (ADR 0003).
      Release -Release: the win-release workflow instead, which also packages the kit with
              CPack into build/win-release/ (ADR 0023).

    $env:UV_PYTHON picks the Python for the tests (e.g. 3.10). Tests that need the extracted
    IPA are reported as skipped until scripts/bootstrap has created work/ipa/. Everything is
    written below build/ (test output: build/test-output/).

.PARAMETER Release
    Run the release workflow (win-release, mac-release-universal or linux-release): configure,
    build, test and package the kit into build/<preset>/. Implies -CppOnly.

.PARAMETER Preset
    Workflow preset for the C/C++ part. Default: win-debug, mac-debug or linux-debug for this
    OS, or $env:MAPLETHIEF_PRESET.

.PARAMETER CppOnly
    Skip the Python tests.

.PARAMETER PythonOnly
    Skip the C/C++ tests (needs only uv).

.PARAMETER PytestArgs
    Extra arguments for pytest, e.g. -PytestArgs '-k','fat','-x'.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\test.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\test.ps1 -Release
#>
[CmdletBinding()]
param(
    [switch]$Release,
    [string]$Preset = '',
    [switch]$CppOnly,
    [switch]$PythonOnly,
    [string[]]$PytestArgs = @()
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $Repo

# $IsMacOS/$IsLinux only exist in PowerShell 6+; Windows PowerShell 5.1 is always Windows.
if ((Test-Path variable:IsMacOS) -and $IsMacOS) { $osPresets = 'mac-debug', 'mac-release-universal' }
elseif ((Test-Path variable:IsLinux) -and $IsLinux) { $osPresets = 'linux-debug', 'linux-release' }
else { $osPresets = 'win-debug', 'win-release' }
if ($Release) {
    $CppOnly = [switch]$true
    if (-not $Preset) { $Preset = $osPresets[1] }
}
if (-not $Preset) { $Preset = $env:MAPLETHIEF_PRESET }
if (-not $Preset) { $Preset = $osPresets[0] }

$cppStatus = 'skipped'
$pythonStatus = 'skipped'

if (-not $PythonOnly) {
    Write-Host "== C/C++ tests: cmake --workflow --preset $Preset"
    & cmake --workflow --preset $Preset
    $cppStatus = if ($LASTEXITCODE -eq 0) { 'passed' } else { 'FAILED' }
}

$packageStatus = $null
if ($Release -and $cppStatus -eq 'passed') {
    # The workflow's package step ran; name the kit it wrote.
    $kits = @(Get-ChildItem -Path (Join-Path $Repo "build/$Preset") -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'MapleThief-kit-*' -and $_.Name -match '\.(zip|tar\.gz)$' })
    if ($kits.Count -gt 0) { $packageStatus = "package: $(($kits | ForEach-Object { $_.FullName }) -join ', ')" }
    else { $packageStatus = "package: FAILED (no MapleThief-kit-* in build/$Preset/)" }
}

if (-not $CppOnly) {
    Write-Host '== Python tests: uv run --locked --group test pytest'
    if (-not (Get-Command uv -ErrorAction SilentlyContinue)) {
        Write-Host 'scripts/test.ps1: uv not found. Install uv: https://docs.astral.sh/uv/'
        $pythonStatus = 'FAILED'
    } else {
        # The environment and bytecode live in build/ (one environment per OS and CPU, like
        # scripts/test); -p no:cacheprovider keeps pytest from writing .pytest_cache/ into the tree.
        if ($env:OS -eq 'Windows_NT') { $platform = "Windows-$env:PROCESSOR_ARCHITECTURE" }
        else { $platform = "$(uname -s)-$(uname -m)" }
        $saved = @{ env = $env:UV_PROJECT_ENVIRONMENT; pyc = $env:PYTHONPYCACHEPREFIX }
        $env:UV_PROJECT_ENVIRONMENT = Join-Path $Repo "build/python-env/$platform"
        $env:PYTHONPYCACHEPREFIX = Join-Path $Repo 'build/pycache'
        try {
            & uv run --locked --group test pytest -p no:cacheprovider @PytestArgs
            $pythonStatus = if ($LASTEXITCODE -eq 0) { 'passed' } else { 'FAILED' }
        } finally {
            $env:UV_PROJECT_ENVIRONMENT = $saved.env
            $env:PYTHONPYCACHEPREFIX = $saved.pyc
        }
    }
}

Write-Host ''
Write-Host '== scripts/test.ps1 summary'
Write-Host "   C/C++ tests ($Preset): $cppStatus"
if ($packageStatus) { Write-Host "   $packageStatus" }
Write-Host "   Python tests: $pythonStatus"
if ($cppStatus -eq 'FAILED' -or $pythonStatus -eq 'FAILED' -or "$packageStatus" -like '*FAILED*') { exit 1 }
exit 0
