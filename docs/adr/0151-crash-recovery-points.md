# 151. What the port keeps when the game traps: rolling recovery points at safe points

- **Status:** Accepted (delegated by Joris, 2026-11-13)
- **Date:** 2026-11-13
- **Issue:** [#334](https://github.com/armaatus/maplethief-port/issues/334) (epic [#289](https://github.com/armaatus/maplethief-port/issues/289)); the implementation is [#335](https://github.com/armaatus/maplethief-port/issues/335)
- **Builds on:** [ADR 0105](0105-save-states-in-process-snapshot-at-safe-points.md) (save states: the snapshot, the safe point, the restore), [ADR 0089](0089-autosave-on-quit-and-background.md) (autosave and the game's save rules), [ADR 0070](0070-settings-registry-and-presets.md) (the settings and presets), [ADR 0030's #198 addendum](0030-runtime-diagnostics.md#addendum-the-players-log-crash-file-dialog-and-safe-start-198-2026-11-09) (the crash marker and the safe start), [ADR 0141](0141-switch-lifecycle-applet-mode-and-audio.md) (applet-mode memory), [ADR 0074](0074-save-files-atomic-writes-sanitising-and-ios-documents.md) (atomic writes), [ADR 0145](0145-switch-performance-session-and-defaults.md) (the Switch's CPU)

## Context

Every trap ends the game: an import with no implementation, a message nobody answers, a guest memory fault (`runtime/src/boot/trap.c`). Since #198 the player gets a crash file, a dialog, and at the next start an offer of default settings. Nothing of the session is kept. Autosave runs only when the player leaves (ADR 0089), and the game itself refuses to save on 16 maps. Two thirds of the game has never run in the port (#285), so a player is most likely to meet the first latent bug deep in a session.

#334 asked what the port should keep, with measurements. The options:

1. **A rolling save state at safe points** (ADR 0105's snapshot, bound to the binary, the port's version and the slot). It lives outside the game's saves, so the game's files stay as the game wrote them. A port update makes it unreadable, which is fine for crash recovery.
2. **The game's own save on a timer,** wherever ADR 0089's guards allow a save.
3. **A state written at the trap.**
4. **Nothing** beyond #198's crash file and safe start.

### The measurements

`play.save_state` (the regression replay, ADR 0104) saves at two safe points: map 2.8 (step 47,377) and the hunting field 2.9 just before the snail fight (step 73,628). For #334 the test driver takes a fixed render scale from `MAPLETHIEF_SETTINGS=render_scale=N`, and `MT_SAVE_STATE_MEASURE=1` saves each point a second time stored (`MT_SNAPSHOT_STORE`, not deflated) and writes the deflated state as a file through `mt_saves_replace_file`, as `quick_state.cpp` does. Every run passed: at each scale and both points the restored 200 ticks equal the uninterrupted run's (12 pairs of equal digests; the render scale changes the frame hashes, so each scale has its own pair, `f970fd2b49c31151` and `819786c3ce7d80e1` at scale 1).

Apple M4, macOS, local real assets, 2026-11-13, with other builds running (load 6 to 8). The **Release** build is a host-architecture `CMAKE_BUILD_TYPE=Release` configure of `play_real_test`. Times are for map 2.8 / 2.9, sizes in MB (10⁶ bytes):

| Render scale | Raw state | Deflated state | Stored save (Release) | Deflated save (Release) | File write | Restore (Release) | Deflated save, restore (Debug) |
|---|---|---|---|---|---|---|---|
| 1 | 23.5 / 22.9 | 4.66 / 4.61 | 51 / 48 ms | 214 / 213 ms | 6 / 12 ms | 55 / 51 ms | 527 / 490 ms, 278 / 252 ms |
| 2 | 25.3 / 24.8 | 5.08 / 5.01 | 58 / 53 ms | 233 / 228 ms | 7 / 7 ms | 58 / 53 ms | 533 / 582 ms, 292 / 672 ms |
| 3 | 28.4 / 27.8 | 5.45 / 5.36 | 63 / 58 ms | 247 / 239 ms | 6 / 7 ms | 60 / 56 ms | 583 / 577 ms, 314 / 301 ms |
| 4 | 32.7 / 32.1 | 6.41 / 6.30 | 74 / 68 ms | 283 / 269 ms | 9 / 9 ms | 69 / 73 ms | 671 / 663 ms, 352 / 334 ms |
| 6 | 45.0 / 44.4 | 8.63 / 8.47 | 102 / 95 ms | 372 / 358 ms | 43 / 16 ms | 91 / 80 ms | 900 / 880 ms, 464 / 438 ms |
| 8 | 62.2 / 61.6 | 11.30 / 11.10 | 167 / 138 ms | 489 / 473 ms | 17 / 12 ms | 112 / 105 ms | 1,201 / 1,194 ms, 612 / 632 ms |

What the numbers say:

- **The render scale adds one renderbuffer's pixels**, 480 × 320 × 4 × N² bytes raw: 0.6 MB at scale 1, 39.3 MB at scale 8. The rest (about 22.9 MB raw at 2.8: guest memory, the 8.9 MB of texels, the audio PCM, the host states) does not depend on the scale.
- **Deflating is most of a save's time.** The stored save takes 48 to 167 ms; deflating it adds 160 to 330 ms. The two parts separate: the stored state is a buffer of its own, and deflating it touches nothing of the game.
- **A tick is 50 ms.** The stored save is about one tick at scales 1 to 3, one and a half at 4, two at 6 and three at 8. Writing the file is cheap (6 to 43 ms) and needs no main thread.
- **A Debug build is two to ten times slower** (the stored save about ten times, the restore about five: the snapshot code is `-O0`, and ADR 0134 optimises only a frame's hot code). Debug numbers are for tests, not for defaults.
- **The Switch is not measured.** ADR 0145 puts a 1020 MHz Cortex-A57 at about 23 times slower than the M4 in CPU work. The stored save is mostly copying and CRCs, so it may be less than 23 times slower; at scale 1 (the Switch's default) it lands somewhere between about 0.25 and 1.2 s.

## Options considered

1. **A rolling save state at safe points.** It works on every map, the 16 maps where the game refuses to save included, which is where a crash costs most (a boss). It never touches `theif<N>` or `quest<N>`. Its cost is a main-thread stall of one to three ticks per point on a desktop, and some disk. It needs a restore in a new process, which no test has done yet: `play.save_state` and `play.quick_state` restore in the same process.
2. **The game's own save on a timer.** No new file format, and the restore is the game's own load. But it changes what is on disk, which the Original preset promises not to do. ADR 0089 rejected a timer autosave because a save after a bad move is a loss the player did not choose. It cannot save in a boss room, a scene, or mid-jump. Not chosen: it saves least where crashes hurt most.
3. **A state written at the trap.** A trap is not a safe point. The guest is inside a tick, the host stack is not the run loop's (ADR 0105's restore checks the sjlj chain and the stack pointer against the turn hook's), a host state may be half-updated, and the state is the one that just trapped. A restore of it either refuses or replays straight into the trap. Rejected for ADR 0105's reasons.
4. **Nothing.** The safe start (#198) stays, and a crash costs everything since the last save. That is what epic #289 exists to fix.

## Decision

**Option 1: rolling recovery points, taken at ADR 0105's safe points and offered at the next start after a crash.** Decided by the agent (delegated by Joris, 2026-11-13); Joris can veto it in the PR.

**When a point is taken.** Only in the turn hook, at the first turn where `mt_snapshot_safe` holds after one of these:

- a **map load**: a new area or map outside the loading modes 6 to 8 (the rule `AppletWatch` uses). The loading screen has already stopped play, so the stall falls into a pause the player expects;
- a **game save**: the slot's `theif<N>` or `quest<N>` was written, by the game's menu, a fixed save point or ADR 0089's autosave. ADR 0078's hook on `CDataMgr::SaveQuestData` `0x38F55` sees every one, since each `SaveSaveData` call is followed by it. Without this, a resume would go back to before progress that is already on disk;
- **five minutes of play** since the last point, counted on the guest clock (a pause, a minimised window and the HOME menu do not count). This covers a long hunt on one map.

A point that is due while the previous one is still being written is skipped; the next trigger takes it. Nothing is taken while `save_states`' quick-save or quick-load runs in the same turn.

**How it is written.** The main thread makes the stored state (`MT_SNAPSHOT_STORE`): the one-to-three-tick stall in the table. A worker thread deflates it (a new runtime call that re-packs a stored state's sections deflated, the same file `mt_snapshot_save` makes) and writes it with `mt_saves_replace_file`, so a crash mid-write leaves the previous file whole (ADR 0074). The worker touches only its own buffer. A clean exit, or the run's end after a trap, waits for a write in progress to finish before it goes on.

**Where the files live, and how many are kept.** `<data dir>/states/recovery-1.state` (the newest) and `recovery-2.state` (the one before it), beside the quick-save's `slot<N>.state` and never in the game's `saves/`. Two, because the newest point may sit just before a crash that repeats (a bug right after a map load), and the one before it is one step further back. Each new point moves `recovery-1` to `recovery-2`. At most 2 × 11.3 MB on disk at scale 8, 2 × 4.7 MB at scale 1. A clean exit deletes both, at the moment #198 clears `crashes/running.marker`. So the files exist only while the game runs, or after a crash.

**What the next start asks.** When #198 finds the marker, it also reads the newest recovery file's header without restoring (`mt_snapshot_read_header`: magic, format, the binary's SHA-256, `MT_VERSION`, CRCs). A file that does not read, a state of another port version or of another copy of the game, or one older than its slot's last game save (by the files' modification times) is not offered and is deleted. Otherwise #198's box gains one paragraph, "A recovery point of character N, taken M minutes before the crash, is kept", and four buttons:

| Button | Settings | The recovery point |
|---|---|---|
| **Resume** (default) | the player's | armed |
| **Resume with default settings** | #198's safe start | armed |
| **Start with default settings** | #198's safe start | deleted |
| **Start normally** (Escape) | the player's | deleted |

Without a recovery point, the box is #198's as it is. On a target where an SDL message box cannot be answered with every input method (a console, a Steam Deck in Game Mode), the same question is asked in the window instead. That is #335's task.

**How a resume runs.** A safe point is in the field, so a restore cannot run at the title. An armed point waits for the player to load its character as usual. At the first safe turn in the field with that slot, the frontend restores it as `quick_load` does (`release_for_restore`, `mt_snapshot_restore`, the clock re-based), and the window's notice says it resumed. Once restored, `recovery-1` is deleted (it is now the live game, and the next trigger writes a new point), so `recovery-2` becomes the newest. If the resumed run crashes again before a new point, the next start offers the older one. A point that has been restored is never offered twice. If the player loads another character first, the point is dropped, with a notice. The game's next own save writes what was restored, as after any quick-load.

**The setting.** `crash_recovery = off | on`, in the **Gameplay** group beside `autosave` (not the Assists: it does not change play, and Assists are off in every preset). `--crash-recovery`, `Applies::kAtOnce`, game-only. The row for ADR 0070's table:

| Group | Key | Original | Original + controller | Enhanced |
|---|---|---|---|---|
| Gameplay | `crash_recovery` (#334, ADR 0151; after `autosave`) | off | off | on |

- **Enhanced: on.** A stall of one to three ticks at a map load, after a save and every five minutes is a small price for a session that survives a crash. Enhanced already makes this trade with `render_scale = auto` and `hd_text`.
- **Original and Original + controller: off.** Option 1 leaves the game's files alone, so it would be allowed there. But the Original columns keep the game's own pacing, and a recovery point stalls play. A player can turn it on in any preset (the preset becomes `custom`, as for any key). #198's safe start runs in every preset either way.
- **The Nintendo Switch: off in every column** (a `kPlatformDefaults` row, `SettingsPlatform::kSwitch`, "a recovery point stalls play for an unmeasured 0.25 to 1.2 s on the Cortex-A57 and holds about 28 MB in applet mode"). It changes when a console run measures the stall (#335's hardware item).

**The Switch's applet-mode budget (ADR 0141).** A point holds, for its length, the stored state (about 23 MB at scale 1) and then the deflated one (about 5 MB) beside it. In applet mode that is close to half of `kAppletMarginBytes` (64 MiB). When `crash_recovery` is on and the app runs as an applet, a point is taken only if `AppletWatch`'s last sample leaves the margin plus 1.25 times the last point's raw size free in newlib's heap. Otherwise it is skipped with a log line (`recovery: point skipped: applet memory, N MiB free`). Title takeover has no such limit beyond the commit map's.

**#198's safe start.** The recovery point and the safe start are separate choices on one box. Resume with default settings is the safe start's run with a point armed. The state holds no settings (ADR 0105: the settings, the bindings, the window and the audio device are the player's), so a resume under the defaults is the same game with the safe renderer. The texels in the state are restored as saved, a texture pack's included. They are data, not the pack's code, and they last until the game uploads that texture again. A crash caused by a setting, a pack or the renderer is therefore avoided by the second or third button. A crash in the game's own code is avoided only if the player plays differently. In that case the second crash offers the older point, then none.

**Log lines.** `recovery: point 1 taken at <trigger> (map A.M, slot N): R MB raw in T ms`, `recovery: point written: D MB in T ms`, `recovery: point skipped: <reason>`, and at a start `recovery: offered|not offered (<reason>)|resumed|dropped (<reason>)`.

## Consequences

- A trap in the field costs at most five minutes of play, or less after a map change or a save. That holds in a boss room too, where the game itself never saves.
- **A restore in a new process is new.** `play.save_state` and `play.quick_state` restore in the process that saved. #335's real-asset test has to prove a point saved in one process and restored in a fresh one (same binary, same settings) plays the same checkpoints. It is the first thing to build, and the decision depends on it.
- A stall of 48 to 167 ms on an M4 at each point in Enhanced. The worker thread is new, and it owns only a buffer. ADR 0071's telemetry will show the stall as a slow frame, and `--perf-session` (ADR 0145) as one in the second it falls in.
- Disk: at most two files, 9 to 23 MB in total, and none after a clean exit.
- A port update makes an old point unreadable: it is deleted at the next start, which is right for crash recovery.
- `crash_recovery` joins ADR 0070's pinned lists: `presets_test.cpp`'s `kExpected` gains the row, and the Switch's platform row joins `presets: platform rows set the Switch's defaults and its preset columns`.

**How we'd know this was wrong:**

- The cross-process restore cannot be made exact (a host state that differs per process). Then the recovery point shrinks to option 2 inside ADR 0089's guards, for Enhanced only, and this ADR is superseded.
- Players notice the stall: the five-minute timer goes first, then the stored save gets cheaper (copy-on-write pages, or the texels skipped and re-read from the bundle).
- A console run shows the stall under about two ticks at scale 1: the Switch row goes, and the Switch follows the presets.
- Resumed runs crash again at the same place most of the time: then the points are taken too late, and the older point should be offered first.
