#Requires -Version 5.1
<#
.SYNOPSIS
    Build the game from your own copy of it, wherever that file is (Windows side of
    scripts/build-from-ipa, #85, ADR 0135).

.DESCRIPTION
      scripts\build-from-ipa.ps1 PATH [win|linux] [debug|release]

    The same as `scripts\build.ps1 --ipa PATH`, with the path first; tools/mtport/build.py does
    all the work. Quote a PATH that holds a space.

    The result holds the game and is for your own machines only.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build-from-ipa.ps1 "C:\My Games\MapleThief-1.5.ipa"
#>
# No param(): the words go to the module unchanged, as in scripts\build.ps1.

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

if ($args.Count -lt 1 -or $args[0] -in @('-h', '--help')) {
    [Console]::Error.WriteLine('usage: scripts\build-from-ipa.ps1 PATH [win|linux] [debug|release]')
    exit 2
}

$rest = @($args | Select-Object -Skip 1)
& (Join-Path $PSScriptRoot 'build.ps1') "--ipa=$($args[0])" @rest
exit $LASTEXITCODE
