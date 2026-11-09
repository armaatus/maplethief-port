# 134. A Debug build optimises the code a frame spends its time in

- **Status:** Accepted
- **Date:** 2026-11-09
- **Issue:** #136 (epic #269)
- **Builds on:** [ADR 0021](0021-generated-code-partition-and-build.md) (the generated code's build), [ADR 0025](0025-guest-memory-and-address-space.md) (`MT_MEM_CHECKED`), [ADR 0042](0042-frontend-game-loop-and-presentation.md) (the game loop), [ADR 0071](0071-frame-telemetry-and-frame-time-overlay.md) (the exit log)

## Context

The game drifts instead of skipping frames when the host is slow (`docs/re/timing-threading.md`, "Overrun"), so a slow build is a slow game. The research behind #136 measured a `mac-debug` run at 44 GL frames in 7.3 s, about 6 a second against the game's 20, with compositing at 37 to 98 ms per window frame.

That figure no longer describes a Debug build at the default settings. The run it came from is `platform.game_lifecycle`'s real-time run: 7.3 s, of which 2 s minimised and the rest the start-up. Re-measured on 2026-11-09 before this change (`mac-debug`, real generated code, Apple M4, a fresh run through the intro to the first map with the device goldens' taps, render scale 1, the exit log of `frame_stats.cpp` and `--perf-csv`):

| Build | Drivers | Render scale | Whole run (GL frames/s) | First map (GL frames/s) | mt_gfx per frame |
| --- | --- | --- | --- | --- | --- |
| Debug, all `-O0` | Cocoa + Metal | 1 | 20.34 | 20.00 | 20.72 ms |
| Debug, all `-O0` | dummy | 1 | 20.23 | 19.89 | 19.60 ms |
| Debug, all `-O0` | dummy | 2 | 11.08 | 10.18 | 73.45 ms |

The compositor has since become cheap (1.7 ms per window frame), so the Debug build keeps the tick at render scale 1, with little to spare: mt_gfx alone is 20 of the 50 ms. Anything heavier does not fit: at render scale 2 mt_gfx is 73 ms and the game runs at half speed. A Debug build is still the wrong build to judge the game by unless the code a frame runs in is optimised.

## Options considered

1. **Optimise only what a frame runs in** (the generated code, mt_gfx, `runtime/src/cg`, the compositor) in the Debug configuration, everything else `-O0`. The game plays at its real speed and the code one steps through (the frontend, the HLE functions, the runtime's other parts) stays `-O0`.
2. **All of Debug at `-O1` or `-O2`.** Simple, but stepping through the runtime and frontend, the reason Debug exists, gets worse everywhere.
3. **Leave Debug alone and only document Release.** Keeps the build fast, but the regressions and tests that run in Debug keep running on a build that behaves differently from the game a player gets.

## Decision

Option 1.

**Which code.** `maplethief_debug_opt(<target> [<source>...])` (top-level `CMakeLists.txt`) adds `-O2` (`/O2` on MSVC) in the Debug configuration only: the whole of `maplethief_gen` and `maplethief_gfx_gpu`, and in `runtime` `src/gfx/gfx.cpp`, `src/gfx/raster.cpp`, `src/gfx/texture.cpp`, `src/cg/*.cpp` and `src/uikit/compositor.cpp`. Everything else stays `-O0`. `-g`, asserts (no `NDEBUG`) and `MT_MEM_CHECKED` stay on these targets: the checked memory model is unchanged and the debugger still has line tables, though stepping in those files is what it is at `-O2`. Floating-point contraction stays off where ADR 0039 requires it: `maplethief_strict_fp` now appends to the source's options instead of replacing them, so the two compose in either order.

**MSVC.** CMake's default Debug flags on MSVC are `/Od /Ob0 /RTC1`. `/RTC1` and `/O2` refuse each other (`D8016`), and a later `/O2` overriding the global `/Od` warns (`D9025`). So after SDL is added the top-level `CMakeLists.txt` takes `/Od`, `/Ob0` and `/RTC1` out of `CMAKE_<LANG>_FLAGS_DEBUG` for our own code (SDL keeps its own flags). `/Od` and `/Ob0` are cl's defaults, so code without an `/O` option is still unoptimised. `/RTC1` comes back in Debug on every target that has no optimised code (`maplethief_target_defaults`), and per source on the others (`runtime`: every source not listed above). `maplethief_gen` and `maplethief_gfx_gpu` have no `/RTC1`: they compile at `/O2` as a whole. clang-cl takes the same path (it ignores `/RTC1`). MSVC keeps `/Zi` (CMake's debug information format, separate from the flags), `/MDd` and `_DEBUG`.

**Saying so.** The start-up log has a `build:` line with the build type and optimisation level (`platform/src/build_info.{h,cpp}`, from the configure step). In a Debug build a real-time run warns once on stderr when it falls behind: a window of 40 GL frame intervals of which at least 30 are over the tick's overrun limit (`FrameStats::overrun_ns`, 1.1 ticks), so one hitch does not count, with an interval over 1 s (a load, a hidden window) starting the window over. The `run` CMake target prints, in a Debug build, that it is one and that a player builds with `just build`.

## Consequences

- With the change, the same runs (one after the other on a machine with other builds running, load 3 to 8) reach:

| Build | Drivers | Render scale | Whole run (GL frames/s) | First map (GL frames/s) | mt_gfx per frame |
| --- | --- | --- | --- | --- | --- |
| Debug, this ADR | Cocoa + Metal | 1 | 20.49 | 19.98 | 2.87 ms |
| Debug, this ADR (two runs) | dummy | 1 | 20.32, 20.32 | 19.90, 19.92 | 2.94 ms, 2.83 ms |
| Debug, this ADR | dummy | 2 | 20.31 | 19.86 | 10.89 ms |

  The first map holds the tick with about 40 ms of the 50 to spare at render scale 1, where `-O0` had about 25; at render scale 2 the Debug build now holds it too. The warning fires once at render scale 8 (167 ms a tick) and does not fire in these runs.

- A clean build of `maplethief_gen` in `mac-debug` goes from 88 CPU-seconds (11 s on 10 cores) at `-O0` to 632 CPU-seconds (2 min 43 s under load 6 to 8) at `-O2`. It is rebuilt only when the generated code changes (ADR 0021). The Windows build pays the same for generated code at `/O2`, which only local builds have (CI never has `work/gen`).
- Warnings that only an optimising compiler gives (GCC's `-Wmaybe-uninitialized`, MSVC's `C4701`) now apply to these sources in Debug; the same sources already pass with `-Werror` in the Release builds of CI.
- Other code that later shows up in the frame time (for example `runtime/src/gles/fixed.cpp`, the vertex pipeline in front of mt_gfx) can join the list; the exit log's split shows where a frame goes.
- If a Debug build still warns of falling behind at render scale 1, the list is wrong, not the warning.
