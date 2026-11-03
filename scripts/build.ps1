#Requires -Version 5.1
<#
.SYNOPSIS
    Build the playable Release game from your own IPA in one step (Windows side of `just build`).

.DESCRIPTION
    Called by the justfile (`just build`, `just build linux`); can also be run directly.

      windows  1. enters the Visual Studio x64 developer environment (vswhere) unless it is
                  already active;
               2. scripts\bootstrap.ps1 (checks and extracts the IPA into work\);
               3. py -m recomp all (the generated C in work\gen; rewrites only what changed);
               4. cmake --preset win-release, then cmake --build --preset win-release
                  --target MapleThief (the game only, no test executables).
               Result: build\win-release\platform\MapleThief.exe, linked with the game.
      linux    the bootstrap and recompiler as above, then scripts/linux-game build (Docker)
               through Git for Windows' bash.
      macos    only on a Mac (scripts/build).

    Nothing here is packaged or uploaded: the result holds the game and is for your own
    machines only.

.PARAMETER Target
    windows (default), linux or macos; a leading dash is ignored (-linux).

.EXAMPLE
    just build
#>
[CmdletBinding()]
param([string]$Target = 'windows')

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $Repo

function Fail([string]$Message) {
    [Console]::Error.WriteLine("scripts/build.ps1: $Message")
    exit 1
}

# Runs a native command and stops on a non-zero exit. Native stderr is not an error (Windows
# PowerShell 5.1 would otherwise turn every warning line into one under 'Stop').
function Invoke-Step([string]$Title, [scriptblock]$Command) {
    Write-Host "== $Title"
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Command } finally { $ErrorActionPreference = $saved }
    if ($LASTEXITCODE -ne 0) { Fail "$Title failed (exit $LASTEXITCODE)" }
}

# Python 3.10+, found as scripts\bootstrap.ps1 finds it: $env:PYTHON, py -3, python3, python.
function Find-Python {
    if ($env:PYTHON) { $candidates = @(, @($env:PYTHON)) }
    else { $candidates = @(@('py', '-3'), @('python3'), @('python')) }
    foreach ($candidate in $candidates) {
        if (-not (Get-Command $candidate[0] -ErrorAction SilentlyContinue)) { continue }
        $fixed = @($candidate | Select-Object -Skip 1)
        $saved = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $candidate[0] @fixed -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' *> $null
            if ($LASTEXITCODE -eq 0) { return , $candidate }
        } catch {
        } finally { $ErrorActionPreference = $saved }
    }
    Fail 'need Python 3.10+ (tried $env:PYTHON, py -3, python3, python); see README'
}

# The x64 MSVC environment the win-* presets need (README, ADR 0004). Skipped when it is
# already active (an x64 Native Tools prompt, or a second run in the same shell).
function Enter-VsX64 {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl -ErrorAction SilentlyContinue)) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        Fail 'Visual Studio 2022 not found (no vswhere.exe); install it with the "Desktop development with C++" workload'
    }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { Fail 'no Visual Studio with the x64 C++ tools ("Desktop development with C++")' }
    Write-Host "== Visual Studio x64 environment ($vs)"
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    Set-Location -LiteralPath $Repo
}

# Git for Windows' bash (not WSL's C:\Windows\System32\bash.exe, which PATH may find first).
function Find-GitBash {
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        $root = Split-Path (Split-Path $git.Source)            # ...\Git\cmd\git.exe -> ...\Git
        foreach ($candidate in @('bin\bash.exe', 'usr\bin\bash.exe')) {
            $path = Join-Path $root $candidate
            if (Test-Path -LiteralPath $path) { return $path }
        }
    }
    Fail 'Git for Windows bash not found (needed to run scripts/linux-game)'
}

$Target = $Target.TrimStart('-').ToLowerInvariant()
switch ($Target) {
    { $_ -in 'windows', 'win' } { $Target = 'windows' }
    'linux' { }
    { $_ -in 'macos', 'mac' } { Fail 'the macOS app builds only on a Mac (just build there)' }
    'ios' { Fail 'there is no iOS build in this repo yet' }
    default { Fail "unknown target '$Target' (windows, linux or macos)" }
}

$python = Find-Python
$pyExe = $python[0]
$pyArgs = @($python | Select-Object -Skip 1)

Invoke-Step 'bootstrap (work/ipa)' { & (Join-Path $PSScriptRoot 'bootstrap.ps1') }
Invoke-Step 'recompile (work/gen)' { & $pyExe @pyArgs -B -m recomp all }

if ($Target -eq 'windows') {
    Enter-VsX64
    Invoke-Step 'configure (win-release)' { & cmake --preset win-release }
    # Only the game (and what it links), not the test executables.
    Invoke-Step 'build (win-release)' { & cmake --build --preset win-release --target MapleThief }
    $exe = Join-Path $Repo 'build\win-release\platform\MapleThief.exe'
    if (-not (Test-Path -LiteralPath $exe)) { Fail "the build finished but $exe is missing" }
    Write-Host ''
    Write-Host "Built: $exe"
} else {
    $bash = Find-GitBash
    $env:PYTHON = (Get-Command $pyExe).Source
    if ($pyArgs.Count -gt 0) {
        # bash can't pass `py -3` as one interpreter; use the interpreter py resolves to.
        $env:PYTHON = (& $pyExe @pyArgs -c 'import sys; print(sys.executable)').Trim()
    }
    Invoke-Step 'scripts/linux-game build (Docker)' { & $bash scripts/linux-game build }
    Write-Host ''
    Write-Host "Built: $(Join-Path $Repo 'build\linux-game')"
}
exit 0
