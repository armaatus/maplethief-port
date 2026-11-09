# 131. A 1 ms grid where the display cannot lock

- **Status:** Accepted
- **Date:** 2026-11-09
- **Issue:** [#165](https://github.com/armaatus/maplethief-port/issues/165) (epic #268)
- **Extends:** [ADR 0075](0075-display-locked-frame-pacing.md) (the paced clock and `plan_pacing`), [ADR 0087](0087-game-speed-and-fast-forward.md) (`game_speed`, its real-time measurements). **Builds on:** [ADR 0130](0130-overload-guard-for-display-locked-pacing.md) (the overload guard)

## Context

The game ticks at the first 1 ms run-loop poll after its interval has passed on `GetTimeMs` ([timing-threading.md](../re/timing-threading.md)). On the wall clock a poll is late by the host's wake-up, so a tick lasts about half a millisecond more than its interval. That is the original's own behaviour and the same for every length, but it shows more on #151's shorter ticks. ADR 0087 measured on the wall-clock fallbacks (Release, dummy drivers, vsync off): 65 ms −0.19 %, 50 ms −0.21 %, 40 ms −1.64 %, 33 ms −2.32 %, 25 ms −1.86 %. ADR 0075's display lock takes this out only where the refresh rate is a whole multiple of the tick rate.

A paced clock that stands still between whole guest milliseconds makes every poll read the gate exactly while the wake is less than 1 ms late, so each tick lasts exactly its interval. ADR 0075's machinery already does that for a refresh grid, and ADR 0075's correction keeps the wall clock's speed on it.

The same grid has a catch. The game re-arms its 1 ms timer at `now + 0.001` (`RunLoopTimer_Arm` `0x8E29`, in doubles), and the run loop fires it when `fire <= now`. At exactly 1 ms the next millisecond ties with that fire date. One double's rounding either way decides, so polls are lost and a tick lasts a millisecond more: the same tie ADR 0087 found for its fixed step of 1 ms (`tests/cpp/pacing_test.cpp`, `pacing: the 1 ms grid needs the run loop's tolerance …`: 26 ms against 25 without the tolerance, for every start time tried).

## Options considered

**What the fallbacks get.**

1. **The wall clock, as before.** The slack stays (−1.6 % to −2.3 % at the short lengths).
2. **A 1 ms grid in the paced clock.** Reuses ADR 0075's stepped mapping and correction with `refresh_hz = 1000` and `vblanks` = the interval in ms. **Chosen.**

**The tie at 1 ms.**

1. **Step 1.001 ms** (what `play.game_speed`'s fixed step does), with a held step every 1,000 steps to keep the wall clock. No runtime change. But ticks then last 49, 50 or 51 ms and the "exact interval" is lost.
2. **A tolerance on the run loop's fire test, set only for a grid plan.** `mt_runloop_set_fire_tolerance`, 0.5 µs: under half a guest microsecond (the guest reads whole ones), over two doubles' rounding at the clock's size. Display-locked and fixed-step runs set none, so their fire tests are the old ones and their goldens are untouched. `wait_locked` uses the same slack when it turns the fire date into a guest microsecond. **Chosen.**

**`pacing = wall`.** Keeps today's loop (ADR 0042, ADR 0075): no paced clock, no grid. It is the escape hatch and the A/B baseline for the paced modes, so it must stay what it was.

**What `locked` means.** The grid plan is `grid` and not `locked`. `locked` stays "locked to the display", because the present gate, `PresentHold`, the overload guard and `locked_ns` are all about the display's refreshes, and on a 1 ms grid they have nothing to do. `PacingPlan::stepped()` is the mapping's test (the paced clock's arithmetic is shared by both).

**The overload guard.** The grid does not go through it. `add` ignores a plan that is not `locked` and `apply` passes it through unchanged, so the guard cannot fire or stop firing by accident: a grid has no refresh a late tick could lose, and the guard's reason (a tick that wakes a refresh late stays that late because of `lastFrame = now`) does not apply at a 1 ms step. While it backs off from a display lock the plan it gives is ADR 0130's plain wall clock, not the grid: that plan's job is the wall clock's real-lateness behaviour.

**Presents.** One present a tick: a tick's own frame is presented once, at once, with no hold, and the 0.5 ms margin of the wall path applies to the presents between ticks (at most one a display refresh, 60 Hz when the rate is unknown), so a stream of events or a UIKit animation cannot present at 1 kHz.

**The wait.** `wait_locked` is the grid's wait, without the display's parts (the hold's wake). Its last stretch is a plain `SDL_DelayNS`, not `SDL_DelayPrecise`, on a grid: a wake up to 1 ms late still reads this millisecond's step, and spinning a core at 1 kHz for the run's whole length buys nothing.

## Decision

- `plan_pacing`: with `pacing = display`, a known interval and no fast-forward, what the display cannot lock (no vsync, an unknown rate, a rate that is not a multiple of the tick rate) gets `grid = true`, `refresh_hz = 1000`, `vblanks` = the interval's milliseconds, and keeps its reason in `why`. An unknown interval, `pacing = wall` and fast-forward keep the plans they had.
- `PacedClock` steps on `plan.stepped()`; `locked_ns` counts display locks only.
- `game.cpp`: `g.grid` beside `g.locked`; `follow_clock_rate` sets the run loop's tolerance when it changes; `wait_locked` also serves a grid, without the hold; the present gate as above.
- `runtime`: `mt_runloop_set_fire_tolerance` (`runloop.cpp`, `cf.h`), 0 by default and after `mt_cf_reset`.

## Evidence

- **CI** (synthetic):
  - `tests/cpp/pacing_test.cpp`, `platform.pacing: …`: the plan for every fallback and tick length, and the display locks unchanged (`what the display cannot lock gets a 1 ms grid; the rest is unchanged`); the clock's whole-millisecond steps, no drift over two hours, `host_when`, `PresentHold` never holding on a grid (`on a 1 ms grid the guest clock steps whole milliseconds …`).
  - The game's gate on the real double arithmetic, wakes up to 0.9 ms late: every tick of 65, 50, 40, 33 and 25 ms lasts exactly its interval (`on the 1 ms grid every game_speed length ticks at its own rate`). Without the tolerance a 25 ms tick lasts 26 ms (`the 1 ms grid needs the run loop's tolerance …`).
  - The guard (`overload guard: a 1 ms grid is neither counted nor taken over`): late grid ticks unlock nothing, a display lock on the same guard still unlocks, the back-off's plan is the plain wall clock.
  - `tests/cpp/cf_test.cpp`, `CF: a fire date within the run loop's tolerance is due …`: the tolerance, and a repeating timer that fires inside it stays on its grid (without that it fired twice at one date).
- **Real time** (Release arm64, Apple M4, `--preset original --no-audio`, the main menu, ticks from `--perf-csv` from 6 to 25 s, one run of 26 s each, one after the other, load average 1.9 to 2.5). The first two columns are the dummy drivers: vsync off, and vsync on, where the dummy driver reports no refresh rate. The third is a real 60 Hz window, which cannot lock these lengths (the "not a multiple" path). Ticks a second, and the difference from the length's rate:

  | Length | Rate | No vsync | Unknown rate | Real 60 Hz window |
  |---|---|---|---|---|
  | 65 ms | 15.385 | 15.385 (−0.000 %) | 15.385 (+0.000 %) | 15.385 (−0.000 %) |
  | 50 ms | 20.000 | 20.000 (+0.000 %) | 20.000 (−0.000 %) | locks to the display, below |
  | 40 ms | 25.000 | 25.000 (+0.000 %) | 25.000 (+0.000 %) | 25.000 (+0.000 %) |
  | 33 ms | 30.303 | 30.303 (−0.000 %) | 30.303 (+0.000 %) | 30.303 (−0.000 %) |
  | 25 ms | 40.000 | 40.000 (−0.000 %) | 40.000 (−0.000 %) | 40.000 (−0.001 %) |

  Tick intervals in every one of these runs: p50 the length to 0.01 ms, p99 at most 0.03 ms over it. ADR 0087 measured −0.19 % to −2.32 % for the same lengths on the wall clock. A 144 Hz panel was not available: 144 Hz takes the same plan as the 60 Hz window's lengths and is tested as a plan only.
- **Fast-forward** keeps ADR 0087's wall plan and is not on the grid (dummy drivers, vsync off, held from 4 s, 26 s of host time). `--perf-csv` has a row per window frame, and those are capped at a refresh while fast-forwarding, so these numbers are the exit line's over the whole run, against what the hold makes of the interval:

  | Rate and length | Ticks a second | Against | Median tick |
  |---|---|---|---|
  | ×2, 50 ms | 36.942 | 36.898 (+0.12 %) | 25.00 ms |
  | ×3, 50 ms | 53.882 | 53.799 (+0.15 %) | 16.67 ms |
  | ×2, 25 ms | 73.850 | 73.799 (+0.07 %) | 12.50 ms |
  | ×3, 25 ms | 107.691 | 107.592 (+0.09 %) | 8.35 ms |

- **The display lock is unchanged:** a real 60 Hz window at 50 ms logs `locked to the display: 60.00 Hz, 3 refreshes a 50 ms tick`, locked 26.1 s of 26.1 s, 20.000 ticks a second from 6 s, 519 of 520 GL frames shown for 3 vblanks.
- **Fixed-step runs are unchanged** (local, real assets): `scripts/regress --preset original` gives digest `8a2f05733c8d4603`, identical to the baseline from `dev` at 0fc8370; `platform.device_golden`'s seven screens have 0 differing pixels against the renders recorded before this change. Fixed-step runs never set the tolerance.

## Consequences

- On every display and with vsync off, a game tick lasts exactly its interval on the guest clock, at the wall clock's speed, while the wake is under 1 ms late. These runs were at load average 2 to 3; a busier machine was not measured beyond short trials at 6 to 10 (the same result, over 7 s). The default 50 ms at 60 Hz and the other display locks are unchanged.
- A real 144 Hz panel was not available. The plan is the same plan as any rate that is not a multiple of the tick rate, tested as such; it needs a look on such a display (`needs-human`, as ADR 0075 says for 120/144 Hz).
- Frames at 144 Hz still alternate between 7 and 8 refreshes (7.2 a tick): that needs real vblank times, which SDL does not expose.
- `kGridFireTolerance` is safe only for guest clocks kept at this magnitude. The clock stays under 2³⁰ ms from the shifted epoch (ADR 0087, #155), so a double's rounding stays under 0.2 µs there.
