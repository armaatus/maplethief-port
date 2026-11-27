# Chapter 3's quests in the port

[#300](https://github.com/armaatus/maplethief-port/issues/300) checks that the game can be finished
in the port: chapter 3's main quests, its two bosses (mob 67 and mob 69), the 4th job
advancement, the two ending scenes, and a save that goes on after them. The bot of
[chapter-1-quests.md](chapter-1-quests.md) plays them on a virtual Xbox pad. Quests, maps, items,
skills and mobs are named by id; the [compendium](../compendium/README.md) has their names. Each
quest is ticked when a CTest part has completed it and checked its state in guest memory (the
quest manager: world `+0x58`; 0 not started, 1 active, 2 done).

## Running it

The parts are CTests in one chain after chapter 2's (`play.chapter_2_f_synthetic`, whose save is
chapter 3 on 6.0, [chapter-2-quests.md](chapter-2-quests.md)), each from the save of the part
before it. They need the real assets and generated code and exit 77 (skipped) without them.

```sh
ctest --preset mac-debug -R 'chapter_[23]' --output-on-failure
# chapter 3 alone, from chapter 2's kept output:
ctest --preset mac-debug -R chapter_3 -FA 'play_chapter_2_.*|start_chapter_2.*|bot_world'
```

Part E is **synthetic**: `starts.chapter_3_e` lays `tests/starts/raise/chapter_3_e.json` over part
D's save (`savedata build --base`, [start-states.md](start-states.md#raised-starts-a-chained-save-with-a-few-fields-changed)):
the hero levelled from 72 to 99 by the game's level-up rules. Area 10's mobs are level 98 to 130
and mob 69 is level 150 with 80,000 HP. The played level-72 hero fell 23 times in an hour of game
time on the way through 10.0 to 10.3 and reached 10.5 once. Levelling to the 90s by play would
take game-days. `hero.dat` has 100 levels, so 99 is near the cap. From level 99 the bot kills mob
69 in about 35 game-s with no fall.

| Part | Test | Starts from | Covers | Game time | Falls |
| --- | --- | --- | --- | ---: | ---: |
| A | `play.chapter_3_a` | chapter 2's part F save (6.0, level 70) | main 40, 0, 1; side 0; main 2 given | 341 s | 0 |
| B | `play.chapter_3_b` | A's save | sides 58, 15, 2 (given) and 4 (the Magatia scenes 16 to 19); main 2; main 3 given | 349 s | 0 |
| C | `play.chapter_3_c` | B's save | mob 67 on 7.4 (main 3), main 4, main 5; main 6 given | 659 s | 0 |
| D | `play.chapter_3_d` | C's save | side 25 and the 4th job; main 6; main 7 given | 402 s | 0 |
| E | `play.chapter_3_e_synthetic` | `starts.chapter_3_e`: D's save, level 99 | mob 69 on 10.5 (main 7), both ending scenes watched, main 7 done and main 8 given, the post-game save | 545 s | 0 |
| E, skipped | `play.chapter_3_e_skipped_synthetic` | the same start | part E with `skip_intros` on; the ending's state compared with E's | 528 s | 0 |
| F | `play.chapter_3_postgame_quit`, `_relaunch`, `_reload` | E's save | three processes: load, save, play on, quit, relaunch, reload ([below](#after-the-ending)) | 43 + 39 + 10 s | 0 |

The ten tests took 640 s of wall time on an Apple M4 (`mac-debug`, 2026-11-27, every test passed,
in one `ctest` run after chapter 2's chain). Game time is the run's virtual clock; the same bot
plays the same on every run.

`MT_FROM_SAVE`, `MT_NAV_TRACE`, `MT_ACT_TRACE` and `MT_MAP_DUMP` work as in
[chapter-1-quests.md](chapter-1-quests.md#starting-from-a-later-point).

## The checklist

Giver and completer are the NPC scripts (`<name>.out`) and their maps; "list" is the row value
the bot chooses in the NPC's quest list (`regSelList`).

### Main quests

Chapter 3 has nine main quests, 0 to 8. Main 40 (chapter 2's last) is carried over by the trip
and handed in first.

| # | Giver → completer | Needs | Status | Evidence |
| --- | --- | --- | --- | --- |
| 40 | carried over by chapter 2's trip → ZenumistChief (7.0), list 0 | — | done | A: `talk: ZenumistChief (7.0): main 40 rewarded, main 0 given` |
| 0 | ZenumistChief, list 1 | item 117, side 0's reward | done | A: `ZenumistChief: main 0 done, main 1 given` |
| 1 | ZenumistChief, list 2 | 40 of mob 63 (7.1) | done | A: `farm: 40 of mob 63 on 7.1 (main 1)`; `ok: part A's quests: main 40:2 main 0:2 main 1:2 main 2:1 side 0:2` |
| 2 | ZenumistChief, list 3 | — (list 3 shows only once side 4 is done) | done | B: `ok: part B's quests: main 2:2 main 3:1 …` |
| 3 | ZenumistChief, list 4 | mob 67 on 7.4 | done | C: `run event mob 67 killed`, `ZenumistChief: main 3 done (item 126), main 4 given` |
| 4 | ZenumistChief → AlchadnoChief (8.0), list 0 | item 126 | done | C: `AlchadnoChief: main 4 done, main 5 given` |
| 5 | AlchadnoChief, list 1 | 30 of item 84 (mob 63, 7.1), 25 of item 82 (mob 56, 7.2) | done | C: `ok: part C's quests: main 3:2 main 4:2 main 5:2 main 6:1` |
| 6 | AlchadnoChief → Alchester (9.0), list 0 | — | done | D: `ok: part D's quests: main 6:2 main 7:1 side 25:2` |
| 7 | Alchester, list 1 | mob 69 on 10.5 | done | E: `run event mob 69 killed`, `Alchester (9.0): main 7 done, main 8 given` |
| 8 | Alchester | — | **open, as the scripts leave it** | E: `ok: main 0-7 done, main 8 open, chapter 3: main 0:2 … main 7:2 main 8:1 main 40:2`; the save: `main 8 1 in quest0` |

No script ends main 8 (no completer in the knowledge base): it stays in the quest list for good,
as the compendium's post-game page says. #300's acceptance criterion counts the nine quests from
1, so its "1 to 7 and 9 reach state 2; main 8 is open" is read here as main 0 to 7 done and main 8
open.

### Side quests on the main line

| # | Giver → completer | Needs | Status | Evidence |
| --- | --- | --- | --- | --- |
| 0 | BrokerHan (6.0), list 1 | 10 of item 8 (Etc60 sells it), 10,000 meso | done | A: `BrokerHan: side 0 done, item 117` |
| 58 | AlchadnoChief, on a talk | item 118 | done | B: `AlchadnoChief: side 58 done` |
| 15 | BrokerHan, list 4 (while 58 is active) | 40 of mob 60 (8.2) | done | B: `BrokerHan: side 15 done, item 118` |
| 2 | AlchadnoChief, list 2 | main 2 active, side 58 done | given | B: `side 2:1`. Main 3 needs only side 4; side 2 stays active |
| 4 | Humanoid3 (8.4) | side 2 active | done | B: the first talk plays scenes 16 to 19 (`runAnimation(16)`; 17 on 8.6, 18 on 7.6, 19 back on 8.4), the next starts and ends side 4: `side 4:2` |
| 25 | ZenumistChief, list 12 → the Darklord (0.0) | the 3rd job; main 0 and 2 done; item 119, which ZenumistChief gives | done | D: `item 119 (side 25's book) in the bag`, `side 25 handed in: side 25:2` |

### The 4th job advancement

Part D: with side 25 active, the Darklord (0.0) hands it in and gives the 4th advancement
(`getClassUpCount` 3 to 4, branch 2 kept). `run event the 4th job (branch 2, tier 4)` and `ok: the
job: branch 2 job_tier 4 in guest memory, branch 2 job_tier 4 in theif0`. The way from 6.0 to 0.0
is Pirate_Magatia (6.0, list 1) to 4.1 and Pirate_Orbis (4.1, list 1) to 2.0; back by the Pirate
(2.0) to 4.1 and over area 5 to 9.1 (5.3's portal is open in chapter 3).

### The bosses

How the fights start and end is in [re/boss-fights.md](../re/boss-fights.md#chapter-3s-two-bosses-played).

- **Mob 67 on 7.4** (part C): the way in from 7.1 opens while main 3 is active and its kill is
  missing; `HomunMap` plays the entrance scene. The played level-70 hero beat it in 36 game-s
  with no fall, with skill 21 cast and potions bought first.
- **Mob 69 on 10.5** (part E, synthetic): `portal1031` on 10.3 opens while main 7 is active;
  `BalogMap` plays the entrance scene (14). The level-99 hero beat it in about 35 game-s with no
  fall.

## The ending

Mob 69's death plays end scene 1 (`CAniManager::makeAniEndingZacum`, 202 steps). Its last command
(52) queues scene 2 and loads 2.11; scene 2 (`makeAniEndingHenesis`, 430 steps) ends with command
28, a warp to 2.0, and the field comes back in chapter 3. The steps are counted by the scene
runner's list (`SceneView.step`) while game mode 9 shows after the kill: on 10.5 for scene 1, on
2.11 for scene 2.

- **Watched** (`play.chapter_3_e_synthetic`): A through the boxes as a player taps them. `the
  ending watched: scene 1 to step 202, scene 2 to step 430, 55 test presses; the skip: 0 scenes`;
  `ok: both ending scenes ran to their last step`.
- **Skipped** (`play.chapter_3_e_skipped_synthetic`, `skip_intros` on, [ADR 0096](../adr/0096-scene-skip-runs-every-frame-faster.md)):
  no test input from the kill to the field of 2.0; the skip ran on three scenes (the entrance and
  both ending scenes) with 23 taps. `ok: skipped = watched in 7 of 7 fields (map, chapter,
  quests, both scenes' last steps)`. The boss fight itself differs between the two runs (the
  skip shortens the entrance scene, and the boss's attack cycle runs with the frames,
  [re/qol.md](../re/qol.md#cutscene-skip)), so the hero's HP, place and drops are not compared.

No trap, no unimplemented call and no error line in either run. Two findings:

- **Original bug 19** ([re/original-bugs.md](../re/original-bugs.md#19-a-boss-killed-while-the-hero-is-knocked-back-locks-its-end-scene)):
  in the watched run mob 69 died with the hero knocked back, and scene 1's box took no key. After
  5 s the run clears `CHero +0x119`, as #420's fix will, and prints so. The skipped run's kill
  did not hit it.
- **No run event for the ending's maps** ([#424](https://github.com/armaatus/maplethief-port/issues/424)):
  the scenes load 2.11 and 2.0 through world `+0x98`/`+0x9C` and game mode 8, not
  `CMapleWorld::changeMap`, so the `MAP_CHANGE` observer of ADR 0156 does not see them. Part E
  reads the maps instead and prints the event count. No shipped speedrun category splits on a
  map change; Any% ends on mob 69's kill, which is reported.

## After the ending

Three processes on one data directory (`build/test-output/<preset>/play_chapter_3_postgame`):

1. `play.chapter_3_postgame_quit` loads part E's save (chapter 3 on 9.0, main 8 open) and saves at
   once. The pair is the same as the file it loaded in every byte the game writes, except two
   counts of the other items: `moved: slot 14 item 4 95->100 slot 21 item 4 100->95`. The game's
   load re-adds each stack and tops up the first one of the id
   ([formats/save.md](../formats/save.md#etc-inventory-after-a-load)). Then free play: a kill on
   9.1, back to 9.0, Save. The hero's state and the files are recorded, and the process ends.
2. `play.chapter_3_postgame_relaunch`, a fresh process: before the boot the files are the bytes
   saved; the load gives the hero as recorded (119 facts: level, EXP, meso, HP, MP, job, skills,
   every inventory slot, the map and every quest); Save again (the same top-up, `99->100`,
   `95->94`), then on to 9.1, chapter 3, main 8 still open.
3. `play.chapter_3_postgame_reload`, a third process, loads the relaunch's save and saves it
   again: `ok: the load saved again: theif0 the same byte for byte (1124 bytes; 0 never-written
   heap bytes differ)`, and the same for quest0. A save made after a load reloads byte for byte.

The bytes the game never writes (heap leftovers, [formats/save.md](../formats/save.md)) are left
out of every compare and counted (`9 never-written heap bytes differ` in quest0 when the save
came from another process).
