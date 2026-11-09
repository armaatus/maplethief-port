# 130. An overload guard for display-locked pacing

- **Status:** Accepted
- **Date:** 2026-11-09
- **Issue:** [#163](https://github.com/armaatus/maplethief-port/issues/163) (epic #268)
- **Extends:** [ADR 0075](0075-display-locked-frame-pacing.md) (the paced clock and `plan_pacing`). **Builds on:** [ADR 0071](0071-frame-telemetry-and-frame-time-overlay.md) (the tick intervals), [ADR 0120](0120-main-thread-priority-in-real-time-runs.md) (the main thread's priority)

## Context

Locked to the display, the guest clock moves only at refresh boundaries. A turn that wakes more than a refresh late starts its tick a whole refresh late, and the game's `lastFrame = now` keeps that delay. On the wall clock the same tick loses only its real lateness. ADR 0075 measured the cost under heavy contention (load average 20 to 36 from parallel container builds, before ADR 0120 raised the main thread's priority): wall held 60.81 % of frames for 3 refreshes at 18.23 ticks a second (−8.8 %), display 81.72 % at 16.86 (−15.7 %).

A run must not be slower locked than it would be on the wall clock. A map load, or a load the lock copes with, must not lose its smoothness.

## Options considered

**What to watch.** The tick interval the frame telemetry already measures (`FrameRecord::tick_ns`, GL present to GL present), fed once per window frame while locked. The alternatives were the wake's own lateness (it needs a second measurement beside the telemetry and says nothing about what the game lost) and the run's mean speed (slow to notice, and a map load moves it).

**What counts as late.** An interval of the tick plus 0.75 of a refresh or more (62.5 ms at 60 Hz for 50 ms). A late wake costs a whole refresh, so the intervals are near 50, 66.7 and 83.3 ms; the work of the tick moves each present by a few milliseconds, so a full refresh would miss some late ticks and half a refresh would catch jitter.

**Stalls.** An interval of more than twice the tick is not counted, neither as late nor as on time. A map load stalls the game on either clock, so it says nothing about the lock. The window also needs half the ticks that fit in it (20 of 40) to decide, so a stall and a few ticks after it cannot unlock, and neither can the first ticks of a retry.

**How much is too much.** 15 % of the window's ticks. At the load ADR 0075 found harmless (up to about 14) under 1.3 % of frames were not held; a share under that would need a burst to mistake. Measured on the real runs below: in the 2 s windows of the runs that kept their speed the late share peaks at 12.5 % (24 compile loops at load 37), and in a starved one it is 48 % at the median.

**What to do about it.** Fall back to the wall clock for a fixed back-off of 30 s and try again, as the issue asks. A retry that fails costs about a second locked (20 ticks) every 30 s. A growing back-off was left out: nothing measured needs it.

## Decision

- `OverloadGuard` (`platform/src/pacing.{h,cpp}`, pure, every time passed in) sits beside `plan_pacing`. `add(now, tick_ns, plan)` takes each tick interval of a locked plan; `apply(plan, now)` returns the plan, or while backing off a wall plan whose `why` holds the reason. Constants: `kWindowNs` 2 s, `kBackoffNs` 30 s, `kLateRefreshes` 0.75, `kLateShare` 0.15, `kMinShareOfTicks` 0.5.
- `game.cpp`: `follow_clock_rate` passes each turn's plan through `apply`; `present` feeds `add` from the window frame's record while `g.locked`. Plans that are not locked pass through unchanged, so fast-forward, `pacing = wall`, 144 Hz and vsync off are as before. A fixed-step run has no paced clock and never reaches the guard.
- The unlock is logged through the existing `pacing:` line, with the numbers: `pacing: the wall clock (overloaded: 6 of 20 ticks in the last 2.0 s late by a refresh or more; the lock is tried again in 30 s), at 236.949 s`. The retry is the `pacing: locked to the display` line. The exit log adds `game: clock: the overload guard left the lock N times; last: …` only when it did.

## Evidence

- **CI** (`tests/cpp/pacing_test.cpp`, `pacing: overload guard: …`, synthetic): steady ticks and ±8 ms of jitter keep the lock; a third of the ticks a refresh late unlocks, with the reason; one tick in 10 late does not; a 2 s stall, 340 ms stalls with short presents between them, and a stall with a few late ticks do not; the back-off, the retry, a second unlock within a window of a still-starved machine; a recovered machine keeps the lock; plans that are not locked pass through (fast-forward's reason and rate kept); a refresh is the plan's own (120 Hz: 58.4 ms is late, 60 Hz: it is not); a new plan starts the count over.
- **A real window, local** (Release arm64, Apple M4, Metal, 60 Hz, software 1×, `--no-audio`, the post-tutorial save loaded by `--tap`, Henesys with no input, 325 s a run, back to back, ADR 0075's method). The numbers are the map's, 24 to 325 s of `--perf-csv`. "Raise off" is `MAPLETHIEF_MAIN_THREAD_PRIORITY=normal` (the condition ADR 0075 measured in, before ADR 0120). Load is the 1-minute average.

  | Load generator (load) | Priority raise | Run | Held 3 refreshes | Ticks a second | Tick p99 | Unlocks |
  |---|---|---|---|---|---|---|
  | none (9 → 3) | on (the default) | display | 99.98 % | 20.001 (+0.01 %) | 53.69 ms | 0 |
  | 30 `yes` (25 → 49) | on (the default) | wall | 96.15 % | 19.863 (−0.69 %) | 51.13 ms | |
  | 30 `yes` (49 → 41) | on (the default) | display | 99.98 % | 20.001 (+0.01 %) | 54.13 ms | 0 |
  | 24 compile loops (35 → 37) | off | wall | 65.59 % | 19.716 (−1.42 %) | 77.36 ms | |
  | 24 compile loops (37 → 38) | off | locked, no guard | 98.55 % | 19.998 (−0.01 %) | 64.87 ms | |
  | 24 compile loops (38 → 37) | off | display | 85.39 % | 19.991 (−0.04 %) | 62.83 ms | 0 |
  | 24 compile loops (37 → 35) | off | wall | 63.62 % | 19.699 (−1.51 %) | 80.17 ms | |
  | 48 `yes` (57 → 63) | off | wall | 16.07 % | 14.943 (−25.29 %) | 136.20 ms | |
  | 48 `yes` (63 → 81) | off | display | 17.08 % | 15.308 (−23.46 %) | 133.26 ms | 11 |
  | 48 `yes` (81 → 70) | off | wall | 11.87 % | 13.492 (−32.54 %) | 166.28 ms | |
  | 48 `yes` (31 → 58) | off | locked, no guard | 59.94 % | 16.429 (−17.85 %) | 125.03 ms | |
  | 48 `yes` (58 → 58) | off | display | 20.30 % | 15.955 (−20.23 %) | 126.47 ms | 11 |
  | 48 `yes` (58 → 61) | off | locked, no guard | 56.35 % | 16.425 (−17.87 %) | 124.77 ms | |

  "Locked, no guard" is a temporary build with `kLateShare` set out of reach, for this comparison only.
  - The whole-run exit line of the quiet run: `game: pacing: 6543 GL frames (ticks) shown for 3 vblanks 99.14% (6487), …`, and `game: clock: … locked 325.0 s of 325.1 s; the guest clock is -0.006 s from the wall clock`. Its 0.82 % of 0 and 1 refreshes are the loading screen's presents, as in ADR 0075.
  - A display run while the machine was busier (load 19 to 54, no generator of mine, the raise on): 99.97 % held 3 refreshes, 20.000 ticks a second, no unlock.
  - The guard's log in the 48 `yes` display run: 11 unlocks, each retry failing after about 20 ticks, locked 24.1 s of 325 s.
  - One pair is left out of the table because its load was not steady: 24 `yes`, the raise off. Wall ran at load 22 to 30 (19.750 ticks a second), then display at load 30 rising to 76 from other work on the machine (19.394, 4 unlocks). Up to the first unlock, at load about 30, that display run held 99.72 % at 19.987 ticks a second.
- **Fixed-step runs are unchanged** (local, real assets): `scripts/regress --preset original` gives digest `8a2f05733c8d4603` with the guard and with the same sources before it, identical at all 85 checkpoints (`--baseline`).

## Consequences

- A starved locked run goes to the wall clock within about a window of the moment the ticks run late, and tries the lock again every 30 s. At the display's whole-multiple rates with a machine that keeps up, the guard does nothing: the light and moderate runs above log no unlock.
- `pacing = display` kept at least `pacing = wall`'s speed in every set of the table that has a wall run.
- **The premise did not reproduce.** In these runs the lock was never slower than the wall clock: with the raise on, and with the raise off under compile loops, it was faster, and at load 58 from `yes` staying locked (16.43) beat the guarded run on the wall clock (15.96) by 2.9 %. ADR 0075's measurement (container builds, load 20 to 36, before ADR 0120) did not repeat here with `yes` or compile loops, and container builds were not tried. The guard is built as the issue specifies and meets its criteria, but what it trades (smoothness for speed, at a load that is rarely reached with the raise on) is worth checking on a weaker machine. #142, the per-platform performance check, is where that shows.
- The guard reads only what the window frame already measured, so a run that keeps up pays one comparison a tick.
- Wave 3's 1 ms grid (#165) can sit beside `plan_pacing` and the guard as they are: `apply` only decides between the plan it is given and the wall clock.
