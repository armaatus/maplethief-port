# 156. A speedrun kit: splits from observed functions, clean runs by the Assists rule, replays verified headless

- **Status:** Accepted (delegated by Joris, 2026-11-13)
- **Date:** 2026-11-13
- **Issue:** [#377](https://github.com/armaatus/maplethief-port/issues/377) (research), epic [#402](https://github.com/armaatus/maplethief-port/issues/402)
- **Builds on:** [ADR 0053](0053-observed-functions-game-events-and-rumble.md) (observed functions, game events, listeners), [ADR 0070](0070-settings-registry-and-presets.md) (Assists: anything that changes gameplay, never on through a preset), [ADR 0075](0075-display-locked-frame-pacing.md) and [ADR 0131](0131-a-1-ms-grid-where-the-display-cannot-lock.md) (the paced clock), [ADR 0104](0104-input-replay-and-regression-checkpoints.md) (the replay file, checkpoints, digests), [ADR 0105](0105-save-states-in-process-snapshot-at-safe-points.md) (save states)

## Context

#377 asked what a speedrun and practice kit would take: a run timer with splits, an input display and replays a player can record and share. Comparable ports ship these for their runners. Most of the parts exist:

- **Game events without touching game logic** (ADR 0053). The lifter calls `mt_observe_entry` at the entry of a listed function; the runtime reads the arguments, applies the function's own guard and reports an event to a hook and up to four listeners. It never writes guest memory. Today it reports hits, level-ups, the death popup, revives and the choice of a new or a saved character (`MT_GAME_EVENT_NEW_CHARACTER`, the natural start of a run).
- **The places a split can come from**, all known from the decompiler:
  - *Chapter end.* The script native `clearAll` `0x996ED` resets the quest states and adds one to the chapter (quest manager `+0x04`). Three script paths call it: the tutorial's last portal (chapter 0 to 1), the trip to Orbis (1 to 2) and chapter 2's end ([script-vm.md](../re/script-vm.md), [ANALYSIS.md](../ANALYSIS.md)).
  - *Job advancement.* The native `setClassUp` `0x997E1` sets the hero's branch and tier through `0x655AD`. Eleven advancements exist across the chapters, all given by one NPC.
  - *Boss kill.* `CMob::Die` `0x73349` handles every mob's death; mob ids 64 and 66 to 69 are the bosses, and each queues its closing scene. Mob 69's scene is the first ending scene ([mechanics.md](../re/mechanics.md)), so its death is the end of a full-game run.
  - *Map change.* Every warp goes through `CMapleWorld::changeMap` `0x81369` (area, map, portal), so a map can be a split too.
- **Determinism.** The game is single-threaded and reads all time through one clock hook (`mt_libc_config.clock`, plus GCD's), and the clock at boot is its only random seed ([timing-threading.md](../re/timing-threading.md)). ADR 0104 replays exactly, but only in the test driver on the virtual clock. It rejected recording in the shipped game because "a recording in real time does not replay, because the guest reads real time".
- **What a run may have on.** ADR 0070 puts everything that changes gameplay in the Assists group (today `cash_shop`, `game_speed` other than `game`, `fast_forward`, `turbo_attack`, `auto_run`, `hold_toggle`, `auto_potion`, `auto_loot`, `save_states`), neutral in every preset. Gameplay holds the rows that change what the game does without being a help: `fix_original_bugs` (crashes and dead ends) and `skip_intros` (scenes at ×8) among them.

Joris delegated the product calls on 2026-11-13 (CLAUDE.md, Human hand-offs); this ADR makes them.

## Options considered

**Where the splits come from.**

1. **Poll guest memory every turn** (the chapter byte, the job, the map). No recompiler change, but it guesses at transitions (a load restores another chapter; ADR 0053 rejected polling for the same reason).
2. **LiveSplit auto-splitter scripts** reading the port's process memory. Every build moves the guest heap's host address and the scripts break; and the Switch and iOS have no LiveSplit.
3. **Observed functions. Chosen.** One `OBSERVED` entry, one case in `mt_observe_entry` and one event kind each (ADR 0053's own cost estimate), the guest unchanged byte for byte.

**What the timer counts.**

1. **Real time (RTA)**, the host's monotonic clock from the run's start to its last split. What runners and leaderboards expect, and what the timer shows.
2. **Game ticks**, the number of 50 ms ticks the game ran (the speed option of the game's own options changes the interval, not the count). Independent of the machine, of load times and of the window; a fair second time for a leaderboard that wants one.

Both are recorded, RTA is shown. Which one a leaderboard ranks by is the community's choice.

**How a shared replay is made exact.**

1. **SDL events in `MapleThief`** (ADR 0104's option 1). Bound to the player's bindings and devices, and wrong as soon as real time reaches the guest.
2. **A state snapshot plus inputs.** Large (about 4.6 MB a state), and it proves nothing about how the state was reached.
3. **Inputs at the guest's boundary plus a clock tape. Chosen.** Record what reaches the guest, at the point it reaches it: every key pad press and release, touch, lifecycle message and port write into guest memory, and every value the guest's clock hook returns (run-length coded: under display pacing the guest's time only moves at refreshes, so a value repeats for every read in between). The game is a function of those and its starting files. A replay feeds the same values back through a clock that plays the tape, on the fixed-step loop, as fast as the host can go. It does not matter how the original was paced; display pacing only makes the tape small.

**What a replay is checked against.** ADR 0104's frame and audio hashes depend on the player's presentation (`hd_text`, a texture pack, `gapless_music`), which a verifier may not have. A **digest of guest memory** (the writable image pages and the heap, as a save state reads them) plus ADR 0104's state line is the same whatever the presentation, and names the first point where two runs part.

## Decision

**Run events** (runtime, observed functions). New event kinds: `CHAPTER_END` (the chapter that ends), `JOB_ADVANCE` (branch and tier), `BOSS_KILLED` (the mob id; bosses only, other deaths are not reported), `MAP_CHANGE` (area, map, portal). The observer for job advancements sits on whichever of `0x997E1` and `0x655AD` is reached only by `setClassUp`; the child that builds it checks the callers.

**The run and its splits** (frontend, no guest writes).

- A run starts at `MT_GAME_EVENT_NEW_CHARACTER` and ends at its category's last split. A category is our own data file: a list of splits, each an event kind with its value (chapter 1 ends, boss 66 killed, map 4.1 reached). The port ships at least *Any%* (new character to mob 69's death) with the chapter ends and boss kills as splits.
- The timer shows RTA and the current split on an overlay that draws over the picture, like the frame-time overlay (ADR 0071), on every platform. Each finished run is written to `<data dir>/runs/` with both times per split, the settings and the run variables. A personal best per category is kept beside it and compared live.
- **LiveSplit**: a desktop-only, off-by-default link to LiveSplit's own TCP server on the player's machine sends start, split, reset and the game time. No auto-splitter script.

**A clean run.** Every Assists row holds its neutral value (every preset's column) from the run's start to its end. The frontend checks it every turn: one change, even back again, marks the run *assisted* for good, the overlay says so and the run log records which Assist. Practice tools (save states, segment practice) are Assists already, so a practice run is never clean. `fix_original_bugs` and `skip_intros` are **run variables**: allowed, recorded and shown, and a category names the values it allows (the shipped categories allow both off only, the original game's rules; a community can add its own). Every other setting is presentation or comfort and free.

**The replay file** (`.mtrun`, our own data, deflated as ADR 0105's states are):

- **Bound to** the armv7 slice's SHA-256, the port's version (a port update may change timing, so replays are verified with the version that made them), the replay format version, the full settings at the start and every change during the run, and the game's own files the guest reads at boot (its three save slots and its options; a few kilobytes, the player's own data, never game content).
- **Holds** the clock tape, the guest-boundary inputs at their tape position, the splits with both times, and a checkpoint (guest memory digest and state line) at each split and every 1,000 ticks.
- **Recorded** by an `record_runs = off | on` setting (Other, off by default) from the boot of the session in which the run starts. One session per replay: a run that quits and reloads through the game's own saves is not verifiable in this version.
- **Verified** by `MapleThief --verify-run FILE` (headless, no window, no audio device): it refuses a file of another binary or port version, replays it, compares every checkpoint and recomputes the split times from the tape. A replay that verifies proves that this input on this binary and this port gives this run with these times. It does not prove a human played it in real time: a hand-made file (a tool-assisted run) verifies too. The clean-run status is recomputed from the recorded settings, not trusted from the file.

**Input display**: an overlay of the frontend's action layer (the keys, buttons and directions the game is being given this turn), presentation only, off by default.

**Not now.** Splits on side quests or item pickups (one observed function each, when a community asks); leaderboards or uploads hosted by the project (the port stays offline); segment practice beyond save states per split.

## Consequences

- The kit is epic [#402](https://github.com/armaatus/maplethief-port/issues/402) in the Backlog: run events #403, the input display #404, the replay recorder #405, the timer and clean-run status #406, the LiveSplit link #407, the verifier #408 and practice states per split #409.
- The end of a full-game run, mob 69's death, has never been reached in the port. Its event can be tested on a synthetic mob and with a save moved to the boss's map; a whole run to it waits for #285's verification chain.
- Every port write into guest memory (the Assists, `game_speed`, the cash shop's counters, auto-loot's hook) must be recorded or refused by the recorder. The recorder's child inventories them as #202 did the host states, and a test fails by name on one it misses.
- How we'd know this was wrong: a replay that fails to verify on the build that recorded it (a guest input or clock read the tape misses: the census or the clock hook is incomplete); a guest digest that differs between two presentations of one run (a presentation setting reaches the guest, and becomes a run variable); or runners who ignore the overlay for LiveSplit's own timer everywhere (then the overlay can shrink to the split name).

## Addendum (#406): the timer, categories and run logs

Built in [`platform/src/run_timer.h`](../../platform/src/run_timer.h) (no SDL) with `run_timer = off | on` and `run_category = NAME` (Other, outside the presets, applied at once). The calls this child made (delegated by Joris, 2026-11-26):

- **Splits in order.** Only the next split's event counts; a later split's event before it does nothing. A category is a sequence, as in LiveSplit, so #407 can send `split` without a mapping.
- **Starts and resets.** A new character starts a run, and a new character during a run ends that run as reset first. Turning the timer off, another category and quitting also reset it. Loading a saved character does not: a runner may reload after a death.
- **The neutral value of an Assist** is its Original preset column (every column is the same for an Assist, `check_registry`). The check walks `settings_registry()` by group, so a new Assist is covered without a change to the timer.
- **Run variables a category does not name** may only take their Original value. A run outside its category's values is timed, logged and shown, never a personal best.
- **The personal best** is the fastest finished run that is clean and inside its category, by real time; it is stored as a run log (`runs/<category>.best`), and the overlay compares the last split reached with the best's.
- **The category file and the run log** are `key = value` text ([docs/formats/run-category.md](../formats/run-category.md)). The shipped categories live in `platform/categories/` and are embedded in the binary by CMake; a player's own go in `<data dir>/categories/` and replace a shipped one of the same name.
- **Any%'s splits** are the three chapter ends and the bosses the main quests need (64, 67, 69). Mob 66 is a side quest's boss and mob 68 has no fight in version 1.5, so splitting on them would leave a run waiting. (Wrong about 68: see the [#434 addendum](#addendum-434-any-splits-on-mob-68).)
- **The overlay** is the frame-time overlay's box (ADR 0071) in the safe area's top right corner, drawn after every read-back, so never in a shot or a golden.

The times are the host's monotonic clock (`SDL_GetTicksNS`) and the input layer's tick count (`mt_input_get_stats`). With #405's clock tape a replay can recompute both from the tape (#408); the timer takes its clock as a parameter for that.

## Addendum: the recorder, its census and the file's size (#405, 2026-11-26)

Accepted (delegated by Joris, 2026-11-26). #405 built the recorder and `--play-run`. The issue asked for this addendum on ADR 0147; it belongs here, with the replay it builds.

**Where the tape sits.** In the runtime (`maplethief/runtape.h`), not the frontend:

- *The clock tape* wraps whatever clocks the platform installed (the virtual, paced or launch clock as `mt_libc_config.clock`, and GCD's clock and sleep) and records every value the guest reads, run-length coded per stream. One more value reaches the guest from real time and is a third stream: `-[AVAudioPlayer isPlaying]`, since a player plays out on the audio device's thread.
- *The boundary* is the runtime's own entry points: `mt_input_press`, `mt_input_release`, `mt_input_release_all`, `mt_uikit_touch`, `mt_uikit_lifecycle_event`, `mt_runloop_set_fire_tolerance` and `mt_runtape_poke` (a port write into guest memory) record each call at its run-loop turn. While a tape plays they take only the tape's calls, which UIApplicationMain's loop delivers after the turn's run-loop work and before the turn hook. So everything above them in the frontend (the devices, bindings, the focus engine, menu repeat, the Assists' latches, `skip_intros`' taps, `--tap`) is recorded without the frontend knowing, and plays back without its devices.
- *The host's own reads of the time* (its waits, pacing, overlays, menu timing) go through `mt_runtape_host_time`, which is not taped: a replay runs on the fixed-step loop and makes none of them.

**The replay** (`MapleThief --play-run FILE`, headless with `SDL_VIDEO_DRIVER=dummy`) puts the file's game files in a scratch data folder, takes the file's settings (only `fullscreen` and `vsync` from the command line), installs no platform clock, applies each recorded setting change at its turn through `apply_live_setting`, never waits, and compares a checkpoint (guest memory digest and state line) every 1,000 game ticks and at the recording's last whole turn. It refuses a file from another binary or port version. Exit status 6 if a checkpoint differs or the replay ends early.

**The census** (`platform/src/run_census.cpp`, checked by `tests/test_run_census.py`, which scans `platform/src` and fails by file and call on a site it does not list). 54 sites in 21 files:

| Kind | Sites | What |
|---|---|---|
| recorded | 15 | every action press and release (actions, gamepads, focus, menu repeat, the Assists' latches, `skip_intros`), touches, lifecycle messages, the 1 ms grid's fire tolerance |
| pure | 35 | a function of guest state and the settings, which the file holds with every change: the hooks of `fix_original_bugs`, `screen_shake`, `flashes`, `auto_loot`, list keys, the list scroll, `keypad_layout`; `game_speed`'s table and interval; `ui_animations`; widescreen's view width and touch area; the key pad's and the minimap's host transforms; observers that pass through (haptics, crash recovery's save count, the backups' save hook); a save state's own file |
| refused | 4 | `autosave`'s guest save (the window closes or hides, which a replay has no window for), a save state's load, a save import or backup restore, a data folder switch: the recording ends with the last whole turn before it (`END_`: refused, and which) |

The Assists are not refused: their input goes through the boundary (turbo, auto-potion, auto-run) or they are pure (`game_speed`, `auto_loot`, fast-forward only changes the clock the tape records). The cash shop's counters are written by the runtime's own cash shop, inside the guest's calls, so they replay as the guest does. Whether an Assist makes a run *assisted* is #406's clean-run rule, not the recorder's.

**Evidence** (local, real assets, Debug, Apple M4, 2026-11-26). A scripted real-time session of 120 s: the title, Play, a new character, the intro and the first map (2.8, the field), driven by `--tap` and a virtual Xbox 360 pad (`--pad`, 121 button changes taken from `tests/regress/title_to_combat.replay`'s timing), recorded with `record_runs = on`, then `--play-run` three times on each recording:

| Pacing | Turns | Events | File | Checkpoints (ticks) | 3 replays |
|---|---|---|---|---|---|
| display (window, locked to 60 Hz) | 3,604 | 112 | 6,697 B | 1000, 2000, 2392 (map 2.8) | all equal |
| `pacing = wall` (window) | 84,261 | 124 | 474,357 B | 1000 (2.8, an NPC chat), 2000, 2382 (2.9) | all equal |
| the 1 ms grid (dummy display, ADR 0131) | 96,655 | 123 | 18,758 B | 1000, 2000, 2386 (map 2.8) | all equal |

Each replay ran the 120 s in about 41 s (display) and reproduced every digest and state line. The three recordings reached different places (each is its own real-time run), so pacing does not matter to the replay: each plays back exactly.

**Size per hour** (from those 120 s, the boot included): about **0.2 MB** locked to a display, **0.6 MB** on the 1 ms grid, **14 MB** with `pacing = wall`, where every read sees a new microsecond and the deltas do not repeat. The events are a few bytes each and do not matter at that scale. The default pacing is display, so a player's hour is under a megabyte.

**Consequences.**

- #408's `--verify-run` is `--play-run` with the split times recomputed; #406 writes the splits into `SPLT`; #409's practice states are save states, which the recorder refuses.
- A new way into the guest from the frontend fails `test_run_census.py` until it is classified. A `pure` site that turns out to read host state (a device, the host's clock) is a replay that diverges at its first checkpoint: it becomes recorded (through `mt_runtape_poke` or a new event kind) or refused.
- The file names actions by number, so it plays only on the version that recorded it, as this ADR already required.
- How we'd know this was wrong: a replay that differs on the build that recorded it (a census site that is not what it says, or a clock read that does not go through the hooks); or `pacing = wall` files that players keep for hours (then the clock stream needs a coarser coding, for example the delta from the paced plan).

## Addendum (#407): the LiveSplit link

Built in [`platform/src/livesplit.h`](../../platform/src/livesplit.h) with `livesplit = off | HOST:PORT` and `livesplit_game_time = on | off` (Other, outside the presets, applied at once, `--livesplit`, `--livesplit-game-time`). The rows need `kCapLiveSplit` (ADR 0142), which only the desktop has. The calls this child made (delegated by Joris, 2026-11-27):

- **LiveSplit's server, as of LiveSplit 1.8.37** (its latest release, 2026-02-15; `src/LiveSplit.Core/Server/CommandServer.cs` and its README). The TCP server listens on port 16834 by default and is started by the player (Control, Start TCP Server, or the server's startup setting). A command is one line ending in `\n`, with its argument after a space. `starttimer`, `split`, `reset`, `pausegametime` and `setgametime TIME` have no reply. TIME is `[-][[[d.]hh:]mm:]ss[.fffffff]`, and fields need not be padded. `LiveSplitState.SetGameTime` fixes the game time only while game time is paused; otherwise it runs on with real time. `TimerModel.Start` clears the game time, and `Reset` unpauses it.
- **What a run sends.** On a start, `starttimer`. With game time, it then sends `pausegametime` and `setgametime 0:00:00.000`, so LiveSplit's game time moves only when the port sets it. On each split, `setgametime` with the split's ticks, then `split`. While a run is in progress, `setgametime` on every tick that changes the count. On a reset, `reset`. A finished run is reset (`reset`) before the next start, because LiveSplit ignores `starttimer` after a run ends. The game time is ticks × 50 ms as `H:MM:SS.mmm`. The timer tells the link through a listener (`RunTimer::set_listener`: start, split, reset), so the link needs `run_timer = on`. A category's splits map one to one onto the LiveSplit layout's segments, as the #406 addendum planned.
- **A practice sends nothing** (#409, ADR 0189). A practice is never a run, so the timer gives the listener no start, split or game time for it. Loading a practice state during a run ends that run as reset, and a finished run is cleared first, so LiveSplit gets `reset` either way.
- **Game time is on by default.** It changes nothing for a runner who compares by real time, because LiveSplit records both. Turning it off leaves LiveSplit's game time to LiveSplit.
- **Addresses are typed, never looked up.** HOST is `localhost`, an IPv4 address or a bracketed IPv6 one. A name would need a DNS query, and a query can block. A second computer running LiveSplit on the network still works by its address.
- **No turn waits.** The socket is non-blocking. A connect carries on across turns and gives up after 3 s. A refused or lost connection is retried every 3 s and logged once per run of failures. Lines are queued while connecting, up to 64 KiB (a full queue drops the connection), and dropped while there is no connection: LiveSplit then misses them. The link reads LiveSplit's replies only to notice that it closed. POSIX sockets, and Winsock (`ws2_32`) on Windows. The Switch build has no socket code (no capability, no row).
- **Evidence.** `tests/cpp/livesplit_test.cpp` (CI) sets up a TCP server on 127.0.0.1 that receives the exact lines of a synthetic run (start, two splits, a reset, the game time) and of a finished run followed by the next start. A refused port and an address that never answers keep every turn under 50 ms, log one line and reconnect once a server is there. A headless game run against a local listener sent `starttimer`, `pausegametime`, a `setgametime` per tick and `reset` at the quit.

## Addendum (#408): the verifier, splits in the file and the presentation check

Accepted (delegated by Joris, 2026-11-27). Built in [`platform/src/run_verify.h`](../../platform/src/run_verify.h) and `MapleThief --verify-run FILE` (`scripts/verify-run`, `scripts/verify-run.ps1`). The calls this child made:

- **`--verify-run` is `--play-run` with no window and no audio device** (`SDL_VIDEO_DRIVER=dummy`, `--no-audio`). Before it plays, it refuses a file that does not read (a flipped byte fails the packed body's CRC-32), one of another binary or port version, and one with a missing checkpoint or checkpoints out of turn order (`run_file_problem`). A checkpoint is required every 1,000 game ticks up to the last checkpoint's tick, and at every split's turn. It prints a report on the process's own stdout (the guest's printing goes to stderr for the run) and exits 0 when the file verifies, 6 when it does not, refusals included.
- **Checkpoints are matched by turn.** A replay takes a checkpoint at every turn the file has one and compares the digest, the tick count and the state line. One where the replay's own triggers (a 1,000-tick mark, a split) ask for a checkpoint and the file has none is a difference too, so a cut or shortened file cannot verify. The recorder keeps at most one checkpoint a turn.
- **Splits go into the file.** The recorder takes each split from the timer's listener: its title, its turn, its real time in the timer's whole milliseconds and its ticks (`SPLT`). It also takes a checkpoint at that turn. `SPLT` also holds the run categories the session timed (id and `run_category_text`), and `--play-run` writes them into its scratch `categories/`, so a player's own category travels with the file. The report says whether each one is the shipped category or the file's own. The format stays 1: #405's file was never released.
- **One clock for the timer.** While a tape records or plays, the timer's real time is the guest's clock as the guest last read it (`mt_runtape_guest_clock_us`), which the tape holds, so the replay gives the recorded times to the microsecond. That clock is the paced clock, within half a tick and a refresh of the host's (pacing.h); without a tape the timer keeps the host's monotonic clock. The split times are recomputed and compared, never taken from the file. Clean or assisted is the replay's own timer's, from the recorded settings and every change.
- **A run from a save.** A category may name its start: `start = EVENT`, a split's event or `load_character SLOT` (`MT_GAME_EVENT_LOAD_CHARACTER`). The run then starts there, and only while no run is in progress, in place of a new character. A loaded save fires no map change, so `load_character` is the way to time a stretch from a save.

**Evidence** (local, real assets, Debug, Apple M4, 2026-11-27).

- *Chapter 1's last stretch, from a save.* A save built from `play.chapter_1_k`'s (`mtport.savedata describe`/`build`, kept in a scratch folder): the hero on the Pirate's platform of map 2.0 with the Pirate's Request done, and a category `start = load_character 0`, `split chapter_end 1 = Orbis`. A real-time session in a window (display pacing at 60 Hz, `record_runs = on`, a virtual Xbox 360 pad from `--pad`: Play, the slot, Start, up to the Pirate, A through the chat) reached Orbis. The live timer showed **0:23.8** and logged 23,817 ms and 466 ticks. `scripts/verify-run` on the file (7,416 bytes, 1,006 turns, 37 input events, 2 checkpoints) printed `0:23.817 466 ticks` recorded and replayed, `same`, `clean`, 2 of 2 checkpoints the same and `verdict: VERIFIED`, exit 0, in 17 s.
- *One input moved.* The same file with one action press (event 17, turn 552) moved 3 turns later: exit 6, `a checkpoint differs at turn 593` (the split's checkpoint, the first after the move; ticks 470 recorded, 439 replayed). The split also differs (0:22.417, 435 ticks).

**The presentation check.** The recording above was replayed with each presentation setting changed in the file's settings, comparing every checkpoint's digest:

| Changed | Guest digest |
|---|---|
| `hd_text = on` with `render_scale = 2` (at render scale 1 it does nothing) | the same |
| `render_scale = 2` | the same |
| `gapless_music = on` | the same |
| `texture_pack`: a 2× pack of the game's GL textures only (`mtport.texpack build --no-ui`, 614 textures) | the same |
| `texture_pack`: the same pack with its UIKit images (691 files) | **differs** at the first checkpoint, with the same tick and state line |
| `widescreen = auto` | **differs** at the first checkpoint |

`widescreen` writes the view's width into guest memory (the census's pure sites), so the game lays out a wider field. A pack's UIKit images are decoded into the guest's own image buffers, so their pixels are in the digest. Neither changed the run here (the same tick count and state line), but by this ADR's rule both are now **run variables**: `kRunVariables` is `fix_original_bugs`, `skip_intros`, `widescreen` and `texture_pack`. A pack is recorded as `off` or `on`, never by its folder, and a category's `allow texture_pack = off, on` takes those. The shipped categories name neither, so both must be off in them, like the original game. A run with a texture pack also verifies only where the verifier has the same pack in the same folder.

**Consequences.**

- A file verifies only on the build that recorded it, as before. A player's verifying build needs the same port version and their own copy of the game.
- The guest's printing on stdout no longer reaches a verifier's report. A script reads the report from stdout and the log from stderr.
- How we'd know this was wrong: a recording that does not verify on the build that made it (a clock read or input the tape misses), or a split time that differs between the live timer and the replay (a timer reading that does not go through the tape).

## Addendum (#404): the input display

Built in [`platform/src/input_display.h`](../../platform/src/input_display.h) with `input_display = off | on` (Video, outside the presets, applied at once, `--input-display`). The calls this child made (delegated by Joris, 2026-11-27):

- **What it shows.** The four directions as a d-pad, jump, attack, quick slots 1 to 4, and the menu keys OK, back, the popup menu and the map, each lit while held. The port's own actions (fast-forward, auto-run, the save states' keys) never reach the game and have no cell. The cash shop and the revives are left out: under `hide_online_ui` they may send nothing (#400), and the panel stays small.
- **Where a cell's state comes from.** The runtime's held actions (`mt_input_held`), which every device reaches through the action layer (ADR 0048): keys, pads, the Assists' latches and `skip_intros`' taps, after direction resolution (with `direction_priority = last` only the delivered direction lights). And the touch key pad's own key BOOLs (`TouchKeyPadView` `onLeftKey` … `onShort4Key`, ivars +104 to +113), which a finger sets without passing the action layer. The runtime reads them at each turn and after the turn's touches and gives them as `mt_input_pad_keys()`. It only reads; nothing is written, so the run census has no new site. The Map, Bag and Cash buttons on the touch screen are UIKit buttons that call the game directly, so a touch on them does not light a cell.
- **Where it goes.** The bottom left of the border left of the picture when that bar has room for it at the window text's scale (or down to half of it), else of the border under the picture, else over the picture's left edge between the Map button (y < 52) and the d-pad (y ≥ 176), where the HUD has nothing. With widescreen the picture is the wide one. It is hidden while a port overlay (the settings menu, the remap screen, the controls card) is open, since those take the input.
- **How it draws.** The port's Canvas (its 5x7 font and shapes, no game art) into a small texture, drawn after every read-back like the minimap and the EXP figure. It is never in a shot, a golden of the game, a `--capture` or the regression replay's digests, so it is not a run variable.

**Evidence.** `tests/cpp/input_display_test.cpp` (CI): the action layer driven by keys and a pad lights exactly the delivered cells; the pad keys light theirs; the placement in a side bar, a bar below and over the picture; goldens `tests/golden/data/input_display_idle.png` and `input_display_held.png` of the panel alone on a synthetic frame. Locally with real assets, `scripts/regress --settings input_display=on` gives the same digests as with it off.

## Addendum (#434): Any% splits on mob 68

Accepted (delegated by Joris, 2026-11-27). The #406 addendum left mob 68 out of Any% because it "has no fight in version 1.5". The port's own chapter 2 run disproves that: mob 68 (32,000 HP) is main quest 8's boss on 9.5, the map's own spawn with no map script and no boss scene ([docs/re/boss-fights.md](../re/boss-fights.md)), and `play.chapter_2_f_synthetic` kills it with the run event `boss_killed 68` ([docs/testing/chapter-2-quests.md](../testing/chapter-2-quests.md), main 8). Main 8 leads to main 9 and the trip to chapter 3, so every run fights it.

- **Any%'s splits** are now seven: the tutorial's end, mob 64, chapter 1's end, **mob 68 (`Chapter 2 boss`)**, chapter 2's end, mob 67 and mob 69. The new split sits between chapter 1's and chapter 2's ends, where the game fires it.
- **Mob 66 stays out.** It is side quest 37's boss on 4.17, which a run can skip, so a split on it could leave a run waiting; the #406 reason holds for 66 only.
- A personal best or a `.segments` file saved with the six-split Any% no longer lines up with the category; no release shipped one (#406 and #434 are in the same unreleased line).

**Evidence.** `tests/cpp/run_timer_test.cpp`, `run categories: both shipped categories parse` (CI): Any%'s split list, in order, with 68 fourth. Locally with real assets, `play.chapter_2_f_synthetic` times Any%'s splits from `Chapter 2 boss` to `Chapter 2`: the 68 split fires on `run event mob 68 killed` and the run finishes at chapter 2's end.
