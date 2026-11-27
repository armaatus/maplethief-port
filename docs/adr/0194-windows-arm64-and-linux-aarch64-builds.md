# 194. Windows ARM64 and Linux aarch64: presets, hosted ARM64 runners, and two more kits and player builds

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#362](https://github.com/armaatus/maplethief-port/issues/362) (epic [#292](https://github.com/armaatus/maplethief-port/issues/292))
- **Builds on:** [ADR 0004](0004-build-presets-and-generators.md) (presets per OS), [ADR 0065](0065-linux-and-steam-deck-build.md) (the Linux build container), [ADR 0193](0193-linux-glibc-2-35-appimage-and-flatpak-bundle.md) (that container on Ubuntu 22.04: glibc 2.35 for both CPUs; the AppImage and Flatpak x86_64 only), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (player builds and the release flow), [ADR 0126](0126-dev-branch-release-pull-requests-and-ci-by-target.md) (CI by target), [ADR 0146](0146-distribution-on-itch-io.md) (itch.io channels)

## Context

The desktop downloads were x86_64 only on Windows and Linux, while ARM64 laptops (Snapdragon Windows machines, ARM Linux boards and laptops) are common. The generated C already runs on arm64 (macOS, iOS, the Switch, Android); nothing in our code assumes x86 (no intrinsics; the crash handler reads the PC on Linux aarch64 already).

Facts:

- GitHub's hosted ARM64 runners, `windows-11-arm` and `ubuntu-24.04-arm`, are free for public repositories.
- The player builds need `work/gen` and so are built on the Mac and the Windows PC (ADR 0125). The Mac runs the arm64 Linux containers natively; the PC is x64. MSVC ships x64-hosted ARM64 cross tools (`vcvarsall x64_arm64`).
- In a Ninja + MSVC cross build without a toolchain file, `CMAKE_SYSTEM_PROCESSOR` is the host's (AMD64), so the kit would be named `windows-x64`. MSVC's `CMAKE_CXX_COMPILER_ARCHITECTURE_ID` is the target's (`x64`, `ARM64`).
- An x64 PC can't run an ARM64 executable, so the cross-built player's `--version` can't be checked there.

## Options considered

1. **Presets, hosted ARM64 runners in the full CI profile and in release.yml, the player builds cross-compiled on the PC and built in the arm64 container on the Mac** (chosen).
2. **Only player builds, no kits or CI jobs.** Less CI, but a release would carry ARM64 code that nothing built or tested since the last local run.
3. **ARM64 in the pull-request profile too.** Every pull request into dev would wait on two more runners; ARM64-only breaks are rare (the Mac's arm64 build and the Switch job compile the same code), so the weekly run and the release pull request are enough.
4. **Windows ARM64 player built on a hosted runner.** It needs `work/gen`, which CI never has (ADR 0100). Rejected.
5. **Run x64 builds under emulation** (Windows 11's Prism, FEX/box64 on Linux). Works on Windows, slower, and absent on most ARM Linux systems. The x64 downloads stay, but they are not the answer for ARM64.

## Decision

- **Presets.** `win-arm64-{debug,release,player}` inherit `win-base` with `architecture: arm64`; they are configured from an ARM64 Visual Studio developer environment: `vcvarsall arm64` on an ARM64 machine, `vcvarsall x64_arm64` on the x64 PC. `linux-arm64-{debug,release,player}` inherit the Linux ones. Each sets `MAPLETHIEF_TARGET_ARCH=arm64`. `CMakeLists.txt` derives `MAPLETHIEF_ARCH` (`x86_64` or `arm64`) from MSVC's architecture id, or elsewhere from `CMAKE_SYSTEM_PROCESSOR`, and fails at configure when a preset's target architecture isn't the compiler's. `cmake/Packaging.cmake` names Windows packages from it, so the cross build is `windows-arm64`. Linux keeps `CMAKE_SYSTEM_PROCESSOR`: `linux-aarch64`, matching `x86_64` and `uname -m`.
- **Defaults follow the machine.** `scripts/test` picks the `linux-arm64-*` presets on aarch64, `scripts/test.ps1` the `win-arm64-*` ones in an ARM64 developer environment (`VSCMD_ARG_TGT_ARCH`), `scripts/linux-game` the `linux-arm64-*` ones in the arm64 container.
- **CI.** The full profile (the release pull request into main, manual and weekly runs) adds `ubuntu-24.04-arm` and `windows-11-arm` to the build matrix, Debug and Release; pull requests into dev stay Linux x64. Their job names are required on main (`.github/rulesets/main.json`) and expected by the weekly report. The download cache is keyed by OS and architecture. A self-hosted manual run maps them to `[self-hosted, Linux|Windows, ARM64]`.
- **Kits.** `release.yml` builds `MapleThief-kit-<version>-windows-arm64.zip` on `windows-11-arm` (win-arm64-release) and `…-linux-aarch64.tar.gz` on `ubuntu-24.04-arm` (scripts/test-linux --arch arm64), each checked by `--version` like the others. `mtport.release.KIT_PLATFORMS` lists them after the x64 ones.
- **Player builds.** The Mac builds `MapleThief-<version>-linux-aarch64.tar.gz` in the arm64 linux-game container (glibc 2.35 floor, as x86_64; no AppImage or Flatpak, ADR 0193) (`scripts/release build` platform `linux-arm64`, on by default) and checks `--version` in the arm64 CI container. The PC builds `MapleThief-<version>-windows-arm64.zip` with `scripts\release.ps1 build TAG -PlayersOnly -Arch arm64` from the `x64_arm64` environment: the `win-arm64-player` workflow, whose CTest preset runs only `platform.player_contents` (it reads the executable), and a check of the executable's PE machine type (ARM64) in place of `--version`. `--version` and a first run are checked on an ARM64 device when one is at hand (needs-human).
- **Release and itch.io platforms.** `scripts/release` platforms are `mac`, `linux`, `linux-arm64`, `windows`, `windows-arm64` and `switch`; `publish` requires all six by default, each found by a name key that matches only its own files. The itch.io channels `linux-aarch64` and `windows-arm64` join the others (the Mac pushes `linux-aarch64`, `scripts\release.ps1 itch TAG -Arch arm64` pushes `windows-arm64`).
- **The Steam Deck and the one-command builds are unchanged**: the Deck is x86_64, and `scripts/build.ps1` still enters the x64 environment (Windows on ARM runs it under emulation).

## Consequences

- A release has five kits and seven player builds. The release checklist adds the PC's ARM64 step and the device check.
- Two more runners on the weekly run and the release pull request, at no cost for a public repository.
- An ARM64-only compile break (MSVC ARM64 especially) shows up on the weekly run or the release pull request, not on a feature pull request; a manual `gh workflow run ci.yml --ref <branch>` runs it on any branch.
- Until someone runs the Windows ARM64 build on a device, its `--version` and play are unverified; the hosted runner's tests cover our own code and the shell, not the game.
