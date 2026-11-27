# Chapter 3's quests in the port

[#300](https://github.com/armaatus/maplethief-port/issues/300) checks that the game can be finished
in the port: chapter 3's main quests, its two bosses (mob 67 and mob 69), the 4th job
advancement, the two ending scenes, and a save that goes on after them. The bot of
[chapter-1-quests.md](chapter-1-quests.md) plays them on a virtual Xbox pad. Quests, maps, items,
skills and mobs are named by id; the [compendium](../compendium/README.md) has their names. Each
quest is ticked when a CTest part has completed it and checked its state in guest memory (the
quest manager: world `+0x58`; 0 not started, 1 active, 2 done).
[#301](https://github.com/armaatus/maplethief-port/issues/301) adds everything chapter 3 offers
beside its main line: the 33 side quests, every offer path of sides 1, 14 and 16, the climb of
6.1 and 6.2, and chapter 3's recipes and trades ([below](#side-quests-the-climb-crafts-and-trades-301)).

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
- **The ending's maps are run events** ([#424](https://github.com/armaatus/maplethief-port/issues/424)):
  the scenes load 2.11 and 2.0 through world `+0x98`/`+0x9C` and game mode 8, not
  `CMapleWorld::changeMap`. Since #424 the runtime also observes `CStateGame::LoadMap`
  `0x868D5` (ADR 0156's addendum), and part E hears `MAP_CHANGE` 2.11 and 2.0, each with
  portal −1.

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

## Side quests, the climb, crafts and trades (#301)

All 33 side quests of chapter 3 in the knowledge base reach state 2. The main parts take 0, 2's
start, 4, 15, 25 and 58 (above). The side parts below take 27 more, chained from part D's save
(9.0, main 7 active, the played level-72 Bandit with the 4th job). Sides 1, 16 and 42 are also the
Dark Lord's job advancements, which the chain's hero took in earlier chapters; the job runs of
[skills-and-jobs.md](skills-and-jobs.md) hand them in (below). Each part checks its quests in
guest memory and in the save it writes (`then_quests_are`), and the common checks hold on every
part (no trap, no unimplemented call, no error line, the game ticks and draws).

```sh
ctest --preset mac-debug -R 'chapter_3_(sides|climb|side_14|crafts)' --output-on-failure
# from part D's kept output:
ctest --preset mac-debug -R 'chapter_3_(sides|climb|side_14|crafts)' \
    -FA 'play_chapter_[23]_[a-f]|start_chapter_2.*|bot_world'
```

| Part | Test | Starts from | Covers | Game time | Falls |
| --- | --- | --- | --- | ---: | ---: |
| sides_a | `play.chapter_3_sides_a` | D's save (9.0, level 72) | sides 3, 1 and 16 (BrokerHan's path), 2, 5, 17 done; 6 and 18 given; item 127 farmed | 1,143 s | 0 |
| sides_b | `play.chapter_3_sides_b` | sides_a's | sides 7, 6, 8, 10 done; 9 and 11 given | 864 s | 0 |
| sides_c | `play.chapter_3_sides_c` | sides_b's | sides 12 (the smith, 0.0), 11, 9, 18, 13 done; 14 given | 614 s | 0 |
| climb | `play.chapter_3_climb` | sides_c's | the climb of 6.1 and 6.2, side 34 (item 85) | 391 s | 0 |
| side_14 | `play.chapter_3_side_14` | climb's | side 14 by the Bandit: dagger 32 | 15 s | 0 |
| side 14, Assassin | `play.chapter_3_side_14_assassin_synthetic` | `starts.chapter_3_side_14_assassin`: sides_c's save, an Assassin, side 14 not yet offered | side 14 offered to an Assassin, the climb again, claw 44 | 264 s | 0 |
| sides_10_a | `play.chapter_3_sides_10_a_synthetic` | `starts.chapter_3_sides_10`: side_14's save, level 99 | sides 20, 22, 23, 24 (area 10) | 2,378 s | 0 |
| sides_10_b | `play.chapter_3_sides_10_b_synthetic` | sides_10_a's | sides 26 to 30 (area 10) | 4,499 s | 0 |
| sides_10_c | `play.chapter_3_sides_10_c_synthetic` | sides_10_b's | side 31 (area 10); every side quest checked | 4,589 s | 17 |
| crafts | `play.chapter_3_crafts_synthetic` | `starts.chapter_3_crafts`: D's save with the materials | the 10 trades and 18 recipes | 222 s (wall) | 0 |

The synthetic starts (`tests/starts/raise/chapter_3_*.json`, laid over a chained save with
`savedata build --base`):

- `chapter_3_sides_10.json`: level 99, as part E, wearing chapter 3's best crafted set (hat 11,
  dagger 35, overall 23, earrings 71, shoes 59, each for level 65 and each made by the crafts
  part). Area 10's mobs are level 98 to 130 (sides 29 and 31 want 60 drops each of mobs 54 and
  55, level 125 and 130); the played level-72 hero fell 23 times there in an hour (part E above).
  In the first run the hero still wore the level-30 hat 4, overall 16 and shoes 53 of chapter 2:
  sides 26 to 30 were done with 17 falls in 2.4 game-hours, and on 10.3 mob 55 felled it within
  a minute each time, so side 31's 60 drops did not come in an hour. In the crafted set sides 26
  to 30 took 75 game-minutes with no fall; side 31 took 67 game-minutes and 17 falls on 10.3
  (mob 55 is 31 levels above the hero, and the bot only fights and drinks). Areas 6 to 8 and the climb are played at level 72 to
  75.
- `chapter_3_side_14_assassin.json`: over sides_c's save, branch 1 (an Assassin, the Bandit's
  skills and their quick slots cleared), side 14 back to not offered and its record removed.
  Side 14's reward is chosen by `getHeroClass` (`chapter3/ZenumistChief.out`) when the quest is
  offered, not when it is handed in: claw 44 for an Assassin, dagger 32 otherwise. The chain's
  hero is a Bandit; a first try that made it an Assassin after the offer still got dagger 32.
- `chapter_3_crafts.json`: 100,000 meso, the 10 trades' items (50 each), the recipes' materials and
  an empty equipment bag for the 18 results.

### Every side quest

Giver and completer are the NPC scripts and their maps; "list" is the row value of the NPC's quest
list (`regSelList`, read from the scripts).

| # | Giver → completer, list | Needs | Status | Evidence |
| --- | --- | --- | --- | --- |
| 0 | BrokerHan (6.0), 1 | 10 of item 8, 10,000 meso | done | part A (above) |
| 1 | BrokerHan, 2 (his path) | 15,000 meso | done | sides_a: `talk: BrokerHan: side 1 done (15,000 meso)` |
| 1 | the Dark Lord (0.0) (his path: the 1st job) | a Beginner of level 10 | done | `play.jobs_3_1_synthetic`: `ok: the quests the advancement hands in: side 1:2` |
| 2 | AlchadnoChief (8.0), 2 | side 4 done | done | sides_a: `talk: AlchadnoChief: side 2 done` |
| 3 | BrokerHan, 3 | side 2 active; 10,000 meso | done | sides_a: `side 3:2` |
| 4 | Humanoid3 (8.4) | side 2 active | done | part B (above) |
| 5 | ZenumistChief (7.0), 6 | 25 of item 83 (mob 58, 7.2) | done | sides_a: `farm: 30 of mob 56 and 25 of item 83 (mob 58) on 7.2`, `side 5:2` |
| 6 | ZenumistChief, 7 | item 121 (side 7's reward) | done | sides_b: `talk: ZenumistChief: side 6 done (item 121)` |
| 7 | Alchester (9.0), 2 | side 6 active; item 127 (mob 57 on 7.5, 10 %) | done | sides_a: `farm: item 127 from mob 57 on 7.5 (side 7)`; sides_b: `talk: Alchester: side 7 done (item 127), item 121` |
| 8 | ZenumistChief, 8 | 30 of item 80 (mob 59, 8.3) | done | sides_b: `side 8:2` |
| 9 | ZenumistChief, 9 | item 122 (side 11's reward) | done | sides_c: `talk: ZenumistChief: side 9 done (item 122)` |
| 10 | Humanoid3, 2 | side 9 active; 30 of item 79 (mob 60, 8.2) | done | sides_b: `talk: Humanoid3: side 10 done, item 124` |
| 11 | Humanoid3, 3 | item 125 (side 12's reward) | done | sides_c: `talk: Humanoid3: side 11 done (item 125), item 122` |
| 12 | the smith (0.0), 1 | side 11 active; item 124, 10,000 meso | done | sides_c: `talk: smith: side 12 done (item 124, 10,000 meso), item 125` |
| 13 | ZenumistChief, 10 | 30 of item 81 (mob 62, 8.4) | done | sides_c: `side 13:2` |
| 14 | ZenumistChief, 11 | item 85 (side 34's reward) | done | side_14: `ok: branch 2: equipment 32 0 -> 1, equipment 44 0, item 85 0`; the Assassin: `ok: branch 1: equipment 44 0 -> 1, equipment 32 0, item 85 0` |
| 15 | BrokerHan, 4 | 40 of mob 60 | done | part B (above) |
| 16 | BrokerHan, 5 (his path) | 30 of mob 56 (7.2), 10,000 meso | done | sides_a: `talk: BrokerHan: side 16 done (10,000 meso)` |
| 16 | the Dark Lord (his path: the 2nd job, lists 6 and 7) | a Rogue of level 30 | done | `play.jobs_3_2_assassin_synthetic`, `_bandit_synthetic`: `side 16:2` |
| 17 | AlchadnoChief, 3 | 30 of item 78 (mob 61, 8.1) | done | sides_a: `talk: AlchadnoChief: side 17 done` |
| 18 | AlchadnoChief, 4 | 40 of mob 62 (8.4) | done | sides_c: `talk: AlchadnoChief: side 18 done` |
| 20 | Skadru (9.0), 1 | 30 of item 73 (mob 43, 10.0) | done | sides_10_a: `side 20:2` |
| 22 | the SnowQueen (9.0), 2 | main 6 started; 30 of mob 43 | done | sides_10_a: `side 22:2` |
| 23 | the SnowQueen, 3 | 33 of mob 44 (10.1) | done | sides_10_a: `side 23:2` |
| 24 | the SnowQueen, 4 | 30 of item 75 (mob 33, 10.2), 30 of item 123 (mob 37, 10.1) | done | sides_10_a: `side 24:2` |
| 25 | ZenumistChief, 12 → the Dark Lord | the 4th job | done | part D (above) |
| 26 | the Miner (9.0), 1 → Alchester, 3 | side 24 done | done | sides_10_b: `side 26:2` |
| 27 | Alchester, 4 | 30 of item 74 (mob 29, 10.2) | done | sides_10_b: `side 27:2` |
| 28 | Alchester, 5 → Skadru, 2 | side 27 done | done | sides_10_b: `side 28:2` |
| 29 | Skadru, 3 | 60 of item 76 (mob 54, 10.2) | done | sides_10_b: `side 29:2` |
| 30 | Alchester, 6 | 60 of mob 54 | done | sides_10_b: `side 30:2` |
| 31 | Alchester, 7 | 60 of item 77 (mob 55, 10.3) | done | sides_10_c: `side 31:2` |
| 34 | remem2 (6.2), on a talk | the climb | done | climb: `talk: remem2 (6.2): the climb, side 34 done, item 85` |
| 42 | the Dark Lord (the 3rd job) | 2 advancements, level 60 | done | `play.jobs_3_4_assassin_synthetic`, `_bandit_synthetic` (first talk): `side 42:2` |
| 58 | AlchadnoChief, on a talk | item 118 | done | part B (above) |

sides_10_c ends with every side quest the chain takes in one check: `ok: chapter 3's side
quests but 42: side 0:2 side 1:2 … side 34:2 side 58:2` (32 quests; 42 is the 3rd job, which
the chain's hero took in chapter 2).

**The offer paths.** Side 1 is offered by BrokerHan (15,000 meso for 4,000 EXP) and by the Dark
Lord to a Beginner (the 1st job, claw 36 and 200 of item 11). Side 16 by BrokerHan (30 of mob 56
and 10,000 meso for 2 of item 90 and 4,000 EXP) and by the Dark Lord to a Rogue (the 2nd job:
list 6 an Assassin with claw 40, list 7 a Bandit with dagger 28; lists 4 and 5 only explain the
two jobs). Side 14 is one offer with two rewards by the hero's job (above). Each path is played
once: BrokerHan's in sides_a, the Dark Lord's in the job runs, side 14's in side_14 and its
Assassin run.

### The climb of 6.1 and 6.2

While side 14 is active, 6.0's portal to 6.1 opens (`link 3 6 0 1 6 1 q1.14==1` in the bot
world). 6.1 and 6.2 are a climb of ropes and ledges with portal611 at 6.1's top and remem2 at
6.2's; the game refuses to save on both ([re/autosave.md](../re/autosave.md)). The bot climbed
both in 237 game-s with no fall: `ok: run event 6.0's portal to 6.1 (side 14 active): heard 1
time(s)`, `ok: run event 6.1's top to 6.2: heard 1 time(s)`, `talk: remem2 (6.2): the climb,
side 34 done, item 85`, `ok: part climb's quests: side 14:1 side 34:2`. The way back down takes
portal620, which lands on 6.1 where the hero stood on 6.2 ([original bug 14](../re/original-bugs.md#14-commonportal620-warps-to-a-portal-that-does-not-exist)),
then portal610 to 6.0.

### Crafts and trades

`play.chapter_3_crafts_synthetic` plays each trade and recipe of chapter 3 once, checked in the
bag, the meso and the quest manager before and after (a trade is side 78, a craft side 77, each
at state 2 after).

| NPC | Lists | Takes | Gives | Evidence |
| --- | --- | --- | --- | --- |
| Ericsson (4.0) | 1 | 50 of item 54 | 3 of item 94 | `ok: Ericsson: trade 1, … (side 78:2) … before item 54:50 item 94:50 meso 100000; after item 54:0 item 94:53 meso 100000` |
| Ericsson | 2 | 50 of item 50 | 3 of item 100 | `ok: Ericsson: trade 2, …` |
| Ericsson | 3 | 50 of item 66 | 3 of item 95 | `ok: Ericsson: trade 3, …` |
| Ericsson | 4 | 50 of item 68 | 3 of item 101 | `ok: Ericsson: trade 4, …` |
| BrokerHan (6.0) | 6, 1 | 50 of item 79 | 3 of item 96 | `ok: BrokerHan: trade 1, …` |
| BrokerHan | 6, 2 | 50 of item 81 | 3 of item 102 | `ok: BrokerHan: trade 2, …` |
| BrokerHan | 6, 3 | 50 of item 74 | 3 of item 97 | `ok: BrokerHan: trade 3, …` |
| pig (1.0) | 1 | 50 of item 103 | 3 of item 98 | `ok: pig: trade 1, …` |
| pig | 2 | 50 of item 42 | 3 of item 93 | `ok: pig: trade 2, …` |
| pig | 3 | 50 of item 112 | 3 of item 99 | `ok: pig: trade 3, …` |

In chapter 3 `common/Ericsson.out` has no quest to list, so his trade list opens at once (in
chapter 2 it is his list 5). The smith (`chapter3/smith.out`) lists side 12 (list 1) only while
side 11 is active; otherwise the Craft's kinds open at once (1 hat, 2 overall, 3 dagger, 4 glove, 5
shoes, 6 earrings), then the row (1 to 3, the kind's recipes by result id). All 18 of chapter 3
passed against the knowledge base's recipe: equipment 7, 9, 11 (hats), 19, 21, 23 (overalls),
31, 33, 35 (daggers), 43, 45, 47 (gloves), 55, 57, 59 (shoes), 67, 69, 71 (earrings), e.g. `ok:
smith: hat 7: the compendium's recipe (1 of equipment 7 for 1800 meso, 5 of item 98, 6 of item
100, 6 of item 101): before item 98:43 item 100:63 item 101:53 equipment 7:0 meso 100000; after
…`, then `ok: crafted 18 recipes; the knowledge base has 18 in chapter 3`.

### A port bug found: no way off 7.5 for the bot

7.5's only portal (portal750, to 7.3) stands on its top platform (y 159). The way up is a rope
that hangs 63 px above the floor, 8 px beside a lower ledge, with nothing at its top: a player
jumps from the ledge onto it, climbs and hops off right. The bot's route planner
(`tests/cpp/world_map.h`) had no such move, and the first run of sides_a stayed on 7.5 after
farming item 127 (`FAIL: timed out after 300 s: talk: BrokerHan: side 16 done …; map 7.5 x 200 y
271`). The planner now has a rope jump that starts with a jump onto the rope from a platform
beside it; like a long hop it is kept only where nothing else leads to the far platform, so no
other map's moves change (the moves of all 90 maps were compared: only 3.8 and 4.6 had such a
rope, and both already had a way). `world_map_test`'s `a rope with no top is caught from a ledge
and hopped off` checks it on a synthetic grid; sides_a then left 7.5 with no fall. This was a bug
of the test bot, not of the port or the game.

### A second bot bug: a ban that left no way to 10.3

The bot bans a move that failed three times on a map for the rest of the task (`Nav::banned` in
`tests/cpp/quest_bot.h`). On 10.2 mobs knocked the hero off the one climb toward 10.3's portal
three times; with that climb banned there was no route, and the hero stood at the climb's foot
(x 779) until it fell, 26 times in a row, so side 31's first run timed out. Where the bans
alone leave no route the bot now lifts that map's bans and tries again; the rerun lifted them
twice and reached 10.3 every time. The same run also logged 12 failed writes of
`gamecenter.json` while the disk was nearly full (another job filled it); the rerun had none.

## Run coverage

Rerun of #295's report (`python3 -m mtport.runcoverage run --tests 'play\.chapter_3.*'`,
`mac-coverage`, the same M4) on this branch, 2026-11-27: the ten chapter-3 tests and the chapter-2
chain they need as fixtures, 20 runs, all passed. The runs were then reported by chapter:

| | Chapter 2's runs (10) | Chapter 3's runs (10) | Both | Chapter 2's rerun ([chapter-2-quests.md](chapter-2-quests.md#run-coverage)) |
|---|---:|---:|---:|---:|
| Game functions (of 1,980) | 1,197 | 1,204 | **1,240** | 1,183 |
| `engine` (of 1,353) | 975 | 990 | 1,017 | 964 |
| `app-objc` (of 620) | 218 | 210 | 219 | 215 |
| Scripts read (of 307) | 121 | 109 | **171** | 112 |
| Natives called (of 65) | 33 | 33 | 35 | 32 |
| Maps entered (of 90) | 33 | 35 | **50** | 32 |
| Game imports (of 225) | 134 | 134 | 134 | 134 |
| (class, selector) pairs | 622 | 614 | 623 | 619 |

Against chapter 2's runs, chapter 3's add 43 game functions, 50 scripts, 17 maps and 2 natives:

- **Functions:** the eight scenes of chapter 3 (`CAniManager::makeAniMagatiaHumanoid1` to `4`,
  `makeAniHomunIn`, `makeAniHomunEnd`, `makeAniBalogIn`, and the two ending scenes
  `makeAniEndingZacum` and `makeAniEndingHenesis`), the floating background of the ending's map
  (`CFloatingBG`, `CFloatingBGFlowerAlgorithm`, `CFloatingBGImage` and their helpers), the scene
  effects (`CEffects_Get`, `CEffects_AddSprite`, `CEffects_AddAt`), `CNpcs_AddSceneNpc`,
  `CMMap::SetObjFrame`, the fifth quest-list button (`-[NpcKeyPadView questButton4]`), and 14
  unnamed engine functions.
- **Scripts:** 18 of `script/chapter3/` (the NPCs of the quests, `HomunMap`, `BalogMap` and
  three portals) and 32 of `script/common/` (26 portals of areas 5 and 7 to 10 and six NPCs met on
  the way).
- **Maps:** 7.0, 7.1, 7.2, 7.4, 7.6, 8.0, 8.2, 8.4, 8.6, 9.6, 9.7, 10.0 to 10.3, 10.5 and 2.11.
- **Natives:** 29 `regRequireMeso` and 44 `getItem`.

The report is in the git-ignored `work/coverage/chapter-3/`. The total with chapter 1 whole was
not rerun.

#301's parts (the side quests, the climb, side 14's two paths, area 10's sides and the crafts)
are not in this report yet: the rerun needs a second build tree (`mac-coverage`) and the chain
from chapter 1 as fixtures, and the disk had 3 to 6 GB free while #301 was built. A rerun with
`--tests 'play\.chapter_3.*'` picks them up.
