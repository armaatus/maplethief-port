# 145. Switch performance: a logged session judged on the host, and the GPU at render scale 1 by default

- **Status:** Accepted
- **Date:** 2026-11-11
- **Issue:** [#244](https://github.com/armaatus/maplethief-port/issues/244) (epic [#233](https://github.com/armaatus/maplethief-port/issues/233))
- **Builds on:** [ADR 0071](0071-frame-telemetry-and-frame-time-overlay.md) (the telemetry), [ADR 0075](0075-display-locked-frame-pacing.md) (display-locked pacing), [ADR 0080](0080-render-scale-auto-and-frame-budget-guard.md) (render scale auto and the guard), [ADR 0087](0087-game-speed-and-fast-forward.md) (fast-forward), [ADR 0091](0091-frame-interpolation.md) (frame interpolation), [ADR 0070](0070-settings-registry-and-presets.md)'s platform rows, [ADR 0141](0141-switch-lifecycle-applet-mode-and-audio.md) (the CPU boost of loads only), [ADR 0143](0143-gpu-backend-on-deko3d.md) (the GPU on deko3d)

## Context

#244 asks for a steady pace on a stock-clocked Switch at default settings, handheld and docked: 20.00 ticks a second and at least 99 % of frames held 3 vblanks over five minutes on a map, with fast-forward and the busiest scene logged. Two things were missing.

**A run that answers the question in one go.** ADR 0071's exit lines cover a whole run, menus and loads included, and do not say which screen a frame was on. `--perf-csv` has every frame, but on the console it lands on the SD card, has no screen or scene column, and needs a spreadsheet to read. The console is Joris's (`needs-human`), so the run has to be one command and one log, with the verdict computed for him.

**Defaults chosen before the console is there.** On the Switch the CPU runs at 1020 MHz in games (ADR 0141 boosts it only for loads), and no console is on the build machine. The defaults must come from what can be measured here plus a stated factor, and the console run decides.

## Options considered

**The session's log.**

1. **`--perf-csv` to the SD card.** Exists, but per frame, without the screen or the scene, and it has to be copied off the card and analysed by hand.
2. **A summary only at exit.** One line, but a crash or a cut connection loses everything, and per-screen numbers need the state machine in C++.
3. **A line per second of counts and sums, judged on the host (chosen).** Counts and sums add up exactly over any set of seconds, so the host can sum by screen and scene. The lines go over nxlink and into the player log (ADR 0030's addendum), so a cut run still has every second up to the cut.

**The defaults.**

1. **Software at render scale 1, as on the desktop.** Rejected by the measurement below: the rasterizer alone needs several ticks' worth of a 1020 MHz Cortex-A57.
2. **GPU with `render_scale = auto`.** ADR 0080's governor budgets the CPU's time per frame (the frontend and mt_gfx). On deko3d the GPU's own time is not in it, so auto would climb to the cap (3 handheld, 4 docked) without seeing what that costs the GPU.
3. **GPU at render scale 1 (chosen).** The CPU keeps the guest, the compositor and the present. Auto and higher scales stay a choice; ADR 0080's warning for a fixed N > 1 is unchanged.

## Decision

**`--perf-session`** (`platform/src/perf_session.{h,cpp}`, `PerfSession`, pure, no SDL). `game.cpp` gives it each window frame's `FrameRecord` with its context: the screen (`handheld` or `docked` from `appletGetOperationMode` through `mt::switch_nx::display_mode`, `desktop` elsewhere), the scene (`perf_scene` from autosave.h's game mode, area and map: `map:A.M`, `load`, `scene`, `menu`, `none`), fast-forward's rate, the render scale and the backend in use. Once a second it logs one `perf:` line:

```text
perf: t=… s=… display=handheld scene=map:2.0 ff=1 scale=1 backend=gpu frames=20 ticks=20 held=20 held3=20 over=0 tick_max=50.00 guest=… gfx=… take=… compose=… draw=… present=… wait=… other=…
```

These are the window frames and ticks of that second, the frames whose scan-out time is known and those held exactly 3 vblanks, the ticks over the overrun threshold, the longest tick, and ADR 0071's split summed in milliseconds. A window also ends at a change of context and at a pause (HOME, sleep), so no line mixes handheld with docked, a map with a load, or play with fast-forward. The run logs `perf: session start: …` with the settings and `perf: session ended: N lines`. It only reads the telemetry and three words of guest memory, so it changes nothing in the game.

**The judge**, `python3 -m mtport.perfsession judge LOG [--displays handheld,docked]`, sums the lines per screen over the map seconds without fast-forward. Each screen passes with at least 300 s, `ticks / seconds` reading 20.00 to two decimals, and `held3 / held` at least 99 %. It also reports fast-forward's ticks a second against its target, and the busiest map second (the most CPU work for its length: the split without SDL_RenderPresent and the wait). Fast-forward must be in the log unless `--no-fast-forward` is given. The last line is `perfsession: verdict: PASS|FAIL`, and the exit status says the same.

**`scripts/switch-perf`** is the one command. It runs `scripts/switch-run` with `--perf-session --fast-forward x2 --gfx-gpu-timing`, keeps the output in `build/test-output/switch/MapleThief-perf-<UTC>.log`, and then judges it. `--judge LOG` judges a kept log or the player log copied from the SD card.

**Switch rows of `kPlatformDefaults`** (ADR 0070's addendum): `gfx_backend = gpu` and `render_scale = 1`, so in the defaults and every preset column (Enhanced's `auto` included). The software rasterizer stays the fallback when no GPU device starts (ADR 0143). `filter = none` and `frame_rate = original` need no row: the registry's columns already have them everywhere. Both rows are pinned by `presets: the Switch's performance defaults hold the pace in every preset (#244)`. The filters cost a shader pass at every present on SDL's GPU renderer. Smooth frame rate presents every refresh, three times the presents, and adds two refreshes of latency (ADR 0091). Overclocking is not done (ADR 0138). The CPU boost stays for loads only (ADR 0141).

**Compiler flags are unchanged** (`-O3` from CMake's Release, no LTO): their speed can only be measured on the console. [#279](https://github.com/armaatus/maplethief-port/issues/279) measures them with this session.

## Evidence

Measured on the Mac only. There is no console here, and the console's numbers are `needs-human` on #244.

- **CI** (`platform_test`): `perf session: …` (the line's exact text, windows of a second, a change of screen, scene or rate, a pause, vblanks and late ticks) and `presets: the Switch's performance defaults hold the pace in every preset (#244)`. `gfx dksh: gpu is the Switch's default, software everywhere else` replaces #242's software-default test. `tests/test_perfsession.py` covers the judge on synthetic sessions (pass; 299 s; 98.98 % against 99.00 %; 19.97 against 20.00 ticks a second; no fast-forward; the busiest second; a cut log; the CLI; `scripts/switch-perf --judge`). The line the C++ test pins is the one the Python test parses.
- **Real assets, local** (Release arm64, Apple M4, a 60 Hz Metal window, `--no-audio`; the machine shared with a sibling epic's builds, load average 2 to 11, so every time here is noisy). A real-time session on map 7.4 (a copy of `play.autosave_map_7_4`'s save, the key pad walking), fast-forward x2 held for 12 s. The judge's lines, 56 s on the map:
  - GPU (Metal): `desktop on a map: PASS … 20.00 ticks a second (1119 ticks); 99.91 % held 3 vblanks (1118 of 1119); … per tick guest 0.77, mt_gfx 0.05, take 0.05, compositor 0.88, draw 0.01, present 0.23, other 0.20 ms; CPU busy 3.9 %`. Fast-forward x2 reached 99.9 % of 40 ticks a second. The busiest second used 5.2 % of the CPU.
  - Software: `… 20.00 ticks a second (1121 ticks); 99.91 % held 3 vblanks; … guest 0.38, mt_gfx 11.29, … compositor 0.55 …; CPU busy 25.8 %`. The busiest second used 35.8 % (mt_gfx 16.2 ms a tick).
  - Another save (`play.mechanics`', walking through a scene) gives the same picture: the CPU's work per tick averaged 1.3 ms on the GPU (p99 2.9) and 14 to 15.5 ms on software (p99 19 to 22.5), over 30 s, four runs.
- **The factor to a 1020 MHz Cortex-A57** (an assumption, sourced): by Geekbench 6 single core, the Apple M4 scores about 3,750 ([Notebookcheck](https://www.notebookcheck.net/Apple-M4-10-cores-Processor-Benchmarks-and-Specs.835975.0.html)) and the Tegra X1 about 317 at its 2.0 GHz rating (CPU-Monkey's average; the page does not name the device). Scaled to the Switch's 1020 MHz that is about 162, so roughly 23 times slower. Geekbench is not this code (memory-bound rasterizing, the guest's checked memory model, GCC instead of clang), so the true factor is uncertain.
  - Software at 1x would cost about 12.9 ms × 23 ≈ 300 ms of CPU a tick on the map, six ticks' worth. This rules it out at any plausible factor: even 4× would be 52 ms.
  - The GPU path's CPU work would cost about 1.95 ms × 23 ≈ 45 ms a tick on average, and about 60 ms in the busiest second. It fits the 50 ms tick at a factor of about 17 or less on average, and about 13 or less at the peak. So the GPU is necessary. Whether it is enough, and with what headroom, only the console can say. Most of the remaining cost is the guest (0.77 ms) and the compositor (0.88 ms), which #279 looks at if the console misses.
- **A run on E-cores** (`taskpolicy -b`, the same scene on software) took 28 ms of work a tick against 14 ms on a P-core. That run fell to 14.6 ticks a second, because background QoS also delays the waits. It shows that the pace breaks once the work nears the tick, which is the console's risk.

## Consequences

- The console run is one command and one log, and the verdict is computed: `scripts/switch-perf`, then paste the `perfsession:` lines. The same session runs on every platform (`--displays desktop`), so a desktop regression can be judged the same way.
- On the Switch the default backend is the GPU, which #242's hardware checks have not yet confirmed. If deko3d draws wrong on the console, the `gfx_backend` row is the one line to remove. The software fallback then shows the game, slower.
- On the Switch, Enhanced no longer means `auto`. Players can still choose it.
- ADR 0143's "Default" paragraph is superseded by this ADR (its addendum says so).

**How we'd know this was wrong:**

- The console session passes on software too: then the factor above was far too pessimistic, and the `gfx_backend` row can go.
- The GPU session fails on the console with the CPU busy (guest, compositor): then the next step is #279's profile and flags, not a higher render scale.
- The GPU session fails with the CPU idle and SDL_RenderPresent long (the GPU's time, `--gfx-gpu-timing`): then deko3d's path, not the CPU, is the limit.
- The console passes with a lot of headroom: then `render_scale = auto` (or 2 handheld) is worth a measured run, with the GPU's time in the guard's budget.

## Addendum (#284)

`scripts/switch-perf` measured whatever `build/switch-release/MapleThief.nro` was, and `--build` made the hello NRO there (ADR 0139), so its numbers said nothing about the game. It now sends the player build's NRO, `build/switch-player/MapleThief.nro`, and `--build` runs `scripts/build-switch HEAD --preset switch-player --gen work/gen`. Before sending, the release guard's player mode checks the NRO against `work/gen/dispatch.c`, and a hello NRO (no recompiled code) is refused. `--build-only` stops after the build and that check. `just build switch` with the IPA builds the same `switch-player` preset, since `switch-release`'s kit guard refuses the generated code; the game's Switch build is Release only. `tests/test_switch_perf.py` and `tests/test_build_oneshot.py` pin both choices.
