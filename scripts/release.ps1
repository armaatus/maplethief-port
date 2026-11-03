#Requires -Version 5.1
<#
.SYNOPSIS
    Build the Windows kit of a release and upload it to the draft (ADR 0024; the Mac side is
    scripts/release).

.DESCRIPTION
    The Windows part of the release flow (docs/release-checklist.md). `start` and `publish`
    run on the Mac (scripts/release); this script only has `build`:

      1. checks the tag (annotated vX.Y.Z or vX.Y.Z-rc.N), that it is pushed and that the
         draft release exists (both skipped with -DryRun);
      2. checks the tag out into build/release/<TAG>/src as a shared clone with Git LFS's
         smudge filter off (no IPA in the checkout, and a guard checks that), sharing
         build/_downloads;
      3. runs scripts\test.ps1 -Release there with CI=true and MAPLETHIEF_VERSION from the
         tag: the win-release workflow (configure, build, CTest, package);
      4. unpacks the kit and checks that bin\MapleThief.exe --version reports the tag;
      5. runs the content guard (python -m mtport.release guard), writes <kit>.sha256;
      6. uploads the kit and its .sha256 to the draft (gh release upload; -Replace adds
         --clobber). With -DryRun the gh command is printed instead.

    Run it from the x64 Visual Studio developer environment (docs/build/README.md), with gh logged in.
    Needs Python 3.10+ (the py launcher, python3 or python on PATH, or $env:PYTHON) and uv.

.PARAMETER Command
    build (the only command on Windows).

.PARAMETER Tag
    The release tag, vX.Y.Z or vX.Y.Z-rc.N.

.PARAMETER DryRun
    No GitHub calls: they are printed, and the pushed/draft checks are skipped.

.PARAMETER Replace
    Replace assets already on the draft (gh release upload --clobber).

.PARAMETER Keep
    Keep the clone build/release/<TAG>/src and its build tree.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\release.ps1 build v0.0.1-rc.1
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('build', 'start', 'publish')][string]$Command,
    [Parameter(Mandatory = $true, Position = 1)][string]$Tag,
    [switch]$DryRun,
    [switch]$Replace,
    [switch]$Keep
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$Repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $Repo

function Fail([string]$Message) {
    [Console]::Error.WriteLine("scripts/release.ps1: $Message")
    exit 1
}
function Step([string]$Message) { Write-Host ''; Write-Host "== $Message" }

# Runs a native command and fails unless it exits 0; returns its output lines.
function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = & $Exe @Arguments } finally { $ErrorActionPreference = $saved }
    if ($LASTEXITCODE -ne 0) { Fail "$Exe $($Arguments -join ' ') failed (exit $LASTEXITCODE)" }
    return $output
}

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

# python -B -m mtport.release ARGS from this checkout; returns its output lines.
function Invoke-MtRelease([string[]]$Arguments) {
    $savedPath = $env:PYTHONPATH
    $env:PYTHONPATH = Join-Path $Repo 'tools'
    try {
        return Invoke-Native $python[0] (@($python | Select-Object -Skip 1) + @('-B', '-m', 'mtport.release') + $Arguments)
    } finally {
        $env:PYTHONPATH = $savedPath
    }
}

# Deletes the clone. Its build/_downloads link goes first, on its own: Windows PowerShell 5.1's
# Remove-Item -Recurse would follow a junction and empty the shared download cache.
function Remove-Clone([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $cache = Join-Path $Path 'build/_downloads'
    $item = Get-Item -LiteralPath $cache -Force -ErrorAction SilentlyContinue
    if ($item -and $item.LinkType) { $item.Delete() }
    Remove-Item -Recurse -Force -LiteralPath $Path
}

# gh ARGS, or print it with -DryRun.
function Invoke-Gh([string[]]$Arguments) {
    if ($DryRun) { Write-Host "   dry-run, not run: gh $($Arguments -join ' ')"; return }
    Write-Host "+ gh $($Arguments -join ' ')"
    Invoke-Native 'gh' $Arguments | Out-Host
}

if ($Command -ne 'build') {
    Fail "'$Command' runs on the Mac: scripts/release $Command $Tag"
}
$python = Find-Python
if (-not $python) { Fail 'need Python 3.10+ (the py launcher, python3 or python on PATH, or $env:PYTHON)' }

$parsed = @(Invoke-MtRelease @('tag', $Tag))
$version = $parsed[0]
$kitName = "MapleThief-kit-$version-windows-x64.zip"
$out = Join-Path $Repo "build/release/$Tag"
New-Item -ItemType Directory -Force -Path $out | Out-Null

Step 'Checks'
$tagRef = "refs/tags/$Tag"
if ((Invoke-Native 'git' @('cat-file', '-t', $tagRef)) -ne 'tag') {
    Fail "$Tag is not an annotated tag (git tag -a $Tag -m ...)"
}
$commit = Invoke-Native 'git' @('rev-parse', "$Tag^{commit}")
Write-Host "   tag $Tag ($version): $commit"
if ($DryRun) {
    Write-Host '   dry-run: not checking that the tag is pushed and the draft release exists'
} else {
    $remote = Invoke-Native 'git' @('ls-remote', '--tags', 'origin', $tagRef)
    $local = Invoke-Native 'git' @('rev-parse', $tagRef)
    if (-not $remote -or ($remote -split "`t")[0] -ne $local) { Fail "$Tag is not pushed to origin" }
    $isDraft = Invoke-Native 'gh' @('release', 'view', $Tag, '--json', 'isDraft', '--jq', '.isDraft')
    if ($isDraft -ne 'true') { Fail "no draft release $Tag; run scripts/release start $Tag on the Mac" }
    Write-Host '   ok: pushed, draft release exists'
}

Step "Clean checkout of $Tag"
$src = Join-Path $out 'src'
Remove-Clone $src
Invoke-Native 'git' @('clone', '--quiet', '--shared', '--no-checkout', $Repo, $src) | Out-Null
Invoke-Native 'git' @('-C', $src, '-c', 'filter.lfs.process=', '-c', 'filter.lfs.smudge=cat',
    '-c', 'filter.lfs.required=false', '-c', 'advice.detachedHead=false', 'checkout', '--quiet', $commit) | Out-Null
# The same IPA guard as CI and scripts/local-ci: the checkout holds no *.ipa (ADR 0100).
$ipas = @(Get-ChildItem -LiteralPath $src -Recurse -Force -Filter '*.ipa' |
    Where-Object { $_.FullName -notlike "*$([IO.Path]::DirectorySeparatorChar).git$([IO.Path]::DirectorySeparatorChar)*" })
if ($ipas.Count -gt 0) {
    Fail "IPA guard: $($ipas[0].FullName) is in the checkout; the repository must never contain an IPA"
}
Write-Host "   IPA guard: OK, no *.ipa in the checkout"
$downloads = Join-Path $Repo 'build/_downloads'
New-Item -ItemType Directory -Force -Path $downloads, (Join-Path $src 'build') | Out-Null
$link = Join-Path $src 'build/_downloads'
if ($env:OS -eq 'Windows_NT') { New-Item -ItemType Junction -Path $link -Target $downloads | Out-Null }
else { New-Item -ItemType SymbolicLink -Path $link -Target $downloads | Out-Null }

Step "scripts\test.ps1 -Release (MAPLETHIEF_VERSION=$version, CI=true)"
$saved = @{ ci = $env:CI; version = $env:MAPLETHIEF_VERSION }
$env:CI = 'true'
$env:MAPLETHIEF_VERSION = $version
Push-Location -LiteralPath $src
try {
    & (Join-Path $src 'scripts/test.ps1') -Release
    $testStatus = $LASTEXITCODE
} finally {
    Pop-Location
    $env:CI = $saved.ci
    $env:MAPLETHIEF_VERSION = $saved.version
}
if ($testStatus -ne 0) { Fail "scripts\test.ps1 -Release failed on $Tag" }

Step 'Kit'
$built = Join-Path $src "build/win-release/$kitName"
if (-not (Test-Path -LiteralPath $built)) { Fail "no $built" }
$kit = Join-Path $out $kitName
Copy-Item -LiteralPath $built -Destination $kit -Force
Write-Host "-- $kit ($((Get-Item -LiteralPath $kit).Length) bytes)"
$unpack = Join-Path $out 'unpack'
if (Test-Path -LiteralPath $unpack) { Remove-Item -Recurse -Force -LiteralPath $unpack }
Expand-Archive -LiteralPath $kit -DestinationPath $unpack
$exe = Get-ChildItem -LiteralPath $unpack -Recurse -File |
    Where-Object { $_.Name -in @('MapleThief.exe', 'MapleThief') -and $_.Directory.Name -eq 'bin' } |
    Select-Object -First 1
if (-not $exe) { Fail "$kitName has no bin\MapleThief.exe" }
# A GUI program prints only into a redirected stdout: Out-String collects it.
$got = (& $exe.FullName --version | Out-String).Trim()
Remove-Item -Recurse -Force -LiteralPath $unpack
if ($got -ne "MapleThief $version") { Fail "--version printed '$got', expected 'MapleThief $version'" }
Write-Host "   --version: $got"

Step 'Content guard'
Invoke-MtRelease @('guard', $kit) | Out-Host

Step 'Checksum'
Invoke-MtRelease @('sha256', $kit) | Out-Host

Step 'Upload to the draft'
$ghArgs = @('release', 'upload', $Tag, $kit, "$kit.sha256")
if ($Replace) { $ghArgs += '--clobber' }
Invoke-Gh $ghArgs

if (-not $Keep) { Remove-Clone $src }
Write-Host ''
Write-Host "Done: $kitName. Then on the Mac: scripts/release publish $Tag"
exit 0
