#Requires -Version 5.1
<#
.SYNOPSIS
    Verify a shared run file (Windows counterpart of scripts/verify-run; #408, ADR 0156).
.DESCRIPTION
    Runs MapleThief --verify-run FILE, which replays the file with no window and no audio device,
    compares every checkpoint, recomputes the split times from its clock tape and the clean-run
    status from its recorded settings, and prints a report (docs/play/speedrun.md). It needs the
    game build that recorded the file (the same port version) and your own copy of the game.
    Exit status: 0 verified, 6 not verified (or refused), 4 no game; 2 usage.
.PARAMETER File
    The .mtrun file.
.PARAMETER Game
    The MapleThief.exe to run (default: $env:MAPLETHIEF_GAME, then dist\win as scripts\build.ps1
    stages it, then build\win-debug).
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\verify-run.ps1 runs\20261127-101500.mtrun
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$File,
    [string]$Game = ''
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$Repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

if (-not $Game -and $env:MAPLETHIEF_GAME) { $Game = $env:MAPLETHIEF_GAME }
if (-not $Game) {
    $candidates = @()
    $candidates += @(Get-ChildItem -Path (Join-Path $Repo 'dist/win') -Filter 'MapleThief.exe' `
            -Recurse -ErrorAction SilentlyContinue | Sort-Object FullName -Descending |
        ForEach-Object { $_.FullName })
    $candidates += (Join-Path $Repo 'build/win-debug/platform/MapleThief.exe')
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c -PathType Leaf) { $Game = $c; break }
    }
}
if (-not $Game) {
    Write-Error 'verify-run: no MapleThief.exe found: build one (scripts\build.ps1) or pass -Game EXE' `
        -ErrorAction Continue
    exit 4
}
# A Windows GUI program prints only when its output is redirected: Out-Host keeps the report.
& $Game --verify-run $File | Out-Host
exit $LASTEXITCODE
