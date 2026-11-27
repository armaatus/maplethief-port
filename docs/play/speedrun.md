# Speedrun timer

The port has a timer for speedruns. It starts by itself when you start a new character, splits by itself at the points of your category, stops at its last split, and says whether the run was clean. It compares each split with your personal best and keeps a log of every run.

## Turning it on

In the settings menu's **Other** tab:

- **Speedrun timer** (`run_timer = on`, off by default) shows the timer in the top right corner of the picture.
- **Speedrun category** (`run_category`) picks what you run: **any** or **chapter_1**, or a category of your own (below).

From the command line, `--run-timer on --run-category chapter_1` does the same for one start. Neither setting is part of a preset.

## What it shows

```
Any%  12:34.5
next: Chapter 1 boss (2/6)
Tutorial -0:12.3 to the best
clean
fix_original_bugs off  skip_intros off
widescreen off  texture_pack off
```

1. The category and the run's time (hours, minutes, seconds and tenths).
2. The next split, and how many splits there are. At the end: *finished*.
3. At the last split you reached, how far ahead (`-`) or behind (`+`) your personal best you were there. *No personal best yet* until you finish one.
4. *clean*, or *assisted* and which Assists were on (see below).
5. The run variables and their values, two a line.

Before a run it says *a new character starts the run*.

## When a run starts and ends

- A run starts when you choose an empty slot and start a new character.
- Each split fires when its event happens, in the category's order: the end of a chapter, a job advancement, a boss's death or arriving on a map.
- The run ends at its category's last split. The time stays on screen until the next run.
- Starting another new character during a run ends that run as *reset* and starts a new one. Turning the timer off, choosing another category or quitting the game also resets it.
- Loading a saved character does not stop or reset a run.
- A category of your own can start somewhere else with a `start` line, for example `start = load_character 0` for a run from the save in the first slot, or `start = map_change 2.0` (below). Its run starts at that event, and only when no run is in progress.

The timer shows **real time**: the time on your computer's clock from the start to the last split, pauses and loading included. It also records **game ticks**, the number of steps the game ran (20 a second at the game's normal speed), which do not depend on how fast your machine is or how long it loads. A leaderboard can rank by either.

## The categories

| Category | From | To | Splits |
|---|---|---|---|
| **Any%** (`any`) | a new character | the last boss's death (the first ending scene) | the tutorial's end, the chapter 1 boss, chapter 1's end, chapter 2's end, the chapter 3 boss, the last boss |
| **Chapter 1** (`chapter_1`) | a new character | the trip to Orbis | the tutorial's end, the chapter 1 boss, the trip to Orbis |

### Your own categories

A category is a small text file. Put `NAME.mtcat` in the `categories` folder of your data folder (next to `settings.ini`; make the folder if it isn't there) and set `run_category = NAME`. The name is lower-case letters, digits and `_`. A file named like a shipped category replaces it. For example, `categories/bandit.mtcat`:

```
title = To the Bandit
split chapter_end 0 = Tutorial
split map_change 2.0 = Henesys
split job_advance 2 2 = Bandit
```

A split is `chapter_end N` (0 is the tutorial), `job_advance BRANCH TIER`, `boss_killed MOB` or `map_change AREA.MAP`. `start = EVENT` (one of the same events, or `load_character SLOT` for a saved character chosen in slot 0, 1 or 2) starts the run there in place of a new character. A category can allow run variables with `allow skip_intros = off, on`. If the file has a mistake the game leaves it out and its log says which line; the [format](../formats/run-category.md) has every rule.

## A clean run

A run is **clean** when every Assist stays at its normal value from the start to the last split: the cash shop, game speed, fast-forward, turbo attack, auto-run, hold toggle, auto-potion, auto-loot, save states, and any Assist added later. The timer checks them all the time. If one is turned on during the run, even for a moment and then off again, the run is *assisted* for good, and the overlay and the run's log say which one. Save states are an Assist, so a practice run with them is never clean ([practice from a split](#practice-from-a-split)).

Everything else in the settings (the picture, sound, controls, the minimap, the language, HD text, gapless music) is free: it doesn't change the game.

### Run variables

Four settings change what the game does or holds without being a help, so they are allowed but recorded and shown:

- **Fix original bugs** (`fix_original_bugs`): the original game's crashes and dead ends fixed.
- **Skip scenes** (`skip_intros`): scenes run fast.
- **Widescreen** (`widescreen`): the field is wider, so you see more of it.
- **Texture pack** (`texture_pack`): recorded as *on* or *off*, never by its folder. A pack's menu pictures are kept in the game's own memory, so a run with one is not the same run to the verifier (below), which also needs the same pack in the same folder.

A category says which values it allows (for a texture pack, `off` or `on`). Both shipped categories allow them **off** only, the original game's rules. A run with another value is shown as *outside the category's run variables*: it is still timed and logged, but it can't become a personal best of that category.

## Run logs and personal bests

Every finished or reset run is written to the `runs` folder of your data folder, one file per run, for example `runs/any-20261126-210304.log`. It holds both times at every split, whether the run was clean and which Assists were on, the run variables, and all your settings at the start.

Your **personal best** of a category is your fastest finished run that is clean and inside the category. It is kept as `runs/any.best` (the same text as a log). To start over, delete that file.

## Practice from a split

With **Save states** (`save_states = on`, an Assist) and the timer both on, the port keeps a save state after each split of your category, so you can practise one segment again and again without playing the run before it ([ADR 0189](../adr/0189-practice-states-per-split-and-segment-times.md)).

- **Keeping the states.** At the first moment after a split where a state can be taken (in the field, nothing open, every key let go: the same rule as F5), the port saves one, and a short notice says so. There is one state per category, character slot and split, in `states/<category>/slot<N>-split<M>.state` of your data folder. A newer one of the same split replaces the old one. The last split gets none: nothing follows it.
- **F7** (`practice_pick`) picks the next split that has a state for the character you play; the overlay shows *practice from: ...*. **F8** (`practice_load`) loads it (with none picked, it picks the first one). A pad has no button for them by default: bind them in the remap overlay or `bindings.ini`.
- F8 is refused for the same reasons as F9, with the same notice ("Not loaded: not in a menu"), and also when that split has no state yet.

After a load the timer is in **practice**. A run in progress ends as *reset*. The timer then times the segment from the load to the next split, then the next segment, and so on to the last split:

```
Any% practice  0:42.1
loaded: Chapter 1 boss (2/6)
next: Chapter 1's end (3/6)
best segment 0:39.8
last segment 0:41.0 (+0:01.2)
sum of best 1:58:03.4
practice, never a run
```

A practice is never a run: it writes no run log and makes no personal best. Starting a new character starts a real run again.

### Best segments and the sum of best

A **segment** is the time from one split to the next (from the start, for the first). The port keeps the **best segment** of each split, from your runs and your practice alike, in `runs/<category>.segments`. A segment counts only when no Assist other than save states was on during it, and the run variables stayed inside the category. The **sum of best** adds up every split's best segment once each has one. It shows what a run could be at best, not a run you made.

## Recording and verifying a run

The port can record a session so that someone else can check your run on their computer ([ADR 0156](../adr/0156-speedrun-kit-splits-clean-runs-and-verifiable-replays.md)).

**Recording.** Turn on **Record runs** (`record_runs = on`, in the **Other** tab; off by default). From the next start, each session writes a run file to the `runs` folder of your data folder when you quit, for example `runs/20261127-101500.mtrun`. A file holds what the game was given: your inputs, the game's clock and your settings, plus your three save slots and the game's options as they were at the start. It holds no game content. One session makes one file. Loading a save state, importing or restoring a save, or switching the data folder ends the recording at that point.

**Verifying.** Anyone with the same version of the port and their own copy of the game can check a run file:

```
MapleThief --verify-run 20261127-101500.mtrun
```

In a checkout, `scripts/verify-run FILE` (or `scripts\verify-run.ps1 FILE` on Windows) finds the build and does the same. The game opens no window and plays no sound. It replays the file as fast as the computer can, then prints a report:

```
MapleThief --verify-run 20261127-101500.mtrun
file: recorded by MapleThief 1.2.0, 9603 turns, 142 input events, 7 checkpoints
category: chapter_1 (shipped)
run 1: Chapter 1 (chapter_1), finished, 3 of 3 splits
  clean: every Assist held its neutral value
  run variables: fix_original_bugs = off; skip_intros = off; widescreen = off; texture_pack = off
splits:  name                    recorded                  replayed
  1. Tutorial                4:12.350 5021 ticks       4:12.350 5021 ticks  same
  ...
checkpoints: 7 of 7 compared, all the same
verdict: VERIFIED
```

It exits with 0 when the file verifies and 6 when it does not. The file is refused straight away if another version of the port recorded it, if it was recorded on another copy of the game binary, if it is damaged (one changed byte is enough), or if a checkpoint is missing.

**What a verified run means.** The game ran again from the same start with the same input and reached the same state at every checkpoint (every 1,000 game ticks and at every split). The split times are worked out again from the recorded clock, and *clean* or *assisted* from the recorded settings, so neither is taken from the file on trust. A verified file proves that **this input on this version gives this run with these times**. It does **not** prove that a person played it in real time: a file made by a tool (a tool-assisted run) verifies too. Whether a leaderboard accepts a run is up to the leaderboard.

While a session is recorded, the timer's real time comes from the game's own clock, which the file keeps. That clock follows your computer's clock, at most about one game tick plus one screen refresh apart, so the times you see are the times the verifier prints.

## LiveSplit

On a computer (macOS, Windows, Linux) the timer can drive [LiveSplit](https://livesplit.org/), so you keep your own layout and splits there. The port sends LiveSplit its start, each split and each reset; LiveSplit does the timing and the comparisons.

1. In LiveSplit, right-click, **Control**, **Start TCP Server** (each time you start LiveSplit, unless its server settings start it by themselves). The default port is 16834.
2. In the port, turn on **Speedrun timer** and set `livesplit = localhost` in `settings.ini` (or `localhost:PORT` for another port), or start it with `--run-timer on --livesplit localhost`. The **LiveSplit server** row in the **Other** tab shows the value.
3. Give LiveSplit as many splits as your category has, in the same order (Any% has six, Chapter 1 three).

The link is off by default. It connects to LiveSplit's own server only: `localhost`, or an IP address (another computer on your network, as `192.168.1.20:16834` or `[fe80::1]:16834`), never a name to look up. If LiveSplit isn't there, the game plays on as usual, the log says so once and the port tries again every few seconds without a word. A run that starts before LiveSplit connects is not in LiveSplit. A practice ([Practice from a split](#practice-from-a-split)) is never a run, so LiveSplit hears nothing of it; loading a practice state during a run resets LiveSplit's.

**Game time.** With **LiveSplit game time** on (`livesplit_game_time = on`, the default), LiveSplit's game time is the game's own ticks, 50 ms each: compare against *Game Time* in LiveSplit to rank by it. The port pauses LiveSplit's own game time and sets it on every tick. Turn it off to keep LiveSplit's game time as LiveSplit runs it.

No LiveSplit auto-splitter script is needed or offered: such scripts read the game's memory, which moves with every build of the port.

## Not yet

- A run that quits and goes on in a later session can't be verified: one session makes one run file.
- No full Any% run has been timed in the port yet: the last boss has not been reached in the port's own tests.
