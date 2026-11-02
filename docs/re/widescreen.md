# Widescreen: where the engine allows a wider view

Issue [#86](https://github.com/armaatus/maplethief-port/issues/86), [ADR 0090](../adr/0090-widescreen.md). Read in the Ghidra decompile of every function (armv7 slice, 2,655 functions) and checked on real assets with `play.widescreen_off` / `play.widescreen_on` (`tests/cpp/play_real_test.cpp`).

**Verdict:** the field can be drawn wider than 480 points with no change to gameplay. The screen width is read only by drawing code; nothing that updates the world reads it. The port draws the field up to 568 × 320 points (the iPhone 5's screen, 16:9 at 320 high) on every map wider than 30 tiles. Menus, the title, popups, the shop, the map screen and the loading screens stay 3:2, because they are laid out for 480 points (some with the literal 480).

## Where the width comes from

`Screen_Init(480, 320)` `0x9909` runs once at start-up and writes the size into:

| Where | What |
|---|---|
| `0x11AA88` → surface | the screen surface: `+0x00` width, `+0x10` bytes a row (width × 4) |
| `0x11AA8C` | the width |
| `0x11AA84` | the height (`FUN_6088` flips y with it) |
| `SkyCanvas +0x88` / `+0x90` | the width and half of it, copied from the surface by `SkyCanvas::SkyCanvas` `0x26529` |
| `SkyCanvas +0x8C` / `+0x94` | the height and half of it |

`-[EAGLView setupView]` `0x3335` sets `glViewport` and `glOrthox` from the renderbuffer's size (`glGetRenderbufferParameterivOES`), not from these globals. A wider renderbuffer therefore gives a wider projection with no guest change.

## Who reads it

18 functions read `SkyCanvas +0x88`. Every one of them draws; none writes world state or calls `rand`.

| Group | Functions | What the width does there |
|---|---|---|
| Camera | `CMMap_ClampCamera` `0xC069`, `CMMap_FollowCamera` `0xC175` | clamp the camera (`CMMap +0x70` / `+0x74`) to `0 … map width (+0x5C) − screen width`; follow: `target = p − screen / 2`, then `cam += (target − cam) / 3` |
| Culling | `0x1F051`, `0x1F2B9`, the object loop `0x5C0AD` | skip sprites and objects outside `0 … width` |
| Clip | `0x233D5` (clamps the clip rect to `0 … +0x88`), `0x23719` | the full-screen clip |
| Fill | `0x23DC1` | full-screen fills |
| Field HUD | `HUD_Paint` `0x5A465` (passes `width − 0x6F` to `QuickSlots_Paint`), the hero's draw and its flash fills `0x61A19` | little: the bar, the quick slots and the attack key's icon are drawn at fixed 480-point places whatever the width (measured, [below](#with-the-touch-key-pad-168)) |
| Other screens | `0x56E91`, `0x77C19`, `CChatBox`, `drawSkillInfoBox`, `Loading_Progress`, `PaintExitToMenu`, `CStateGame::Paint` `0x87501`, `CStateTitle::Paint` | layout; popups, the chat box and `Map_Paint*` centre on `+0x90` |

Some menus ignore the globals and use the literal 480: the popup menu's background fill `0x56B99` fills (0, 0, 480, 320), and `0x4E499`, `0x4EAED`, `0x4ED75`, `0x4F3D1` and `0x4FAD9` place their pieces at fixed x. Those screens can't be widened without rewriting them.

The camera getters `0x5CF6D` / `0x5CF81` have 13 callers: the draws, and the damage-number spawns `0x369E5` (from `CHero::TakeDamage`, `CHero::CheckMobContact`, `0x73715`, `0x766F1`, `0x76AED`). The spawns store screen coordinates in a list of floating numbers that is only drawn, so a different camera moves the numbers with the view and changes nothing else.

The `rand` callers (`CFloatingBGFlowerAlgorithm`, the mobs, `CStateGame::Paint`'s camera shake) don't depend on the width, and culling never skips a `rand` call. The sequence of random numbers is therefore the same at any width.

## Why the GL layer alone can't widen the view

The sprite draw `Sprite_DrawClipped` `0x7CCD` clips every quad on the CPU, before `glDrawArrays`, to the surface's width and to the canvas's clip rect (the graphics context at `SkyCanvas +0x10`: x0 `+0x14`, y0 `+0x18`, x1 `+0x1C`, y1 `+0x20`). Nothing past column 480 ever reaches GL unless the engine itself believes the screen is wider. Widening only the projection would stretch the picture; widening only the viewport would show black.

## What the port changes (ADR 0090)

- **The renderbuffer.** `-[EAGLContext renderbufferStorage:fromDrawable:]` gives the 480-point layer 568 × 320 points of storage (`runtime/src/gles/eagl.cpp`). `setupView` then sets the viewport and the projection to 568 × 320.
- **The view width E** (`maplethief/widescreen.h`, `runtime/src/hooks/widescreen.cpp`). Each run-loop turn, and in the two camera hooks before the original runs:
  - in the field, E = the map's width clamped to 480 … 568. The field means: the current state is a `CStateGame` (vtable pointer `0x119B78`), the game mode `0x11AA00` is 0 (field) or 4 (NPC chat, centred on `+0x90`), a map is loaded, and the host allows it;
  - everywhere else, E = 480.

  The port writes E into the surface (width and row bytes), `0x11AA8C`, `SkyCanvas +0x88` / `+0x90`, and the clip rect's x1 when the clip is the whole old screen. The original clamp then keeps the camera inside the map, and culling and clipping widen with E.
- **The backdrop.** The field's sky (`CStateGame +0x454`, a fixed 480 × 320 image drawn at (0, 0) through `DrawImage` `0x24DC9`) is stretched to E columns by a host clip-space transform (`mt_gles_set_host_x_transform`) around that one call.
- **Presentation.** The UIKit screen image stays 480 × 320, so the key pad, every overlay, input mapping and screenshot work as before. It shows the GL frame's centre 480 points. The Presenter draws the whole GL frame in a 568-point placement behind it, so the extra columns show as wings on both sides (`platform/src/present.cpp`). #86 allowed the wide view only while the touch key pad was hidden; #168 keeps it on with the pad shown ([below](#with-the-touch-key-pad-168)).

The game mode changes the view between field and menu. When a popup or the map screen opens, E drops to 480 at the next turn and the picture is 3:2 again. On a map change, the camera hook sees the new map's width before the first clamp.

## Measured on real assets

`play.widescreen_off` and `play.widescreen_on` play `first_map`'s route with the same input and the same virtual clock: the tutorial's 2.8 and 2.9, Henesys 2.0 and the field 2.3, then 2,400 game ticks of hunting. Each run writes the game mode, the hero (map, position, flags, state, HP, MP, level, EXP, meso), every mob (id, position, HP) and the drop count at every game tick.

- The two files are **identical**: 2,985 ticks.
- The view widths are 480 on 2.8 and 2.9 (30 tiles) and 568 on 2.0 and 2.3. The game spent 60,280 of 153,916 turns wide; E changed 5 times; the camera hooks ran 5,423 times; the backdrop was stretched 1,187 times.
- The map screen opened during the run (game mode 3) and was drawn at 480.

The window shots (`OUT_DIR/wide_<area>_<map>.png`, local only):

- On 2.0 and 2.3 the map continues into the wings, and the sky is stretched to match.
- On 2.8 the picture is 3:2 in the middle with black wings.
- The GL HUD stays at its 480-point places, the left 480 points of the engine's view (#86 read this as following the width; #168 measured it and moves it to the middle).

## With the touch key pad (#168)

Measured with a GL dump of one 568-point field frame (`MT_GL_DUMP`, `play_real_test widescreen_pad_on`) against a 480-point one: `HUD_Paint`'s draws are **the same in both**. The bar's frame is at x 150 … 329 and its HP and MP fills start at the literals 155 (`0x9B`) and 265 (`0x109`). The quick slots' field mode draws slot 0 at 430 … 452 (on the attack key) and slots 1–4 at 454 … 470, whatever `width − 0x6F` it is passed. So in a 568-point view the HUD sat 44 points left of the 3:2 picture's middle, and the icons 44 points left of their (centred) keys.

What the port does ([ADR 0090's addendum](../adr/0090-widescreen.md#addendum-the-touch-key-pad-in-the-wide-view-168-2026-11-02)):

- **The HUD in the middle.** A hook on `HUD_Paint` runs it with `+0x88` / `+0x90` at 480 / 240 and every draw moved right by (E − 480) / 2 points (the host clip-space x transform), so the bar is centred and each icon is over its key's 3:2 place. With E = 480 it does nothing.
- **The pad at the edges** (`widescreen_hud = edges`, the default). The d-pad moves left by (E − 480) / 2 and jump, attack and the quick slots right by as much, into the wings; the quick slots' draw filter (ADR 0086) moves their icons with them. Map, Bag and Cash stay where the game put them: they are `UIButton`s at the top right, clear of the other groups.
- **Touches in the engine's x.** The game pad's touches reach the guest at the screen image's x + (E − 480) / 2, the x the engine draws at, so the NPC-touch check (`npcPosition` is in engine points) and the GL widgets agree with the picture. A touch may begin in the wings, and then belongs to the pad. The hit mask hook takes the offset back and reads the mask under the original key.
- **`widescreen_hud = narrow`** keeps the pad and the whole HUD at their 3:2 places.

`play.widescreen_pad_off` / `play.widescreen_pad_on` hold left, attack and right by touch on 2.3, the second with the pad at the edges of a 568-point view, touching where the keys are drawn now: the hero's state matches at every tick. The window shots `pad_2_0.png`, `pad_2_3.png` and `pad_2_3_hold.png` show each icon on its key.

## Maps

90 shipped maps; a tile is 16 points, so a 568-point view needs 36 tiles. 74 maps are wide (E = 568). Map 36 (32 tiles) gets E = 512 and map 37 (35 tiles) gets E = 560: the picture is centred and the rest of the wings are black. The 14 maps exactly 30 tiles wide stay 3:2. No map is narrower than 30 tiles or lower than 20.

| Map file | Tiles (w × h) | Width, px | View width E | Verdict |
|---:|---:|---:|---:|---|
| 0 | 40 × 28 | 640 | 568 | wide |
| 1 | 45 × 35 | 720 | 568 | wide |
| 2 | 50 × 35 | 800 | 568 | wide |
| 3 | 50 × 30 | 800 | 568 | wide |
| 4 | 55 × 32 | 880 | 568 | wide |
| 5 | 50 × 20 | 800 | 568 | wide |
| 10 | 40 × 30 | 640 | 568 | wide |
| 11 | 50 × 26 | 800 | 568 | wide |
| 12 | 55 × 25 | 880 | 568 | wide |
| 13 | 45 × 30 | 720 | 568 | wide |
| 14 | 40 × 35 | 640 | 568 | wide |
| 20 | 51 × 20 | 816 | 568 | wide |
| 21 | 50 × 26 | 800 | 568 | wide |
| 22 | 55 × 35 | 880 | 568 | wide |
| 23 | 60 × 30 | 960 | 568 | wide |
| 24 | 50 × 30 | 800 | 568 | wide |
| 25 | 42 × 30 | 672 | 568 | wide |
| 26 | 45 × 50 | 720 | 568 | wide |
| 27 | 46 × 50 | 736 | 568 | wide |
| 28 | 30 × 30 | 480 | 480 | 3:2 |
| 29 | 30 × 30 | 480 | 480 | 3:2 |
| 30 | 48 × 25 | 768 | 568 | wide |
| 31 | 48 × 22 | 768 | 568 | wide |
| 32 | 45 × 25 | 720 | 568 | wide |
| 33 | 45 × 32 | 720 | 568 | wide |
| 34 | 48 × 32 | 768 | 568 | wide |
| 35 | 48 × 35 | 768 | 568 | wide |
| 36 | 32 × 40 | 512 | 512 | partly wider |
| 37 | 35 × 42 | 560 | 560 | partly wider |
| 38 | 30 × 24 | 480 | 480 | 3:2 |
| 40 | 45 × 25 | 720 | 568 | wide |
| 41 | 30 × 20 | 480 | 480 | 3:2 |
| 42 | 38 × 35 | 608 | 568 | wide |
| 43 | 38 × 32 | 608 | 568 | wide |
| 44 | 40 × 30 | 640 | 568 | wide |
| 45 | 40 × 35 | 640 | 568 | wide |
| 46 | 38 × 35 | 608 | 568 | wide |
| 47 | 45 × 28 | 720 | 568 | wide |
| 48 | 45 × 30 | 720 | 568 | wide |
| 49 | 48 × 28 | 768 | 568 | wide |
| 50 | 30 × 45 | 480 | 480 | 3:2 |
| 51 | 30 × 45 | 480 | 480 | 3:2 |
| 52 | 30 × 45 | 480 | 480 | 3:2 |
| 53 | 30 × 45 | 480 | 480 | 3:2 |
| 60 | 57 × 20 | 912 | 568 | wide |
| 61 | 46 × 52 | 736 | 568 | wide |
| 62 | 44 × 50 | 704 | 568 | wide |
| 63 | 50 × 20 | 800 | 568 | wide |
| 70 | 40 × 20 | 640 | 568 | wide |
| 71 | 50 × 25 | 800 | 568 | wide |
| 72 | 48 × 25 | 768 | 568 | wide |
| 73 | 48 × 22 | 768 | 568 | wide |
| 74 | 30 × 20 | 480 | 480 | 3:2 |
| 75 | 40 × 20 | 640 | 568 | wide |
| 76 | 30 × 20 | 480 | 480 | 3:2 |
| 80 | 40 × 22 | 640 | 568 | wide |
| 81 | 48 × 22 | 768 | 568 | wide |
| 82 | 48 × 22 | 768 | 568 | wide |
| 83 | 48 × 20 | 768 | 568 | wide |
| 84 | 45 × 22 | 720 | 568 | wide |
| 85 | 40 × 20 | 640 | 568 | wide |
| 86 | 30 × 20 | 480 | 480 | 3:2 |
| 90 | 45 × 24 | 720 | 568 | wide |
| 91 | 50 × 27 | 800 | 568 | wide |
| 92 | 51 × 27 | 816 | 568 | wide |
| 93 | 54 × 27 | 864 | 568 | wide |
| 94 | 36 × 40 | 576 | 568 | wide |
| 95 | 30 × 22 | 480 | 480 | 3:2 |
| 96 | 36 × 40 | 576 | 568 | wide |
| 97 | 36 × 42 | 576 | 568 | wide |
| 98 | 45 × 27 | 720 | 568 | wide |
| 99 | 36 × 50 | 576 | 568 | wide |
| 100 | 45 × 23 | 720 | 568 | wide |
| 101 | 45 × 23 | 720 | 568 | wide |
| 102 | 50 × 22 | 800 | 568 | wide |
| 103 | 50 × 20 | 800 | 568 | wide |
| 104 | 45 × 25 | 720 | 568 | wide |
| 105 | 30 × 20 | 480 | 480 | 3:2 |
| 210 | 40 × 20 | 640 | 568 | wide |
| 211 | 58 × 20 | 928 | 568 | wide |
| 410 | 38 × 45 | 608 | 568 | wide |
| 411 | 42 × 38 | 672 | 568 | wide |
| 412 | 45 × 30 | 720 | 568 | wide |
| 413 | 48 × 24 | 768 | 568 | wide |
| 414 | 38 × 35 | 608 | 568 | wide |
| 415 | 42 × 42 | 672 | 568 | wide |
| 416 | 45 × 30 | 720 | 568 | wide |
| 417 | 30 × 22 | 480 | 480 | 3:2 |
| 418 | 50 × 20 | 800 | 568 | wide |
| 910 | 46 × 50 | 736 | 568 | wide |

## What stays 3:2, and why

| Screen | Why |
|---|---|
| Title, main menu, character select | not a `CStateGame`; UIKit views laid out for 480 |
| Every game mode other than 0 and 4 (cutscenes and scripted scenes included) | not checked one by one; the safe default is 480 |
| Popup menu (mode 1), shop (2), map screen (3), cash shop (0xB) | literal 480 in their layout and fills |
| Loading and exit (6–10) | `Loading_Progress` and `PaintExitToMenu` are centred on 480 artwork |
| The field while the touch key pad shows | the pad is UIKit at 3:2; the GL quick slots would leave its keys |
