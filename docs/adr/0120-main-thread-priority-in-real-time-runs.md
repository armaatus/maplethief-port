# 120. The main thread at SDL's high priority in real-time runs

- **Status:** Accepted
- **Date:** 2026-11-05
- **Issue:** [#206](https://github.com/armaatus/maplethief-port/issues/206) (epic #201)
- **Builds on:** [ADR 0042](0042-frontend-game-loop-and-presentation.md) (the game on the main thread), [ADR 0045](0045-audio-mixer-and-output.md) (SDL's audio device thread), [ADR 0075](0075-display-locked-frame-pacing.md) (the paced clock and the game's gate), [ADR 0087](0087-game-speed-and-fast-forward.md) and its addendum ("Load is still felt")

## Context

The game's tick gate takes `lastFrame = now` with no catch-up ([re/timing-threading.md](../re/timing-threading.md)). A wake that comes late because another process holds the core makes that tick late, and the game keeps the loss. PR #205 removed the port's own causes (presents over the gate while fast-forwarding), and on a quiet machine ×2 and ×3 came within 1 %. Under heavy load (load average 12 to 24 on 10 cores: other builds, real-asset suites) `platform.game_speed` still failed now and then: Debug ×3 −21.75 % once, Debug `ff_wall` −8.85 %, Release ×3 −2.3 to −2.9 %, and the untouched 1× 25 ms run −5.47 % in Debug.

The main thread ran at the default priority, the same as every compiler and test process competing for the cores. An app opened from Finder gets the interactive QoS from its role; a game started from a terminal or by ctest does not. The A/B experiment of ADR 0087's addendum, with the main thread at SDL's time-critical priority, measured +0.15 % and +0.17 % at Release ×3 under load 12 to 14, against −1.46 % and −2.59 % as is.

## Options considered

1. **macOS QoS `QOS_CLASS_USER_INTERACTIVE`** (`pthread_set_qos_class_self_np`). Apple's own way, but measured on this Mac, a process started from a terminal gets a Mach priority of only 37 for it (31 by default), not the 46 to 47 an app in the foreground role gets. It is also macOS/iOS only.
2. **SDL's `SDL_THREAD_PRIORITY_TIME_CRITICAL`** (the A/B). On Apple `SCHED_RR` 47, the top of the range. That is the same priority as SDL's own audio device thread, which SDL sets to time-critical for playback. On Windows it is `THREAD_PRIORITY_TIME_CRITICAL`, above nearly everything in the system.
3. **SDL's `SDL_THREAD_PRIORITY_HIGH`** on every platform. **Chosen.**
   - macOS and iOS: `SCHED_RR` at priority 45. That is a fixed priority above the default 31 that other processes start from and decay below, and under the audio thread's 47.
   - Windows: `THREAD_PRIORITY_HIGHEST` (+2), under the audio thread's `TIME_CRITICAL`. MMCSS (`AvSetMmThreadCharacteristics("Games")`) was left out: it cannot be measured here, and `HIGHEST` already wins over normal processes.
   - Linux: nice −10 for the thread (`setpriority`), else through RealtimeKit over D-Bus. Without the rights for either, the call is refused and the thread keeps its nice value.

**Where.** Options were every run, real-time runs only, or a setting. **Real-time runs of the game only, no setting**:
- The loop sleeps between ticks (the gate's 1 ms polls), so a raised thread holds a core only for a tick's work.
- A fixed-step run (`--fixed-step`) never waits. At a raised priority it would hold a core for the whole run, and on a stuck guest it would hold it for good. It keeps the default, and its output does not depend on timing anyway.
- A player has nothing to tune. `MAPLETHIEF_MAIN_THREAD_PRIORITY=normal` turns the raise off, for A/B measurements and for a machine where it misbehaves.

## Decision

- `platform/src/thread_priority.{h,cpp}`: `raise_main_thread_priority` calls `SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH)` and reads the thread's scheduling back before and after (`pthread_getschedparam`, the thread's nice value on Linux, `GetThreadPriority` on Windows).
- `main.cpp` calls it just before `run_game`, unless the run is fixed-step or the environment opts out. The start-up log names the result in one line:
  - `game: main thread: SCHED_RR priority 45 (SDL's high priority; it was SCHED_OTHER priority 31)`;
  - `game: main thread: SDL's high priority refused (<SDL's error>); it keeps nice 0`;
  - `game: main thread: SCHED_OTHER priority 31, the default (a fixed-step run)`.
- The audio device callback records its thread's scheduling on its first call. The exit log adds `game: audio: the device's callback thread ran at SCHED_RR priority 47 (the main thread at SCHED_RR priority 45)`, so a run shows the audio thread above the main thread.
- GCD's worker threads (ADR 0033) keep the default. Only one guest thread runs at a time and the main thread waits while a worker holds the baton, so a worker never competes with the main thread. Only Chartboost's blocks use them.
- `platform.game_speed` checks the start-up line in each run and reports every deviation with the machine's load average (`/proc/loadavg`, `sysctl vm.loadavg`), so a failure on a busy machine reads as such.

## Consequences

**Measured under load** (`platform.game_speed`, M4 Mac mini, 10 cores; each run's ×2, ×3, ×2 with `pacing = wall`, and 25 ms). The load was other worktrees' builds and suites plus 12 `yes` processes started 45 s before each run. The load column is the 1-minute load average sampled every 3 s during the run (min / mean / max).

| Build | Run | Load | ×2 | ×3 | ×2 wall | 25 ms |
|---|---|---|---|---|---|---|
| Debug (`mac-debug`) | 1 | 18.7 / 20.4 / 23.3 | −0.88 % | −0.51 % | −0.94 % | −2.30 % |
| Debug | 2 | 27.4 / 28.2 / 29.8 | −0.66 % | −0.66 % | −0.98 % | −2.75 % |
| Debug | 3 | 27.9 / 28.2 / 28.8 | −0.76 % | −0.48 % | −0.80 % | −2.49 % |
| Release (`mac-release-universal`) | 1 | 22.4 / 24.3 / 26.7 | +0.17 % | +0.12 % | +0.01 % | −2.41 % |
| Release | 2 | 21.7 / 26.0 / 28.8 | +0.09 % | +0.13 % | +0.02 % | −1.38 % |
| Release | 3 | 15.8 / 17.4 / 19.8 | +0.05 % | +0.15 % | −0.00 % | −0.66 % |
| Debug, `normal` (A/B) | 1 | 16.5 / 17.5 / 18.7 | **−3.98 %** (failed) | | | |
| Release, `normal` (A/B) | 1 | 20.7 / 21.3 / 21.8 | −2.11 % | **−3.32 %** (failed) | | |

An earlier series at a lighter load (8 `yes`, load means 13.5 to 16.9) passed 6 of 6 runs with the raise too: Debug ×2 −0.81 to −0.90 %, ×3 −0.28 to −0.66 %; Release within +0.17 % at ×2, ×3 and wall. The A/B at that load failed in Debug (×2 −5.13 %) and measured −2.35 %, −1.13 %, −2.04 % and −3.90 % in Release.

- With the raise, ×2, ×3 and wall stay within 1 % in Debug and 0.2 % in Release at load 15 to 30. The 25 ms run keeps its −0.7 to −2.8 %, the game's own poll slack (ADR 0087's addendum), within its 5 %.
- **Audio.** A 60 s real-time run with the device (CoreAudio, the Mac mini's speakers, load 13 to 18) logged `0 late device callbacks` in Release and in Debug, and `the device's callback thread ran at SCHED_RR priority 47 (the main thread at SCHED_RR priority 45)`.
- The A/B measurements use `MAPLETHIEF_MAIN_THREAD_PRIORITY=normal` on the same build.
- A stuck guest in a real-time run (a guest loop that never returns to the run loop) holds one core at priority 45. The machine stays usable: the audio thread is above it and the other cores stay free. Time-critical (47) was not taken, partly for this reason.
- Windows (MSVC) and Linux are not measured under load here. Linux in a container usually runs as root, so the raise succeeds there; on a desktop without rights it is refused and logged.
- `thread_priority_test.cpp` covers the opt-out's values, the descriptions, the start-up line for each outcome, and a real raise on a thread of the test's own (or its refusal, where the system says no).
