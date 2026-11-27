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
```

1. The category and the run's time (hours, minutes, seconds and tenths).
2. The next split, and how many splits there are. At the end: *finished*.
3. At the last split you reached, how far ahead (`-`) or behind (`+`) your personal best you were there. *No personal best yet* until you finish one.
4. *clean*, or *assisted* and which Assists were on (see below).
5. The run variables and their values.

Before a run it says *a new character starts the run*.

## When a run starts and ends

- A run starts when you choose an empty slot and start a new character.
- Each split fires when its event happens, in the category's order: the end of a chapter, a job advancement, a boss's death or arriving on a map.
- The run ends at its category's last split. The time stays on screen until the next run.
- Starting another new character during a run ends that run as *reset* and starts a new one. Turning the timer off, choosing another category or quitting the game also resets it.
- Loading a saved character does not stop or reset a run.

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

A split is `chapter_end N` (0 is the tutorial), `job_advance BRANCH TIER`, `boss_killed MOB` or `map_change AREA.MAP`. A category can allow run variables with `allow skip_intros = off, on`. If the file has a mistake the game leaves it out and its log says which line; the [format](../formats/run-category.md) has every rule.

## A clean run

A run is **clean** when every Assist stays at its normal value from the start to the last split: the cash shop, game speed, fast-forward, turbo attack, auto-run, hold toggle, auto-potion, auto-loot, save states, and any Assist added later. The timer checks them all the time. If one is turned on during the run, even for a moment and then off again, the run is *assisted* for good, and the overlay and the run's log say which one. Save states are an Assist, so a practice run with them is never clean.

Everything else in the settings (the picture, sound, controls, the minimap, the language) is free: it doesn't change the game.

### Run variables

Two settings change what the game does without being a help, so they are allowed but recorded and shown:

- **Fix original bugs** (`fix_original_bugs`): the original game's crashes and dead ends fixed.
- **Skip scenes** (`skip_intros`): scenes run fast.

A category says which values it allows. Both shipped categories allow them **off** only, the original game's rules. A run with another value is shown as *outside the category's run variables*: it is still timed and logged, but it can't become a personal best of that category.

## Run logs and personal bests

Every finished or reset run is written to the `runs` folder of your data folder, one file per run, for example `runs/any-20261126-210304.log`. It holds both times at every split, whether the run was clean and which Assists were on, the run variables, and all your settings at the start.

Your **personal best** of a category is your fastest finished run that is clean and inside the category. It is kept as `runs/any.best` (the same text as a log). To start over, delete that file.

## Not yet

- A link to LiveSplit ([#407](https://github.com/armaatus/maplethief-port/issues/407)), replays that can be checked ([#405](https://github.com/armaatus/maplethief-port/issues/405), [#408](https://github.com/armaatus/maplethief-port/issues/408)) and practice from a split ([#409](https://github.com/armaatus/maplethief-port/issues/409)) are planned.
- No full Any% run has been timed in the port yet: the last boss has not been reached in the port's own tests.
