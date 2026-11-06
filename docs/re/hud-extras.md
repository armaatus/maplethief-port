# HUD extras: mob HP bars, the EXP percentage and a pick-up log

What the field HUD and the mobs already show, for the three HUD extras of [#191](https://github.com/armaatus/maplethief-port/issues/191): a mob's HP bar after a hit, the EXP percentage next to the bar, and a short log of pick-ups and meso gained ([ADR 0109](../adr/0109-hud-extras-exp-percentage.md)). Addresses are armv7 VM addresses; functions carry the Thumb bit. The evidence is the Ghidra decompilation of the 1.5 binary (2026-11-05), the bundle's `ScriptExtra.strings` and data tables, and the real-asset run under [Verification](#verification). The names are `python3 -m mtport.hudextras names`.

## Verdict

| Candidate | What the game already draws | Decision |
|---|---|---|
| Mob HP bars | **A bar over each mob for 1 s after every hit it survives**: `CMob::Paint` `0x72685` draws the mob's name and a 40 × 2 point HP gauge above it while the hit counter `+0xD0` runs (20 ticks). Bosses get a bar at the top of the screen instead (`HUD_Paint`) | **Dropped.** The port's bar would be the game's drawn twice. Only its length differs (1 s against the 3 s asked for), and stretching the game's would mean writing the counter |
| EXP percentage | A 2-point EXP line under the HP / MP bar (153 points at a full level), no figure. The popup menu's status tab shows the EXP and a whole percentage | **Done:** `exp_percent = off \| on` (Video; on in Enhanced only): "EXP 42.17%" above the HUD's plate, left of the level badge |
| Pick-up log | **A line for each pick-up and meso gain**, 5 s each, fading for its last half second: "Red Potion Obtained!", "Meso obtained.(+70)", in the notify list under the compass, with "EXP obtained.(+7)" for a kill | **Dropped.** The game's lines are the log. The port's would repeat each of them (the only new part, the "×2" of a stack, the game never has: a drop is one item) |

The two dropped candidates are no follow-up issues: what they would add is already on the screen.

## The mobs and their HP

- **The list.** `CMobs::CMobs` `0x775DD` stores its list of the map's mobs at `0x11A9EC` (a `PtrArray`: count `+0x04`, items `+0x08`); `CStateGame +0x34` is the same `CMobs` (flow.py's `CNpcs_*`). The bosses (ids `0x40`–`0x45`) are in it too.
- **A mob** (`CMob`, a `CMapleObject`): position `+0x04` / `+0x08` (its feet, map pixels), flags `+0x0C` (`0x10` dead), box `+0x40`–`+0x4C` (left, top, right, bottom; `0x6CE5D` from the size `+0x38` / `+0x3C`), sprite height `+0x34`, **HP `+0x88`**, **max HP `+0x90`** (mob.dat's HP of the mob's id, checked on every hit below), name `+0x9C`, the mob.dat id `+0xA0`.
- **A hit.** `CMob::TakeHit` `0x73715` (mob, hero, ...; from `0x766F1` and `0x76AED`, the hero's attack checks) rolls the damage (a miss sets `+0xCC`; a critical doubles it), spawns the damage number at the mob's box top on screen (`+0x04` − camera x, `+0x44` − camera y − 4) and takes the damage off `+0x88`. At 1 or less it calls `CMob::Die` `0x73349`. Otherwise it sets **the knock-back counter `+0xC8` and the bar's counter `+0xD0` to 1**.
- **The counter.** `CMob::Flush` `0x7458D` (the mob's update, every tick) counts `+0xD0` up to 20 and then to 0 (it also stops at 0 for a dead mob unless its death is playing, `+0xC4`). `CMob::Die` leaves `+0xD0` alone: a kill does not start a bar, and an earlier hit's bar runs out as before.
- **The bar.** `CMob::Paint` `0x72685` (each mob, from `CNpcs_Paint` `0x76D05` in `CStateGame::Paint`) draws the mob, and then, while `+0xD0 > 0` and the id is below `0x40`, its name in yellow 2 points under its feet and over its head a frame image (46 × 8 at x − 23, y − height − 14), a black 41 × 3 back, a dark green 40 × 2 gauge and the green fill `Gauge_Width(40, max HP, HP)` (`0x36FF1`: `full × value / max`, 0 for a max of 0, `full` from `value ≥ max`). The camera is `CMMap +0x70` / `+0x74` (`CMMap_CameraX` `0x5CF6D`, `CMMap_CameraY` `0x5CF81`).
- **Bosses.** On the five boss maps (3.8, 4.17, 9.5, 7.4, 10.5) `HUD_Paint` draws the first mob of id `0x40` or more (not `0x41`) as a portrait and a wide bar at the top of the screen (`Gauge_Width` over `+0x90` and `+0x88`), flashing white for 2 of every 4 ticks of `+0xC8`. `CMob::Paint` draws no bar for them.

## The EXP

- **The hero** (`CStateGame +0x30`): level `+0x80`, **EXP `+0x90`** (what the level has gathered, from 0), HP `+0x94`, MP `+0x98`, max HP `+0x9C`, max MP `+0xA0`.
- **The need.** `CHero::ExpToLevelUp` `0x65919` is hero.dat's EXP column for the level: `CDataMgr::FindRow` `0x3990D` finds the first row whose u8 key (offset 0) is the level, and `CDataMgr::ReadI32` `0x385D1` reads the i32 at `table + 6 + row × stride + 1`. The table is the raw file, its pointer at `0x11A9A8` ([formats/data-tables.md](../formats/data-tables.md)). Level 1 needs 10, level 5 40, level 12 430.
- **Gaining.** `CHero::setExp` `0x6654D` (hero, gain) adds the gain and, while the sum reaches the level's need, takes the need off and calls `CHero::LevelUp` (up to 99, where the EXP is set to 0). So the EXP is always below the need. It then prints "EXP obtained.(+N)" (or "EXP reduced.(N)") as a notify line. `CMob::Die` gives mob.dat's EXP column (`+0x19`) through it.
- **What the HUD shows.** `HUD_Paint` `0x5A465` draws the bar image at (150, H − 36) (the plate x 150 … 329 from y 304, the level badge x 215 … 265 from y 285), the level number, HP and MP gauges (`Gauge_Width(59, …)` at x 155 and 265, y H − 11, 5 points high) and the **EXP line**: `Gauge_Width(153, need, EXP)` at (163, H − 3), 2 points high, in `0xB6DA00`. No figure.
- **What the status tab shows.** `PopupMenu_PaintStatusTab` `0x56321` draws the EXP as a number and `EXP × 100 / need` as a whole percentage with a "%" image, in the popup menu (game mode 1) only.

## Pick-ups and meso

- **The path.** `CDropItems::Update` `0x5BC75` calls `CHero::addInven(hero, item, 1)` `0x64309` for a drop that reached the hero (docs/re/qol.md, Auto-loot). For meso (item kind `+0x84` = 1, item table column 0x41 = 0) it calls `CHero::setMeso(hero, amount +0x100, 1)` `0x618E1`; otherwise it fills a stack or a free place. With the third argument 1 both print a line: `addInven` "%s %s" with the item's name (`+0x94`) and `CHero.addInven.1` / `.2` ("Obtained!"), `setMeso` "%s(+%d)" with `CHero.setMeso.3` ("Meso obtained."), or "%s(%d)" with `.2` ("Lost Meso(s)").
- **The lines.** Each goes to `CNotifyTextList::Add` `0x78339` on the `CStateGame`'s notify list `+0x4C` (`CHero +0x150` is the `CStateGame`). A `CNotifyText` (`0x7798D`) wraps the text at 240 points (lines `+0x04`, count `+0x08`), keeps its colour `+0x0C` and the time it came `+0x10` (ms, 64-bit). The list (`0x788A9`): entries `+0x04`, total lines `+0x08`, the most entries `+0x0C` (the oldest goes first), line height `+0x10`, style `+0x14`, now `+0x18`.
- **Shown.** `CStateGame::Paint` `0x87501` paints the list at (4, H − 254) (`CNotifyTextList::Paint` `0x77C19`, under the compass) and `CNotifyTextList::Expire` `0x77B39` drops the lines older than 5,000 ms. A line fades out over its last half second (alpha 255 − 51 × the tenths past 4.5 s). The lists `+0x50` and `+0x54` are painted at the screen's top centre (y 40) for other messages.

## Verification

`play.hud_extras` (`play_real_test hud_extras`, local, real assets; 2026-11-05): a new game through the tutorial (the snail on 2.9) and Henesys to 2.3, then a hunt until a Red Potion has been picked up and a level gained there, with `exp_percent = on`. 148,521 field turns:

- **The EXP figure** was shown in every field turn, levels 1 to 7, 22 figures from "EXP 0.00%" to "EXP 95.00%". In every turn its level and EXP were the hero's (`+0x80`, `+0x90`), its need was the bundle's hero.dat row of the level, and its whole percent was the status tab's `EXP × 100 / need`.
- **The game's mob bar:** 65 hits the mob lived on and 17 kills. Every surviving hit set `+0xD0` (1 or 2 at the next turn), every hit mob's max HP `+0x90` was mob.dat's HP of its id, and all 16 bars that closed did so 19 field ticks after the hit, the counter at 20. The first: a Blue Snail hit from 20 to 15 HP of 20, a bar of 30 of 40 points. A shot of the frame after a hit shows the name and the bar over the mob (in the test's output, never committed).
- **The game's pick-up lines:** 1 Red Potion picked up on 2.3 with its "Red Potion Obtained!" line, 11 meso gains whose sum equals the sum of their "Meso obtained.(+N)" lines, and no line kept longer than 5.03 s.
