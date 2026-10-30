# 71. Frame telemetry: tick intervals, the split of a frame, simulated scan-out, and a frame-time overlay

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #137
- **Builds on:** [ADR 0042](0042-frontend-game-loop-and-presentation.md) (the present and turn hooks, the frame-time lines), [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (the fixed-step mode, goldens and shots), [ADR 0068](0068-gpu-presentation-without-read-back.md) (the `game: frame time:` line)

## Context

Research for #80 on 2026-10-30 had to patch timestamps into a scratch copy of `game.cpp`: the exit log only had means per window frame (`game: per window frame: …`, `game: frame time: …`, read by `platform.gpu_compare`). It found that ticks average 50.2 to 50.6 ms instead of 50.0, that frames at 60 Hz are sometimes held for 4 vblanks instead of 3, and that render scale 4 drops the software path to 10 GL frames a second. Later work builds on such numbers at run time, not only after the run:

- #80, display-locked pacing: present timestamps and how many vblanks each frame stayed on screen.
- #151, game-speed options: the measured tick rate within 1 % of each speed.
- #152, a frame-budget guard and `render_scale = auto`: the frontend's cost per frame while the game runs.

Measurement must not change the game: a `--fixed-step` run, its shots and the goldens must be the same bytes with or without it.

What the hooks can see (ADR 0042): the guest runs on the main thread between hook calls. The GL present hook (`-presentRenderbuffer:`) comes once per game tick on a map (and many times inside a map load). The turn hook comes after every run-loop turn and waits. mt_gfx's draws run inside the guest's GL calls; `mt_gfx_stats.total.ns` already sums `mt_gfx_submit`'s time. SDL3 has no presentation timestamps or vblank counters. On Metal, `SDL_RenderPresent` returns at once with vsync on (0.2 ms); the wait lands elsewhere or nowhere.

## Options considered

**Where the numbers live.**

1. **Counters in `game.cpp`'s `Game`, logged at exit.** That is how the frame-time lines work today. But #80, #151 and #152 need them inside the run, and nothing could test them without the game.
2. **A small module fed with timestamps (`FrameStats`).** It has no clock and no SDL: the hooks pass `SDL_GetTicksNS`, tests pass made-up times. The same object serves the exit log, the CSV, the overlay and the later children. **Chosen.**

**What a "tick" is.** The game has no tick event the host sees. Its 50 ms tick ends with one GL present, so the interval between GL present hooks is the tick interval. During a map load the game presents from inside the load, so those intervals are short. Window presents without a new GL frame (a UIKit animation, the 0.25 s refresh, an event) are window frames but not ticks.

**Vblanks.**

1. **Round each interval to whole refresh periods.** That hides drift: 50.4 ms is always "3" at 60 Hz, yet a real display shows a 4 every few seconds.
2. **Simulate scan-out.** A frame presented at *t* is first shown at the next vblank after *t* and stays until the next frame is shown. The vblanks are put at whole periods from the run's start. The display's phase is unknown, but the distribution of the counts does not depend on it. This is the method the #80 research used. **Chosen.**

**The overlay's drawing.**

1. **Into the composited screen with `Canvas`** (the remap overlay's way). The screen image is what shots, the `--frames` check and GPU presentation's layers use. The overlay would then have to be kept out of each of them separately.
2. **With SDL's renderer, into the window only, after every read-back** (the controller focus frame's way, `SDL_RenderDebugText`). It never touches the screen image. **Chosen.**

## Decision

- **`platform/src/frame_stats.{h,cpp}`, `FrameStats`**, fed by `game.cpp`:
  - `start` before `mt_process_run`, with the display's refresh rate (`SDL_GetCurrentDisplayMode`; updated on `SDL_EVENT_WINDOW_DISPLAY_CHANGED`) and whether vsync is on.
  - `enter_host`/`leave_host` around both hooks (`HostCall`). The time between them is the guest's, and nested hooks count as the outer one.
  - `gl_present` in the GL present hook (a tick). `add(Phase, ns)` for taking the GL frame, the compositor, upload and draw (with the focus frame and the overlay), `SDL_RenderPresent`, and the turn hook's wait.
  - `frame` after `SDL_RenderPresent` (one window frame), with mt_gfx's running total. That much of the guest's time moves to mt_gfx.
  - `gap` after a minimise (`wait_hidden`) and after the remap overlay (`run_remap`). A pause is neither an interval nor an overrun.
  - Each window frame gets a `FrameRecord`:
    - when its present returned, the interval since the last one, and the tick interval if it shows a new GL frame;
    - the vblanks the previous frame was shown for (simulated scan-out; 0 if replaced before it was shown; −1 if unknown: no vsync or refresh rate, the first frame, after a gap);
    - the split, which adds up to the interval: guest, mt_gfx, take, compose, draw, present, wait, and other (the hooks' own work and guest code they call: lifecycle messages, focus).
  - `frontend_ns()` is take + compose + draw, what the existing `game: frame time:` line sums.
- **The API the later children read:**
  - `last()`: the newest `FrameRecord`.
  - `recent()`: means and maxima over the window frames of the last second (fps, ticks a second, tick ms, each phase, `frontend_ms`, `max_frontend_ms`), stopping at a gap.
  - `ticks()`: count, rate, p50, p99, max, mean, and overruns over `overrun_ns()` (55 ms by default; `set_overrun_ns` for another game speed, #151).
  - `held()`: the vblank histogram. `refresh_hz()`, `vsync()`.
- **The exit log** keeps `game: per window frame: …` and `game: frame time: …` unchanged (`platform.gpu_compare` parses the latter) and adds three lines:
  - `game: ticks: N GL frames, R a second; tick interval p50 …, p99 …, max …, mean … ms; K over 55 ms`
  - `game: frame split: N window frames; per frame guest …, mt_gfx …, taking the GL frame …, compositor …, upload and draw …, SDL_RenderPresent …, waiting …, other … ms`
  - `game: pacing: H Hz display, vsync on, scan-out simulated from the present times: N window frames shown for 3 vblanks P% (n), …`, or why there are no counts.
  - In the fixed-step mode the ticks line says the times are wall clock and the run is not paced.
- **`--perf-csv FILE`**: one row per window frame (not only per tick, so extra presents show up): `frame,gl_frame,tick,present_s,interval_ms,tick_ms,vblanks,guest_ms,gfx_ms,take_ms,compose_ms,draw_ms,present_ms,wait_ms,other_ms`. Unmeasured cells are empty. Buffered (`PerfCsv`).
- **The overlay, `platform/src/perf_overlay.{h,cpp}`, `PerfOverlay`**:
  - Four lines in a translucent box at the window's top left, in `SDL_RenderDebugText`'s 8 × 8 font at the window's pixel density: fps, ticks/s, tick ms (max); guest, gfx, take; compositor, draw, frontend (max); present, wait, the last frame's vblanks.
  - Toggled by **F3** (Shift allowed; the key is the overlay's, so no binding sees it), and shown from the start by `perf_overlay = on` (`settings.ini`, `--perf-overlay`; off by default).
  - Drawn in `present()` after the read-back check, the captures and the focus frame, then `SDL_RenderPresent`. It never touches `Screen` or `GpuScreen`.
  - **Never in a run that writes images:** `perf_overlay_allowed()` is false with `--fixed-step`, `--shot` or `--capture`. F3 then only logs why.
- **Gameplay is untouched.** The telemetry only reads `SDL_GetTicksNS` and `mt_gfx_get_stats`. It never reads or advances the guest's clock, and nothing in the loop branches on it.

## Evidence

- **CI** (`tests/cpp/frame_stats_test.cpp`, `platform.frame_stats: …`, `platform.perf_overlay: …`; SDL's dummy driver):
  - A synthetic real-time run, timestamped as the hooks would be: 203 ticks (99 at 50 ms, then 56, 70 and 80 ms, a 2 s minimise, 99 more). The three exit lines are checked to the character: 19.89 GL frames a second, p50 50.00, p99 56.00, max 80.00, mean 50.28, 3 over 55 ms; the split's means; at 60 Hz, 3 vblanks 99.00 % (199), 4 and 5 once.
  - Per-frame splits add up to the interval; nested hooks; scan-out at 60 and 144 Hz (7 and 8 alternate); no vsync and no refresh rate.
  - The last second's means; the CSV text and file.
  - F3 toggles live; repeats, releases, Ctrl+F3 and other keys do not; the window's read-back has the overlay's pixels only while it is visible; the renderer's state is restored. `perf_overlay_allowed` is false for `--fixed-step`, `--shot` and `--capture`, and an overlay not allowed never draws, even with the setting on and after F3.
  - `perf_overlay` in `settings.ini` (default off, the key order).
- **Real assets, local** (`platform.perf_telemetry`, `platform/perf_test.cmake`, Release, Apple M4, dummy drivers):
  - A 6 s real-time run logs the three lines and writes a row per window frame (e.g. 142 GL frames, 23.70 a second while loading, p50 50.01 ms, p99 55.81 ms; 200 rows).
  - A fixed-step run with `--shot`, `--capture` and `--audio-capture` writes the same three PNGs and WAV byte for byte with `--perf-overlay on --perf-csv` as without.
  - `platform.device_golden` on the Release build with the telemetry: all 7 screens match our renders (0 pixels differ).
- **A real window** (Release, Apple M4, Metal, 60 Hz, software 1x, 45 s: main menu, Play, the loading screen, the intro on its map):
  - `ticks: 919 GL frames, 20.42 a second; tick interval p50 50.26 ms, p99 55.64 ms, max 76.67 ms, mean 48.98 ms; 10 over 55 ms`
  - `frame split: 1027 window frames; per frame guest 0.37 ms, mt_gfx 3.23 ms, taking the GL frame 0.06 ms, compositor 5.11 ms, upload and draw 0.15 ms, SDL_RenderPresent 0.21 ms, waiting 34.24 ms, other 0.48 ms`
  - `pacing: 60.00 Hz display, vsync on, …: 3 vblanks 78.75 % (808), 1 vblank 9.94 %, 0 vblanks 6.14 %, 4 vblanks 3.31 %, 2 vblanks 1.85 %`. The 0s and 1s are the main menu's 60 Hz animation presents. From 25 s on (the map), 3 vblanks 95.5 %, 4 vblanks 3.0 %, 2 vblanks 1.5 %: #80's hitch, now visible in every run.
  - In all 1,026 CSV rows that have an interval, the split sums to `interval_ms` within 0.01 ms.

## Consequences

- #80 reads `last()` (present time, vblanks) and `held()`, and its exit criterion ("≥ 99 % of frames held for exactly 3 vblanks") is the pacing line. #151 reads `ticks()` and moves `set_overrun_ns` with the game speed. #152 reads `recent().frontend_ms` / `max_frontend_ms` against its budget, and can say so in the overlay.
- The vblank counts are a simulation from present timestamps, not the display's. On Metal `SDL_RenderPresent` does not block per frame, so a present's return is not its scan-out. If #80 needs the real thing, it has to come from the platform (`CAMetalDrawable.presentedTime`, DXGI frame statistics), outside SDL.
- Event-driven presents (a mouse moved over the window presents in real time, ADR 0042) show up as extra rows with `tick = 0`. #80 decides whether they may present between ticks.
- Every window frame costs a few `SDL_GetTicksNS` calls and one `mt_gfx_get_stats`, which walks the texture table (microseconds). If that ever shows in a profile, the runtime gets a cheaper accessor for `total.ns`.
- `perf_overlay` sits in `settings.{h,cpp}` the current way. The settings registry (#144) takes it over.
- How we'd know this was wrong: a fixed-step run that differs with the telemetry's options (`platform.perf_telemetry` compares them), a split whose phases do not add up (the CSV's columns sum to `interval_ms`), or #80's pacing that looks right in the log but judders on screen. In that last case the simulation is not good enough, and the platform's timestamps are needed.
