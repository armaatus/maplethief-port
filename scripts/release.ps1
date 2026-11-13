#Requires -Version 5.1
<#
.SYNOPSIS
    Build the Windows kit and player build of a release and upload them to the draft (ADR 0024,
    ADR 0125; the Mac side is scripts/release).

.DESCRIPTION
    The Windows part of the release flow (docs/release-checklist.md). `start` and `publish`
    run on the Mac (scripts/release); this script has `build` and `itch`. `build`:

      1. checks the tag (annotated vX.Y.Z or vX.Y.Z-rc.N), that it is pushed and that the
         draft release exists (both skipped with -DryRun), and that work/gen/ exists;
      2. checks the tag out into build/release/<TAG>/src as a shared clone with Git LFS's
         smudge filter off (no IPA in the checkout, and a guard checks that), sharing
         build/_downloads;
      3. runs scripts\test.ps1 -Release there with CI=true and MAPLETHIEF_VERSION from the
         tag: the win-release workflow (configure, build, CTest, package), the kit;
      4. links this checkout's work/gen/ into the clone (a junction, read only) and runs the
         win-player workflow there the same way: the player build (ADR 0125), the game with
         the recompiled code and none of its assets, MapleThief-<version>-windows-x64.zip;
      5. unpacks each and checks that MapleThief.exe --version reports the tag, and that the
         player's executable needs no MSVC runtime DLL (dumpbin /dependents);
      6. runs the content guard (python -m mtport.release guard): the kit in kit mode, the
         player build in player mode; writes <file>.sha256;
      7. uploads both and their .sha256 to the draft (gh release upload; -Replace adds
         --clobber). With -DryRun the gh command is printed instead.

    Since ADR 0126 a merged release pull request builds and uploads the kits on hosted CI
    (.github/workflows/release.yml); -PlayersOnly skips step 3 and the kit, and builds,
    checks and uploads the player build only.

    `itch` (ADR 0146), after `scripts/release publish` on the Mac: downloads the published
    release's Windows player build and SHA256SUMS, checks the file against SHA256SUMS and the
    content guard's player mode, and pushes it to the itch.io channel windows-x64 with the
    pinned butler (python -m mtport.release itch TAG --platforms windows). butler reads its key
    from $env:BUTLER_API_KEY. With -DryRun the butler commands are printed instead.

    Run it from the x64 Visual Studio developer environment (docs/build/README.md), with gh logged in.
    Needs Python 3.10+ (the py launcher, python3 or python on PATH, or $env:PYTHON), uv, and
    work/gen/ in this checkout (scripts\bootstrap.ps1, then python -m recomp all).

.PARAMETER Command
    build, or itch (push the published release's Windows player build to itch.io).

.PARAMETER Tag
    The release tag, vX.Y.Z or vX.Y.Z-rc.N.

.PARAMETER DryRun
    No GitHub calls: they are printed, and the pushed/draft checks are skipped.

.PARAMETER Replace
    Replace assets already on the draft (gh release upload --clobber).

.PARAMETER PlayersOnly
    The player build only; release.yml built and uploaded the kit.

.PARAMETER Keep
    Keep the clone build/release/<TAG>/src and its build tree.

.PARAMETER From
    itch: take the player build and SHA256SUMS from this directory, not the published release.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\release.ps1 build v0.0.1-rc.1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\release.ps1 itch v1.0.0 -DryRun
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('build', 'itch', 'start', 'publish')][string]$Command,
    [Parameter(Mandatory = $true, Position = 1)][string]$Tag,
    [switch]$DryRun,
    [switch]$Replace,
    [switch]$PlayersOnly,
    [switch]$Keep,
    [string]$From
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

# Deletes the clone. Its build/_downloads and work/gen links go first, on their own: Windows
# PowerShell 5.1's Remove-Item -Recurse would follow a junction and empty the shared download
# cache or this checkout's generated code.
function Remove-Clone([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    foreach ($link in @('build/_downloads', 'work/gen')) {
        $item = Get-Item -LiteralPath (Join-Path $Path $link) -Force -ErrorAction SilentlyContinue
        if ($item -and $item.LinkType) { $item.Delete() }
    }
    Remove-Item -Recurse -Force -LiteralPath $Path
}

# Unpacks a zip into $out/unpack, runs <name>.exe --version from it and fails unless it reports
# the tag's version; returns the executable's path (the unpacked copy is deleted by the caller).
function Test-Version([string]$Zip, [string]$Name) {
    $unpack = Join-Path $out 'unpack'
    if (Test-Path -LiteralPath $unpack) { Remove-Item -Recurse -Force -LiteralPath $unpack }
    Expand-Archive -LiteralPath $Zip -DestinationPath $unpack
    $exe = Get-ChildItem -LiteralPath $unpack -Recurse -File |
        Where-Object { $_.Name -eq 'MapleThief.exe' } | Select-Object -First 1
    if (-not $exe) { Fail "$Name has no MapleThief.exe" }
    # A GUI program prints only into a redirected stdout: Out-String collects it.
    $got = (& $exe.FullName --version | Out-String).Trim()
    if ($got -ne "MapleThief $version") { Fail "$($Name): --version printed '$got', expected 'MapleThief $version'" }
    Write-Host "   $($exe.FullName.Substring($unpack.Length + 1)) --version: $got"
    return $exe.FullName
}

# gh ARGS, or print it with -DryRun.
function Invoke-Gh([string[]]$Arguments) {
    if ($DryRun) { Write-Host "   dry-run, not run: gh $($Arguments -join ' ')"; return }
    Write-Host "+ gh $($Arguments -join ' ')"
    Invoke-Native 'gh' $Arguments | Out-Host
}

if ($Command -notin @('build', 'itch')) {
    Fail "'$Command' runs on the Mac: scripts/release $Command $Tag"
}
$python = Find-Python
if (-not $python) { Fail 'need Python 3.10+ (the py launcher, python3 or python on PATH, or $env:PYTHON)' }

if ($Command -eq 'itch') {
    # The Windows channel only; the Mac pushes macOS, Linux and the Switch (scripts/release itch,
    # ADR 0146).
    $itchArgs = @('itch', $Tag, '--platforms', 'windows')
    if ($DryRun) { $itchArgs += '--dry-run' }
    if ($From) { $itchArgs += @('--from', $From) }
    # Run directly, not through Invoke-MtRelease, so the checks print as they go.
    $savedPath = $env:PYTHONPATH
    $env:PYTHONPATH = Join-Path $Repo 'tools'
    if ($savedPath) { $env:PYTHONPATH += [IO.Path]::PathSeparator + $savedPath }
    try {
        & $python[0] @(@($python | Select-Object -Skip 1) + @('-B', '-m', 'mtport.release') + $itchArgs)
        $itchStatus = $LASTEXITCODE
    } finally {
        $env:PYTHONPATH = $savedPath
    }
    if ($itchStatus -ne 0) { Fail "itch failed on $Tag" }
    exit 0
}

$parsed = @(Invoke-MtRelease @('tag', $Tag))
$version = $parsed[0]
$kitName = "MapleThief-kit-$version-windows-x64.zip"
$playerName = "MapleThief-$version-windows-x64.zip"
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
# The player build links the recompiled code of this checkout (ADR 0125).
$gen = Join-Path $Repo 'work/gen'
if (-not (Test-Path -LiteralPath (Join-Path $gen 'gen.cmake'))) {
    Fail 'no work/gen/gen.cmake for the player build: scripts\bootstrap.ps1, then python -m recomp all'
}
Write-Host "   ok: generated code in $gen"

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

if ($PlayersOnly) {
    Write-Host '   -PlayersOnly: the kit is release.yml''s'
} else {
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
}

Step "cmake --workflow --preset win-player (MAPLETHIEF_VERSION=$version, CI=true)"
# The clone's work/gen is this checkout's (a junction; the build only reads it). Nothing else
# from work/ is linked: the player build needs no bundle.
New-Item -ItemType Directory -Force -Path (Join-Path $src 'work') | Out-Null
New-Item -ItemType Junction -Path (Join-Path $src 'work/gen') -Target $gen | Out-Null
$saved = @{ ci = $env:CI; version = $env:MAPLETHIEF_VERSION }
$env:CI = 'true'
$env:MAPLETHIEF_VERSION = $version
Push-Location -LiteralPath $src
try {
    & cmake --workflow --preset win-player
    $playerStatus = $LASTEXITCODE
} finally {
    Pop-Location
    $env:CI = $saved.ci
    $env:MAPLETHIEF_VERSION = $saved.version
}
if ($playerStatus -ne 0) { Fail "the win-player workflow failed on $Tag" }

$kit = $null
if (-not $PlayersOnly) {
    Step 'Kit'
    $built = Join-Path $src "build/win-release/$kitName"
    if (-not (Test-Path -LiteralPath $built)) { Fail "no $built" }
    $kit = Join-Path $out $kitName
    Copy-Item -LiteralPath $built -Destination $kit -Force
    Write-Host "-- $kit ($((Get-Item -LiteralPath $kit).Length) bytes)"
    $exe = Test-Version $kit $kitName
    if ((Split-Path -Leaf (Split-Path -Parent $exe)) -ne 'bin') { Fail "$kitName has no bin\MapleThief.exe" }
    Remove-Item -Recurse -Force -LiteralPath (Join-Path $out 'unpack')
}

Step 'Player build'
$built = Join-Path $src "build/win-player/$playerName"
if (-not (Test-Path -LiteralPath $built)) { Fail "no $built" }
$player = Join-Path $out $playerName
Copy-Item -LiteralPath $built -Destination $player -Force
Write-Host "-- $player ($((Get-Item -LiteralPath $player).Length) bytes)"
$exe = Test-Version $player $playerName
# Static MSVC runtime (win-player preset): a player needs no Visual C++ redistributable.
if (Get-Command dumpbin -ErrorAction SilentlyContinue) {
    $deps = @(Invoke-Native 'dumpbin' @('/nologo', '/dependents', $exe) | Where-Object { $_ -match '\.dll\s*$' } |
        ForEach-Object { $_.Trim() })
    $crt = @($deps | Where-Object { $_ -match '^(vcruntime|msvcp|ucrtbase|api-ms-win-crt)' })
    if ($crt.Count -gt 0) { Fail "$playerName needs the MSVC runtime: $($crt -join ', ')" }
    Write-Host "   dumpbin /dependents: $($deps -join ', ') (no MSVC runtime DLL)"
} else {
    Write-Host '   WARNING: no dumpbin (not in the VS developer environment); MSVC runtime not checked'
}
Remove-Item -Recurse -Force -LiteralPath (Join-Path $out 'unpack')

Step 'Content guard'
if ($kit) { Invoke-MtRelease @('guard', $kit) | Out-Host }
Invoke-MtRelease @('guard', '--mode', 'player', $player) | Out-Host

Step 'Checksums'
$files = @($player)
if ($kit) { $files = @($kit, $player) }
Invoke-MtRelease (@('sha256') + $files) | Out-Host

Step 'Upload to the draft'
$ghArgs = @('release', 'upload', $Tag)
foreach ($f in $files) { $ghArgs += @($f, "$f.sha256") }
if ($Replace) { $ghArgs += '--clobber' }
Invoke-Gh $ghArgs

if (-not $Keep) { Remove-Clone $src }
Write-Host ''
Write-Host "Done: $(($files | ForEach-Object { Split-Path -Leaf $_ }) -join ', '). Then on the Mac: scripts/release publish $Tag"
exit 0
