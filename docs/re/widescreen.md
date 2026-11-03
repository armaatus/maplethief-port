# Widescreen: where the engine allows a wider view

Issues [#86](https://github.com/armaatus/maplethief-port/issues/86) (the field) and [#169](https://github.com/armaatus/maplethief-port/issues/169) (the screens over it), [ADR 0090](../adr/0090-widescreen.md). Read in the Ghidra decompile of every function (armv7 slice, 2,655 functions) and checked on real assets with `play.widescreen_off` / `play.widescreen_on` (`tests/cpp/play_real_test.cpp`).

**Verdict:** the field can be drawn wider than 480 points with no change to gameplay. The screen width is read only by drawing code; nothing that updates the world reads it. The port draws the field up to 568 × 320 points (the iPhone 5's screen, 16:9 at 320 high) on every map wider than 30 tiles. The popup menu, the NPC shop, the map screen and the cash shop are drawn at 480 points in the middle of the wide field (#169, [below](#the-screens-over-the-field-169)). The title, the loading screens and the scripted scenes stay 3:2.

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
- **Presentation.** The UIKit screen image stays 480 × 320, so the key pad, every overlay, input mapping and screenshot work as before. It shows the GL frame's centre 480 points. The Presenter draws the whole GL frame in a 568-point placement behind it, so the extra columns show as wings on both sides (`platform/src/present.cpp`). Since #170 it composes the wings, the screen image and the key pad's strips into one picture first, so the colour-blind filter, the border's blur and the shader filters treat the wide picture as one image. #86 allowed the wide view only while the touch key pad was hidden; #168 keeps it on with the pad shown ([below](#with-the-touch-key-pad-168)).

The game mode changes the view between field and the 3:2 screens: when a loading screen or a scripted scene starts, E drops to 480 at the next turn and the picture is 3:2 again. The popup menu, the shop, the map screen and the cash shop keep the field's E (#169). On a map change, the camera hook sees the new map's width before the first clamp.

## Measured on real assets

`play.widescreen_off` and `play.widescreen_on` play `first_map`'s route with the same input and the same virtual clock: the tutorial's 2.8 and 2.9, Henesys 2.0 and the field 2.3, then 2,400 game ticks of hunting. In Henesys, on Stan's ledge, both runs also open the popup menu (and touch its item tab at (156, 277)), the map screen, the cash shop and Mr. Wang's shop (#169). Each run writes the game mode, the hero (map, position, flags, state, HP, MP, level, EXP, meso), every mob (id, position, HP) and the drop count at every game tick.

- The two files are **identical**: 2,985 ticks.
- The view widths are 480 on 2.8 and 2.9 (30 tiles) and 568 on 2.0 and 2.3. The game spent 62,820 of 153,916 turns wide; E changed 9 times; the camera hooks ran 5,417 times; the backdrop was stretched 1,237 times (2026-11-02, with #169).
- The four screens kept E = 568 behind them. 137 screen paints were drawn in the wide view's centre (the popup menu, the map screen in Henesys and on 2.3, the shop), and the popup menu's dim was stretched 52 times.
- The touch on the item tab opened it in both runs: the GL touch widgets hit.
- The popup menu window's opaque top bar, (78, 32, 320, 20) of the composited screen, has the same pixels in both runs, on the status and the item tab. The rest of the window lets the field through, and the field is placed for the wider view, so it differs.

The window shots (`OUT_DIR/wide_<area>_<map>.png`, local only):

- On 2.0 and 2.3 the map continues into the wings, and the sky is stretched to match.
- On 2.8 the picture is 3:2 in the middle with black wings.
- The GL HUD stays at its 480-point places, the left 480 points of the engine's view (#86 read this as following the width; #168 measured it and moves it to the middle).
- `menu_popup.png`, `menu_popup_item.png`, `menu_map.png`, `menu_shop.png`: the screen in the middle 480 points, exactly over the 3:2 picture's place, the field drawn wide behind it (dimmed beside the popup menu like under it). `menu_cash_shop.png`: the UIKit cash shop over the wide field.

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

## The screens over the field (#169)

`CStateGame::Paint` `0x87501` draws the field (the backdrop, the map, its objects, the HUD) in every game mode while a map is loaded. Then it switches on the game mode `0x11AA00` and draws the screen over the field. The popups (`Popup_Paint`) come after the switch, centred on `+0x90`, in every mode. Each screen's paint below is called only from `Paint`.

| Mode | Screen, its paint | Reads `+0x88` / `+0x90` | At literal coordinates | Choice | Why |
|---|---|---|---|---|---|
| 0 | the field | the camera, culling, clip, HUD | the backdrop (480 × 320 at (0, 0)) | wide (#86) | everything follows E; the backdrop is stretched |
| 1 | popup menu `0x56B99` | its skill tab's `drawSkillInfoBox` `0x4C2C1` | a black fill at alpha 100 over (0, 0, 480, 320) (`FillRect` `0x8079`); the window (332 × 264) at (0x4A, 0x1C); the tabs `0x56320`, `0x4FAD9`, `0x4F3D1`, `0x4E499`, `0x4ED75`, `0x4EAED` from (0x4A, 0x1C); the tab marker at 0x4B + 54 × tab; the 11 GL touch widgets at fixed rects (`Screen_Init`) | **centred** over the wide field; the dim stretched over it | all of it is at fixed x, and the touch widgets hit-test fixed 480-point rects: the screen has to stay where the touches land |
| 2 | NPC shop `0x56E91` | its list's clip (`0x233D5` to (0, 0, `+0x88`, …)), the item's info at `+0x90` + 0x31 | its windows at fixed x; the UIKit market panel (6) takes the touches | **centred** over the wide field | fixed x, and the UIKit panel sits in the 480-point screen image |
| 3 | map screen `Map_PaintSub0` `0x59BB1`, `Map_PaintSub1` `0x59119`, `Map_PaintSub2` `0x58AE9` | boxes centred on `+0x90` (Sub0 moves it 10 points right and back) | art at (0x4A, 0x19), (0x73, 0x31), …; the UIKit map panel (8), its frame set from the same points | **centred** over the wide field | mixed centred and fixed x; the UIKit town taps sit in the screen image |
| 4 | NPC chat `CChatBox::paint` | its box centred on `+0x90` | — | wide (#86) | centred on the view |
| 5 | a waiting box (`CStateGame +0xD4`, then `SetPigmeState(9)`) | centred on `+0x90` | — | 3:2 | no scripted run reaches it; left at the safe default |
| 6, 7, 8 | loading | — | `Loading_Progress` `0x6AF09`: 480-point art | 3:2 | `Paint` returns before the field: no map to show beside it |
| 9 | `PaintExitToMenu` `0x83C71`, then the switch's fades, speech box (`CChatBox::drawChatBox`) and letterbox bars | fades and bars over `+0x88` | — | 3:2 | scripted: its script places the actors and the camera for a 480-point stage; not widened without checking each scene |
| 10 | letterbox bars, then the scene `0x853E5` | bars over `+0x88` | — | 3:2 | the same |
| 0xB | cash shop | — (GL draws only the field and the popups) | the UIKit cash shop panel (9) | **wide field behind it**, nothing wrapped | the UIKit panel is in the screen image already |

**Centred, how** (`runtime/src/hooks/widescreen_menus.cpp`). `mt_widescreen_decide` keeps the field's E in modes 1, 2, 3 and 0xB, so the field is drawn wide behind the screen and the camera doesn't jump when a screen opens. Hooks on the five paints wrap the original:

1. The engine's width words go back to 480 for the call (the surface's width and row bytes, `0x11AA8C`, `+0x88`, `+0x90`, the full-screen clip).
2. A host clip-space x offset of (E − 480) / W (W = 568, the renderbuffer) moves every draw (E − 480) / 2 points right, into the GL frame's centre 480 points. That is exactly the 480-point screen image the host shows (`gl_screen_columns`) and takes touches on, so the GL touch widgets and the UIKit panels (market, map) line up with what is drawn, as at 3:2.
3. Afterwards the words are what they were, and the clip is E wide again if it is the full 480.

Inside the popup menu, a hook on `FillRect` `0x8079` matches the dim (0, 0, 480, 320) and draws it with the backdrop's stretch instead (x' = k x + (k − 1) w, k = E / 480), so the wings show the frozen field dimmed like the middle. The shop and the map screen have no dim: the field shows around their windows at 3:2 too, so their wings show it plain. While a screen's paint runs, the camera and backdrop hooks leave the width and the transform alone.

Only drawing code reads the width. The paints see the same 480 as the original, and the key and touch handlers run outside `Paint` (they read no width), so gameplay is unchanged: `play.widescreen_on` opens all four screens and matches `play.widescreen_off` tick for tick.

## What stays 3:2, and why

| Screen | Why |
|---|---|
| Title, main menu, character select | not a `CStateGame`; UIKit views laid out for 480 |
| Loading (modes 6–8), the scripted modes 9 and 10, the waiting box (5) | see [the table above](#the-screens-over-the-field-169) |
| The field while the touch key pad shows | the pad is UIKit at 3:2; the GL quick slots would leave its keys (#168) |
