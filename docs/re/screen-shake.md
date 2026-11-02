# The scenes' camera shake, and the port's `screen_shake`

Where the game shakes the screen, and how the port's `screen_shake = 0..100` scales it ([#158](https://github.com/armaatus/maplethief-port/issues/158), [ADR 0093](../adr/0093-accessibility-colour-filters-screen-shake-and-text-height.md)). Addresses are armv7 VM addresses; functions carry the Thumb bit. The evidence is the Ghidra decompilation and disassembly of the 1.5 binary (2026-11-01), `python3 -m mtport.screenshake scenes` (an `armscan` of the scene builders) and the real-asset runs under [Verification](#verification).

## Verdict

**Only scenes shake the screen, and the shake goes through one place.** A scene's command 19 turns it on with an amplitude in screen points, command 20 turns it off, and `CStateGame::Paint` `0x87501` draws it each frame by moving the map's camera by a random offset and back. Combat, damage, bosses and skills don't shake the camera: the shake's fields are written only by the runner's two commands and by `ResetSceneCamera`, and only read in Paint's game mode 9 (a scene). So a contained hook exists: the port scales the amplitude for the frame and puts the game's value back after it.

## The fields (`CStateGame`)

| Offset | Type | What |
|---|---|---|
| `+0x47D` | byte | the scene camera follows a point (`CMMap::FollowPoint`) instead of scrolling |
| `+0x47E` | byte | the shake is on |
| `+0x480`, `+0x484` | int | the scene camera's scroll per frame |
| `+0x488` | int | the shake's amplitude, screen points |
| `+0x48C`, `+0x490` | int | the offset drawn this frame, x and y |

`CStateGame::ResetSceneCamera` `0x814E1` zeroes `+0x47C..0x47E`, `+0x1E9`, `+0x480..0x494` and `+0x4C4` (no scene camera, no shake). It is called from `0x8572D` and `0x87324`, when a scene starts and ends.

## The scene commands

The scene runner `CStateGame::PaintExitToMenu` `0x83C71` runs the current step's commands (a `tbh` on command − 1 at `0x83D26`):

- **command 19** (case `0x12`, `0x8427E`): `+0x47E = 1`, `+0x488 = P2`, the command's second argument;
- **command 20** (case `0x13`, `0x842A8`): `+0x47E`, `+0x488`, `+0x48C`, `+0x490` all 0.

The runner can also end the game state: a scene's exit to the main menu (`0x84604`) deletes the `CStateGame` inside the call.

The scene builders that add them (`python3 -m mtport.screenshake scenes`, step: amplitude):

| Scene builder | On | Off |
|---|---|---|
| `CAniManager::makeAni1` `0x302F5` (the intro) | 135: 10, 274: 15, 359: 5 | 145, 285, 362 |
| `makeAniChapter2Perion` `0x27AED` | 85: 10, 100: 20 | 120 |
| `makeAniEndingZacum` `0x2BE55` | 64: 5 | 177 |
| `makeAniBalogIn` `0x2CBD1` | 66: 10 | 71 |
| `makeAniYetiIn` `0x2E5E1` | 7 times 10, at steps 3 + 11k | 8 + 11k |

The largest amplitude is 20 (Perion's chapter 2 scene); the intro's is 15.

## How a frame draws it

`CStateGame::Paint` `0x87501` (`this` in r0, kept at `[r7, #-0x90]`):

1. calls the scene runner `0x83C71` at `0x88320` (the commands above run here);
2. in game mode 9 (`*0x11AA00`, a scene), if `+0x47E` is set: `+0x48C = Rand_Range(0, amp) − amp / 2` and `+0x490` the same, with `amp = +0x488` (`amp >> 1`, an arithmetic shift). `Rand_Range` `0x22641` draws exactly one `rand()` (the state at `0x11A510`) whatever its range, so the number of random draws does not depend on the amplitude;
3. if `+0x47D`: `CMMap::FollowPoint` `0xC175` to the followed point plus the offset; otherwise `CMMap::ScrollCamera` `0xC225` by (`+0x480 + offset x`, `+0x484 + offset y`);
4. draws the map, then at `0x8A5E8` scrolls back by −offset.

`CMMap::ScrollCamera` moves the camera (`CMMap +0x70`, `+0x74`) and clamps it with `CMMap::ClampCamera` `0xC069` to 0 .. map − screen. At a map's edge the clamp can eat part of the offset or of its undo; the port keeps that as it is.

## The port's hook

`screen_shake` (Accessibility, 0 to 100 in steps of 10, default 100, not in the presets; `--screen-shake`) installs two wrappers through the guest function hooks (ADR 0069, `runtime/src/hooks/builtin.cpp`), both off at 100:

- **`CStateGame::Paint` `0x87501`**: reads the game's amplitude, writes `round(amp × P / 100)` into `+0x488` while the shake is on, runs the original, and writes the game's value back.
- **The scene runner `0x83C71`**, only inside that Paint: puts the game's value back before the commands run (they read and set it), then scales the value they leave for the rest of the frame.

Between frames, guest memory holds the game's own amplitude, so a save, a scene's own reads and the next command see the original value. Nothing is written into a state that the runner deleted: the hooks check the canvas's current state (`0x11A950`, `+0xC`) and the state's vtable pointer first. The random draws are the game's (step 2 draws one `rand()` whatever the amplitude), so only the picture changes.

## Verification

- `tests/cpp/hooks_test.cpp`: `hooks: screen_shake scales a scene's shake amplitude for the frame only (#158)` and `hooks: screen_shake inside the frame: the scene runner's commands see the game's value` (synthetic guest functions with the same fields: the scaled amplitude inside Paint, the game's value after it and inside the runner, a new command's value scaled for the rest of the frame, off at 100).
- `tests/test_screenshake_names.py`: the names and the scene table above (`test_the_scenes_that_shake` reads the real binary; skipped without `work/ipa/`).
- `play.screen_shake_{100,50,0}` (real assets, local): a new game through the intro, the shake fields read every run-loop turn in game mode 9.

  | `MT_SCREEN_SHAKE` | turns with the shake on | the game's amplitude | largest offset | Paints scaled | `rand()` state after the intro |
  |---|---|---|---|---|---|
  | 100 | 1218 | 15 | 8 | 0 | `0xEFDB42BC` |
  | 50 | 1218 | 15 | 4 | 28 | `0xEFDB42BC` |
  | 0 | 1218 | 15 | 0 | 28 | `0xEFDB42BC` |

  The game's amplitude stays 15 in memory at every percent, the offsets stay within the scaled amplitude (`a − a / 2`, `a = round(15 × P / 100)`), and the random state after the intro is the same: the game drew the same numbers.
