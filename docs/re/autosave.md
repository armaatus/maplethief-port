# When the game lets the player save, and the port's autosave

What the game checks before it saves, and how the port's `autosave = on_quit` keeps to it ([#150](https://github.com/armaatus/maplethief-port/issues/150), [ADR 0089](../adr/0089-autosave-on-quit-and-background.md)). Addresses are armv7 VM addresses; functions carry the Thumb bit. The evidence is the Ghidra decompilation of the 1.5 binary (2026-10-30) and the real-asset runs named under [Verification](#verification).

## Verdict

**The port autosaves only where the game's own menu Save would succeed, at the moment the player leaves.** The save goes through the game's own pair, `CDataMgr::SaveSaveData` `0x38711` and `SaveQuestData` `0x38F55`, as every save in the game does ([formats/save.md](../formats/save.md#when-the-game-saves-and-loads)). The port adds one guard of its own (the hero must be at rest) and never saves where the game would refuse.

**"Save anywhere" stays rejected (#90).** The game refuses to save on 16 maps: the tutorial, the five boss maps, three climbs and the Monster Carnival rooms. Boss state is not saved; a save there would reload the hero into the room without the fight's state, or into a climb's middle. Loading tip 16 says so ("The game cannot be saved during boss stages."). The autosave keeps that list exactly.

## How the game saves

The player saves from the popup menu's option tab (tab 5). The path:

1. **`CStateGame::OnKeyPressed` `0x8D4C9`** opens the popup menu (game mode 1) on −16 or −7, but only in game mode 0 with no popup open (`*0x108C64 == −1`) and the hero's action not 0 or 1 (dying, dead). Before it opens the menu it stops the hero: `CHero::StopForMenu` `0x61329` when `flags & 7` (`0x6D0BD`), `CHero::StopToStand` `0x61385` otherwise. Both stand the hero (unless on a ladder) and zero its speed.
2. Before any of that, `0x8D4C9` ignores every key when:
   - the game mode (`*0x11AA00`) is 6, 7 or 8 (the loader, `LoadMap` `0x868D5`, entering or leaving the game);
   - a map change is pending (`CStateGame +0xB4`, set by `CMapleWorld::changeMap`);
   - the hero is being knocked back (`CHero +0x119`, set by `CHero::TakeDamage` `0x65B55`, cleared by `CHero::vf2` `0x66189` when the hero lands);
   - a scroll is being applied (`CDataMgr +0x108`, set by `CHero::UseItem` `0x66695`, cleared by `CHero::Flush` `0x62ADD`).
3. **`Game_ExitToMainMenu` `0x54BF9`** handles the option tab. Item 0 opens popup 13 ("Do you want to save?"); item 3 opens popup 14 (exit). With popup 13 open, −5 or `'5'` with Yes selected (`[*0x11A96C] + 0x14 == 0`) closes the popup (`Popup_Close`) and compares (area, map), `state +0x84` / `+0x88`, with [the 16 maps](#the-16-maps-where-the-game-cannot-save):
   - on a listed map it opens popup 18 ("Cannot save.");
   - otherwise it calls `SaveSaveData(dm, state + 0x30)` (the hero) and `SaveQuestData`, then opens popup 2 ("Successfully saved!").

   The popups' text is `CDataMgr.m_strPopupText.(id + 1)` in `ScriptExtra.strings`.
4. Key −10 in `0x8D4C9` saves directly after the same 16-map check (a feature-phone key that no iOS control sends). The Cash button's −26, also in `0x8D4C9`, does not save, but refuses to open the cash shop on the same 16 maps ([cash-shop.md](cash-shop.md)). The other save callers (`0x3ED19` after a purchase is granted, `CHero::Flush` `0x62ADD` after a scroll's result, `0x7C2D9` after a quest reward item) save at fixed points of their own flow and check nothing that matters here.

The slot in play is `CDataMgr +0x4E4`. `*0xF1454` holds `CDataMgr*` `0x11A970`, `*0xF1434` the popup id's address `0x108C64`.

### Game modes (`*0x11AA00`)

| Mode | Meaning | Evidence |
| --- | --- | --- |
| 0 | the field | the menu opens only from here |
| 1 | the popup menu (Save is here) | `0x8D4C9` sets it |
| 2 | a shop | |
| 3 | the world map | |
| 4 | an NPC's dialog | |
| 5 | Pigme | `CStateGame::SetPigmeState` `0x823A1` |
| 6 | the loader (Paint) | `0x8D4C9` ignores keys |
| 7 | a map load | `LoadMap` `0x868D5` |
| 8 | entering or leaving the game | `Enter` / exit |
| 9 | a scene playing (`CAniManager`) | `CStateGame::StartScene` `0x853E5` (`CAniManager::makeAni*`) sets it and `Shell_EnableAnimationKey`; called from `LoadGame` `0x86C1D` and `Paint` |
| 10 | a scene queued | `CStateGame::QueueScene` `0x80935` (the script's `runAnimation`); `QueueBossScene` `0x80965` picks scene 3, 5, 7, 9 or 14 (`+0x1E4`) for areas 3, 4, 9, 7 and 10, the boss areas |
| 11 | the cash shop | |

### The hero (`CHero`, `state + 0x30`)

- **Action** (`+0x10`, set by `CHero::SetAction` `0x60551`): 0 dying (`CHero::Die` `0x65979`), 1 dead, 2 standing, 3 walking, 4 climbing, 5 jumping or falling, 6 attacking, 7 attacking a target (`CHero::onAttack` `0x605FD`), 11 a skill.
- **Flags** (`+0xC`): bit 1 (0x2) in the air (a jump or a knock-back), bit 2 (0x4) on a ladder or rope (`0x6D139` tests it), bit 3 (0x8) attacking (set by `onAttack`, cleared by `Flush`; `0x6D119`). `0x6D0BD` tests `flags & 7`, `0x6D0E1` bit 1, `0x6D101` bit 0.

### The 16 maps where the game cannot save

The same compare chain in `0x54BF9` and in `0x8D4C9`'s −10 (save) and −26 (the cash shop), in this order:

| Area.map | Kind | Why it is on the list |
| --- | --- | --- |
| 2.8, 2.9 | tutorial | `TutorialMap`: the tutorial's quests 0–6 |
| 3.8 | boss | `MelodyMap`; boss music 0x22 in `PlayMapBGM` `0x81031` |
| 4.17 | boss | `WolfMap`; music 0x22 |
| 7.4 | boss | `HomunMap`; music 0x22 |
| 9.5 | boss | music 0x22 |
| 10.5 | boss | `BalogMap`; music 0x22 |
| 2.6, 2.7 | climb | the Forest of Patience (`Remem`): a tall map, portal at the bottom, the NPC at the top |
| 6.1, 6.2 | climb | `remem2`, the same shape |
| 9.9, 9.10 | climb | `Miner2`, the same shape |
| 0.5, 4.18, 6.3 | Monster Carnival | reached through Spiegelman 1/2/3's `changeMap`; 4.18 and 6.3 have no objects |

`platform/src/autosave.cpp` (`no_save_maps()`) and `tools/mtport/autosave.py` (`NO_SAVE_MAPS`) hold the list; `tests/test_autosave_names.py` keeps the two equal.

## The port's guards

`check_autosave` (`platform/src/autosave.{h,cpp}`) reads guest memory (image segments and the committed heap only) and stops at the first guard that fails, in this order:

| Guard | Fails when | Game's rule |
| --- | --- | --- |
| not in the game | the engine's state (`[*0x11A950] + 0xC`) is not a `CStateGame` (vtable `0x119B78`): the title, the main menu, the slot screen | there is no hero to save |
| loading | game mode 6, 7 or 8 | `0x8D4C9` ignores keys |
| scene | game mode 9 or 10 | no menu during a scene; boss scenes play on boss maps |
| not the field | any mode but 0 and 1 | Save is reached from the field through the popup menu |
| popup | `*0x108C64 != −1` | the menu opens only without a popup; a popup's choice (a purchase, "Do you want to save?") is half done |
| warp | `CStateGame +0xB4` set | `0x8D4C9` ignores keys |
| knock-back | `CHero +0x119` set | `0x8D4C9` ignores keys |
| scroll | `CDataMgr +0x108` set | `0x8D4C9` ignores keys |
| dying | hero action 0 or 1 | the menu does not open |
| no-save map | (area, map) in the 16 | "Cannot save." |
| hero busy (the port's) | hero action not 2, 3 or 4, or `flags & 0xB` (in the air or attacking) | see below |

**Why the port adds "hero busy".** The game's menu stops the hero before it saves (`0x61329` / `0x61385`), so its save never sees a hero mid-jump or mid-attack. The autosave does not open the menu, so it cannot stop the hero the same way without changing the game's state at a quit. It skips instead, so a jump, a fall, an attack or a skill is never saved half done (the hero's position mid-air, an attack between its hit and its `Flush`). Climbing (action 4, flag 0x4) passes, as the game's menu allows it.

A skipped autosave logs the guard: `autosave: not saved at the quit (SDL_EVENT_QUIT, ...) (12.000 s): a scene is playing (game mode 9)`. A save logs `autosave: slot 0 saved at the background (...) (12.000 s): map 2.0, through the game's SaveSaveData and SaveQuestData (33.1 ms)`.

## When it runs

`autosave = off | on_quit` (Gameplay; off in Original and Original + controller, on_quit in Enhanced; `--autosave`). With on_quit, `game.cpp`'s turn hook calls `Autosaver::before_deliver` right before `Lifecycle::deliver` sends:

- `applicationWillResignActive:`: a minimise, iOS's background, a focus loss with `pause_unfocused` (ADR 0063, ADR 0077);
- `applicationWillTerminate:`: the close button, Cmd-Q, `SDL_EVENT_QUIT`, iOS's terminate, `--quit`.

It runs between two run-loop turns, never inside a tick, so the game is between frames as when the player taps Yes. It checks once per turn: a turn with both a resign and a quit queued saves once. The save pair is called through `mt_call_guest` with every register kept and inside an autorelease pool; it writes through `mt_saves_replace_file` (ADR 0074) and the backup hook on `SaveQuestData` (ADR 0078) as the game's own saves do. There is no timer and no autosave during play.

## Verification

Real assets, on this Mac (Release build, 2026-10-31):

- `play.autosave_scene`: a new game; a minimise in the intro (game mode 9) is skipped as a scene, a quit on 2.8 as a no-save map.
- `play.autosave_map_<area>_<map>`, 16 tests: play.save_quit's save moved to each listed map (theif0's map at 0x41C/0x41D; quest0's chapter 3 for 10.5, whose scripts are chapter 3's), loaded with A through any scene; a quit saves nothing and theif0 and quest0 are unchanged. 3.8 and 10.5 also play their boss scene first, which is skipped as a scene.
- `play.autosave_quit`: in Henesys, the popup menu passes, "Do you want to save?" open with a minimise is skipped as a popup; after a Red Potion and a step left the quit saves slot 0 (Red Potions 4 → 3, x 548 → 513) and a new backup appears.
- `play.autosave_reload`: a second process finds the files as autosaved and loads the hero with that progress.
- `platform.game_autosave` (`platform/autosave_test.cmake`): the frontend itself, scripted fixed-step: with `--autosave on_quit` a minimise and a quit in Henesys save before their lifecycle messages; without the flag nothing is saved; at the main menu and in the intro's scene the guard is logged.

Synthetic, in CI: `platform_test`'s `platform.autosave:` cases (`tests/cpp/autosave_test.cpp`) check every guard and its text on a fake guest, the order of the checks, the 16 maps against every area 0–10 and map 0–19, the setting's row and once-per-leave.

The names above go into Ghidra through `tools/mtport/autosave.py` (`work/re/names/autosave.json`, run by `scripts/re-analyze`).
