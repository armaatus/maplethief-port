# 195. The hang watchdog: a stalled run loop writes a report and asks Keep waiting or Quit

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#337](https://github.com/armaatus/maplethief-port/issues/337) (epic [#289](https://github.com/armaatus/maplethief-port/issues/289))
- **Builds on:** [ADR 0030](0030-runtime-diagnostics.md) and its #198 addendum (the crash report, the crash file, the dialog helper, the crash marker), [ADR 0063](0063-app-lifecycle-background-and-minimise.md) (the background), [ADR 0014](0014-recompiler-design.md) (the guest is single-threaded; its registers live in `mt_cpu`)

## Context

A trap, a hardware fault or a guest that ends by itself writes a crash report (#198). A guest loop that never ends, or a host call that never returns, leaves a frozen window and nothing else: the player kills the game and the bug report has no position. (A GCD deadlock already traps.)

The guest runs on the main thread. Every iteration of the iOS run loop calls the frontend's turn hook (`on_turn` in `game.cpp`), so a turn that does not come back is a hang. Loops that hold the turn on purpose (the settings menu and the other overlays, the key pad editor) and the background wait (`wait_hidden`) are the frontend's own.

Generated code does not store the guest PC (ADR 0030): the hardware fault report names the guest function from the host PC through `dladdr`. A hung thread is alive, so its host PC has to be taken from another thread.

## Decision

**A heartbeat and a watchdog thread** (`platform/src/hang_watchdog.{h,cpp}`). `hang_watchdog_beat()` runs at the top of every run-loop turn, in every pass of the loops that hold the turn, in `pump_events`, and from an SDL event watch on the main thread (a macOS window drag or live resize runs the system's own loop between turns; any event SDL hands the main thread means the thread is alive). The watchdog thread checks every tenth of the limit (10 to 250 ms).

**The limit: 15 seconds.** No real-asset turn comes near it (measurements below). A `HangPause` stops the clock while the window is hidden (`wait_hidden`: the background, minimised); a system dialog that blocks the main thread takes one too. The run's own system dialogs, the save and backup file pickers, are SDL's asynchronous ones: the loop keeps turning under them, and the message boxes of #198 come before the run or after it. The watch runs from `mt_process_load` to the end of `mt_process_run`, so the boot is watched and the import screen (before the game) never is.

**When it is on.** Desktops only: macOS, Windows, Linux. iOS, Android and the Switch compile it and `hang_watch_seconds` answers 0 (their systems watch apps themselves, and the dialog's helper process does not exist there). Off in a `--fixed-step` run, whose virtual clock says nothing of real time and whose captures may be slow, unless `--hang-seconds S` or `--force-hang` asks. `--hang-seconds 0` turns it off anywhere.

**The sample** (`mt_hang_watch_thread`, `mt_hang_report` in `maplethief/diag.h`, `runtime/src/diag/crash.cpp`). On POSIX the watchdog sends `SIGURG` to the guest's thread (ignored by default, so a late one is harmless); its handler copies the host PC from the signal context and the thread's `mt_cpu`, both async-signal-safe. On Windows the watchdog suspends the thread, reads its context and copies `mt_cpu`, then resumes it before formatting anything (the suspended thread may hold the heap's lock). A thread that does not answer in half a second is reported without a host PC. The report is ADR 0030's (`mt_crash_format`) with a new kind, `MT_CRASH_HANG`: "no run-loop turn for N s at host pc …", the generated guest function there where `dladdr` names one, the registers as last stored and the symbolized r7 backtrace.

**The file and the question.** The report goes to `crashes/<time>.txt` as "MapleThief hang report" with the version header and the log's tail, written by the crash report's own writer (`hang_report_write`). A hang is not a crash: `crash_reported()` stays false, so a game that recovers and quits cleanly clears the crash marker. The main thread is the one that hangs, so the question comes from a helper process, as #198's dialog does: `MapleThief --hang-dialog FILE` shows **Keep waiting** (the default, Return and Escape) or **Quit**, and exits with 3 for Quit. The watchdog waits for it. Keep waiting: the question does not come back for this stall, only after a turn has come and gone. Quit: the process ends with `kExitHang` (6) through `std::_Exit`. The crash marker is not cleared, so the next start offers the safe start. A scripted run logs the report and keeps waiting. The test seams are #198's: `MAPLETHIEF_CRASH_DIALOG_LOG` (`hang dialog: <path>`) and `set_hang_dialog_for_test`.

**`--force-hang`** (a test aid, like `--force-crash`): 2 s after the first turn the turn sleeps forever.

## Measurements

The play harness (`tests/cpp/game_driver.h`) runs the watchdog at the player's limit in every real-asset run and fails a run with a hang report. Apple M4, macOS, Debug, local real assets, 2026-11-27, with other builds running:

| Run | What it loads and plays | Longest run-loop turn |
|---|---|---|
| `play.first_map` | the boot, a new character, the intro scene, the tutorial maps 2.8 and 2.9, Henesys, the map screen, field 2.3 for 15 game-minutes | 0.61 s |
| `play.save_quit` | the boot, a save and the quit | 0.17 s |
| `play.save_relaunch` | the boot and loading that save | 0.21 s |

The longest, 0.61 s, is a twenty-fifth of the limit. The limit leaves room for a slow machine, a Debug build and a busy disk.

`MapleThief --force-hang --hang-seconds 5` on the real game wrote the report after 5.0 s with the host PC in `__semwait_signal` (the sleeping turn) and the backtrace `#0 0x00002808 f_000027E0+0x28` from the build's symbol table.

## Consequences

- A frozen desktop game leaves a report with the guest's position and lets the player quit cleanly.
- The guest's thread takes `SIGURG` on POSIX. Nothing else in the port uses it; a host library that does would need another signal.
- The registers are those generated code last stored to `mt_cpu`, as in a hardware fault's report. A host function that loops without storing its registers shows its caller's frames; the host PC still names where it is.
- A player who holds the window still for 15 s without any event (a modal system loop that sends none) would see the question once. Keep waiting answers it.
- Tests: `platform.hang watchdog: *` (the limit by run, pauses, one report per stall; a synthetic guest loop through `run_game` with a symbolized backtrace, on every OS CI builds; a child process that quits with `kExitHang`, POSIX). Windows and Linux runs of the real game are a needs-human check.
