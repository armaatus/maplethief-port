# One command per job (docs/build/README.md, ADR 0133). Every recipe is a one-liner over a script,
# so nobody needs `just` installed: scripts/build (macOS, Linux) or scripts\build.ps1 (Windows)
# does the work, and takes the same words.
#
#   just build [mac|win|linux|ios|switch] [debug|release]   the game, into dist/<platform>/
#   just build mac --ipa PATH                    from your own copy of the game (ADR 0135)
#   just run [debug] [-- GAME ARGS]              start this OS's build from dist/
#   just doctor                                  check the tools and the IPA, each with its fix
#   just bootstrap | assets | recomp | shaders   one step, skipped when it is up to date
#   just test | ci | package | clean [work]
#
# The arguments reach the scripts through a shell, so a path that holds a space needs quotes
# inside the quotes (--ipa "'/my games/x.ipa'"), or use the script directly. A relative path is
# read from this folder. What `build` makes holds the game: keep it to your own machines.

set windows-shell := ["powershell.exe", "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command"]

build_script := if os_family() == "windows" { "& .\\scripts\\build.ps1" } else { "scripts/build" }
test_script := if os_family() == "windows" { "& .\\scripts\\test.ps1" } else { "scripts/test" }
ci_script := if os_family() == "windows" { "& .\\scripts\\build.ps1 ci" } else { "scripts/local-ci" }
pass_status := if os_family() == "windows" { "; exit $LASTEXITCODE" } else { "" }

# List the recipes.
default:
    @just --list

# Build the game into dist/<platform>/: [mac|win|linux|ios|switch] [debug|release] [--ipa PATH].
build *args:
    @{{ build_script }} build {{ args }}{{ pass_status }}

# Start this OS's build from dist/: [debug] [-- GAME ARGS].
run *args:
    @{{ build_script }} run {{ args }}{{ pass_status }}

# Check the IPA and extract it into work/.
bootstrap:
    @{{ build_script }} bootstrap{{ pass_status }}

# Convert the assets into assets-out/ (optional; the game doesn't need it).
assets:
    @{{ build_script }} assets{{ pass_status }}

# Recompile the game into work/gen/.
recomp:
    @{{ build_script }} recomp{{ pass_status }}

# Recompile the GPU shaders when a source changed (Docker).
shaders:
    @{{ build_script }} shaders{{ pass_status }}

# Run every unit test: scripts/test (Windows: scripts\test.ps1).
test *args:
    @{{ test_script }} {{ args }}{{ pass_status }}

# Run what CI runs: scripts/local-ci (a Mac or Linux machine only).
ci *args:
    @{{ ci_script }} {{ args }}{{ pass_status }}

# Package the own-code kit, from a clean checkout: [mac|linux].
package *args:
    @{{ build_script }} package {{ args }}{{ pass_status }}

# Remove dist/ and the builds; the word work adds work/ and assets-out/. Saves stay.
clean *args:
    @{{ build_script }} clean {{ args }}{{ pass_status }}

# Check Python, CMake, Ninja, compilers, the IPA and disk space, each with its fix.
doctor:
    @{{ build_script }} doctor{{ pass_status }}
