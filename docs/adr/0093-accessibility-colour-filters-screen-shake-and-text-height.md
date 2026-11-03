# 93. Accessibility: colour-blind filters, screen-shake scaling and the text height

- **Status:** Accepted
- **Date:** 2026-11-02
- **Issue:** #158
- **Builds on:** [ADR 0066](0066-shader-filters-and-text-at-the-render-scale.md) (the present stage's shader passes, `hd_text`), [ADR 0085](0085-borders-around-the-picture.md) (the present stage as an ordered list of steps), [ADR 0069](0069-guest-function-hooks.md) (guest function hooks), [ADR 0070](0070-settings-registry-and-presets.md) (the settings registry; its planned table lists `colorblind_filter` / `screen_shake` as not in the presets)

## Context

#158 asks for three things that make the game playable for more people without changing it (the bar set by Celeste and Steam Deck Verified): colour-blind filters, a screen-shake option, and text at least 9 px tall at 1280×800. The game's text is drawn into fixed 512×32 textures, so bigger text would break its layouts; `hd_text` (ADR 0066) makes it sharper instead. Where the game shakes the screen was not known.

## Options considered

### Colour-blind filter

1. **Simulation** (show what a dichromat sees). That helps people with typical colour vision check a design; it doesn't help the player who is colour-blind.
2. **Daltonisation** (shift the colours a dichromat confuses into ones they can tell apart, keep the rest). This is what games' colour-blind modes do.
3. **Per-asset recolouring** (texture packs, ADR 0081). Exact but needs art for every sprite; out of scope.

Where in the present stage:

- **A. As the filter's pass** (replace `copy`/`xbr`/`crt`/`lcd`). It would rule out combining a colour filter with a look filter.
- **B. A step before everything** (step 0 of ADR 0085's list): the screen image is corrected once at its own size, and the border, the picture and every filter read the corrected image.

### Screen shake

1. **Patch the scene scripts' amplitudes** in the loaded data. The scenes are built in code (`CAniManager::makeAni*`), not data, and patching code is out.
2. **Hook the draw** (`CStateGame::Paint` `0x87501`) and scale the amplitude for the frame, putting the game's value back after it.
3. **Hook `Rand_Range`** and scale its result. It is called from many places, and the change would leak into game logic.

## Decision

Daltonisation (option 2), as step 0 (B); the shake by hooking the draw (option 2); text unchanged, the height documented.

### `colorblind_filter = off | protanopia | deuteranopia | tritanopia`

- **Setting:** Accessibility group, default `off`, not in the presets (ADR 0070's planned table: "– (off / 100)"), `--colorblind-filter`, live (`kAtOnce`: `apply_live_setting` calls `Presenter::set_colorblind`).
- **The maths** (`platform/src/color_filter.{h,cpp}`): `D = I + E (I − S(I))` in linear RGB (sRGB transfer both ways), with `S` the Machado, Oliveira and Fernandes (2009) dichromacy matrices at severity 1.0 and `E` the error-shift matrices of Fidaner, Lin and Ozguven (2005): protan and deutan move the lost red–green error into green and blue (`[[0,0,0],[0.7,1,0],[0.7,0,1]]`), tritan moves the lost blue–yellow error into red and green (`[[1,0,0.7],[0,1,0.7],[0,0,0]]`). Greys, black and white are kept exactly. Alpha is kept.
- **GPU way:** on SDL's GPU renderer, `present_colorblind.frag` (HLSL, built by `scripts/build-shaders` into the shader library) draws the screen texture through an `SDL_GPURenderState` into an image-sized target. The border's step and the picture (and its filter pass) then read that target. One draw at 480×320 (or the render scale's size).
- **CPU way:** on any other renderer, `ColorTransform` transforms the composed image before the upload: a 16-bit sRGB decode table, Q12 matrix coefficients, a 4096-step encode table, within 1 of the float reference.
- **Off:** no step, no target, no transform: the Presenter draws as before, byte for byte. `main.cpp` asks for the GPU renderer when the filter is on, as for a shader filter.
- **Read-back checks:** `Presenter::check_readback` builds its reference through the same transform and allows ±2 on the GPU way (the shader's float maths against the CPU tables).

### `screen_shake = 0..100` (percent)

[docs/re/screen-shake.md](../re/screen-shake.md) has the evidence. Only scenes shake the camera: the scene runner `0x83C71`'s command 19 sets `CStateGame +0x47E` (on) and `+0x488` (amplitude, screen points), command 20 clears them, and `CStateGame::Paint` `0x87501` draws `Rand_Range(0, amp) − amp / 2` per axis by scrolling the map's camera and back.

- **Setting:** Accessibility, 0 to 100 in steps of 10, default 100, not in the presets, `--screen-shake`, live (`mt_hooks_install_screen_shake`).
- **Hooks** (`runtime/src/hooks/builtin.cpp`, two new hookable functions in `recomp/hooks.py`): Paint scales the amplitude for its call and writes the game's value back after it. The scene runner, called inside Paint, sees the game's value, and a value its commands set is scaled for the rest of the frame. Between frames guest memory holds the game's own amplitude. Nothing is written into a state the runner deleted (the canvas's current state and the vtable pointer are checked). At 100 both are registered but off: the original.
- **Same random numbers:** `Rand_Range` `0x22641` draws one `rand()` whatever the range, so scaling the amplitude changes only the picture. `play.screen_shake_{100,50,0}` print the same `rand()` state after the intro.

### Text height at 1280×800

The text the game draws (a survey of every `draw_text` size over the intro, the field, the popup menus, the cash shop and the shell's menus) uses NanumGothic (the port's substitute for MarkerFelt-Thin and Helvetica) at 12, 13, 14, 16 and 24 points of the 480×320 screen. The smallest, 12 pt (the character card's job name), has an x-height of 6.50 pt, a cap height of 8.92 pt and a 12 pt line.

On a 1280×800 display the picture is scaled 2× with `scale_mode = integer` (2.67 × 2.5 does not reach 3) and 2.5× with `fit`. So the smallest text is 17.8 px to the cap and 13.0 px to the x-height with integer scaling, 22.3 px and 16.3 px with `fit`, well over the 9 px bar. With `hd_text` (render scale 2 or more) the glyphs are rasterised at 2× instead of upscaled: the same height, sharp edges. The text's size is unchanged, so no layout moves.

## Consequences

- A colour-blind player picks a filter in the settings menu and sees it at once; it combines with every look filter, every scale mode, render scale and border.
- Cost (Release, Apple M4): the GPU way adds at most 0.105 ms of GPU time per frame (render scale 3, 2560×1080) and 0.054 ms at 1080p, render scale 1 (`gfx_gpu.present colorblind: GPU time ...`); in the game's own frame split, upload and draw go from 0.23 to 0.32 ms per frame. The CPU way takes 0.80 ms at 480×320 and 2.47 ms at 1440×960 (render scale 3); under the 1 ms bar only at render scale 1, so the GPU way is the one to use when a filter is on (it is asked for).
- The shake option scales the scenes' shake from none to the original's; it cannot make it bigger than the original. Gameplay is untouched.
- Two more hookable functions (`0x83C71`, `0x87501`). One handler per function (ADR 0069): another feature that wants to hook `CStateGame::Paint` must share this handler.
- Text stays at the game's sizes; a larger-text option would need the game's layouts rebuilt and is not planned.
- Tests: `platform_test`'s `colour-blind filter: ...` and `settings: colorblind_filter and screen_shake (#158), ...` (CI); `gfx_gpu.present colorblind: ...` and the colour-blind part of `GPU presentation: a shader filter's pass draws the composed screen` (needs a GPU); `runtime_test`'s `hooks: screen_shake ...`; `tests/test_screenshake_names.py`; `play.screen_shake_{100,50,0}` and the game's read-back under every filter on both ways (real assets, local).

## Addendum: with #86's widescreen (2026-11-02)

Merged after [ADR 0090](0090-widescreen.md): step 0 runs after the wide placement and grades the 3:2 screen image only; the wings (the GL frame's points beside it) are drawn ungraded until #170. Filter off and widescreen off leave the Presenter's output byte-identical (`platform.border: color with black is byte-identical…` and "colour-blind filter: off is byte-identical to the Presenter before #158"). The settings menu's tab bar gains Accessibility with these rows, which changes the menu's drawing hashes (`settings_menu_test.cpp`).

## References

- G. M. Machado, M. M. Oliveira, L. A. F. Fernandes, "A Physiologically-based Model for Simulation of Color Vision Deficiency", IEEE TVCG 15(6), 2009.
- O. Fidaner, P. Lin, N. Ozguven, "Analysis of Color Blindness", Stanford EE368 project, 2005 (the daltonisation error matrices).
- [docs/re/screen-shake.md](../re/screen-shake.md).
