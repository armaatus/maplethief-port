#Requires -Version 5.1
<#
.SYNOPSIS
    Prepare work/ from original/MapleThief-1.5.ipa (Windows counterpart of scripts/bootstrap).

.DESCRIPTION
    Safe to re-run: every run re-verifies the IPA, re-extracts it and rewrites the same outputs.

      work/ipa/                     extracted IPA
      work/slices/MapleThief.armv6  thin slices of the main binary, split without lipo
      work/slices/MapleThief.armv7
      work/slices/SHA256SUMS        slice hashes
      work/bootstrap.json           paths + hashes + tool version for other tools

    All the work (presence, size and SHA-256 checks, extraction, splitting) is done by the
    same Python module as scripts/bootstrap (python -m mtport.bootstrap, in
    tools/mtport/bootstrap.py); this script only finds a Python that can run it. Needs Python
    3.10+: the py launcher, python3 or python on PATH, or $env:PYTHON pointing at an
    interpreter.

.PARAMETER NoLipoCheck
    Skip the lipo -thin cross-check (only ever runs on macOS).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
#>
[CmdletBinding()]
param([switch]$NoLipoCheck)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

# Probes an interpreter quietly. Windows PowerShell 5.1 turns native stderr into errors under
# 'Stop', so the probe runs leniently.
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
    # Python 3.10+ (ADR 0002). This probe stays in the script: an older Python can't be relied
    # on to run the module and explain.
    $python = Find-Python
    if ($null -eq $python) {
        $hint = 'install one from python.org (or winget install Python.Python.3.12), ' +
            "with 'uv python install 3.12', or on macOS with Homebrew (brew install python), " +
            'then put it on PATH or set $env:PYTHON to its path'
        if ($env:PYTHON) { throw "need Python 3.10+, but `$env:PYTHON ($env:PYTHON) is missing or older; $hint" }
        throw "need Python 3.10+, but none was found (tried py -3, python3, python); $hint"
    }

    $tools = Join-Path $Repo 'tools'
    if ($savedPythonPath) { $env:PYTHONPATH = $tools + [System.IO.Path]::PathSeparator + $savedPythonPath }
    else { $env:PYTHONPATH = $tools }
    # -B: write no __pycache__ into tools/.
    $moduleArgs = @($python | Select-Object -Skip 1) + @('-B', '-m', 'mtport.bootstrap', '--repo', $Repo)
    if ($NoLipoCheck) { $moduleArgs += '--no-lipo-check' }
    # Called directly (not through a function), so the module's output goes straight to
    # stdout/stderr; lenient about stderr for the same reason as Test-Python.
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
