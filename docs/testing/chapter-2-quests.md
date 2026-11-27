# Chapter 2's quests in the port

[#298](https://github.com/armaatus/maplethief-port/issues/298) checks that chapter 2's main
quests, its two bosses (mob 66 and mob 68) and the 3rd job advancement run in the port, by
playing them with the bot of [chapter-1-quests.md](chapter-1-quests.md) on a virtual Xbox pad.
Quests, maps, items, skills and mobs are named by id here; the
[compendium](../compendium/README.md) has their names. Each quest is ticked when a CTest part
has completed it and checked its state in guest memory (the quest manager: world `+0x58`; 0 not
started, 1 active, 2 done).

## Running it

The parts are CTests in one chain, `play.chapter_2_a` → `_b` → … → `_f`, each from the save of
the part before it. Part A starts from `starts.chapter_2`, which builds
`tests/starts/chapter_2.json` (equal to chapter 1's part L save,
[start-states.md](start-states.md)). They need the real assets and generated code and exit 77
(skipped) without them.

```sh
ctest --preset mac-debug -R 'chapter_2' --output-on-failure
```

Part B is **synthetic**: `starts.chapter_2_b` lays `tests/starts/raise/chapter_2_b.json` over
part A's save (`savedata build --base`, [start-states.md](start-states.md#raised-starts-a-chained-save-with-a-few-fields-changed)):
the hero levelled from 34 to 60 by the game's level-up rules, with item 4 and item 7 on quick
slots 1 and 2. Levels 34 to 60 take 385,050 EXP; mob 40 on 4.2 (level 56) gives about 90 and
the level-34 hero killed about four a minute and fell every few minutes (its HP potion, item 3,
heals 50 HP and area 4's shops sell none), so by play they would take game-days. Parts C–E go on
from B's save as played.

Part F is **synthetic** too: `starts.chapter_2_f` raises part E's hero from level 62 to 70
(`tests/starts/raise/chapter_2_f.json`). Mob 68 is level 121 with 32,000 HP and hits for about
470 a second. The played level-62 hero, with Equip90's dagger 34, skill 21 cast and 300 of item 4,
fell 46 times in an hour of game time; the best try took about 6,400 HP, and the boss is whole
again each time the map loads. From level 70 the same bot kills it in about 320 game-s; from
level 80 in 12. A part that starts from a raised save is named `…_synthetic`, so a failure there
is read first as a possible artefact of the raise.

| Part | Test | Starts from | Covers | Game time | Falls |
| --- | --- | --- | --- | ---: | ---: |
| A | `play.chapter_2_a` | `starts.chapter_2` (4.1, level 34) | main 40, 0, 1; main 2 given | 334 s | 2 |
| B | `play.chapter_2_b_synthetic` | `starts.chapter_2_b`: A's save, level 60 | main 2; the 3rd job | 1,027 s | 0 |
| C | `play.chapter_2_c` | B's save | sides 0, 1, 37 (mob 66); main 3, 4 | 822 s | 0 |
| D | `play.chapter_2_d` | C's save | main 5 (scene 20), side 2, main 6: to area 9 | 2,154 s | 6 |
| E | `play.chapter_2_e` | D's save | main 7, sides 35, 36, main 71, 72 | 2,691 s | 0 |
| F | `play.chapter_2_f_synthetic` | `starts.chapter_2_f`: E's save, level 70 | main 8 (mob 68), main 9, the trip to chapter 3 | 1,007 s | 3 |

The six parts took 8,036 game-s (2.2 game-hours). The whole chain, with `play.bot_world`, ran
in 2,444 s of wall time on an Apple M4 (`ctest --preset mac-debug -R chapter_2`, 2026-11-27,
10 of 10 passed).

Game time is the run's virtual clock (a game-second is 20 ticks); the same bot plays the same
on every run.

`MT_FROM_SAVE`, `MT_NAV_TRACE`, `MT_ACT_TRACE` and `MT_MAP_DUMP` work as in
[chapter-1-quests.md](chapter-1-quests.md#starting-from-a-later-point); `MT_MAP_DUMP` now also
lists each map's ladders and ropes. #299 and #300 go on from part F's save (chapter 3 on 6.0),
or from any part's output directory.

## The checklist

Giver and completer are the NPC scripts (`<name>.out`) and their maps; "list" is the row value
the bot chooses in the NPC's quest list.

### Main quests

| # | Giver → completer | Needs | Status | Evidence |
| --- | --- | --- | --- | --- |
| 40 | chapter 1's last, carried over by the trip → Heracle (4.0) | — | done | A: `talk: Heracle: main 40 rewarded, main 0 given` |
| 0 | Heracle | 20 of mob 40 (4.2) | done | A: `farm: 20 of mob 40 on 4.2 (main 0)`, `talk: Heracle: main 0 done, main 1 given` |
| 1 | Heracle → Fairy (4.0) | — | done | A: `talk: Fairy: main 1 done, main 2 given`; `ok: part A's main quests: main 40:2 main 0:2 main 1:2 main 2:1` |
| 2 | Fairy | 40 of item 58 (mob 38, 4.3) | done | B: `farm: 40 of item 58 from mob 38 on 4.3 (main 2)`, `ok: part B's main quests: main 2:2 main 3:1` |
| 3 | Fairy | item 105, the reward of side 37 | done | C: `ok: item 105 (side 37's reward) in the bag`, `talk: Fairy: main 3 done (item 105), main 4 given` |
| 4 | Fairy → Heracle | — | done | C: `talk: Heracle: main 4 done, main 5 given` |
| 5 | Heracle → PerionMap (1.0's map script, scene 20) | item 120 | done | D: `chapter 2's PerionMap (1.0, scene 20): main 5 done, main 6 given` |
| 6 | PerionMap → Alchester (9.0), list 1 | reach 9.0 (portal531 on 5.3 opens with side 2 done) | done | D: `talk: Alchester (9.0): main 6 done, main 7 given`; `ok: part D's quests: main 5:2 main 6:2 main 7:1 side 2:2` |
| 7 | Alchester, list 2 | 25 of mob 41 (9.2) | done | E: `farm: 25 of mob 41 on 9.2 (main 7)`, `talk: Alchester: main 7 done, main 71 given` |
| 71 | Alchester, list 3 | sides 35 and 36 done | done | E: `talk: Alchester: main 71 done, main 72 given` |
| 72 | Alchester, list 4 | 30 of item 66 (mob 45, 9.3) | done | E: `talk: Alchester: main 72 done, main 8 given`; `ok: part E's quests: …` |
| 8 | Alchester, list 5 | mob 68 on 9.5 (32,000 HP) | done | F (synthetic): `farm: mob 68 on 9.5 (main 8)`, `run event mob 68 killed`, `talk: Alchester: main 8 done, main 9 given` |
| 9 | Alchester → Pirate_Orbis (4.1), list 2 | — (no reward call: the trip clears it) | ends at the trip | F (synthetic): `ok: chapter 2 before the trip: main 8:2 main 9:1`, then the trip below |

### Side quests on the way

| # | Giver → completer | Needs | Status | Evidence |
| --- | --- | --- | --- | --- |
| 0 | Ericsson (4.0), list 1 → Wilson (4.2), list 0 | main 3 active | done | C: `talk: Wilson: side 0 done` |
| 1 | Wilson, list 1 | 50 of mob 51 (4.11) | done | C: `farm: 50 of mob 51 on 4.11 (side 1)`, `talk: Wilson: side 1 done` |
| 37 | Wilson, list 7 | mob 66 on 4.17 (23,000 HP) | done | C: `farm: mob 66 on 4.17 (side 37)`, `run event mob 66 killed`, `ok: part C's quests: main 3:2 main 4:2 main 5:1 side 0:2 side 1:2 side 37:2` |
| 2 | Barun (5.1), list 1 | 20 each of mobs 18, 19 and 20 (5.0–5.3) | done | D: `talk: Barun: side 2 done` |
| 35 | Skadru (9.0), list 4 | 3 of item 69 (mob 34, 9.1) | done | E: `talk: Skadru: side 35 done` |
| 36 | SnowQueen (9.0), list 3 | 30 of item 63 (mob 25, 9.1) | done | E: `talk: SnowQueen: side 36 done` |

Side 20 was also given on the way in part A: farming beside Elma on 4.0, the bot's up opened her
chat. It changes nothing on the main line.

### The bosses

- **Mob 66 on 4.17** (part C): portal4111 on 4.11 opens while side 37 is active and its kill is
  missing. `WolfMap`, 4.17's map script, plays the entrance scene first (panel 3, game mode 9).
  The level-60 hero beat it in 118 game-s with no fall.
- **Mob 68 on 9.5** (part F): chapter 2's portal942 on 9.4 opens while main 8 is active and its
  kill is missing. 9.5 has no map script (`YetiMap.out` is never named by a map object), so the
  fight starts with no scene and no message: the boss is one of the map's spawns, there from the
  first tick. Part F checks it (`ok: 9.5 after 3 s: mob 68 there, boss scene none`). Before it
  the bot buys dagger 34 at Equip90 and wears it, puts skill 21 (attack and accuracy up) on quick
  slot 4 and casts it every 20 s in the fight, and drinks to full HP on 9.4. The level-70 hero
  beat it in 322 game-s, falling 3 times (the death popup's OK returns it to 9.0; the bot walks
  back and the fight goes on). Its death plays end scene 8 (from the program, `CMob::Die`, not a
  script). How the fights start and end is written up in
  [re/boss-fights.md](../re/boss-fights.md).

### The 3rd job advancement

Part B: the Darklord (0.0) gives the 3rd job at level 60, branch 2 (the bot's chapter-1 build).
Before the job the bot spends its SP only up to skill 16 and keeps the rest for skill 21, which the
3rd job opens (`k_chapter_2_before_job`); with all of it spent on 17 and 18 first, skill 21 had
no level by part F.
Checked by the run event (`ok: run event the 3rd job (branch 2, tier 3): heard 1 time(s)`), in
guest memory and in the save file the game then writes (`ok: the job: branch 2 job_tier 3 in
guest memory, branch 2 job_tier 3 in theif0 (want 2, 3), level 60`).

### The change to chapter 3

Part F: Pirate_Orbis (4.1), list 2, with main 9 active, runs `clearAll` and the trip: the
chapter-end run event (`ok: run event chapter 2 ends`), the map change to 6.0, chapter 3 in the
quest manager (`chapter 3 on 6.0 in guest memory`) and in the save (`chapter 3 on 6.0 in the
save`: `quest0[0]` 3, `theif0` `0x41C`/`0x41D` 6, 0).

### The common checks

Every part ends with `ok: the run ended from the script, no trap`, `ok: the unimplemented
report: unimplemented report: none`, the GL check (no misuse) and `ok: no error lines in the
runtime's log`, then `all ok`.

## Bugs found

No port bug turned up: the scripts, both bosses, the scenes, the 3rd job, the portals' gates,
`clearAll` and the chapter change ran as the scripts say.

One bug of the game itself turned up: **a boss killed while the hero is knocked back locks its
end scene** ([re/original-bugs.md, 17](../re/original-bugs.md#17-a-boss-killed-while-the-hero-is-knocked-back-locks-its-end-scene)).
A level-80 try killed mob 68 with the hero in the air from a hit; the end scene's box then took
no key for 15 minutes. The game drops every key while the hero's knock-back byte (`CHero +0x119`)
is set, and nothing clears it during a scene. It is the device's behaviour; the Enhanced fix is
[#420](https://github.com/armaatus/maplethief-port/issues/420). In a boss fight the bot starts
no attack while knocked back (`FarmStyle::land_first`). That only lowers the odds: mob 68 hits
about every 1.5 s, so a blow already under way can still land the kill during a knock-back. It
did in the coverage run below (`play.chapter_2_f_synthetic` under `mac-coverage`: 12 hits in the
19 s fight, one on the kill's tick, then 15 minutes in game mode 9). So part F has a step after
the kill, *mob 68's end scene*: when A has not moved the scene on for 5 s with `+0x119` still set,
the run prints `original bug 17: …` and clears that byte, as #420's fix will. The scene, the talk
with Alchester and the chapter change after it are the game's own. The two `mac-debug` reruns of
part F after the change did not hit the bug (350 s and 365 s); a run with the byte forced at the
scene's start took the step and went on to chapter 3.

The bot needed new moves for chapter 2's maps (none is a port bug, each with a test):

- **Ropes beside platforms** (4.3, 5.1). Chapter 2's ropes hang beside a platform's end, not
  onto it. The route planner now has a *rope jump* (climb to the platform's height, then the
  direction and jump together hop off sideways) and a *rope grab* (a jump from the platform
  toward a rope that hangs too high to reach from the floor, then up alone, which catches it in
  the air). The pad sends one key a tick and a key switch takes two ticks, so the hop holds the
  diagonal jump for a quarter second. `world_map_test`: "a ledge beside a rope is reached by a
  rope jump", "a rope beside a ledge is caught in a jump from it".
- **Portal gates on `checkRequire`** (4.11, 9.4). The bot's world tables wrote `checkRequire` as
  an unknown term, which shut portal4111 and portal942, so there was no way to either boss. A
  requirement term (`r<kind>.<n>`) now reads the quest's requirements as the game does.
  `test_botworld.py`, `world_map_test` ("a requirement term opens until the quest's requirements
  are met").
- **Ice** (area 9). The hero slides on ice, a jump keeps the slide's speed and the air has no
  steering; the bot swung past ladders and NPCs for ever (9.1's ladder at x 192, Alchester on
  9.0). It now coasts, then taps the d-pad from rest, and starts a move only at rest. On ice a
  held up on the ground also kept the jump under a ladder from starting, so up waits for the air.
- **The HP potion** came from a fixed item (3); it is now whatever quick slot 1 holds.
- **A full bag** (part E). The bag's 48 other-item slots fill with chapter 1's and 2's drops; on
  9.1 item 69 then stayed on the ground. The bot now sells its materials (other items of type 5,
  outside a keep list) at Etc90 before farming there (`then_sell`): on the Sell tab A opens popup
  6 (how many, 1 at first), and left from 1 goes round to the whole stack.
- **5.3's steps going up** (part F, back to Pirate_Orbis). The tower's steps are 32 px wide and
  32 px apart; a running jump from a step's end carries the hero 42 px (7 px a tick), past the
  next step, so the way up failed. A jump onto a narrow platform now leaves from further back to
  come down on its middle. `world_map_test`: "a jump onto a narrow step leaves from far enough
  back to land on it".
- **Boss 68's fight**: the bot can buy and wear a weapon (`then_gear`), cast a buff skill on a
  timer in a farm (`FarmStyle::buff`) and drink to a given HP before a boss map (`then_heal`).
- **9.0's portal900 on ice**: after a one-tick tap the game sometimes kept walking with the d-pad
  let go; the bot now brakes with the other direction for a tick when the slide still speeds up.

## What the runs showed about the game

- **Levels.** Chapter 2's first field mobs are level 56 (mob 40); the chapter-1 hero reaches
  chapter 2 at level 34. The hero gains 2 SP a level from 30, and HP and MP rise by `hero.dat`'s
  gains.
- **Bosses are spawns.** Both bosses are their maps' own spawns; the map script (4.17) only
  adds the message and the entrance scene, and 9.5 has none.
- **Ice** (area 9): the hero speeds up 1 px a tick to 5 px and slides about v² px after the d-pad
  is let go; up on the ground under a ladder neither climbs nor starts the jump there.
- **Shops.** Area 4's and area 9's shops (Etc40, Etc90) sell item 4 and item 7; one buy is one
  popup round, so 300 of item 4 take about 200 game-s.

## Run coverage

Rerun of #295's report (`python3 -m mtport.runcoverage`, `mac-coverage`, the same M4) on this
branch, 2026-11-27, in two runs, then one report over both:

| | Run 1: the baseline's tests and `starts.chapter_2` (55) | Run 2: chapter 2 (10) | Both | Baseline ([run-coverage.md](run-coverage.md)) |
|---|---:|---:|---:|---:|
| Game functions (of 1,980) | 1,245 | 1,183 | **1,280** | 1,291 |
| `engine` (of 1,353) | 972 | 964 | 1,005 | 1,008 |
| `app-objc` (of 620) | 269 | 215 | 271 | 279 |
| Scripts read (of 307) | 65 | 112 | **149** | 113 |
| Natives called (of 65) | 30 | 32 | 34 | 36 |
| Maps entered (of 90) | 18 | 32 | **39** | 30 |
| Game imports (of 225) | 136 | 134 | 136 | 136 |
| (class, selector) pairs | 743 | 619 | 745 | 753 |

Run 1 is not the baseline: `play.chapter_1_d` timed out under coverage (its 7,800-meso farm on 1.2
ran past its 3,600 game-s: with this branch's bot changes the farm starts with 804 meso, not
dev's 5,350, and dev already took 3,114 game-s; the farm now has 5,400), and parts E to M then
started from stale saves and failed, so the
chapter-1 half of the game is short (1,245 functions, 65 scripts, 18 maps against the baseline's
1,291, 113 and 30). Run 2's `play.chapter_2_f_synthetic` hit original bug 17 after killing
mob 68 (above), so its Alchester talk and the chapter change are missing from it. Measured
against run 1, chapter 2's runs add 35 game functions, 84 scripts (23 of `script/chapter2/`,
3 of them portals; 45 portals and 16 others of `script/common/`), 21 maps and 4 natives (49
`startBoss`, 50 `setBoss`, 53 `getHeroClass`, 54 `getClassUpCount`). A full rerun with chapter 1 whole would put
the total near the baseline's 1,291 plus most of those 35 functions; that rerun did not fit in
this issue's time.
