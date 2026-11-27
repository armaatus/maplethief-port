# Widescreen: where the engine allows a wider view

Issues [#86](https://github.com/armaatus/maplethief-port/issues/86) (the field), [#169](https://github.com/armaatus/maplethief-port/issues/169) (the screens over it) and [#396](https://github.com/armaatus/maplethief-port/issues/396) (the scenes), [ADR 0090](../adr/0090-widescreen.md). Read in the Ghidra decompile of every function (armv7 slice, 2,655 functions) and checked on real assets with `play.widescreen_off` / `play.widescreen_on` (`tests/cpp/play_real_test.cpp`).

**Verdict:** the field can be drawn wider than 480 points with no change to gameplay. The screen width is read only by drawing code; nothing that updates the world reads it. The port draws the field up to 568 × 320 points (the iPhone 5's screen, 16:9 at 320 high) on every map wider than 30 tiles. The popup menu, the NPC shop, the map screen and the cash shop are drawn at 480 points in the middle of the wide field (#169, [below](#the-screens-over-the-field-169)). Seven of the 21 scripted scenes keep the field's width, their speech box at 480 points in the middle (#396, [below](#scenes-at-the-fields-width)). The title, the loading screens and the other scenes stay 3:2.

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

The game mode changes the view between field and the 3:2 screens: when a loading screen or an unlisted scripted scene starts, E drops to 480 at the next turn and the picture is 3:2 again. The popup menu, the shop, the map screen, the cash shop and the listed scenes keep the field's E (#169, #396). On a map change, the camera hook sees the new map's width before the first clamp.

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
| 9 | `PaintExitToMenu` `0x83C71`, then the switch's fades, speech box (`CChatBox::drawChatBox` `0x35189`) and letterbox bars | fades and bars over `+0x88`; the speech box's frame over `+0x88`, its lines wrapped at `+0x88` − 24 by the runner | — | **wide for 7 listed scenes** (#396), else 3:2; the speech box **centred** | [below](#scenes-at-the-fields-width): checked scene by scene |
| 10 | letterbox bars, then the scene `0x853E5` | bars over `+0x88` | — | as mode 9 | the scene queued: the same list |
| 0xB | cash shop | — (GL draws only the field and the popups) | the UIKit cash shop panel (9) | **wide field behind it**, nothing wrapped | the UIKit panel is in the screen image already |

**Centred, how** (`runtime/src/hooks/widescreen_menus.cpp`). `mt_widescreen_decide` keeps the field's E in modes 1, 2, 3 and 0xB, so the field is drawn wide behind the screen and the camera doesn't jump when a screen opens. Hooks on the five paints wrap the original:

1. The engine's width words go back to 480 for the call (the surface's width and row bytes, `0x11AA8C`, `+0x88`, `+0x90`, the full-screen clip).
2. A host clip-space x offset of (E − 480) / W (W = 568, the renderbuffer) moves every draw (E − 480) / 2 points right, into the GL frame's centre 480 points. That is exactly the 480-point screen image the host shows (`gl_screen_columns`) and takes touches on, so the GL touch widgets and the UIKit panels (market, map) line up with what is drawn, as at 3:2.
3. Afterwards the words are what they were, and the clip is E wide again if it is the full 480.

Inside the popup menu, a hook on `FillRect` `0x8079` matches the dim (0, 0, 480, 320) and draws it with the backdrop's stretch instead (x' = k x + (k − 1) w, k = E / 480), so the wings show the frozen field dimmed like the middle. The shop and the map screen have no dim: the field shows around their windows at 3:2 too, so their wings show it plain. While a screen's paint runs, the camera and backdrop hooks leave the width and the transform alone.

Only drawing code reads the width. The paints see the same 480 as the original, and the key and touch handlers run outside `Paint` (they read no width), so gameplay is unchanged: `play.widescreen_on` opens all four screens and matches `play.widescreen_off` tick for tick.

## Scenes at the field's width

[#396](https://github.com/armaatus/maplethief-port/issues/396), [ADR 0090's addendum](../adr/0090-widescreen.md#addendum-scenes-at-the-fields-width-396-2026-11-27). Measured 2026-11-27 on the port's own runs (the window shots local only).

**Where each scene plays.** A scene runs on the map that is loaded when it starts: the boss entrances (`QueueBossScene` `0x80965`, areas 3, 4, 7, 9, 10) on the boss maps 3.8, 4.17, 7.4, 9.5 and 10.5; the closing scenes (`CMob::Die` `0x73349`) on the same maps; the chapter cards 11–13 (`CQuestList::ClearAll` `0x7C61D` queues chapter + 10) on the map the chapter's last warp leads to (2.0, 4.1, 6.0); the map scripts' `runAnimation` (chapter 1's and 2's `PerionMap`: 15 and 20 on 1.0; chapter 3's `Humanoid3` on 8.4: 16); a scene's command 52 the next scene on its map (2 on 2.11, 17 on 8.6, 18 on 7.6, 19 on 8.4); the intro on 2.10 (`LoadGame`). **Every boss map is 30 tiles wide**, so every boss entrance and closing scene, and the chapter-2 card, is 480 points wide whatever the list says: the field there is 3:2 already, and nothing drops. (#376's note named 3.8 and the intro on 2.8; 3.8 is 30 tiles, and the intro plays on 2.10 and warps to 2.8.)

**What a wider stage changes.** Only drawing code reads the width (ADR 0090), so no scene's state changes. What is seen changes in three places:

- **Actors in the wings.** `python3 -m mtport.scenes stage` replays each scene's commands a step a frame (the hero, the scene NPCs and the scene sprites move by their speed each frame; the camera follows the hero, scrolls or is centred, clamped to the map) with a 480-point view and one of E, and lists every actor that only the wider view shows. The intro does it all through: its five NPCs wait at x 0 (and walk in from x 530–540) where the wider view's left and right columns show them before their entrances. Every other scene on a wide map shows nothing extra.
- **The speech box.** The runner's command 50 wraps the box's text at `+0x88` − 24 (`Text_Wrap` `0x22EC5`, called at `0x85158`), and `drawChatBox` `0x35189` draws the frame over `+0x88`. At E = 568 a line wrapped to 544 points runs past the box's right edge and is cut (seen on 8.4, scene 16). The port wraps the runner's call at 456 points (E − 480 less) and draws the box as the menus are drawn ([above](#the-screens-over-the-field-169)): the engine's width at 480 for the call and a host offset of (E − 480) / 2 points, so the box is the 3:2 box in the middle of the view.
- **The pictures.** A picture (commands 30–36) is a line of the scene's text drawn as an image at screen x `+0x4A0`; command 30 alone leaves it at alpha 0. Every picture a scene fades in (the intro's three captions, the chapter cards' titles, scene 16's time caption) is placed by its builder at `+0x88` / 2, read when `StartScene` builds the list. A scene queued from the field (game mode 10) is built with the listed scene's E, so the caption is centred in the wide view; the intro is built during the load, at 480.

The letterbox bars (command 1) and the fades (29, 53) fill over `+0x88`, so they cover the whole view; the five shaking scenes move the camera in `Paint` (`+0x48C` / `+0x490`), whatever the width.

**The decision** (the generated table below, `python3 -m mtport.scenes widescreen`): wide where the map is wider than 480 points and the stage shows nothing extra. `mt_widescreen_decide` keeps the field's E in game modes 9 and 10 when `CStateGame +0x1E4` is on the list (`k_wide_scenes`, `runtime/src/hooks/widescreen.cpp`; game mode 9 also needs the command list `+0x38`), else 480.

<!-- BEGIN generated:scene_stage -->
<!-- Generated by `python3 -m mtport.scenes widescreen`: do not edit by hand. -->
| Scene | Builder | Map (points) | E | Boxes | Verdict | Why | Evidence |
|---|---|---|---|---|---|---|---|
| 0 | `makeAni1` `0x302F5` | 2.10 (640; LoadGame 0x86C1D, a new game) | 568 | 4 | 480 | NPC 0 before its entrance at x 0-35, steps 36-53, only in the wider view | stage: 20 hidden sightings |
| 1 | `makeAniEndingZacum` `0x2BE55` | 10.5 (480; CMob::Die 0x73349, mob 69) | 480 | 11 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 2 | `makeAniEndingHenesis` `0x26F89` | 2.11 (928; scene 1's end (command 52)) | 568 | 5 | **wide** | nothing staged where only the wider view shows it | stage: 0 sightings, none hidden; `play.widescreen_scene_2_on_synthetic` |
| 3 | `makeAniMelodyIn` `0x2FE21` | 3.8 (480; QueueBossScene 0x80965, area 3 (MelodyMap)) | 480 | 3 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 4 | `makeAniMelodyEnd` `0x2F825` | 3.8 (480; CMob::Die, Malady (mob 64)) | 480 | 2 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 5 | `makeAniWolfIn` `0x2F319` | 4.17 (480; QueueBossScene, area 4 (WolfMap)) | 480 | 3 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 6 | `makeAniWolfEnd` `0x2EDD9` | 4.17 (480; CMob::Die, mob 66) | 480 | 2 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 7 | `makeAniYetiIn` `0x2E5E1` | 9.5 (480; QueueBossScene, area 9 (no map script names it: never played)) | 480 | 3 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 8 | `makeAniYetiEnd` `0x2E03D` | 9.5 (480; CMob::Die, the Yeti (mob 68)) | 480 | 3 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 9 | `makeAniHomunIn` `0x2DA85` | 7.4 (480; QueueBossScene, area 7 (HomunMap)) | 480 | 4 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 10 | `makeAniHomunEnd` `0x2D351` | 7.4 (480; CMob::Die, mob 67) | 480 | 5 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 11 | `makeAniChapter1` `0x2BA59` | 2.0 (816; CQuestList::ClearAll 0x7C61D, chapter 1 (portal281: 2.0)) | 568 | 0 | **wide** | nothing staged where only the wider view shows it | stage: 0 sightings, none hidden; `play.widescreen_scene_11_on_synthetic` |
| 12 | `makeAniChapter2` `0x2B65D` | 4.1 (480; CQuestList::ClearAll, chapter 2 (Pirate: 4.1)) | 480 | 0 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 13 | `makeAniChapter3` `0x2B261` | 6.0 (912; CQuestList::ClearAll, chapter 3 (Pirate_Orbis: 6.0)) | 568 | 0 | **wide** | nothing staged where only the wider view shows it | stage: 0 sightings, none hidden; `play.widescreen_scene_13_on_synthetic` |
| 14 | `makeAniBalogIn` `0x2CBD1` | 10.5 (480; QueueBossScene, area 10 (BalogMap)) | 480 | 6 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 15 | `makeAniChapter1Perion` `0x28521` | 1.0 (640; chapter 1's PerionMap, runAnimation(15)) | 568 | 7 | **wide** | nothing staged where only the wider view shows it | stage: 0 sightings, none hidden; `play.widescreen_scene_15_on_synthetic` |
| 16 | `makeAniMagatiaHumanoid1` `0x291D9` | 8.4 (720; chapter 3's Humanoid3 (an NPC on 8.4), runAnimation(16)) | 568 | 3 | **wide** | nothing staged where only the wider view shows it | stage: 0 sightings, none hidden; `play.widescreen_scene_16_on_synthetic` |
| 17 | `makeAniMagatiaHumanoid2` `0x2AAF5` | 8.6 (480; scene 16's end (command 52)) | 480 | 7 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 18 | `makeAniMagatiaHumanoid3` `0x29765` | 7.6 (480; scene 17's end (command 52)) | 480 | 9 | 480 | its map is 480 points wide: the view is 480 either way | the map's width |
| 19 | `makeAniMagatiaHumanoid4` `0x28D81` | 8.4 (720; scene 18's end (command 52)) | 568 | 3 | **wide** | nothing staged where only the wider view shows it | stage: 0 sightings, none hidden; `play.widescreen_scene_16_on_synthetic (after 17 and 18)` |
| 20 | `makeAniChapter2Perion` `0x27AED` | 1.0 (640; chapter 2's PerionMap, runAnimation(20)) | 568 | 10 | **wide** | nothing staged where only the wider view shows it | stage: 0 sightings, none hidden; `play.widescreen_scene_20_on_synthetic` |

7 of 21 scenes wide. Map: where the scene plays, its width and what starts it there. Boxes: its speech boxes (command 50). A sighting is an actor (the hero, a scene NPC, a scene sprite, a moved map object) at an x the E-point view shows and the 480-point one does not, over consecutive steps; hidden: before its entrance, never on stage, or standing where it went off.
<!-- END generated:scene_stage -->

**Measured** (`play.widescreen_scene_<n>_off_synthetic` / `_on_synthetic`, from built starts on each map: a scene queued from the field as `runAnimation` queues it, or played by the map's own script on entry). In every run the game state at every tick equals the 3:2 run's: scene 16 to 19 1,072 ticks, 15 288, 20 333, 2 595, 11 and 13 137 each, 12 137.

- Scene 16 starts from the field of 8.4 and its chain ends in 19, back in the field of 8.4: E is 568 on the last field tick and the first scene tick, and on the last scene tick and the first field tick after. 17 (8.6) and 18 (7.6) are on 30-tile maps: 480.
- 15 and 20 (PerionMap on entry to 1.0), 11 (2.0) and 13 (6.0) end in the field at 568 with no change at the edge; 2 (2.11) ends in its warp to 2.0.
- 12, unlisted, queued on 2.0: the view drops to 480 at its start and widens again after it.
- The window shots: the speech box at 480 points in the middle with its lines whole; the letterbox bars and the fades across the whole width; the chapter titles and scene 16's caption in the middle of the view.

## What stays 3:2, and why

| Screen | Why |
|---|---|
| Title, main menu, character select | not a `CStateGame`; UIKit views laid out for 480 |
| Loading (modes 6–8), the waiting box (5) | see [the table above](#the-screens-over-the-field-169) |
| 14 of the 21 scenes (modes 9 and 10) | 13 play on 30-tile maps, the intro stages actors in the wings ([below](#scenes-at-the-fields-width)) |
| The field while the touch key pad shows | the pad is UIKit at 3:2; the GL quick slots would leave its keys (#168) |

## Past 568 points (#387)

[ADR 0214](../adr/0214-widescreen-past-568-points.md) decides it. Measured 2026-11-27 on the port's own runs (the window shots local only).

**The engine has no limit at 568.** Every guest-side piece reads the configured width: the renderbuffer's storage (`eagl.cpp`), `mt_widescreen_decide`'s clamp, the backdrop's stretch k = E / 480 and the HUD's move of (E − 480) / 2 points. A local build with `MT_WIDESCREEN_WIDE` at 768 (not committed) ran `play.widescreen_on`: the view was 768 on 2.0 and 2.3 and 480 on 2.8 and 2.9, and the hero's and the mobs' state at every one of 2,985 ticks equalled the 3:2 run's. `play.widescreen_pad_on` at 768 held left, attack and right by touch, partly in the wings (touches at x −109 and 584 of the screen image): the hero equalled the off run's at all 119 ticks of the hold, and the d-pad and the right-hand groups sat at the 768-point picture's edges. Its only failure was the test's literal "groups 44 points out", which is (568 − 480) / 2.

**What bounds it is data.** How many of the 90 maps fill each width (a map fills a view E when it is at least E points wide; tile steps of 16 points):

| View width, points | Aspect at 320 high | Maps that fill it |
|---:|---|---:|
| 512 | 16:10 | 76 |
| 568 … 576 | 16:9 | 74 |
| 584 … 608 | | 70 |
| 616 … 640 | | 65 |
| 648 … 672 | | 55 |
| 680 … 704 | 19.5:9 (iPhone X and later, 693) | 52 |
| 712 … 720 | 20:9 (most Android phones, 711) | 51 |
| 728 … 736 | | 35 |
| 744 … 768 | 21:9 (747) | 32 |
| 776 … 800 | | 21 |

The tile widths behind it: 14 maps of 30 tiles, one each of 32 and 35, then 36 (4 maps), 38 (5), 40 (10), 42 (3), 44 (1), 45 (16), 46 (3), 48 (11), 50 (12), 51 (2), 54, 55 (3), 57, 58 and 60 (one each). A map narrower than the view is drawn as wide as it is, centred, with black wings beside it (today only maps 36 and 37). At 768 that is 42 maps between 36 and 47 tiles, each with its own picture width; on the 11 maps exactly 48 tiles wide the whole map fits and the camera stops scrolling sideways.

**The backdrop tiles.** The 11 backdrops (`map/back/<n>.png`, each 480 × 320) were compared column by column with themselves shifted by 32 … 400 points. Nine (0, 1, 2, 3, 4, 6, 7, 9, 10) repeat exactly every 160 points: shifted by 160 the pixels are identical, and 480 is three periods, so a copy of the image drawn at x = 480 continues it with no seam and no distortion at any width. Backdrops 11 and 12 repeat exactly in their top 96 rows and their bottom rows but not in a middle band (rows 96 … 224, mean difference up to 19 per channel), so a copy beside them shows a step there. The options, judged on window shots of 2.3 at 768:

| Option | At 568 | At 768 | Verdict |
|---|---|---|---|
| Stretch to E (today) | 18 % wider | 60 % wider: hills and trees visibly broad | fine for 11 and 12 |
| Tile by the 160-point period | exact | exact, no seam (a local hook drawing the image again at x = 480) | **best** for the nine periodic backdrops |
| Mirror at the edges | — | a symmetry axis at each edge of the 480-point image, obvious in clouds and cave roots | rejected |
| Clamp or blur the edge column | — | horizontal streaks | rejected |
| A per-area cap at 568 | — | the extra width wasted on the maps it would suit | not needed once the backdrop tiles |

**What else reads 568:** `MT_WIDESCREEN_WIDE` (the renderbuffer, the start window's size in `game.cpp`, `mt_widescreen_configure`); the `widescreen` setting (`off` / `auto`) and its comment; the comments in `keypad_layout.h` and `minimap.h` (the corner's narrowing applies only below 568, so a wider view has more room); and the real-asset tests, which use the constant except for the literal 44-point offset ((568 − 480) / 2) in `play.widescreen_pad_on` and the key pad's label check (`hud_labels`), and the unit tests of the key pad's move (`keypad_layout_test`, `hud_labels_test`, `ui_scaling_test`) that are written at 568. `gl_screen_columns`, the presenter's wings and their pass (`upload_wings`), frame interpolation (it compares each tick's view width) and the key pad's move all take the width as a value. Bag, Cash and the compass are `UIButton`s in the 480-point screen image and stay at their 3:2 places: at 768 they sit 144 points in from the picture's edges.
