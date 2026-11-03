# One command for a playable Release build of the game from your own IPA (README, "One-command build").
#
#   just build            the game for this OS: Windows .exe, macOS .app, or the Linux folder
#   just build linux      the Linux / Steam Deck folder, built in Docker (from any OS)
#   just build windows    (only on Windows)  just build macos  (only on a Mac)
#
# A leading dash works too (`just build -linux`). Every target first runs the bootstrap
# (work/ipa) and the recompiler (work/gen), which redo only what changed. Everything lands in
# build/ and is for your own machines only. The work is in scripts/build (macOS, Linux) and
# scripts/build.ps1 (Windows, which also sets up the Visual Studio x64 environment itself).

set windows-shell := ["powershell.exe", "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command"]

# List the recipes.
default:
    @just --list

# Build the Release game: for this OS, or for TARGET (windows, macos, linux).
[windows]
build target="windows":
    @& '{{ justfile_directory() }}\scripts\build.ps1' '{{ target }}'; exit $LASTEXITCODE

# Build the Release game: for this OS, or for TARGET (windows, macos, linux).
[unix]
build target=os():
    @'{{ justfile_directory() }}/scripts/build' '{{ target }}'
