# Chapter 1's quests in the port

[#77](https://github.com/armaatus/maplethief-port/issues/77) checks that chapter 1's NPCs,
quests and scripts run in the port by playing them. The test uses the bot of
[first-map-run.md](first-map-run.md) on a virtual Xbox pad: the game's own input path, no
pokes into guest memory. The quest list below comes from the decompiled scripts
(`script/chapter1/*.out`, [formats/script-out.md](../formats/script-out.md)). Each quest is
ticked when a CTest part has completed it and checked its state in guest memory (the quest
manager: world `+0x58`, the main quests' states `+0x08 + 4n`, the side quests' `+0x19C + 4n`;
0 not started, 1 active, 2 done).

#77 covers parts A–C; [#160](https://github.com/armaatus/maplethief-port/issues/160) adds part D
(Perion, main 10–16, with a hero built for them). The rest of the chapter is split into follow-ups:
[#161](https://github.com/armaatus/maplethief-port/issues/161) (the remaining side quests) and
[#162](https://github.com/armaatus/maplethief-port/issues/162) (main 17–20 and the trip to
Orbis).

## Running it

The parts are CTests in one chain, `play.chapter_1_a` → `_b` → `_c` → `_d`. Each part goes on from the
save of the part before it. They need the real assets and generated code, like
`play.first_map`, and exit 77 (skipped) without them.

```sh
ctest --preset mac-debug -R play.chapter_1 --output-on-failure
```

A Release build of one architecture is much faster than Debug. The timings below come from
`-DCMAKE_BUILD_TYPE=Release`, arm64, on an M-series Mac (2026-10-30); the virtual clock makes
every run play the same, and a busier machine only stretches the real time (up to about 4×
while other builds ran).

| Part | What it plays | Game time | Real time |
| --- | --- | --- | --- |
| A | a new game, the tutorial, main 7, 8 and 21 (level 11), the save | 723 s | 61 s |
| B | from A's save: the side quests of Henesys and Kerning City (level 16), the save | 2,605 s | 216 s |
| C | from B's save: Perion, main 9 checked, side 22 given, the save | 75 s | 6 s |
| D | from C's save: SP, gear, level 20, Perion's main quests 10–16, sides 22 and 23, the save | 5,302 s | 903 s (Debug) |

Part D's times are from the Debug chain (`ctest --preset mac-debug -R play.chapter_1`,
2026-11-10), which plays about six times faster than real time; parts A, B and C took 99 s,
329 s and 14 s in the same run.

### Starting from a later point

Every part ends with the game's own save (START → Save, slot 0), written to
`<output dir>/data/Documents/theif0` and `quest0` ([formats/save.md](../formats/save.md)). A
part starts from a save when `MT_FROM_SAVE` names an earlier part's output directory: the
test copies `<dir>/data` into its own data directory, then loads slot 0 from the main menu
(`from_save`: Play, the slot, Start).

- **CTest:** the parts are fixtures (`FIXTURES_SETUP play_chapter_1_<part>`). Running a later
  part on its own runs the parts before it first, unless their output is kept and `-FA` is
  given:

  ```sh
  ctest --preset mac-debug -R play.chapter_1_c -FA '.*'   # C from B's last output
  ```

- **Directly**, from any part's output directory, or from a copy of a real device save placed
  under `<dir>/data/Documents/`:

  ```sh
  MT_FROM_SAVE=build/test-output/mac-debug/play_chapter_1_b \
  SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy MAPLETHIEF_LANGUAGE=en \
    build/mac-debug/tests/play_real_test chapter_1_c OUT_DIR
  ```

  The window test of [#146](https://github.com/armaatus/maplethief-port/issues/146) can start
  the same way: the output of part B is a level-16 Thief in Kerning City (0.0) with Henesys's
  and Kerning City's side quests done, the output of part C is the same hero in Perion
  (1.0) with Missing Master (main 10) and Missing Father (side 22) active, and the output of
  part D is a level-24 Thief in Perion with Double Step (15) on LB, the Triple-Tipped Zamadar,
  Red Pao, Red Loose Cap and Gidder Shoes, main 16
  (Roy's Advice 2) active and sides 22 and 23 done.

- `MT_NAV_TRACE=1` prints each move the bot takes between platforms and each failed move.
  `MT_ACT_TRACE=1` prints the hero's state on every tick of a move.

## The checklist

Giver and completer are the NPC scripts (`<name>.out`) and their maps (area.map, x). "List" is
the value of the row in the NPC's quest list (`regSelList`) that the bot chooses. Requirements
are from `regRequireItem(1, item, n)` / `regRequireMob(mob, n)` and the level checks.

### Main quests

| # | Quest | Giver → completer | Needs | Status | Evidence |
| --- | --- | --- | --- | --- | --- |
| 0–6 | the tutorial (MacArthur) | McAthur, 2.8 / 2.9 | the moves, a snail | done | A: `talk: MacArthur: …` (also `play.first_map`) |
| 7 | Elder's Advice | Stan (2.0, x 552) | 5 Red Snails (mob 2) | done | A: `talk: Stan: Elder's Advice (main 7) done, Find the Dark Lord (8) given` |
| 8 | Find the Dark Lord | Stan → Darklord (0.0, x 584), list 1 | — | done | A: `talk: Darklord: Find the Dark Lord done, …` |
| 21 | Job Advancement Evaluation | Darklord, list 2 | level 10, 10 Stumps (mob 4) | done | A: `talk: Darklord: the Job Advancement done (the Thief), Depart for Perion (9) given`; the Fruit Knife (24) reward is worn in B |
| 9 | Depart for Perion | Darklord → PerionMap (1.0's map script) | reach Perion | done | B goes through Perion on its way to 2.2 (Henesys's portal201 to 2.2 stays shut until main 9 is done), and the map script runs; C: `Perion's map script (on the first visit): Depart for Perion (9) done, Missing Master (10) given` |
| 10 | Missing Master | PerionMap → Manzie2 (1.0, x 376) | 30 Leaves (39, Dark Stumps) | done | D: `farm: 30 Leaves from the Dark Stumps on 1.2`, `talk: Manzie: Missing Master (10) done, Missing Master 2 (11) given` |
| 11 | Missing Master 2 | Manzie2 → Bruce (2.0, x 712), list 1 | — | done | D: `talk: Bruce: Missing Master 2 (11) done, Bruce's Research (12) given` |
| 12, 13 | Bruce's Research | Bruce → Bruce → Stan, list 3 | 30 Blue Mushroom Caps (103) | done | D: `farm: 30 Blue Mushroom Caps on 2.2`, `talk: Bruce: Bruce's Research (12) done, 13 given`, `talk: Stan: Bruce's Research (13) done, …` |
| 14 | Elder Stan's Assumption | Stan → Roy (Loy, 1.0, x 120), list 1 | — | done | D: `talk: Roy: Elder Stan's Assumption (14) done, Roy's Advice (15) given` |
| 15, 16 | Roy's Advice 1, 2 | Roy, list 1 / 2 | 20 Axe Stumps (mob 12), 20 Dark Stumps (mob 9) | 15 done, 16 given | D: `talk: Roy: Roy's Advice (15) done, Roy's Advice 2 (16) given`; `main quests 10-15 done (2), 16 given (1)` read in guest memory; 16 (the Ant Tunnel) is #162 |
| 17 | Sabitrama's Help | Sabi (3.0, x 632) | 30 Charms (45) | not yet | #162 |
| 18 | Defeat Malady | Sabi; portal371 (3.7) → 3.8 | the boss (mob 64) | not yet | #162 |
| 19, 20 | Master in Danger | Sabi2 → Stan, list 4 | — | not yet | #162 |

The chapter ends at the Pirate (2.0, x 744): `toOrbis` clears chapter 1's quests and starts
chapter 2 on 4.1. Every side quest has to be done before that.

### Side quests

| # | Quest | Giver → completer | Needs | Status | Evidence |
| --- | --- | --- | --- | --- | --- |
| 2 | Pia's Play Costume | Pia (2.0, x 184) | level 9, 5 Tree Branches (34) | done | B: `talk: Pia: Pia's Play Costume done` |
| 3 | Human Longings | Humanoid0 (2.0, x 376) | level 7, 8 Blue Snail Shells (32) | done | B: `talk: Humanoid: Human Longings done` |
| 4 | JM's Music 1 | Ming (2.0, x 168), list 2 → JM (0.0, x 184), list 1 | — | done | B: `talk: JM: JM's Music 1 done` |
| 5 | JM's Music 2 | JM, list 2 | 15 Squishy Liquids (37, Slimes) | done | B: `talk: JM: JM's Music 2 done` |
| 6 | Show Me What You Got | Darklord, list 4 | 30 Orange Mushroom Caps (35) | done | B: `talk: Dark Lord: Show Me What You Got done` (needed the pad fix below) |
| 30 | Father's Revenge | JM, list 3 | side 6 done; 30 Pigs (mob 5) | done | B: `talk: JM: Father's Revenge done` |
| 21 | Alex's Worry | Alex (0.0, x 440), list 1 | level 15; side 24 active to finish | given | B: `talk: Alex: Alex's Worry (side 21) given`; #161 |
| 22 | Missing Father | Iyan (Ayan, 1.0, x 488), list 1 | — (gives her necklace, 108) | done | C: `talk: Ayan: Missing Father (side 22) given`; D: `talk: Ayan: Missing Father (side 22) done`, `side quests 22 and 23 done` in guest memory |
| 23 | Missing Father | Bruce, list 2 | the necklace (108) | done | D: `talk: Bruce: the necklace, Missing Father (side 23) done` |
| 24 | Ayan's Mr. Right | Iyan, list 2 | side 22 done | not yet | #161 |
| 25 | Alex's Request | Alex, list 2 | a rose (104) from Remem | not yet | #161 |
| 15 | Forest of Patience | Remem (2.7, x 632) | the climb through 2.6 (portal212 while 25 is active) | not yet | #161 |
| 26 | Afraid of Green Mushrooms | Iyan, list 3 | main 10; 30 Green Mushrooms (mob 10) | not yet | #161 |
| 27 | Repairing Blackbull's House | pig (1.0, x 296), list 1 | side 26; 30 Axes (42) | not yet | #161 |
| 28 | A Present for Ayan | Alex, list 3 | main 10; 30 Pig's Ribbons (43) | not yet | #161 |
| 7 | The Security Guard's Mistrust | Guard (2.4, x 104) | main 16; the pass (106) | not yet | #161 |
| 8 | Dark Lord's Faith | Darklord, list 5 | 30 Dark Axe Stumps (mob 16); gives the pass | not yet | #161 |
| 29 | Crafting Armor | Guard | side 7 done; 30 Horned Mushroom Caps (44) | not yet | #161 |
| 9, 10, 14 | Chrishrama's Request 1–3 | Cri (3.0, x 488) | 30 Evil Eye Tails (46); 30 Curse Eye Tails (110); 50 Cold Eyes (mob 28) | not yet | #161 |
| 11 | Pirate's Request | Pirate (2.0, x 744) | 30 Horned (44) + 30 Green Mushroom Caps (40) | not yet | #161 |
| 16 | Job Advance (2nd job) | Darklord | level 30, main 21; list 6 Assassin / 7 Bandit | not yet | #161 |
| 31 | Father's Revenge 2 | JM, list 4 | 25 Octopus Legs (36) | not yet | #161 |
| 33 | Smith's Tool | smith (0.0, x 104) | side 7 started; 20 Axes (42) + 30 Firewood (41) | not yet | #161 |
| 34 | Winter Preparations | Stan, list 6 | main 12; 30 Firewood (41) | not yet | #161 |
| 77 | Craft (repeatable) | smith | crystals, ores and meso per item | not yet | #161 |
| 78 | Trade (repeatable) | pig, list 2 | 50 Blue Mushroom Caps / Axes / item 112 | not yet | #161 |

Done in the port: the tutorial, 10 of the 15 main quests (7, 8, 21, 9, 10–15) and 8 of the 26
side quests (2, 3, 4, 5, 6, 30, 22, 23); given: main 16, side 21.

## Bugs found

- **The NPC chat's quest rows were touch-only** (fixed in #77, `platform/src/focus.cpp`). The
  rows of an NPC's quest list (panel 5) are `NpcKeyPadView`'s `questButton1`–`6`, and their
  actions (`pressQuestNButton:` / `releaseQuestNButton:`, `0xB0551` …) only set the chat box's
  `questSelector` to N − 1. The d-pad moved nothing, so a pad or keyboard player could only
  take the first row: Darklord's "Show Me What You Got" (row 4) was out of reach. Now, while
  the list shows, up and down tap the row above or below, and OK stays the game's key
  (`focus: up and down choose the NPC chat's quest rows by touches (#77)`).
- **The test's own black-screen check** (not a port bug) failed in long runs. The
  once-a-minute "the screen is not black" check of the monitor (shared with `play.first_map`)
  failed whenever a minute's shot landed on a fade: a fall, or a portal's map change. A black
  shot is now taken again 3 s later, and only a screen that is still black fails.

- **The bot wore the wrong items** (not a port bug). Part D's first runs hit for 1 damage: the
  hero punched with a claw (`RollDamage` `0x64DA1` with no star: 1). The bot's equip step
  counted right presses from the first cell, but the equip tab's cursor (`CDataMgr +0xDC`) stays
  where it was left, and it counts carried items without the empty slots between them. The bot
  now reads the cursor and moves it to the item's cell before A, A.
- **The bot's own moves** (not port bugs), found on 2.2 and 1.4 in part D: walking off a
  ladder's top started with a jump (the walk looked stuck for the whole climb), off the end of
  2.2's short platform 13; a running jump that let go of the d-pad at once fell short of a
  platform one step up; stopping before a jump at a platform's end slid the hero off it; and the
  route planner left out ladders hanging more than 44 px up, though the game grabs a ladder a
  quarter of the hero's height above its feet (`0x61474`), so 1.4's ladder to portal140 (48 px
  up) had no route.

## What the runs showed about the game

- **The chat box** (world `+0x5C`): mode `+0x08`, page `+0x10` of `+0x14`, the quest list's
  rows `+0x20` (each row's list value at `+0x08`), the selected row `+0x30`.
- **A quest's requirements** (`checkRequire` `0x7B151`): one record per requirement in the
  active quest's list `+0x24`, kind `+0x04` (0 item, 1 defeat, 2 meso); an item's kind `+0x08`
  (0 equip, 1 other), id `+0x0C` and count `+0x10`, a defeat's mob `+0x14`, count `+0x18` and
  progress `+0x1C`, meso `+0x20`.
- **Portals gated by quests.** Four portal scripts check a quest before they warp:
  portal201 (2.0, to 2.2) needs main 9 done, portal212 (2.1) side 25 active, portal240 (2.4)
  side 7 done, portal371 (3.7) main 18 active.
- **One key at a time.** The game reads one key per tick. After a jump, a held up no longer
  climbs a ladder until it is let go and pressed again. A jump clears about 50 px up.
- **Leveling dominates the time.** Part B gains about 150 EXP a game-minute on 0.4 (Slimes
  and Stumps). Each fall costs EXP and about 10 % of the meso. Without potions the level-15
  hero fell again and again to the Orange Mushrooms on 0.1 (level 25): part B took 4,133
  game-seconds and ended with about 150 meso. Since the bot restocks Red Potions from the
  area's shop when it runs out with 1,500 meso or more, part B takes 2,605. The way to the
  Pigs on 2.2 goes through area 1 (portal201 is shut), where the Axe Stumps of 1.3 (level 41)
  still kill it many times (57 falls). Against the Dark Stumps on 1.2 (level 29, 260 HP) the
  level-15 hero fell 271 times in 3,600 game-seconds and did not collect the 30 Leaves. The rest
  of the chapter needs skills (the bot spends no SP), gear and levels first (#160).
- **A hero built for Perion** (part D). With Double Step on LB (used while MP is at least 40),
  a Red Woolen Hat, Gidder Shoes (the Red Cloth Vest is left: 331 meso) and Red and Blue Potions
  restocked at the area's shop, the level-16 hero levelled to 20 on 0.1 (Stumps and Orange
  Mushrooms) in 1,286 game-s. With the Triple-Tipped Zamadar (563 game-s for the 30 Leaves), then
  the Red Pao and Red Loose Cap, it took the 30 Blue Mushroom Caps on 2.2 (508 game-s), Roy's 20
  Axe Stumps on 1.3 (506) and 20 Dark Stumps (132). Part D had no fall in 5,302 game-s and 906
  kills. Meso is the limit: the level-20 armour costs more than the Leaves bring, so part D
  farms 7,800 meso on 1.2 first (1,956 game-s, the longest stage), and the Enamel Boots are left
  when it falls short.
