# 90. Widescreen: the engine's view width set per frame, the field up to 568 points

- **Status:** Accepted
- **Date:** 2026-11-02
- **Issue:** #86
- **Builds on:** [ADR 0069](0069-guest-function-hooks.md) (guest function hooks), [ADR 0070](0070-settings-registry-and-presets.md) (the settings registry), [ADR 0040](0040-gles-fixed-function-pipeline.md) (the GLES pipeline), [ADR 0042](0042-frontend-game-loop-and-presentation.md) and [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (presentation and placement). The facts are in [docs/re/widescreen.md](../re/widescreen.md).

## Context

The issue asks for a wider view "where the engine allows it", and for the toggle to change no gameplay. If the engine doesn't allow it, the issue asks for a written analysis instead. The facts that decide it ([re/widescreen.md](../re/widescreen.md)):

- The screen width has one source, `Screen_Init(480, 320)` `0x9909`. It lives in the screen surface `0x11AA88`, the global `0x11AA8C` and `SkyCanvas +0x88` / `+0x90`.
- 18 functions read the width. All of them draw: the camera clamp `0xC069` and follow `0xC175`, culling, the clip, fills, the HUD and menus. **No update function reads it, and none reads the camera** except the cosmetic damage-number spawns. Culling makes no `rand` call.
- The sprite draw `0x7CCD` clips every quad **on the CPU** to the surface's width and the canvas's clip rect. Widening GL alone shows nothing new; the engine itself has to think the screen is wider.
- `setupView` takes the viewport and the projection from the renderbuffer's size.
- 74 of the 90 maps are at least 36 tiles (568 points) wide. 14 are exactly 30 tiles (480), and two are in between.
- Menus, popups, the shop and the map screen are laid out for 480 points, some with the literal 480.

What had to be decided:

- how the engine learns the wider width;
- when it is wide;
- what fills the backdrop;
- how the host shows a frame wider than the UIKit screen;
- how the key pad fits;
- how to prove no gameplay changes.

## Options considered

How the engine draws wider:

1. **A GL-only stretch or a wider projection.** Rejected: `0x7CCD` clips to 480 on the CPU, so the extra columns stay black (or the picture is distorted).
2. **Call `Screen_Init` with 568.** Rejected: one width for the whole game. The menus with a literal 480 would sit at the left of a 568 screen, and narrow maps would show past their edge (the clamp sets the camera to 0 when the map is narrower than the screen).
3. **Set the engine's width per frame. Chosen.** The host writes a view width E into the four places `Screen_Init` wrote, at each run-loop turn and in hooks on the two camera functions, which run before anything reads the camera. E is the map's width clamped to 480 … 568 in the field (`CStateGame`, game mode 0 or 4, a map loaded) and 480 everywhere else. The renderbuffer is always 568 points wide when the setting is on, so the projection never changes; at E = 480 the engine just draws the first 480 columns.

The backdrop:

1. **Leave it.** The sky is a fixed 480 × 320 image drawn at (0, 0), so the wings would show the previous frame (the game never clears). Rejected.
2. **Draw it twice, or mirror it.** Seams in a painted sky. Rejected.
3. **Stretch it to E. Chosen.** A hook on `DrawImage` `0x24DC9` matches only `CStateGame +0x454` at (0, 0) and draws it with a host clip-space x transform (`mt_gles_set_host_x_transform`, x' = k x + (k − 1) w, k = E / 480) that the guest never sees. A sky stretched by up to 18 % reads as the same sky.

Presentation:

1. **A 568-point UIKit screen.** Every overlay, the input mapping, the screenshots and the golden images assume 480 × 320. Rejected for this issue.
2. **The 480-point screen image in the middle of a 568-point picture. Chosen.** The compositor takes the GL frame's centre 480 points (`gl_screen_columns`), so the UIKit screen, its key pad and everything that reads the screen image stay as they are. The Presenter places a 568-point picture (`place(…, screen_w = 568)`), draws the GL frame's first E points centred in it (the wings), then the screen image over its middle 480 points (`wide_part`). The read-back check skips the wing columns.

The key pad: the GL quick slots follow `+0x88` to the right edge, while the UIKit pad stays in the 3:2 middle. The frontend therefore allows the wide view only while the touch key pad is hidden (`hide_keypad`, a pad or keys in use). Moving the UIKit pad to the edges is a follow-up (#168).

## Decision

- **Setting.** `widescreen = off | auto` (Video; Original off, Original + controller off, Enhanced auto; `--widescreen`; next start). With `off`, no hook is registered, the renderbuffer is 480 points, and the game runs exactly as before.
- **Runtime** (`maplethief/widescreen.h`, `runtime/src/hooks/widescreen.cpp`):
  - `mt_widescreen_configure(568)` registers the hooks on `0xC069`, `0xC175` and `0x24DC9` (added to `recomp/hooks.py`).
  - `mt_widescreen_turn(cpu, allow)` decides and applies E once a turn.
  - The camera hooks decide and apply before the original runs.
  - `mt_widescreen_apply` writes the surface's width and row bytes, `0x11AA8C`, `+0x88`, `+0x90`, and the clip's x1 when the clip covers the whole old width.
  - Every read checks that the image maps the globals, so a synthetic guest is never read.
- **GLES.** `eagl.cpp` gives the 480-point layer `mt_widescreen_width()` points of storage. `fixed.cpp` applies the host x transform after the guest's matrices.
- **Platform.**
  - `Screen` and `GpuScreen` record the view at each GL frame and show its centre 480 points.
  - The Presenter has a wide placement and a wings texture: the GPU path's own GL texture, or an upload of the read-back pixels.
  - `game.cpp` configures the width before the boot and calls the turn after the key pad's turn.
- **Proof.** `play.widescreen_off` / `play.widescreen_on` (real assets, local) play the same route with the same input and compare the hero's, the mobs' and the game mode's state at every game tick. Unit tests: `widescreen_test.cpp` (the decision, the writes, the hooks on a synthetic guest) and `scaling_test.cpp` (the wide placement and the GL columns).

## Consequences

- The field is 16:9 on 74 maps and wider than 3:2 on two more. Everything else is 3:2 with black wings, as before.
- The gameplay is unchanged by construction: only drawing code reads the width. On real assets, 2,985 ticks matched across four maps.
- The UIKit key pad and the GL HUD disagree when both show. For now the wide view needs the pad hidden; #168 anchors the pad to the edges.
- The wings get no present-pass filter; the screen image does. With a filter that changes colours (scanlines, an LCD grid) the wings look different. #170 applies the pass to the whole picture.
- Menus, popups and the map screen stay 3:2. Widening them would mean rewriting their layout (#169).
- If a future hook or enhancement reads `+0x88` or `0x11AA8C` in update code, it sees E and not 480. Such code must use `MT_WIDESCREEN_NARROW` for the game's logical width.
- iOS: a 568-point layer on a phone needs the safe area and the UIKit pad at the edges; until #168 lands, `auto` behaves like `off` while the pad shows.

## Addendum: with #157's borders and #89's gear (2026-11-02)

Merged with [ADR 0085](0085-borders-around-the-picture.md)'s ordered present steps, the wings belong to step 3, the picture: the clear in the border colour, then the border (color, blur or image), then the wings into the wide picture's centre `view` points, then the screen image into the placement (through the shader or filter pass, which the wings do not take yet: #170). The blur is still stretched about the 3:2 screen image's rectangle, which has the wide picture's centre, so it covers the drawable around the wide picture in the frame's own aspect. With widescreen off nothing changes, so `border = color` black stays byte-identical to the drawing before #157. The read-back keeps both rules: the bars are checked only under `border = color`, and the wings are never a bar. `Presenter::wide_placement()` is the placement with the wide picture's rectangle. The settings menu's gear (ADR 0076) is placed against it first (`gear_place(image, wide, density)`, settings_menu.h), in the bar beside the wide picture or above it, clear of the wings. On a 16:9 window no such bar fits, so it falls back to the 3:2 screen image's placement, as before #86: beside the image, over the wings' top corner, never over the screen image, and still shown on a desktop. Focus frames, prompts and the price line stay on the 3:2 screen image's points. CTest `platform.border: widescreen's wings go over the border, beside the picture, never a bar` and the widescreen block of `platform.settings menu: the gear's place; the mouse wheel and the list's arrows scroll`.

## Addendum: with #158's colour-blind filter (2026-11-02)

The colour-blind filter ([ADR 0093](0093-accessibility-colour-filters-screen-shake-and-text-height.md)) is step 0 of the present steps, before the clear: it grades the 3:2 screen image (the CPU way at upload, the GPU way through its pass into an image-sized target), and the border's blur and the picture read that graded image. The wings are drawn from the GL frame and are **not** graded: with `widescreen = auto` and a filter on, the 88 points beside the image show the original colours. Grading the wings is #170's (the same pass over the wings' texture). The read-back check compares the 3:2 image against the CPU-filtered reference (within 2 on the GPU way) and skips the wing columns, as before.

## Addendum: the screens over the field (#169, 2026-11-02)

[#169](https://github.com/armaatus/maplethief-port/issues/169) asked, for each screen that stayed 3:2, to choose between keeping it 3:2 with the frozen field dimmed in the wings, centring it over the wide field with a host x offset of (E − 480) / 2, or widening it. The per-mode facts and choices are in [re/widescreen.md](../re/widescreen.md#the-screens-over-the-field-169).

**Decision.** The popup menu (mode 1), the NPC shop (2), the map screen (3) and the cash shop (0xB) are **centred over the wide field**, behind the same `widescreen = auto` (no new setting):

- `mt_widescreen_decide` keeps the field's E in these modes, so the field is drawn wide behind them, and the camera doesn't move when one opens.
- Five more hooks (`recomp/hooks.py`): the popup menu's paint `0x56B99`, the shop's `0x56E91` and the map screen's `Map_PaintSub0/1/2` `0x59BB1` / `0x59119` / `0x58AE9`. Each wraps the original (`runtime/src/hooks/widescreen_menus.cpp`): the engine's width words back to 480 for the call, and a host clip-space x offset of (E − 480) / 568 (`mt_gles_set_host_x_transform`). Its draws land in the GL frame's centre 480 points, which is the screen image. Afterwards the words are restored.
- A sixth hook, on `FillRect` `0x8079`, stretches the popup menu's dim (its fill of (0, 0, 480, 320) at alpha 100) over the whole view with the backdrop's transform, so the wings are dimmed like the middle. Outside the popup menu's paint it passes.
- The cash shop is a UIKit panel: nothing to wrap.
- Inside a screen's paint the camera and backdrop hooks leave the width and the transform alone.

**Why centring and not the other two.** Widening means rewriting the layouts: every piece is at a fixed x or centred on 480. Keeping 3:2 means drawing the field and the screen at different widths in one frame, which is what centring does anyway. Centring is also the only choice that keeps input working without new code. The GL touch widgets hit-test fixed 480-point rects, and the UIKit panels (market, map, cash shop) sit in the 480-point screen image. The screen image shows the GL frame's centre 480 points (`gl_screen_columns`, (E − 480) / 2 from the left), so the offset puts every draw exactly where its touch target already is. The shop and the map screen have no dim of their own: their wings show the field plainly, as the field shows around their windows at 3:2.

**What stays 3:2.** Loading (6–8): `Paint` returns before the field, so there is nothing to show beside it. The scripted modes 9 and 10 (fades, speech box, letterbox bars, a scene): their scripts place the actors and the camera for a 480-point stage, and widening them needs a check of each scene. Mode 5 (a waiting box) is reached by no scripted run, so it stays at the safe default.

**Proof.** `play.widescreen_off` / `_on` open the four screens in Henesys with the same input and touch the popup menu's item tab at (156, 277). The state is still identical at every one of 2,985 ticks. The tab opens in both runs. The popup window's opaque top bar has the same pixels in the composited screen in both runs. The screens are drawn wide (137 paints centred, 52 dims stretched), and the window shots are local only. Unit tests: `widescreen_test.cpp` (the decision per mode; the words and the clip a paint sees and gets back; the offset; the dim's stretch only inside the popup menu; nothing at 480 or off).

## Addendum: the touch key pad in the wide view (#168, 2026-11-02)

**A correction.** The context above says the GL quick slots and the bar follow `+0x88`. Measured on real assets ([re/widescreen.md](../re/widescreen.md#with-the-touch-key-pad-168)), they do not: `HUD_Paint` draws the bar, the quick slots and the attack key's icon at fixed 480-point places, the left 480 points of a wider view. The wide view with the pad hidden had its bar 44 points left of the middle.

Decided:

- **The HUD in the middle.** A hook on `HUD_Paint` `0x5A465` (`MT_HOOK_FN_HUD_PAINT`, registered by `mt_widescreen_configure`, on with widescreen) runs it with `+0x88` / `+0x90` at 480 / 240 and moves its draws right by (E − 480) / 2 points with the host x transform. The HUD is then where it is on the 3:2 picture, with or without the pad.
- **The pad's groups at the edges** (`widescreen_hud = edges`, the default). `keypad_layout`'s transforms gain a widescreen move (`KeypadWide`): the d-pad −(E − 480) / 2, jump / attack and the quick slots +(E − 480) / 2. The quick slots' draw filter (ADR 0086) moves the icons with their groups, and the hit mask hook reads under the original key. `widescreen_hud = narrow` keeps the pad and the HUD at their 3:2 places.
- **Map, Bag and Cash stay.** They are `UIButton`s hit-tested at their frames in the top right, clear of the moved groups. Moving them would need another hit-test path for no gain.
- **The compositor stays 480 points; panel 0 is drawn in the wings.** Widening the UIKit screen to E would change every overlay, the input mapping, the screenshots and the golden images. Instead `mt_uikit_composite_view` composites panel 0 alone into two strips, (E − 480) / 2 points each, left and right of the screen; the Presenter draws them over the wings as step 4 of the picture (premultiplied blending). The screen image itself shows the parts of the moved groups that stay on it.
- **Touches in the engine's x.** `mt_uikit_set_host_touch_area(panel 0, off, off, off)` reports the game pad's own touches to the guest at x + (E − 480) / 2, the x the engine draws at, so `TouchKeyPadView`'s NPC check (`npcPosition` is in engine points) and the GL widgets agree with the picture. A touch may begin in either wing and then belongs to the pad (`Input::set_side_margin` lets the window's touches through there). Map, Bag, Cash and every other view keep the screen's points. Only while panel 0 is the panel in use.
- **The key pad hidden no longer matters.** `game.cpp` calls `mt_widescreen_turn(cpu, 1)` and then `keypad_widescreen_turn` every turn while widescreen is configured.

With `widescreen = off` nothing is registered, no touch area or strip exists, and every frame is as before. Evidence: `play.widescreen_pad_off` / `play.widescreen_pad_on` (real assets, local; the hero's state equal at every tick, the shots `pad_*.png`), the unit cases in `keypad_layout_test.cpp`, `uikit_test.cpp` (`compositor: a view composited alone beside the screen, and the host's touch area`), `input_test.cpp` and `widescreen_test.cpp`.

The consequence above, "the wide view needs the pad hidden", no longer holds. On iOS the pad at the edges of a 16:9 phone still needs a check of the safe area and finger feel on a device (#168's needs-human part).

## Addendum: the key pad at the edges with the screens over the field (#168 with #169, 2026-11-02)

Merged, #169 keeps E wide while the popup menu, the shop, the map screen or the cash shop is open, and #168 moves the game pad (panel 0) to the edges and gives its touches the engine's x. A screen takes its touches on its own panel (1, 6, 8, 9) in the screen image's points, and its paint runs at 480 with the (E − 480) / 2 offset. So `keypad_widescreen_turn` installs the wide move **unanchored** whenever the game pad is not the panel in use: no touch shift, the hit mask read at the screen image's points (also for an alert's invisible pad over the field), the groups and their quick-slot icons at their 3:2 places with the centred HUD, and no wing strips. Back on panel 0 the pad goes to the edges again. `HUD_Paint`'s hook passes inside a screen's paint (`menu_active()`, and the width there is 480 anyway), so nothing is moved twice. Unit test: `keypad layout: widescreen's turn: the engine's x to the mask, the views, the wings` (the panel-1 block).

## Addendum: the wings through the present pass and the filters (#170, 2026-11-02)

Until now the wings were drawn from the GL frame through SDL's own texture shader, beside the screen image, and the screen image alone took the colour-blind grade ([ADR 0093](0093-accessibility-colour-filters-screen-shake-and-text-height.md)) and a shader filter's pass ([ADR 0066](0066-shader-filters-and-text-at-the-render-scale.md)). With a filter on, the 88 points beside the image looked different, and the pass's edges showed as a seam.

**Options.**

1. *Draw the wings through the pass too, with its sizes and a texel offset for the wide picture.* Rejected: two draws are never one image at their shared edge. xbr reads the neighbours across the edge, and each draw would read its own texture's edge, clamped. crt blends neighbouring texels along the row and lays its 3-pixel aperture mask from each draw's own left edge. The scale mode's linear and sharp filters clamp at each texture's edge as well. The grade would need a second target for the wings, and #168's pad strips, blended over the wings, would have to be graded before the blend.
2. **Compose one wide picture and run the grade and the pass once over it. Chosen.** It costs one extra draw into a target of `view x 320` points, and nothing changes for the pass's shaders: their sizes are the picture's texels.

**Decision** (`Presenter::draw`, `compose_picture`; the step list in `platform/src/border.h`):

- With widescreen and the wings drawn, a new first step composes into an opaque render target of `view x 320` points at the screen texture's texels a point. It holds the GL frame's first `view` points texel for texel, the screen image over the middle 480 points, and #168's pad strips over the wings (premultiplied, only their points inside the view).
- Step 0's grade then runs over that picture (the GPU way, into a target of its size), and step 2's blur reads it about its rectangle. Step 3 draws it into the wings' rectangle, `wide_part(wide_rect, wide, view)`, through the scale mode's filter or the pass, with `PresentSizes` = the picture's texels and that rectangle.
- Step 4 now draws only the strips' points beyond the view (a view narrower than the wide picture), over the bars as before. The pad's anchored move never puts a key there.
- The colour-blind filter's CPU way (a renderer without our shaders) grades the wing columns in `upload_wings` and the strips in `upload_pad_wings` (un-premultiplied, graded, premultiplied again), as it grades the screen image in `upload`. GPU presentation's wings are a GPU texture that the CPU way can't grade. GPU presentation always has SDL's GPU renderer, so it takes the GPU way.
- Without wings (widescreen off, a narrow view, a minimised window) nothing is composed: the same draws as before. A renderer without render targets keeps #86's two draws (logged once).
- `check_readback` reads the composed picture back. Its screen columns must equal the uploaded image exactly (graded on the CPU way). Its wing columns must equal `upload_wings`' GL frame with the pad's strips over them, within ±2 (blending). GPU presentation's wings are not checked against a source, since their pixels never reach the CPU, and the report says so. The output is then judged against the whole picture by the existing rules over the wings' rectangle (the scale mode's exact or blended check, the pass's `PassCheck`, the CPU-graded reference within ±2 under the GPU way). The bars outside it are checked as before.

**Consequences.**

- The wings get exactly the screen image's grade and filter. Every evidence below compares against the frame drawn as one 568-point picture through the same pass and finds it **byte for byte equal**:
  - CI (software renderer): `platform.border: widescreen's wide picture is one image, in every scale mode and graded (#170)` and `platform.border: the wide picture holds the key pad's strips, graded, and check_readback sees the wings (#170)`. The second also covers the narrow 520-point view with strips, and a changed wing texel or screen texel caught by the check.
  - GPU (local): `gfx_gpu.present widescreen: the wide picture through every pass and the grade is one image` (128 draws: none, xbr, crt, lcd × the four scale modes × colour-blind off/on × render scales 1 and 3 × 16:9 and 21:9) and `gfx_gpu.present widescreen: the key pad's strips and a narrow view in the wide picture`.
  - Real assets (local): `play.widescreen_filters` draws the frame on 2.3 with the key pad at the edges through every filter, colour-blind off and on. Every draw read back with 0 mismatches over the whole wide picture, and every one equals the one picture. #86's way, drawn alongside, differed from it in 24,072 (xbr) to 317,554 (crt) pixels, about 11,000–14,000 of them within 6 pixels of the wings' edges. It was identical only with no filter and no grade at integer scale.
- GPU cost (Release, Apple M4, fence-timed upper bounds, median of 60, on a machine under a load average of about 17): what the wide picture adds to a draw, against the same wide placement without wings:

  | Output, render scale | copy | copy + colour-blind | crt | crt + colour-blind |
  |---|---|---|---|---|
  | 1920 × 1080, 1 | 0.050 ms | 0.067 ms | 0.033 ms | 0.044 ms |
  | 1920 × 1080, 3 | 0.069 ms | 0.122 ms | 0.103 ms | 0.136 ms |
  | 2880 × 1800, 1 | 0.007 ms | 0.022 ms | 0.065 ms | 0.078 ms |
  | 2880 × 1800, 3 | 0.063 ms | 0.106 ms | 0.139 ms | 0.163 ms |

  `gfx_gpu.present widescreen: the wide picture's GPU time at 1080p and 1800p (under 1 ms)` asserts the 1 ms bar. The CPU way's extra grading is the wing columns (88 of 568 points) and the strips, about a fifth of the screen image's 0.80 ms at render scale 1 (ADR 0093).
- The blur under widescreen with wings now continues the wide picture (it was the 3:2 image's). Beside a 16:9 picture on a 16:9 window there are no bars, so that only shows on wider windows.
- With widescreen off nothing changed: `platform.border: color with black is byte-identical to the Presenter before #157, every mode` and `platform.colour-blind filter: off is byte-identical to the Presenter before #158` pass unchanged.
