# Run categories and run logs (our own formats)

The speedrun timer ([#406](https://github.com/armaatus/maplethief-port/issues/406), [ADR 0156](../adr/0156-speedrun-kit-splits-clean-runs-and-verifiable-replays.md), [`platform/src/run_timer.h`](../../platform/src/run_timer.h)) reads a **category file** that says what a run is, and writes a **run log** for each run it ends. Both are the port's own text formats, not game data. The player's page is [Speedrun timer](../play/speedrun.md).

| File | Where | Written by | Read by |
|---|---|---|---|
| `NAME.mtcat` | shipped: [`platform/categories/`](../../platform/categories/), embedded in the binary at build time; a player's own: `<data dir>/categories/` | the project, a player | `parse_run_category` |
| `NAME-YYYYMMDD-hhmmss.log` | `<data dir>/runs/` (`-2`, `-3`, ... when the name is taken) | the timer, one per finished or reset run | `parse_run_log` |
| `NAME.best` | `<data dir>/runs/` | the timer: the category's personal best, a run log's text | `load_run_best` |
| `NAME.segments` | `<data dir>/runs/` | the timer: the category's best segments ([#409](https://github.com/armaatus/maplethief-port/issues/409), [ADR 0189](../adr/0189-practice-states-per-split-and-segment-times.md)) | `parse_run_segments`, `load_run_segments` |

`NAME` is the category's name, the file name without `.mtcat`: 1 to 32 of `a-z`, `0-9` and `_`. It is the value of the `run_category` setting. A player's file of the same name as a shipped one replaces it.

## Shared rules

- UTF-8 text; a leading byte order mark is skipped; lines end in LF or CR LF.
- Blank lines and lines whose first non-blank character is `#` are comments.
- Every other line is `LEFT = RIGHT`, split at the first `=`. The left side is words separated by blanks (space, tab); blanks around the right side are dropped.
- A title (a category's or a split's) is 1 to 32 characters of printable ASCII (`0x20` to `0x7E`): the overlay draws it with SDL's debug font.

## The category file

| Line | Count | Meaning |
|---|---|---|
| `title = TEXT` | exactly one | the category's title, shown on the overlay (`Any%`) |
| `allow VARIABLE = VALUE[, VALUE...]` | at most one per variable | the values a run variable may take in this category; `VARIABLE` is `fix_original_bugs` or `skip_intros`, each value as `settings.ini` writes it (`off`, `on`; `true`, `1` and the like read as the same) |
| `split EVENT = TITLE` | 1 to 64 | a split, in the order a run reaches them |

A run variable without an `allow` line may only take its Original preset value (`off`).

`EVENT` is one of the run events of [`maplethief/events.h`](../../runtime/include/maplethief/events.h), with its values as whole numbers from 0 to 255:

| `EVENT` | Fires on | Example |
|---|---|---|
| `chapter_end N` | `MT_GAME_EVENT_CHAPTER_END` with `value` N, the chapter that ends (0 is the tutorial) | `split chapter_end 1 = Orbis` |
| `job_advance BRANCH TIER` | `MT_GAME_EVENT_JOB_ADVANCE` with `value` BRANCH and `more[0]` TIER | `split job_advance 2 2 = Bandit` |
| `boss_killed MOB` | `MT_GAME_EVENT_BOSS_KILLED` with `value` MOB (64, 66 to 69) | `split boss_killed 69 = Last boss` |
| `map_change AREA.MAP` | `MT_GAME_EVENT_MAP_CHANGE` with `value` AREA and `more[0]` MAP (any portal) | `split map_change 4.1 = Orbis` |

Refused, each with its line number (`line 4: ...`), and the file as a whole then left out: a line without `=`, nothing before `=`, an unknown line, a second `title`, a bad title, an `allow` of anything but a run variable or a value that variable cannot take, a second `allow` of one variable, an unknown event, the wrong count of values or a value outside 0 to 255, a 65th split. A file larger than 16 KiB, without a `title` or without a `split` is refused as a whole. Every bad line is reported, not only the first.

### The shipped categories

- **`any`**, *Any%*: a new character to the death of mob 69, whose closing scene is the first ending scene. Splits: the tutorial's end, mob 64's death, chapter 1's end, chapter 2's end, mob 67's death, mob 69's death. Mob 66 is a side quest's boss and mob 68 has no fight in version 1.5 ([docs/re/mechanics.md](../re/mechanics.md)), so neither is a split.
- **`chapter_1`**, *Chapter 1*: a new character to the trip to Orbis (chapter 1's `clearAll`). Splits: the tutorial's end, mob 64's death, chapter 1's end.

Both allow `fix_original_bugs` and `skip_intros` off only, the original game's rules.

## The run log

One line each, in this order (a reader takes them in any order but `time` lines, which count up from 1):

| Line | Meaning |
|---|---|
| `category = NAME` | the category's name |
| `title = TEXT` | its title |
| `started = YYYY-MM-DD hh:mm:ss` | the local time the run started |
| `result = finished \| reset` | the last split reached, or ended before it |
| `end = MS ms TICKS ticks` | the run's time: the last split's, or when it was reset |
| `assisted = none \| KEY, KEY...` | the Assists (every `SettingGroup::kAssists` row of the settings registry) that left their neutral value during the run, in the order seen |
| `variable KEY = VALUE[,VALUE...]` | each run variable and every value it had during the run, in order |
| `in_category = yes \| no` | every run variable's values were allowed by the category |
| `split EVENT = TITLE` | the category's splits, as in its file |
| `time N = MS ms TICKS ticks` | the time of split N (from 1), for the splits reached |
| `settings = KEY = VALUE; KEY = VALUE...` | every setting at the run's start |

Times are since the run's start: `MS` real time on the host's monotonic clock in whole milliseconds, `TICKS` the game's ticks (`mt_input_get_stats`). A finished run has a `time` for every split; a reset one for those it reached. A run is **clean** when `assisted = none`; it **counts** for a personal best when it is finished, clean and `in_category = yes`. `NAME.best` is the log of the fastest run that counts, by real time; a run whose splits differ from the best's replaces it.

Refused (`parse_run_log`): a line it does not know, a bad value, a `time` out of order, no `category`, `result` or `end`, more times than splits, a finished run without a time for every split.

## Best segments

`NAME.segments` holds the best segment of each split of category `NAME`: the shortest real time from the split before it (the run's start, for the first) to it, from runs and from practice after a practice state's load ([ADR 0189](../adr/0189-practice-states-per-split-and-segment-times.md)). The timer writes it whole (through a temporary file beside it) whenever a best improves.

| Line | Meaning |
|---|---|
| `category = NAME` | the category's name (once) |
| `split EVENT = TITLE` | the category's splits when the bests were timed, as in its file (1 to 64) |
| `segment N = MS ms TICKS ticks` | split N's best segment (N from 1, once each, in any order); a split with no best has no line |

`MS` and `TICKS` are as in a run log. The timer keeps the file's bests only while its `split` lines equal the category's splits; for other splits it starts with none (and the next best written replaces the file). The **sum of best** is every split's best segment added up, once each split has one.

Refused (`parse_run_segments`): a line it does not know, a bad value, a second `category`, a `segment` numbered 0, twice, or past the last split, no `category` or no `split`.

## Parser and tests

[`platform/src/run_timer.cpp`](../../platform/src/run_timer.cpp): `parse_run_category`, `run_category_text`, `load_run_categories`, `parse_run_log`, `run_log_text`, `write_run`, `load_run_best`, `parse_run_segments`, `run_segments_text`, `write_run_segments`, `load_run_segments`. [`tests/cpp/run_timer_test.cpp`](../../tests/cpp/run_timer_test.cpp) (CI, synthetic files): every refusal by its line, a byte order mark and CR LF, the round trips, a data folder's files beside the shipped ones, both shipped categories parse and match the repository's files, the `.segments` round trip and its refusals. `play.chapter_1_l` (local, real assets) writes the log of a run timed to Chapter 1's last split and reads it back.
