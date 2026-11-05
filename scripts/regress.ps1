#Requires -Version 5.1
<#
.SYNOPSIS
    The regression run (Windows counterpart of scripts/regress; #91, ADR 0104).
.DESCRIPTION
    Plays the recorded replay tests/regress/title_to_combat.replay on the real game (play_real_test
    regress, the virtual clock) and checks that every run passes its expects and gives the same
    digest. Local only: needs work/gen and scripts\bootstrap.ps1. Run it from a Visual Studio x64
    developer shell (the build). The last line is "regress: digest <16 hex>".
.PARAMETER BuildPreset
    The CMake preset whose play_real_test runs (default win-debug); built first unless -NoBuild.
.PARAMETER Out
    The runs' output (default build/test-output/<preset>/regress).
.PARAMETER Rest
    Arguments for python -m mtport.regress: --repeat N, --preset NAME, --settings LIST,
    --baseline FILE, --save-baseline FILE, --replay FILE.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 --repeat 10 --preset original
#>
[CmdletBinding()]
param(
    [string]$BuildPreset = 'win-debug',
    [switch]$NoBuild,
    [string]$Out = '',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest = @()
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$Repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $Repo

if (-not $NoBuild) {
    if (-not (Test-Path -LiteralPath "build/$BuildPreset/CMakeCache.txt")) {
        & cmake --preset $BuildPreset | Out-Null
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    & cmake --build --preset $BuildPreset --target play_real_test
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
$exe = Join-Path $Repo "build/$BuildPreset/tests/play_real_test.exe"
if (-not $Out) { $Out = "build/test-output/$BuildPreset/regress" }
$py = if ($env:PYTHON) { $env:PYTHON } else { 'python' }
$env:PYTHONPATH = Join-Path $Repo 'tools'
& $py -B -m mtport.regress --binary $exe --out $Out @Rest
exit $LASTEXITCODE
