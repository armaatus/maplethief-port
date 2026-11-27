# Chapter 2's quests in the port

[#298](https://github.com/armaatus/maplethief-port/issues/298) checks that chapter 2's main
quests, its two bosses (mob 66 and mob 68) and the 3rd job advancement run in the port, by
playing them with the bot of [chapter-1-quests.md](chapter-1-quests.md) on a virtual Xbox pad.
Quests, maps, items, skills and mobs are named by id here; the
[compendium](../compendium/README.md) has their names. Each quest is ticked when a CTest part
has completed it and checked its state in guest memory (the quest manager: world `+0x58`; 0 not
started, 1 active, 2 done).

[#299](https://github.com/armaatus/maplethief-port/issues/299) adds the rest of the chapter:
the side quests (36 in the knowledge base, side 16's three offer paths included), every recipe
and trade, the climb on 9.9 and 9.10, and the shops of 4.0, 6.0 and 9.0, bought from and sold
to with the pad alone ([below](#the-side-quests-crafts-and-shops-299)).

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
the hero levelled from 35 to 60 by the game's level-up rules, with item 4 and item 7 on quick
slots 1 and 2. Levels 34 to 60 take 385,050 EXP; mob 40 on 4.2 (level 56) gives about 90 and
a level-34 hero (an earlier chapter-1 chain's) killed about four a minute and fell every few minutes (its HP potion, item 3,
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
| A | `play.chapter_2_a` | `starts.chapter_2` (4.1, level 35) | main 40, 0, 1; main 2 given | 337 s | 0 |
| B | `play.chapter_2_b_synthetic` | `starts.chapter_2_b`: A's save, level 60 | main 2; the 3rd job | 1,048 s | 0 |
| C | `play.chapter_2_c` | B's save | sides 0, 1, 37 (mob 66); main 3, 4 | 1,274 s | 0 |
| D | `play.chapter_2_d` | C's save | main 5 (scene 20), side 2, main 6: to area 9 | 556 s | 0 |
| E | `play.chapter_2_e` | D's save | main 7, sides 35, 36, main 71, 72 | 2,580 s | 0 |
| F | `play.chapter_2_f_synthetic` | `starts.chapter_2_f`: E's save, level 70 | main 8 (mob 68), main 9, the trip to chapter 3 | 775 s | 0 |

The six parts took 6,570 game-s (1.8 game-hours) and 1,475 s of wall time on an Apple M4
(`mac-debug`, 2026-11-27, every part passed; A and B in one `ctest` run, C to F in the next).
The start is the chapter-1 chain's part L save as this branch's bot plays it (level 35, 18,705
meso); an earlier start (level 34, 9,710 meso) took 8,036 game-s, with 11 falls.

Game time is the run's virtual clock (a game-second is 20 ticks); the same bot plays the same
on every run.

`MT_FROM_SAVE`, `MT_NAV_TRACE`, `MT_ACT_TRACE` and `MT_MAP_DUMP` work as in
[chapter-1-quests.md](chapter-1-quests.md#starting-from-a-later-point); `MT_MAP_DUMP` now also
lists each map's ladders and ropes. #300 goes on from part F's save (chapter 3 on 6.0), or from
any part's output directory.

### The side quests, crafts and shops (#299)

The side-quest parts branch off part E's save (chapter 2 still open: main 8 active), each from
the save of the one before, by area; the shops part goes on from the last of them. Two parts
are synthetic and start from a built save with a few fields changed
([start-states.md](start-states.md#raised-starts-a-chained-save-with-a-few-fields-changed)), and
`play.chapter_2_shop_6` starts from part F's save (chapter 3 on 6.0).

```sh
ctest --preset mac-debug -R 'chapter_2_(sides|shop|crafts|side_16)' --output-on-failure
```

| Part | Test | Starts from | Covers | Game time | Falls |
| --- | --- | --- | --- | ---: | ---: |
| sides_a | `play.chapter_2_sides_a` | E's save (9.0, level 62) | sides 6, 34 done; 5, 31 given | 593 s | 0 |
| sides_b | `play.chapter_2_sides_b` | sides_a's | sides 25, 33, 20, 4, 19, 3, 23, 22 done; 9, 11, 18, 21 given | 2,576 s | 0 |
| sides_c | `play.chapter_2_sides_c` | sides_b's | sides 26 (Victoria Island's caps), 10, 13, 27, 9, 18 done; 7 given | 1,455 s | 0 |
| sides_d | `play.chapter_2_sides_d` | sides_c's | sides 11, 12, 28 (area 5) | 724 s | 0 |
| sides_e | `play.chapter_2_sides_e` | sides_d's | sides 31, 29, 30, 32 done; 7's items; 21's item 8 bought | 1,477 s | 0 |
| sides_f | `play.chapter_2_sides_f` | sides_e's | sides 5 (item 72 on 10.1), 21, 7 done | 818 s | 1 |
| shops | `play.chapter_2_shops` | sides_f's | Etc40 and Equip40 (4.0), Etc90 and Equip90 (9.0) | 187 s | 0 |
| climb | `play.chapter_2_climb_synthetic` | sides_f's, with the Snowshoes' materials (`starts.chapter_2_climb`) | the Snowshoes crafted and worn; side 38, the climb of 9.9 and 9.10, side 39; all 35 side quests but 16 checked | 459 s | 0 |
| shop_6 | `play.chapter_2_shop_6` | F's save (chapter 3, 6.0) | Etc60 and Equip60 (6.0) | 45 s | 0 |
| crafts | `play.chapter_2_crafts_synthetic` | `starts.chapter_2_crafts` | the 7 trades, the 18 recipes | 294 s | 0 |
| side 16 | `play.chapter_2_side_16_full_synthetic` | `starts.chapter_2_side_16_full` | side 16 with a full equipment bag | 12 s | 0 |

The synthetic starts: `chapter_2_climb.json` replaces the other-item bag of sides_f's save with
potions and the Snowshoes' materials (3 of item 93, 8 of 94, 2 of 99: trade results and a 5.5 %
drop the chain sells as it goes); `chapter_2_crafts.json` gives the hero 40,000 meso and the materials of
every recipe and trade (the recipes alone take 278 ores and 31,210 meso; the ores drop at 5 %
or less, or come 3 for 50 of a quest item from a trade), over `starts.chapter_2`; and
`chapter_2_side_16_full.json` fills the equipment bag (48 pieces) of the level-30 Rogue on 0.0
of [#302](https://github.com/armaatus/maplethief-port/issues/302)'s `starts.jobs_ch2_rogue_30`:
the chain's hero took the 2nd job in chapter 1, and side 16 is offered only to a hero with one
advancement.

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
  The level-60 hero beat it in 309 game-s with no fall (118 in an earlier run).
- **Mob 68 on 9.5** (part F): chapter 2's portal942 on 9.4 opens while main 8 is active and its
  kill is missing. 9.5 has no map script (`YetiMap.out` is never named by a map object), so the
  fight starts with no scene and no message: the boss is one of the map's spawns, there from the
  first tick. Part F checks it (`ok: 9.5 after 3 s: mob 68 there, boss scene none`). Before it
  the bot buys dagger 34 at Equip90 and wears it, puts skill 21 (attack and accuracy up) on quick
  slot 4 and casts it every 20 s in the fight, and drinks to full HP on 9.4. The level-70 hero
  beat it in 12 game-s with no fall in the last run; an earlier run took 322, falling 3 times
  (the death popup's OK returns it to 9.0; the bot walks back and the fight goes on). The last
  run killed it with the hero knocked back, and the run's *end scene* step cleared bug 19 (below). Its death plays end scene 8 (from the program, `CMob::Die`, not a
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
save`: `quest0[0]` 3, `theif0` `0x41C`/`0x41D` 6, 0). The same check as chapter 1's trip compares the hero before and
after it (`the trip kept the hero`, #307; not rerun on this branch).

### Side quests beside the main line (#299)

All 36 side quests of chapter 2 reach state 2. The side parts take 28 of them (sides_a to sides_f
and the climb, each part's quests checked in guest memory and the save, `then_quests_are`); the main
parts took 0, 1, 2, 35, 36 and 37 (above) and 42, the 3rd job (part B). The climb part ends with
`ok: chapter 2's 35 side quests but 16: side 0:2 side 1:2 … side 39:2 side 42:2`. Side 16 (the
2nd job) is the one the chain cannot take: its hero took the 2nd job in chapter 1, and
`chapter2/Darklord.out` offers side 16 only with one advancement. Its three ways are below.

| Part | Side quests | Needs (the part's farm or talk) |
| --- | --- | --- |
| sides_a | 6 (Skadru), 34 (the SnowQueen, once 6 is done) | 25 of item 64 from mob 34 (9.1); 30 of item 71 from mob 35 (9.3). Also given: 31 (Jade: item 114), 5 (the SnowQueen) |
| sides_b | 25 (Barun) | 30 of item 63 from mob 25 (5.3) |
| sides_b | 33 (Wilson) | Jade's letter, item 114 (side 31) |
| sides_b | 20 (Elma), 4 (Ericsson) | 40 of item 55 (mob 40) and 25 of item 53 (mob 21) on 4.2 |
| sides_b | 19 (Ericsson), 3 (Heracle) | mobs 22 and 38 on 4.3, mobs 38 and 39 on 4.9 |
| sides_b | 23 (Humanoid2), 22 (Wilson) | 30 of item 57 from mob 26 (4.10); 23 gives item 111, which 22 takes |
| sides_c | 26 (Heracle) | 20 each of items 35, 40 and 44 from mobs 6, 10 and 13 on Victoria Island (0.1, 1.1, 3.1), there and back by Pirate_Orbis and the Pirate |
| sides_c | 10, 13 (Barun), 27, 9 (Heracle), 18 (Humanoid2) | mobs 24 and 30 on 4.12, mobs 36 and 30 on 4.14 |
| sides_d | 11, 12, 28 (Wilson) | 30 each of items 60, 61 and 62 from mobs 18, 19 (5.0) and 20 (5.2) |
| sides_e | 31, 32 (Jade), 29, 30 (Skadru) | 30 of item 115 from mob 52 (9.4), 25 of item 67 from mob 47 (9.6), 30 of item 68 from mob 48 (9.7). Also farmed: 30 of item 65 from mob 41 (9.2, side 7); bought: 10 of item 8 (side 21) |
| sides_f | 5 (the SnowQueen), 21 (Elma), 7 (Humanoid2) | item 72 from mob 44 (10.1, level 108); item 56 (4.9) and item 8 for 21 |
| climb | 38 (Jade), 39 (the Miner on 9.10) | 38 gives item 87; 39 takes it at the top of the climb |

### The climb of 9.9 and 9.10

While side 38 is active, 9.7's portal to 9.9 opens (`run event: map change 9 9 0`). 9.9 and 9.10
are towers of 40-px steps on ice, with the portal to 9.10 and the Miner at their tops. Most steps
are 40 px of air apart at one height (9.10's lowest ones 16 px lower than the step before), so the
only way up is a running jump from a step's very end, which comes down 1 to 9 px onto the next
step: the bot's planner had no such move (its sideways jumps stop at 40 px from 4 px in), and on
ice the landing slid the hero off the far step (9.10's platform 28: 156 falls in 1,800 game-s).
Snowshoes (shoes 52) turn the slide off (mechanics.md, Movement). The climb part crafts them
(`ok: smith: the Snowshoes (shoes 52): the compendium's recipe (1 of equipment 52 for 1300 meso,
3 of item 93, 8 of item 94, 2 of item 99)`), wears them (`equip: item 52, slot 4 wears 52`) and
climbs both maps in 140 game-s: `go: the climb: 9.7 to 9.9 to 9.10`, `run event: 9.7's portal to
9.9 (side 38 active)`, `run event: 9.9's top to 9.10`, `talk: Miner2 (9.10): side 39 done (item
87)`, `ok: part climb's quests: side 38:2 side 39:2`.

### Crafts and trades

Part crafts (`play.chapter_2_crafts_synthetic`) plays each once, checked in the bag, the meso and
the quest manager before and after: a trade is side 78, a craft side 77, each at state 2 after.

| NPC | Lists | Takes | Gives | Evidence |
| --- | --- | --- | --- | --- |
| Ericsson (4.0) | 5, 1 | 50 of item 54 | 3 of item 94 | `ok: Ericsson: trade 1, … (side 78:2) … before item 54:50 item 94:50 meso 40000; after item 54:0 item 94:53 meso 40000` |
| Ericsson | 5, 2 | 50 of item 50 | 3 of item 100 | `ok: Ericsson: trade 2, …` |
| Ericsson | 5, 3 | 50 of item 66 | 3 of item 95 | `ok: Ericsson: trade 3, …` |
| Ericsson | 5, 4 | 50 of item 68 | 3 of item 101 | `ok: Ericsson: trade 4, …` |
| pig (1.0) | 1 | 50 of item 103 | 3 of item 98 | `ok: pig: trade 1, …` |
| pig | 2 | 50 of item 42 | 3 of item 93 | `ok: pig: trade 2, …` |
| pig | 3 | 50 of item 112 | 3 of item 99 | `ok: pig: trade 3, …` |

The smith (0.0) asks for the kind (list 1 hat, 2 overall, 3 dagger, 4 glove, 5 shoes, 6
earrings), then the row (1 to 3, the kind's recipes by result id). Each craft is checked against
the knowledge base's recipe (`bot_world.txt`'s `recipe` lines): the result, the materials and the
meso fee, as `play.compendium_npcs` does for two. All 18 of chapter 2 passed: equipment 3, 5 and
7 (hats), 15, 17, 19 (overalls), 27, 29, 31 (daggers), 39, 41, 43 (gloves), 51, 52, 55 (shoes),
63, 65, 67 (earrings), e.g. `ok: smith: hat 3: the compendium's recipe (1 of equipment 3 for 660
meso, 6 of item 98, 7 of item 99): before item 98:33 item 99:103 equipment 3:0 meso 40000; after
item 98:27 item 99:96 equipment 3:1 meso 39340`, then `ok: crafted 18 recipes; the knowledge base
has 18 in chapter 2`.

Chapter 1's Craft and Trade ([#161](https://github.com/armaatus/maplethief-port/issues/161)) are
these: `chapter1/smith.out` and `chapter1/pig.out` list them only once main 40 (chapter 2's) is
active, and from chapter 2 on the game runs `chapter2/smith.out` and `common/pig.out`. The six
recipes only `chapter1/smith.out` has (equipment 1, 13, 25, 37, 49 and 61, each its kind's first
row there) are never offered: in chapter 1 the list stays shut, and from chapter 2 the other
script lists the kinds' later rows. `common/Ericsson.out`'s trades are chapter 3's.

### The shops

Each shop opened by up at its NPC, one item bought on the Buy tab and the same sold on the Sell
tab (right), with the d-pad and A alone; the meso and the bag checked after each step, the price
against the knowledge base's shop listing (`then_shop_use`).

| Town | Shop | Bought | Sold back | Evidence |
| --- | --- | --- | --- | --- |
| 4.0 | Etc40 | item 5 for 350 | for 80 | `ok: Etc40 (4.0): bought one item 5 for 350 meso: 0 -> 1, …`, `ok: Etc40 (4.0): sold one item 5 on the Sell tab: 1 -> 0, … (+80)` |
| 4.0 | Equip40 | equipment 53 for 3,500 | for 2,100 | `ok: Equip40 (4.0): …` |
| 9.0 | Etc90 | item 5 for 350 | for 80 | `ok: Etc90 (9.0): …` |
| 9.0 | Equip90 | equipment 54 for 6,770 | for 4,060 | `ok: Equip90 (9.0): …` |
| 6.0 | Etc60 | item 5 for 350 | for 80 | `ok: Etc60 (6.0): …` (`play.chapter_2_shop_6`, chapter 3) |
| 6.0 | Equip60 | equipment 6 for 7,000 | for 4,200 | `ok: Equip60 (6.0): …` |

6.0's shops open in chapter 3 only (the knowledge base lists no chapter-2 shop there), so their
part starts from part F's save.

### Side 16 in chapter 2

`chapter2/Darklord.out` offers the 2nd job (side 16) while the hero has one advancement and level
30, in three ways (the knowledge base's three variants). The chain's hero took it in chapter 1
(part G), so the parts start from a Rogue (#302's `starts.jobs_ch2_rogue_30`):

| Path | Test | Evidence |
| --- | --- | --- |
| list 6: the Assassin, equipment 40 | `play.jobs_2_2_assassin_synthetic` ([skills-and-jobs.md](skills-and-jobs.md)) | the job and equipment 40 in guest memory and the save |
| list 7: the Bandit, equipment 28 | `play.jobs_2_2_bandit_synthetic` | the job and equipment 28 |
| a full equipment bag: the bag-full notice (`invenFullNotify`) | `play.chapter_2_side_16_full_synthetic` (#299) | `ok: branch 0 tier 1, side 16:0, equipment 40: 0, job events 0` |

In this run the two jobs' parts were not rerun (#302 runs them); a run of each path of #299's
own, since removed as a duplicate, passed the same checks (`ok: side 16:2, equipment 40: 0 -> 1`,
`ok: side 16:2, equipment 28: 1 -> 2`).

### The common checks

Every part ends with `ok: the run ended from the script, no trap`, `ok: the unimplemented
report: unimplemented report: none`, the GL check (no misuse) and `ok: no error lines in the
runtime's log`, then `all ok`.

## Bugs found

**#299: the pad could not move the hero after a script's alert from the NPC chat** (fixed,
`runtime/src/input/input.cpp`). With a full equipment bag, the Darklord's side 16 (and the
smith's craft) ends with `invenFullNotify`, popup 8. Closing it runs `disableAlertKey`, which
sets the key pad's status (`EAGLView.statusOfTouchKeyPad`) to −1 and hides panel 7, and no script
or engine call enables a panel again. On the device the game pad stays live (`enableAlertKey`
left `touchKeyPadEnable` and `isJoyPad` set), so the hidden pad still walks the hero. The port
read −1 as a menu: its pad and keyboard sent menu keys, and the hero stood still for good
(`play.chapter_2_side_16_full_synthetic` timed out at the Darklord after 600 s, panel −1, with
the runtime of before the fix; it passes in 12 game-s with it). The
runtime now reports panel 0 when the status is −1, no panel is shown and the pad is live and
visible ([controls/keypad.md](../controls/keypad.md)); the test passes, and the 158 `input`,
`focus`, `menu_nav`, `menus` and `keypad` tests still do.

The rest of #299 found no port bug: 36 side quests' scripts, the trades, the 18 recipes, the
shops' Buy and Sell tabs and 9.9's climb ran as the scripts and the knowledge base say.

The parts of #298:

No port bug turned up: the scripts, both bosses, the scenes, the 3rd job, the portals' gates,
`clearAll` and the chapter change ran as the scripts say.

One bug of the game itself turned up: **a boss killed while the hero is knocked back locks its
end scene** ([re/original-bugs.md, 19](../re/original-bugs.md#19-a-boss-killed-while-the-hero-is-knocked-back-locks-its-end-scene)).
A level-80 try killed mob 68 with the hero in the air from a hit; the end scene's box then took
no key for 15 minutes. The game drops every key while the hero's knock-back byte (`CHero +0x119`)
is set, and nothing clears it during a scene. It is the device's behaviour; the Enhanced fix is
[#420](https://github.com/armaatus/maplethief-port/issues/420). In a boss fight the bot starts
no attack while knocked back (`FarmStyle::land_first`). That only lowers the odds: mob 68 hits
about every 1.5 s, so a blow already under way can still land the kill during a knock-back. It
did in the coverage run below (`play.chapter_2_f_synthetic` under `mac-coverage`: 12 hits in the
19 s fight, one on the kill's tick, then 15 minutes in game mode 9). So part F has a step after
the kill, *mob 68's end scene*: when A has not moved the scene on for 5 s with `+0x119` still set,
the run prints `original bug 19: …` and clears that byte, as #420's fix will. The scene, the talk
with Alchester and the chapter change after it are the game's own. Two `mac-debug` reruns of part F
after the change did not hit the bug; a run with the byte forced at the scene's start, and the
last chain's part F, which hit it in play, took the step and went on to chapter 3.

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
- **Long hops** (9.9, 9.10, #299). A running jump from a step's very end across 41 to 45 px of air
  (on the level, onto a step up to 16 px higher, or down to 32 px lower) is a *long hop*; the
  planner adds one only where nothing else leads from the step to the far one, so every other
  map's moves are as before (an `MT_MAP_DUMP` of every map: long hops on 9.9 (22) and 9.10 (36)
  only). The bot starts it within 4 px of the end, not 7 (from 7 px early the hero came down 1 px
  short). `world_map_test`: "a long hop joins steps 40 px apart only where nothing else does".
- **Snowshoes** (#299): a hero wearing shoes 52 does not slide, so the bot drives area 9 as
  ground, not ice.
- **Item 8** (side 21, sides_e): 0.1 % from mob 48; after 360 kills in 3,600 game-s none had come,
  so the bot buys it at Etc90.

## What the runs showed about the game

- **Levels.** Chapter 2's first field mobs are level 56 (mob 40); the chapter-1 hero reaches
  chapter 2 at level 34 or 35. The hero gains 2 SP a level from 30, and HP and MP rise by `hero.dat`'s
  gains.
- **Bosses are spawns.** Both bosses are their maps' own spawns; the map script (4.17) only
  adds the message and the entrance scene, and 9.5 has none.
- **Ice** (area 9): the hero speeds up 1 px a tick to 5 px and slides about v² px after the d-pad
  is let go; up on the ground under a ladder neither climbs nor starts the jump there.
- **Shops.** Area 4's and area 9's shops (Etc40, Etc90) sell item 4 and item 7; one buy is one
  popup round, so 300 of item 4 take about 200 game-s.
- **Item 8 for side 21** drops from mob 48 (9.7) one time in a thousand (and from mob 37, area
  10); in 360 kills none came. Etc40 and Etc90 sell it for 280, as a player would buy it.
- **Selling** brings a fraction of the price: 80 for item 5 (350), 2,100 for equipment 53
  (3,500), 4,060 for equipment 54 (6,770), 4,200 for equipment 6 (7,000).
- **The level-62 hero after #298** took every side quest of areas 4, 5 and 9 with no fall,
  levelling to 68 on the way.

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
1,291, 113 and 30). Run 2's `play.chapter_2_f_synthetic` hit original bug 19 after killing
mob 68 (above), so its Alchester talk and the chapter change are missing from it. Measured
against run 1, chapter 2's runs add 35 game functions, 84 scripts (23 of `script/chapter2/`,
3 of them portals; 45 portals and 16 others of `script/common/`), 21 maps and 4 natives (49
`startBoss`, 50 `setBoss`, 53 `getHeroClass`, 54 `getClassUpCount`). A full rerun with chapter 1 whole would put
the total near the baseline's 1,291 plus most of those 35 functions; that rerun did not fit in
this issue's time.

#299's parts (the side quests, the climb, the crafts and trades, the shops, side 16 with a full
bag) are not in it: the report needs `work/re/function-tags.json` from `scripts/re-analyze` and a
`mac-coverage` build, which that sweep's shared machine had neither the disk nor a free Ghidra
directory for. [#429](https://github.com/armaatus/maplethief-port/issues/429) reruns it with them.
