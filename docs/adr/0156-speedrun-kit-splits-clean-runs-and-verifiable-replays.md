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
