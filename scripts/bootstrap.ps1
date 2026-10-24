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

    Step 1 is a PowerShell port of scripts/verify-original.sh (LFS pointer, size and SHA-256
    checks, extraction). Step 2 runs the same Python splitter as scripts/bootstrap
    (python -m mtport.bootstrap). Needs Python 3.10+: the py launcher, python3 or python on
    PATH, or $env:PYTHON pointing at an interpreter.

.PARAMETER NoLipoCheck
    Skip the lipo -thin cross-check (only ever runs on macOS).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
#>
[CmdletBinding()]
param([switch]$NoLipoCheck)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Ipa = 'original/MapleThief-1.5.ipa'
$Sums = 'original/SHA256SUMS'
$Out = 'work/ipa'
$Bin = 'work/ipa/Payload/MapleThief.app/MapleThief'
$ExpectedSize = 17872861

function Fail([string]$Message) { throw (New-Object System.InvalidOperationException $Message) }

function Get-RepoPath([string]$Relative) {
    return [System.IO.Path]::GetFullPath((Join-Path $script:Repo $Relative))
}

function Get-Sha256([string]$Relative) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath (Get-RepoPath $Relative)).Hash.ToLowerInvariant()
}

function Read-Sums {
    $table = @{}
    foreach ($line in [System.IO.File]::ReadAllLines((Get-RepoPath $Sums))) {
        if ($line -match '^([0-9a-fA-F]{64}) [ *]?(.+)$') { $table[$Matches[2].Trim()] = $Matches[1].ToLowerInvariant() }
    }
    return $table
}

function Test-Entry([hashtable]$Table, [string]$Relative) {
    if (-not $Table.ContainsKey($Relative)) { Fail "no entry for $Relative in $Sums" }
    $want = $Table[$Relative]
    $got = Get-Sha256 $Relative
    if ($got -ne $want) { Fail "${Relative}: SHA-256 mismatch (got $got, want $want)" }
    Write-Output "ok  $Relative"
}

function Test-LfsPointer([string]$Relative) {
    $stream = [System.IO.File]::OpenRead((Get-RepoPath $Relative))
    try {
        $buffer = New-Object byte[] 64
        $count = $stream.Read($buffer, 0, 64)
    } finally {
        $stream.Dispose()
    }
    return [System.Text.Encoding]::ASCII.GetString($buffer, 0, $count).Contains('git-lfs.github.com/spec')
}

# Returns the interpreter as an array: executable followed by its fixed arguments.
function Find-Python {
    if ($env:PYTHON) { $candidates = @(, @($env:PYTHON)) }
    else { $candidates = @(@('py', '-3'), @('python3'), @('python')) }
    foreach ($candidate in $candidates) {
        $exe = $candidate[0]
        $fixed = @($candidate | Select-Object -Skip 1)
        if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) { continue }
        # Windows PowerShell 5.1 turns native stderr into errors under 'Stop'; probe leniently.
        $saved = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $exe @fixed -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' *> $null
            $ok = ($LASTEXITCODE -eq 0)
        } catch {
            $ok = $false
        } finally {
            $ErrorActionPreference = $saved
        }
        if ($ok) { return , $candidate }
    }
    return $null
}

$Repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$savedPythonPath = $env:PYTHONPATH
$exitCode = 0
Push-Location -LiteralPath $Repo
try {
    # Tools first, so a missing tool is reported before any work is done.
    $python = Find-Python
    if ($null -eq $python) {
        # Python 3.10+ (ADR 0002).
        $hint = 'install one from python.org (or winget install Python.Python.3.12), ' +
            "with 'uv python install 3.12', or on macOS with Homebrew (brew install python), " +
            'then put it on PATH or set $env:PYTHON to its path'
        if ($env:PYTHON) { Fail "need Python 3.10+, but `$env:PYTHON ($env:PYTHON) is missing or older; $hint" }
        Fail "need Python 3.10+, but none was found (tried py -3, python3, python); $hint"
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    # Step 1: same checks as scripts/verify-original.sh, then extract to work/ipa/.
    if (-not (Test-Path -LiteralPath (Get-RepoPath $Sums) -PathType Leaf)) { Fail "$Sums missing" }
    if (-not (Test-Path -LiteralPath (Get-RepoPath $Ipa) -PathType Leaf)) { Fail "$Ipa missing" }
    # A clone without Git LFS leaves a ~130-byte pointer file instead of the IPA.
    if (Test-LfsPointer $Ipa) { Fail "$Ipa is a Git LFS pointer; run 'git lfs install && git lfs pull' first" }
    $size = (Get-Item -LiteralPath (Get-RepoPath $Ipa)).Length
    if ($size -ne $ExpectedSize) { Fail "${Ipa}: size $size, expected $ExpectedSize" }
    $table = Read-Sums
    Test-Entry $table $Ipa

    $outFull = Get-RepoPath $Out
    if (Test-Path -LiteralPath $outFull) { Remove-Item -LiteralPath $outFull -Recurse -Force }
    New-Item -ItemType Directory -Path $outFull -Force | Out-Null
    # ZipFile rejects entries that would land outside the destination (zip-slip).
    [System.IO.Compression.ZipFile]::ExtractToDirectory((Get-RepoPath $Ipa), $outFull)
    if (-not (Test-Path -LiteralPath (Get-RepoPath $Bin) -PathType Leaf)) { Fail "extraction did not produce $Bin" }
    Test-Entry $table $Bin
    Write-Output "extracted to $Out/"

    # Step 2: split the slices, verify them, write work/slices/SHA256SUMS + work/bootstrap.json.
    $tools = Get-RepoPath 'tools'
    if ($savedPythonPath) { $env:PYTHONPATH = $tools + [System.IO.Path]::PathSeparator + $savedPythonPath }
    else { $env:PYTHONPATH = $tools }
    $exe = $python[0]
    $fixed = @($python | Select-Object -Skip 1)
    $splitArgs = @('-m', 'mtport.bootstrap', '--repo', $Repo)
    if ($NoLipoCheck) { $splitArgs += '--no-lipo-check' }
    & $exe @fixed @splitArgs
    if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE }
} catch {
    [Console]::Error.WriteLine("error: $($_.Exception.Message)")
    $exitCode = 1
} finally {
    $env:PYTHONPATH = $savedPythonPath
    Pop-Location
}
exit $exitCode
