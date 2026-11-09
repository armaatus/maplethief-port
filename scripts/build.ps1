#Requires -Version 5.1
<#
.SYNOPSIS
    One command from a fresh clone to a playable build (Windows side of scripts/build, #141,
    ADR 0133). `just build` calls this, and nobody needs just installed.

.DESCRIPTION
      scripts\build.ps1 [win|linux] [debug|release]  the game for PLATFORM, into dist\<platform>\
      scripts\build.ps1 run | bootstrap | assets | recomp | shaders | package | clean | doctor

    "windows" and "macos" work too, and a leading dash is ignored (-linux). `scripts\build.ps1
    --help` lists every word; Python's mtport.build reads them all, so a new argument needs
    no change here. Each step is skipped when it is up to date.

    This script only finds Python 3.10+ ($env:PYTHON, py -3, python3, python) and, for a Windows
    build, enters the Visual Studio x64 developer environment (vswhere) unless it is already
    active. All the work is done by tools/mtport/build.py, the same module scripts/build runs.

    The result holds the game and is for your own machines only.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1 win
#>
# No param(): every word goes to the module unchanged, -linux included.

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $Repo
$Arguments = @($args)

function Fail([string]$Message) {
    [Console]::Error.WriteLine("scripts/build.ps1: $Message")
    exit 1
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
    Fail 'need Python 3.10+ (tried $env:PYTHON, py -3, python3, python); see docs/build/README.md'
}

# The x64 MSVC environment the win-* presets need (docs/build/README.md, ADR 0004). Skipped when it
# is already active (an x64 Native Tools prompt, or a second run in the same shell).
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

$python = Find-Python
$pyExe = $python[0]
$pyArgs = @($python | Select-Object -Skip 1)
$tools = Join-Path $Repo 'tools'
if ($env:PYTHONPATH) { $env:PYTHONPATH = $tools + [System.IO.Path]::PathSeparator + $env:PYTHONPATH }
else { $env:PYTHONPATH = $tools }

# Native stderr is not an error (Windows PowerShell 5.1 would otherwise turn every warning line
# into one under 'Stop').
$ErrorActionPreference = 'Continue'
# The module says whether these words compile with MSVC, so the words are read only there.
$needsMsvc = & $pyExe @pyArgs -B -m mtport.build --needs-msvc @Arguments 2>$null
$needsStatus = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($needsStatus -eq 0 -and "$needsMsvc" -eq 'yes') { Enter-VsX64 }

$ErrorActionPreference = 'Continue'
# -B: write no __pycache__ into tools/.
& $pyExe @pyArgs -B -m mtport.build @Arguments
exit $LASTEXITCODE
