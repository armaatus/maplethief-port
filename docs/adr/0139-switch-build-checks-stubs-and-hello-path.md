# 139. The Switch build: host-side NRO checks, test NROs, Horizon stubs and the hello path

- **Status:** Accepted
- **Date:** 2026-11-10
- **Issue:** [#235](https://github.com/armaatus/maplethief-port/issues/235) (epic [#233](https://github.com/armaatus/maplethief-port/issues/233))
- **Builds on:** [ADR 0138](0138-nintendo-switch-homebrew-target.md) (the Switch target), [ADR 0004](0004-build-presets-and-generators.md) (presets), [ADR 0005](0005-test-harness.md) (tests), [ADR 0023](0023-local-ci-and-packaging.md) (containers, kits), [ADR 0024](0024-release-contents-and-flow.md) and [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (the release guard and its icon allowlist), [ADR 0126](0126-dev-branch-release-pull-requests-and-ci-by-target.md) (CI by target), [ADR 0006](0006-continuous-integration.md) (`scripts/ci-scope`)

## Context

ADR 0138 chose the target: `MapleThief.nro`, devkitA64 in a pinned image, SDL3 from `nx-mod/sdl3-nx`. #235 makes our own code build into that NRO, in CI and locally, before the runtime runs on Horizon (#236) and before the generated code goes in (#238). That raised questions ADR 0138 leaves open: what CTest can check when nothing runs on the build machine, what the NRO does without the game, how code that needs a POSIX feature Horizon lacks compiles, where the NRO's icon comes from, and when CI runs the job.

## Decision

**Checks on the host, tests as NROs.** CTest on the build machine cannot run AArch64 Horizon code. The `switch-*` workflows therefore register only two host-side tests, run in the container after the build: `platform.switch_nro` (`python3 -m mtport.nro check`: the NACP's name `MapleThief`, author `armaatus` and the project version, and a 256 x 256 baseline JPEG icon) and `platform.switch_guard` (`python3 -m mtport.release guard` on the NRO). Every doctest program still compiles and links for the Switch, so a test that does not build fails the job, but `maplethief_doctest_discover` turns each into `build/<preset>/tests/<program>.nro` (no icon, its own NACP) and registers no CTest test for it. They run on a console with `scripts/switch-run`. The test presets filter on `^platform\.switch_`.

**The NRO format is read by our own parser.** `tools/mtport/nro.py` reads the NRO header, the `ASET` section and the NACP fields we use ([docs/formats/nro.md](../formats/nro.md)). The release guard splits an NRO into `code.bin`, `icon.jpg`, `control.nacp` and `data.romfs`, so each of its rules applies to each part: the generated-symbol scan to the code, the icon allowlist to the icon, and the archive rule to any RomFS (the player build carries no asset, [ADR 0100](0100-public-repository-and-player-releases.md)). The guard reads a lone `.nro` as a kit with one file. The Switch kit is a ZIP holding `MapleThief.nro` and the docs (`MapleThief-kit-<version>-switch.zip`).

**The icon is generated, deterministically, and allowlisted by hash.** `python3 -m mtport.appicon switch-icon` draws the iOS app icon (our own drawing) at 256 x 256 and encodes it with a small baseline JPEG encoder of our own (`tools/mtport/jpeg.py`: integer DCT, the standard tables, no randomness, no dependency). The bytes are the same on every machine and Python, so the release guard's icon allowlist names `MapleThief.nro/icon.jpg` by its SHA-256 and the build does not commit a binary. Pillow or `sips` would have made the bytes depend on the host.

**The hello path.** Without the generated code (`MT_HAVE_GENERATED_CODE` unset) and without arguments, `main` on the Switch calls `mt::switch_nx::run_hello()` (`platform/switch/hello.cpp`): it connects stderr to nxlink if the NRO was sent over the network, prints `MapleThief <version>` there, shows the same line on libnx's console, and returns to the Homebrew Menu on +. It proves the toolchain, the NACP and a hardware run before the runtime works on Horizon; #236 and #238 replace it.

**Horizon stubs that say so.** devkitA64's newlib has no `mmap`, POSIX signals, `dladdr`, `fork`/`posix_spawn` or `pthread_getschedparam`. Code that needs one keeps its other platforms' path and gains a `__SWITCH__` branch that compiles to a stub. The stub prints `MapleThief: not on the Nintendo Switch yet (#236): <what> (<file>:<line>)` to stderr through `MT_HORIZON_MISSING` (or `MT_HORIZON_FATAL`, which also aborts) from `runtime/include/maplethief/horizon.h`, then fails the way that function fails on any error (a null pointer, `false`, -1). A stub never pretends to work. Tests that need a child process or file permissions are left out on the Switch. Each stub is listed on #236, which replaces it with libnx's equivalent.

**Compiler settings.** The CPU flag is `-mcpu=cortex-a57+crypto` (`MAPLETHIEF_SWITCH_CPU_FLAGS`): `Switch.cmake` already passes `-march=armv8-a+crc+crypto`, and a bare `-mcpu=cortex-a57` conflicts with it, which `-Werror` turns into a failure. newlib hides POSIX declarations (`fileno`, `strdup`, …) under a strict `-std=`, so the Switch build defines `_DEFAULT_SOURCE`. Our code is kept free of GCC 15 warnings rather than silencing them, and the Mac clang build stays clean. `MAPLETHIEF_MEM_MODEL=reserved` is a configure error on the Switch ([ADR 0025](0025-guest-memory-and-address-space.md)'s reservation of 4 GiB does not exist there); the presets set `checked`.

**When CI builds it.** The `switch` job (`ubuntu-latest`, linux/amd64 image) runs `scripts/build-switch HEAD --preset switch-release` on every pull request into `dev` and every manual run, and `scripts/local-ci` has the same job (arm64 on Apple silicon). `scripts/ci-scope` prints two lines, `native=` and `switch=`: a change that only touches another platform's own files (`platform/{macos,ios,windows,linux,icon}/`, Objective-C++ in `platform/src/`, the Apple CMake modules, the Linux containers, the PowerShell and Linux-only scripts) skips the Switch job, and documentation alone skips both.

## Consequences

- The Switch NRO is built and checked on every pull request, without the IPA or a console. A hardware run stays a `needs-human` step.
- `platform.switch_nro` and `platform.switch_guard` need Python 3 in the image; the Dockerfile checks it is there.
- A change to the iOS icon drawing changes the Switch icon's hash: `tests/test_appicon.py` fails until the allowlist is updated, as for the other icons.
- The stubs make the gaps visible at run time and in one list (#236). Until #236 lands, the NRO without the hello path would fail at its first memory reservation.
- The hello path and the stubs are temporary; ADR 0138's decisions are unchanged.

## Addendum: the stubs replaced, and the test NROs (#236, 2026-11-11)

#236 replaced every stub listed on it: guest memory (ADR 0025's Horizon addendum), the crash handler and the crash file (ADR 0030's), the save replacement and its directory sync (ADR 0074's), the main thread's priority (`svcGetThreadPriority`), the crash dialog (the error applet, in-process), the system version in the player's log (`setsysGetFirmwareVersion`, plus applet mode or title takeover), the tests' fault probe (`svcQueryMemory`) and the runtime's default data folder (`MT_STUBS_SWITCH_DATA_DIR`). `diag_test.cpp`'s crash-child cases are left out on the Switch, and `MapleThiefTests.nro --crash-test` stands in for them. Nothing uses `MT_HORIZON_MISSING` now; `horizon.h` stays for a later gap, whose message no longer names #236. The Switch still links no `-ldl`.

Every test NRO's `main` (`tests/cpp/doctest_main.cpp`, `switch_test_host.cpp`) now connects stdout and stderr to nxlink when a computer sent it, or uses the console, and writes the same text to `sdmc:/switch/maplethief-tests/<program>-results.txt`, with TMPDIR on the SD card. `MapleThiefTests.nro` is the runtime's suite with the saves' and the Horizon cases (`results.txt`), and `scripts/switch-test` runs it through `scripts/switch-run`, keeps a log under `build/test-output/switch/` and says PASS or FAIL. The `switch` job builds it like every other test NRO.

## Addendum: the generated code in the NRO (#238, 2026-11-11)

`scripts/build-switch --gen work/gen` (and `just build switch` when the IPA is in `original/`) builds `maplethief_gen` into `MapleThief.nro`, which then runs the game instead of the hello path. Measured on this Mac (Apple silicon, 10 cores, the arm64 image, `switch-release`, a clean build, `CI=true` so warnings are errors):

- **GCC 15 and the generated C.** The 63 units (62 `fn_*.c` and `dispatch.c`) compile for the Cortex-A57 with `-Wall -Wextra -Wpedantic` without a single warning, so neither the generator nor the build needed a change for them. One warning of our own test code (`-Wformat-truncation` in `tests/cpp/game_driver.h`, a buffer too small for any `size_t`) was fixed in the code.
- **Compile time** (from ninja's log, which `scripts/build-switch` now keeps as `build/<preset>/logs/ninja_log.txt`; two clean builds): 650 to 730 s of compiler time for `maplethief_gen`, 61 to 70 s of wall time alongside the rest of the build, the slowest unit 37 to 42 s; the whole workflow (SDL, the runtime, the frontend, every test NRO, CTest, the kit) about 3.1 to 3.6 minutes.
- **Size.** `MapleThief.nro` is 23.5 MB (23,496,692 bytes) with the game's code, against 8.3 MB for the hello NRO of the same commit (CI's). `platform.switch_nro` and `platform.switch_guard` pass on both.
- **The link.** GNU ld reads each archive once: a program whose own objects never call `mt_dispatch` (the real-asset test programs) left `dispatch.o` out of `libmaplethief_gen.a` and then found the runtime's calls to it unresolved. `maplethief_gen` now carries `LINKER:--undefined=mt_dispatch` in its interface on every non-Apple, non-MSVC linker (`cmake/GeneratedCode.cmake`), which pulls it in when the archive is read. ld64 and link.exe search archives again and are unchanged.

The NRO with the game is derived from the player's IPA and stays on their own console (ADR 0100); CI still builds only the hello path.

## Addendum: the guard's mode follows the generated code (#283, 2026-11-13)

#243 gave `platform.switch_guard` the guard's player mode only in the player build (`MAPLETHIEF_PLAYER_BUILD`). That left the game build above, `switch-release` or `switch-debug` with `--gen` (and `just build switch` with the IPA in `original/`), in kit mode, which refuses recompiled code: the NRO was written and the run then failed, and `scripts/switch-perf --build`, which passed no `--gen`, measured the hello NRO. The guard now runs player mode whenever the build links `maplethief_gen`, with its `dispatch.c`, and kit mode otherwise, so the hello NRO and CI's kit are checked as before. `scripts/switch-perf --build` builds with `--gen work/gen` (refused when it has no `gen.cmake`) and takes `--preset switch-player` as well.
