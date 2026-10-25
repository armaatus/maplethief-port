#Requires -Version 5.1
<#
.SYNOPSIS
    Convert the extracted bundle into assets-out/ (Windows counterpart of scripts/pipeline).

.DESCRIPTION
    Reads work/ipa/Payload/MapleThief.app (from scripts/bootstrap.ps1) and writes assets-out/:
    standard PNGs, JSON for maps, sprites, tables, strings and the nib, script disassembly, the
    png/**/*.dat packs as single images, and assets-out/manifest.json listing every bundle file
    with its outcome (issue #25, ADR 0010, ADR 0011). Incremental and deterministic: a second
    run writes nothing. assets-out/viewers/ is never touched.

    All the work is done by the same Python module as scripts/pipeline (python -m
    mtport.pipeline, in tools/mtport/pipeline.py); this script only finds a Python that can run
    it. Needs Python 3.10+: the py launcher, python3 or python on PATH, or $env:PYTHON.

.PARAMETER Command
    run (the default) converts; check is a dry run that exits 1 if anything is out of date.

.PARAMETER App
    The MapleThief.app directory (default work/ipa/Payload/MapleThief.app).

.PARAMETER Out
    The output directory (default assets-out/).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\pipeline.ps1 check
#>
[CmdletBinding()]
param(
    [ValidateSet('run', 'check')][string]$Command = 'run',
    [string]$App,
    [string]$Out
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

# Probes an interpreter quietly (Windows PowerShell 5.1 turns native stderr into errors).
function Test-Python([string]$Exe, [string[]]$Fixed) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Exe @Fixed -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' *> $null
        return ($LASTEXITCODE -eq 0)
    } catch {
        return $false
    } finally {
        $ErrorActionPreference = $saved
    }
}

# Returns the interpreter as an array: executable followed by its fixed arguments.
function Find-Python {
    if ($env:PYTHON) { $candidates = @(, @($env:PYTHON)) }
    else { $candidates = @(@('py', '-3'), @('python3'), @('python')) }
    foreach ($candidate in $candidates) {
        $exe = $candidate[0]
        $fixed = @($candidate | Select-Object -Skip 1)
        if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) { continue }
        if (Test-Python $exe $fixed) { return , $candidate }
    }
    return $null
}

$Repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$savedPythonPath = $env:PYTHONPATH
$exitCode = 0
try {
    $python = Find-Python
    if ($null -eq $python) {
        throw 'need Python 3.10+, but none was found (tried $env:PYTHON, py -3, python3, python)'
    }
    $tools = Join-Path $Repo 'tools'
    if ($savedPythonPath) { $env:PYTHONPATH = $tools + [System.IO.Path]::PathSeparator + $savedPythonPath }
    else { $env:PYTHONPATH = $tools }
    # -B: write no __pycache__ into tools/.
    $moduleArgs = @($python | Select-Object -Skip 1) + @('-B', '-m', 'mtport.pipeline', $Command)
    if ($App) { $moduleArgs += @('--app', $App) }
    if ($Out) { $moduleArgs += @('--out', $Out) }
    $ErrorActionPreference = 'Continue'
    & $python[0] @moduleArgs
    $exitCode = $LASTEXITCODE
} catch {
    [Console]::Error.WriteLine("error: $($_.Exception.Message)")
    $exitCode = 1
} finally {
    $ErrorActionPreference = 'Stop'
    $env:PYTHONPATH = $savedPythonPath
}
exit $exitCode
