# How the boss fights start

The game has five boss maps, one per boss: 3.8 (mob 64), 4.17 (66), 7.4 (67), 9.5 (68) and
10.5 (69). This page says what starts each fight, as the scripts and the program show it and as
the chapter runs played it ([#298](https://github.com/armaatus/maplethief-port/issues/298),
[testing/chapter-2-quests.md](../testing/chapter-2-quests.md)).

## The boss is a spawn of its map

Every boss is a mob spawn record of its map's `.obj` file (kind 60 and up,
[formats/obj.md](../formats/obj.md#kinds), spawned by `0x86289`), like a field monster: no script
creates it. It is on the map from the first tick after the map loads. Boss maps play the boss music (0x22, `PlayMapBGM` `0x81031`) and refuse the save
([autosave.md](autosave.md)).

## The map script adds the message and the entrance scene

Four boss maps name a map script (the map's object record with cell −1,
[formats/obj.md](../formats/obj.md#names-and-scripts)): `MelodyMap` (3.8), `WolfMap` (4.17),
`HomunMap` (7.4) and `BalogMap` (10.5). The game runs it after the map loads and again when a
boss dies. Each has the same shape (`WolfMap.out`, in words):

1. `initMsg`, then `setBoss`: the message box's speaker is the boss (`0x32B4D` takes the sprite
   of the first mob whose id is 64 or more and not 65, `CMobs_FindBoss` `0x75711`).
2. If `checkRequire(kind, n)` is 0 (the quest that asks for the kill is still missing it:
   main 18 for 3.8, side 37 for 4.17, main 3 of chapter 3 for 7.4, main 7 of chapter 3 for 10.5),
   `startBoss`: world `+0x1E4` is set to the area's entrance scene, which
   `CStateGame::QueueBossScene` `0x80965` plays (scene 3, 5, 9 and 14 for areas 3, 4, 7 and 10).
   The entrance scene ends by setting the bosses on the map to attack.
3. `showMsg(0)`: the boss's line.

So on these maps the fight opens with the entrance scene while the kill is still missing, and
with only the message once it is made.

## 9.5 has no map script

9.5, the Yeti's map, names no map script. `YetiMap.out` exists in `script/chapter2/` and
`script/common/` with the same shape as the others (its check is `checkRequire(0, 8)`, main 8 of
chapter 2, and its scene would be 7), but no map object names it, so the game never runs it
([formats/script-out.md](../formats/script-out.md): one of the ten scripts no `.obj` names). The
Yeti's entrance scene (7) and its message are therefore never shown in version 1.5.

What starts the fight is only the way in: chapter 2's `portal942` on 9.4 warps to 9.5 while main
8 is active and its requirement is not met (`getQuestState(0, 8) == 1` and
`checkRequire(0, 8) == 0`). The Yeti is a spawn of 9.5, so the fight is on from the hero's first
tick there, with no scene and no message.

Played in the port (`play.chapter_2_f_synthetic`, 2026-11-27): on entering 9.5 the step `enter 9.5: mob 68
there, no boss scene` saw mob 68 on the map and no scene (game mode 9) or scene key pad
(panel 3) in the first 3 s. For comparison, `play.chapter_2_c` entered 4.17 into `WolfMap`'s
entrance scene (panel 3, game mode 9) before the fight.

## The end scene comes from the program

The boss's death is not scripted either. `CMob::Die` `0x73349` writes the boss's end scene into
world `+0x1E4` (mob 64: scene 4, 66: 6, 67: 10, 68: 8, 69: 1), sets game mode 10 and turns the
key pad off; `CStateGame::Paint` `0x87501` then starts it (`StartScene` `0x853E5`: game mode 9,
panel 3). So 9.5 has an end scene (8: the boss's last line in the speech box) though it has no
entrance scene. On the maps with a map script the script also runs again after the death.

The end scene's box closes only through `CStateGame::OnKeyPressed` `0x8D4C9`, which drops every
key while the hero is knocked back: a boss killed then locks the game in the scene
([original-bugs.md, 19](original-bugs.md#19-a-boss-killed-while-the-hero-is-knocked-back-locks-its-end-scene)).

## Chapter 3's two bosses, played

Played in the port ([#300](https://github.com/armaatus/maplethief-port/issues/300),
[testing/chapter-3-quests.md](../testing/chapter-3-quests.md)):

- **Mob 67 on 7.4** (`play.chapter_3_c`): the way in from 7.1 opens while main 3 is active and its
  kill is missing; `HomunMap` plays the entrance scene (9) first. The played level-70 hero beat it
  with no fall, and end scene 10 ran to the field.
- **Mob 69 on 10.5** (`play.chapter_3_e_synthetic`, from a level-99 raise): chapter 3's
  `portal1031` on 10.3 opens while main 7 is active; `BalogMap` plays the entrance scene (14).
  The death plays end scene 1, the first of the two ending scenes: its last command (52) queues
  scene 2 and loads its map 2.11, and scene 2 ends with command 28, a warp to 2.0, where free
  play goes on in chapter 3. In the watched run the boss died with the hero knocked back, and
  original bug 19 locked scene 1 until the run cleared the byte. Neither map load is a
  `changeMap`, so neither raises a run event
  ([#424](https://github.com/armaatus/maplethief-port/issues/424)).
