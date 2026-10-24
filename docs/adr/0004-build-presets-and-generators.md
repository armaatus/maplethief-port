# 4. Build presets, generators and compiler flags

- **Status:** Accepted
- **Date:** 2026-10-24
- **Issue:** #2

## Context

[ADR 0002](0002-language-and-toolchain.md) fixed CMake ≥ 3.28 with Ninja preferred, the minimum compilers, and "warnings as errors in CI for our own code", and left the presets and compiler flags to #2. [ADR 0003](0003-dependencies.md) fixed how SDL3 and the other C/C++ dependencies are fetched, with a download cache that every preset shares.

Requirements from #2 and the M0 exit criterion ([ROADMAP](../ROADMAP.md#m0--foundation)):

- Presets `mac-debug`, `mac-release-universal`, `win-debug`, `win-release`, `linux-debug`, `linux-release`.
- A fresh clone + `git lfs pull` + `scripts/bootstrap` + **one CMake command** opens an empty SDL3 window on macOS, Windows and Linux.
- Nothing is written outside `build/` during configure and build.
- CI (#3) runs on GitHub-hosted `macos-latest`, `windows-latest` and `ubuntu-latest`, without a display and without the IPA, and must show that the program starts and exits cleanly.

## Options considered

### Generator on Windows

1. **Ninja + MSVC `cl` from a Visual Studio developer environment.** The same generator and single-config layout (`build/<preset>/platform/MapleThief.exe`) as macOS and Linux. It doesn't name a Visual Studio version, so it works with VS 2022 and later alike. It needs the compiler environment set up first: a "Developer PowerShell / x64 Native Tools" prompt, Visual Studio's or VS Code's CMake integration (which read the preset's `architecture`/`toolset` with `"strategy": "external"`), or one action step in CI.
2. **Visual Studio generator.** Needs no developer environment, but the generator name pins a Visual Studio version (`Visual Studio 17 2022`), so a runner image or machine with only a newer Visual Studio fails to configure. It is multi-config, so build and test presets must also pick a configuration, and outputs land in `platform/Debug/`. Rejected for the presets; anyone can still configure it by hand.
3. **clang-cl now.** ADR 0002 makes clang-cl the shipping compiler for generated code on Windows. There is no generated code yet, and MSVC `cl` must compile everything anyway. A clang-cl preset is added when `work/gen/` is first built (M3).

### Warnings as errors

1. **`CMAKE_COMPILE_WARNING_AS_ERROR`**: applies to every target created after it is set, including SDL's, which ADR 0002 exempts. Rejected.
2. **A project option, applied to our own targets only.** `MAPLETHIEF_WARNINGS_AS_ERRORS` sets `COMPILE_WARNING_AS_ERROR` on our targets. It defaults to `ON` when the `CI` environment variable is set (GitHub Actions sets it), so the CI commands are the same as the local ones, and to `OFF` otherwise, so a newer local compiler with new warnings doesn't block a build.
3. **Separate CI presets** that only add the option: twice the presets for one flag. Rejected.

### Checking that the program runs, headless

1. **A smoke mode in the program**: `MapleThief --smoke-frames N` renders N frames, reads the last one back, checks the clear colour, posts `SDL_EVENT_QUIT` and exits through the normal quit path. With `SDL_VIDEO_DRIVER=dummy` it needs no display. A CTest test runs it, so every test preset and CI exercise it.
2. **An external harness** that starts the program and kills it: it can't tell a clean exit from a crash that happens to return, and it differs per OS. Rejected.

## Decision

- **`CMakePresets.json`** (schema 6) has, for each of the six names, a configure, build, test and **workflow** preset. Hidden base presets set the **Ninja** generator, `binaryDir` `${sourceDir}/build/${presetName}`, `MAPLETHIEF_DOWNLOAD_DIR=${sourceDir}/build/_downloads` (one download cache for every preset, ADR 0003) and `CMAKE_EXPORT_COMPILE_COMMANDS`. Each OS's presets have a `hostSystemName` condition, so only that OS's presets are listed.
  - `mac-*`: `CMAKE_OSX_DEPLOYMENT_TARGET=13.0`. `mac-debug` builds for the host architecture; `mac-release-universal` sets `CMAKE_OSX_ARCHITECTURES=arm64;x86_64`.
  - `win-*`: Ninja with `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` `cl`, x64, run from a Visual Studio x64 developer environment. Configure fails with a hint on any 32-bit target, because the default "Developer PowerShell" shortcut targets x86.
  - `linux-*`: Ninja with the default compiler (GCC 13 on Ubuntu 24.04; Clang works too).
- **One command** per OS: `cmake --workflow --preset <os>-debug` configures, builds and runs CTest. The separate steps (`cmake --preset`, `cmake --build --preset`, `ctest --preset`) work as usual.
- **Our own targets** (`runtime`, `MapleThief`) get C++20 and C17 without extensions, `-Wall -Wextra` (clang/GCC) or `/W4 /utf-8 /permissive-` (MSVC), and `COMPILE_WARNING_AS_ERROR` from `MAPLETHIEF_WARNINGS_AS_ERRORS` (default `ON` if `CI` is set in the environment, else `OFF`), through one CMake function, `maplethief_target_defaults()`. Third-party targets keep their own settings.
- **Nothing outside `build/`**: the top-level `CMakeLists.txt` refuses in-source builds and turns off the CMake user package registry (`CMAKE_EXPORT_NO_PACKAGE_REGISTRY`, `CMAKE_FIND_USE_PACKAGE_REGISTRY`), so `~/.cmake` (or the Windows registry) is neither written nor read. The Python targets write bytecode into the build tree (`-X pycache_prefix`).
- **Targets**: `runtime` (static library), `MapleThief` (executable; a `.app` bundle on macOS from `platform/macos/Info.plist.in`, a GUI-subsystem program on Windows), and `tools` / `recomp` (custom targets that byte-compile the Python packages with the found Python ≥ 3.10, so a syntax error fails the build). All four are part of `ALL`.
- **Smoke test**: `MapleThief --smoke-frames N` as described above, registered as the CTest test `platform.smoke` with `SDL_VIDEO_DRIVER=dummy`. Exit status 0 means it rendered the clear colour and quit cleanly; 3 means the read-back pixel was wrong; 2 is a usage error; 1 is an SDL initialisation failure.

## Consequences

- #3 runs `cmake --workflow --preset <os>-debug`, with a Visual Studio developer environment step first on Windows. Warnings are errors there because `CI` is set. The smoke test needs no display on any runner.
- #6 adds its tests with `add_test()` / `doctest_discover_tests()` anywhere under the top-level project; the existing test presets pick them up. Test presets fail when no tests are found (`noTestsAction: error`), so a broken test registration shows up.
- Windows developers must start from a developer prompt or an IDE that sets one up; the README says how. If that turns out to be a real obstacle, a Visual Studio generator preset can be added next to the Ninja ones without changing anything else.
- A clang-cl preset for Windows and a Clang preset for Linux are added when generated code lands (M3), because ADR 0002 makes clang the shipping compiler for it.
- iOS presets come with the iOS build (M9); they need a full Xcode, and `platform/` already keeps Apple-only files under `platform/macos/` and `platform/ios/`.
