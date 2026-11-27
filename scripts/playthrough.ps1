#Requires -Version 5.1
<#
.SYNOPSIS
    The playthrough (Windows counterpart of scripts/playthrough; #306).
.DESCRIPTION
    Plays the save-chained whole-game runs (chapter 1's parts, chapter 2's main parts, chapter
    3's, the ending and the post-game) through CTest in Release, with ADR 0104's checkpoint
    digest at the end of each part, a summary and the chain's digest. Local only: needs work/gen
    and scripts\bootstrap.ps1. Run it from a Visual Studio x64 developer shell (the build). The
    last line is "playthrough: digest <16 hex>".
.PARAMETER BuildPreset
    The CMake preset whose play_real_test plays (default win-release); built first unless
    -NoBuild.
.PARAMETER Out
    The runs' output (default build/test-output/<preset>/playthrough).
.PARAMETER Rest
    Arguments for python -m mtport.playthrough: --quick, --tests REGEX, --repeat N,
    --baseline FILE, --save-baseline FILE, --every N, --nudge-tick STEP.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\playthrough.ps1 --quick --baseline work\regress\quick.txt
#>
[CmdletBinding()]
param(
    [string]$BuildPreset = 'win-release',
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
if (-not $Out) { $Out = "build/test-output/$BuildPreset/playthrough" }
$py = if ($env:PYTHON) { $env:PYTHON } else { 'python' }
$env:PYTHONPATH = Join-Path $Repo 'tools'
& $py -B -m mtport.playthrough --build "build/$BuildPreset" --out $Out @Rest
exit $LASTEXITCODE
