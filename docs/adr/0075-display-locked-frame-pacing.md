# 75. Display-locked frame pacing: the real-time guest clock steps at the display's refreshes

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #80
- **Builds on:** [ADR 0042](0042-frontend-game-loop-and-presentation.md) (the turn hook's wait, presenting at every GL present), [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (one clock behind every clock the guest reads), [ADR 0071](0071-frame-telemetry-and-frame-time-overlay.md) (the tick intervals and the simulated scan-out this is measured with)

## Context

The game ticks when its 1 ms run-loop timer finds `now − lastFrame ≥ interval` on `GetTimeMs` (50 ms by default; [timing-threading.md](../re/timing-threading.md)). Until now the port ran that gate on the host's wall clock: the turn hook woke about every millisecond and the game ticked at the first turn after its 50 ms. Each tick therefore lasted 50.0–51.0 ms, never exactly 3 refreshes of a 60 Hz display, so the frames drifted against the refreshes.

Measured before this change (Release, Apple M4, Metal, 60 Hz, ADR 0071's telemetry; the five-minute runs below): on the map 84–95 % of the frames were shown for 3 refreshes and the rest for 4 or 2 (a visible hitch every second or two). The game ran at 19.68–19.79 ticks a second, 1.1–1.6 % slow. At 144 Hz, where a tick is 7.2 refreshes, frames alternate between 7 and 8 refreshes by design.

ADR 0071 also found that `SDL_RenderPresent` with vsync returns in about 0.2 ms on Metal: the swap chain queues the frame, the present does not wait, and SDL3 tells nobody when a frame reached the display. So the host does not know the display's vblank phase, only its nominal rate (`SDL_GetCurrentDisplayMode`).

Fixed-step runs (goldens, shots, `play.first_map`) are on ADR 0044's virtual clock and must stay byte-identical.

## Options considered

**What gets locked.**

1. **Call the game's tick from the host every n refreshes.** It bypasses `CMTimer::OnRunLoopTimer`: no `lastFrame`, no sub-timer bookkeeping, no drift semantics (timing-threading.md, "For the runtime" 2).
2. **Present with vsync blocking, one present a refresh, and count the presents.** The swap chain's back-pressure would drive the loop at the real vblanks, but only once three drawables are queued: two refreshes more latency, 60 presents a second of a 20 Hz game, and a different loop on each backend.
3. **Step the guest clock at the refreshes (a paced clock).** The game's own gate does the rest: if the clock moves only at refresh boundaries, by exactly *interval / n* each, the gate opens at every n-th refresh and nowhere else. `lastFrame`, the sub-timers, the overruns and the pauses keep their original meaning. **Chosen.**

**When to lock.** Only when the display's rate is (within 0.5 %) a whole multiple of the tick rate: n = refresh × interval / 1000 (3 at 60 Hz, 6 at 120 Hz, 5 at 100 Hz for 50 ms; 3 at 75 Hz for the 40 ms speed). Locking at 144 Hz would round each tick to 7 or 8 refreshes, 2.8 % fast or 11 % slow; there and with no vsync, an unknown rate or the boot's 10 ms interval the clock is the wall clock as before. The interval is CMTimer's own (`0x11A478`), read every turn, so a speed change in the options menu re-plans.

**Drift against the wall clock.** A display that is 59.94 or 60.06 Hz (or reports 60 and is not) would make a guest clock that steps a fixed *interval / n* per refresh run 0.1 % slow or fast, and real-time timers (sub-timers, the 10 s keep-alive) with it. Instead the clock reads, at refresh k, round(k × period / step) steps. Most refreshes add one step. About once every 1,000 refreshes one adds none or two, so that tick lasts one refresh more or less, and the game keeps the wall clock's speed. The rounding is against the clock's start, so plan changes cannot add up to a drift.

**The display's phase.** A frame is shown at the first vblank after its present. A present whose time wanders (a tick's work takes 7 ms, then 9) crosses a vblank another does not, and that is judder again. Since the phase is unknown, every present is put at the same point of the host's refresh grid instead: the first present after each wake waits for the first grid boundary after the wake plus the longest work of the last 600 such presents (30 s of ticks: a raised point drops back seldom, since each change of the point is one frame held a refresh more or less). Whatever the display's phase, every tick's frame is then presented at the same offset from its vblank. Reading the real vblank times (`CAMetalDisplayLink`, DXGI frame statistics) would let the grid follow the display. SDL3 does not expose them yet.

**Presents between ticks.** Mouse motion, UIKit animations and the 0.25 s refresh make window presents of the same GL frame. One of them queued ahead of a tick's frame in the same refresh pushes that frame a refresh later. While locked, the turn hook makes at most one window present a refresh, never after the tick's own, and its wait does not end early for events (they are handled while it waits, and the next turn is at the next refresh). Input reaches the game at most one refresh later. The game only reads it at its 50 ms ticks anyway.

**The default.** Display pacing is on for everyone (`pacing = display`), since it fixes the judder and the 1.2 % slow speed. `pacing = wall` keeps the loop of ADR 0042 unchanged.

## Decision

- **`platform/src/pacing.{h,cpp}`** (no SDL; host clock and sleep passed in):
  - `plan_pacing(mode, refresh_hz, vsync, tick_ms)` gives a `PacingPlan`: locked with `vblanks` = n, or the wall clock with the reason. `kLockTolerance` = 0.5 %.
  - `PacedClock` is the real-time guest clock: `install()` points `mt_libc_config.clock` (`gettimeofday`, `CFAbsoluteTimeGetCurrent`, the run loop, UIKit) and `mt_gcd_config.clock`/`sleep_until` at it, like ADR 0044's `VirtualClock`. It starts at the host's wall time, so the random seed still differs per launch, and from then on it runs on the host's monotonic clock. It is a pure function of host time: wall, guest = host; locked, guest = steps(refresh index) × interval / n, nanosecond-exact over n steps. `set_plan` rebases so the time is continuous and never goes back. `host_when(guest)` gives the turn hook its wake.
  - `PresentHold`: the present point above.
- **`game.cpp`**, real time with `pacing = display` only. The paced clock is installed before the process loads. Each turn re-plans from `FrameStats::refresh_hz()` (updated on `SDL_EVENT_WINDOW_DISPLAY_CHANGED`), vsync and the interval. While locked, `wait_locked` sleeps until the guest's next timer is due (one wake a refresh, `SDL_WaitEventTimeout` then `SDL_DelayPrecise`), and `present()` holds the first present after a wake. The turn-hook present gate is described above. Not locked, the loop is ADR 0042's. `--fixed-step` keeps the virtual clock and never sees the paced clock.
- **The setting**: `pacing = display | wall` in `settings.ini`, `--pacing`, default `display`. It is a Video row of ADR 0070's registry, `display` in all three presets. `game.cpp` reads it from `GameOptions::settings`.
- **Vsync** is one of `plan_pacing`'s inputs (`Game::vsync`, the renderer's). With vsync off it never locks, and the reason is logged. #145's `vsync = on | off` only has to set that flag; each turn's re-plan picks up a change.
- **With #152's live render scale**, `apply_render_scale` runs in the turn hook before the wait, so its cost is not one of the present point's works. A rescale slower than a tick makes one late tick, as any stall does. The hold's sleep counts as waiting (`Phase::kWait`), so `RenderScaleGovernor`'s budget (the frontend and mt_gfx means of `FrameStats::recent()`) never sees it.
- **`FrameStats::tick_held()`**: the refreshes each GL frame was shown for, from one tick's window frame to the next tick's, whatever window frames came between (the judder of the game's own frames). The exit log adds `game: pacing: N GL frames (ticks) shown for …`, and a `game: clock:` line with the plan, the time locked, the guest clock against the wall clock, and the ticks a second against the game's interval.

## Evidence

- **CI** (`tests/cpp/pacing_test.cpp`, `platform.pacing: …`, synthetic):
  - The plan for 60/120/100/240/59.94/60.06 Hz (locked) and 144/75/165/50/90 Hz, 59.5 Hz, the 40 and 65 ms speeds, no vsync, an unknown rate and an unknown interval (wall).
  - The clock's steps (16,666,666 / 16,666,667 ns, exactly 50 ms in three).
  - `host_when` against `guest_at` for 10,000 random times on five plans; continuity and monotony over 3,000 random plan changes.
  - The game's gate (`GetTimeMs`, `lastFrame = now`, the 1 ms re-arm) driven by the locked wake with 1–9 ms of random work per tick, through FrameStats' scan-out at five display phases: 100 % of frames held 3 refreshes at 60 Hz over 6,000 ticks, more than 99.9 % held 6 at 120 Hz, 3 at 75 Hz for 40 ms. Tick to tick is exactly 50 ms on both clocks.
  - At 59.94 and 60.06 Hz over 24,000 ticks: more than 99 % held 3 refreshes and the rest 2 or 4, the speed is the wall clock's within 0.02 %, and the clock is never more than half a step plus a refresh from the wall clock.
  - A 2 s stall: one late tick, then the cadence again, with no burst.
  - `PresentHold`'s point, lead, stall rule and n = 1. The setting.
- **A real window, five minutes on a map, local** (Release arm64, Apple M4, Metal, 60 Hz, software 1×, `--no-audio`; the machine shared with other builds, load average 3–14). One binary, `--pacing wall` then `--pacing display` (and the reverse order), 325 s each. The post-tutorial save (made by `menu_nav_real_test tutorial`) is loaded with `--tap` (Play, the slot, Start). The hero then stands in Henesys (map 2.0) with no input. The map numbers come from `--perf-csv`, 24–325 s (301 s, about 6,000 ticks):

  | Run (load) | Pacing | Held 3 refreshes | 4 / 2 | Ticks a second | Tick p99 |
  |---|---|---|---|---|---|
  | 1 (23 → 3) | wall | 89.22 % | 6.85 / 3.76 % | 19.788 (−1.06 %) | 58.41 ms |
  | 1 (3 → 7) | display | **99.88 %** | 0.07 / 0.05 % | 19.999 (−0.006 %) | 56.31 ms |
  | 2 (14 → 3) | wall | 95.04 % | 4.15 / 0.81 % | 19.780 (−1.10 %) | 51.59 ms |
  | 2 (10 → 14) | display | 98.70 % | 0.56 / 0.66 % | 19.996 (−0.02 %) | 63.28 ms |
  | 3 (3 → 14) | wall | 84.21 % | 8.85 / 5.64 % | 19.679 (−1.60 %) | 64.43 ms |
  | 3 (3 → 3) | display | **99.70 %** | 0.15 / 0.15 % | 20.000 (−0.000 %) | 55.67 ms |

  Run 2's display run used a 10 s work window. Under a load of 10–14, works over the point raised it and its decay lowered it again, 74 times. The 30 s window is run 3's.
  - The exit lines of run 3 cover the whole run, menus and the map load included. Display: `game: ticks: 6544 GL frames, 20.13 a second; tick interval p50 50.00 ms, p99 55.69 ms, max 64.74 ms, mean 49.66 ms` and `game: pacing: 6543 GL frames (ticks) shown for 3 vblanks 98.91% (6472), 1 vblank 0.41% (27), 0 vblanks 0.40% (26), …` (the 0s and 1s are the loading screen's presents; they also put the whole run's ticks a second 0.67 % over 20). `game: clock: … locked 325.0 s of 325.1 s; the guest clock is -0.001 s from the wall clock`.
  - Wall: `game: ticks: 6441 GL frames, 19.82 a second; … p99 64.11 ms …` and `game: pacing: 6440 GL frames (ticks) shown for 3 vblanks 83.57% (5382), 4 vblanks 8.84% (569), 2 vblanks 5.54% (357), …`.
- **Fixed-step runs are unchanged** (local, real assets): the same `--fixed-step 1` run (the save loaded by taps, shots at 5, 11, 20, 30 and 45 s, `--audio-capture`) with the binary before this change and after it gives byte-identical PNGs and WAV. `play.first_map` passes.

## Consequences

- At 60 and 120 Hz a 50 ms tick is exactly 3 or 6 refreshes. The game runs at the wall clock's speed (20.00 ticks a second instead of 19.7–19.8), and the run loop turns at most once a refresh (about 30 turns a second on the map instead of about 500).
- A tick's frame reaches the window at the first refresh boundary after its work (one refresh after the tick starts at 60 Hz, two under load): about 8 ms later than before, or 25 ms with the point raised.
- A busy machine still breaks the cadence when a tick's work goes over the present point; each raise of the point (and its decay 30 s later) costs one frame held a refresh more or less.
- 144 Hz, 75 Hz at 50 ms, 165 Hz, VRR and no vsync stay on the wall clock and judder as before. A real vblank clock (`CAMetalDisplayLink`, DXGI frame statistics, or SDL if it gains presentation times) would let the grid follow the display's phase and rate. It would also allow frame interpolation (#87), which can draw at every refresh between two ticks: the paced clock's step and `vblank_at` give it the fraction of the tick.
- If the display's real vblank falls within the present's wake jitter (tens of µs) of the grid boundary, frames can still straddle it. That is unlikely and needs real vblank times to rule out.
- `pacing` joins the settings registry (ADR 0070). It is the Original column, but not the behaviour from before #80 (ADR 0070's Defaults say so).
- Windows, Linux and real 120/144 Hz displays are measured by hand (`needs-human`).
