#Requires -Version 5.1
<#
.SYNOPSIS
    Differential test of the lifted leaf functions against Unicorn (Windows counterpart of
    scripts/difftest; #41, ADR 0017).
.DESCRIPTION
    Local only: needs the game binary (scripts/bootstrap) and the GPL-2.0 `difftest`
    dependency group, which never ships and which CI does not install. Runs in its own
    environment, build/python-env/difftest-<os>-<cpu>, with the `test` and `difftest` groups.
    The C side needs a C compiler: run it from a Visual Studio developer shell (cl), or set
    $env:MAPLETHIEF_CC.
.PARAMETER Pytest
    Run the difftest tests (tests/test_recomp_difftest*.py) instead of the command line.
.PARAMETER Rest
    Arguments for `python -m recomp difftest` (or for pytest with -Pytest).
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\difftest.ps1 --jobs 8
#>
[CmdletBinding()]
param(
    [switch]$Pytest,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest = @()
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$Repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $Repo

if (-not (Get-Command uv -ErrorAction SilentlyContinue)) {
    Write-Host 'scripts/difftest.ps1: uv not found. Install uv: https://docs.astral.sh/uv/'
    exit 2
}
if ($env:OS -eq 'Windows_NT') { $platform = "Windows-$env:PROCESSOR_ARCHITECTURE" }
else { $platform = "$(uname -s)-$(uname -m)" }
$env:UV_PROJECT_ENVIRONMENT = Join-Path $Repo "build/python-env/difftest-$platform"
$env:PYTHONPYCACHEPREFIX = Join-Path $Repo 'build/pycache'
if ($Pytest) {
    & uv run --locked --group test --group difftest pytest -p no:cacheprovider `
        tests/test_dependencies.py tests/test_recomp_difftest.py tests/test_recomp_difftest_real.py @Rest
} else {
    & uv run --locked --group test --group difftest python -m recomp difftest @Rest
}
exit $LASTEXITCODE
