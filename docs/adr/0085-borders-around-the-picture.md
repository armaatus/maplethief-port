# 85. Borders around the picture: the colour, the game blurred, or the player's art

- **Status:** Accepted
- **Date:** 2026-10-31
- **Issue:** #157
- **Builds on:** [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (the placement, `border_color`, the read-back check), [ADR 0060](0060-gpu-rendering-backend.md) (the present stage), [ADR 0066](0066-shader-filters-and-text-at-the-render-scale.md) (the filters' passes), [ADR 0068](0068-gpu-presentation-without-read-back.md) (GPU presentation's screen texture), [ADR 0070](0070-settings-registry-and-presets.md) (the registry, Enhanced's `blur`), [ADR 0080](0080-render-scale-auto-and-frame-budget-guard.md) (the screen texture resized while the game runs)

## Context

The game's screen is 480 × 320 points (3:2). On a 16:9, 16:10 or ultrawide window the placement (ADR 0043) leaves bars, filled with `border_color` (black by default). #86's research found an extended camera too risky: 16 of 90 maps are under 36 tiles wide, the backdrop is a fixed 480 × 320 image, and the HUD and UIKit views are laid out for 480. #157 makes the bars look intentional instead, with no gameplay change:

- `border = color | blur | image`. Blur is the current frame stretched and blurred; image is a PNG the player chooses.
- It has to work in every scale mode, on both rendering backends, under the shader filters, with `render_scale = auto` changing the screen texture's size while the game runs (#152), and in fullscreen (#145).
- The port's own overlays (the frame-time overlay, the price line #156, prompts #146, the settings menu's gear #89) sit in the bars and must stay readable.
- `border = color` with black has to stay byte-identical to the drawing before #157, and the blur must cost under 1 ms a frame at 1080p on the Mac.

What exists:

- `Presenter::draw` clears the drawable to the border colour, then draws the screen texture into the placement, through SDL's texture shader or a filter's pass (an `SDL_GPURenderState`, SDL's GPU renderer only).
- The window's renderer is SDL's default (Metal on the Mac, Direct3D on Windows, Vulkan or OpenGL on Linux) with `gfx_backend = software`, and SDL's GPU renderer with `gpu` or a filter. Both support render-target textures, bilinear filtering and colour modulation; so does SDL's software renderer (the dummy driver's, CI's).
- The screen texture is a streaming texture (the CPU's composite, uploaded) or, with GPU presentation, a render target that `GpuScreen` composes. Either can be drawn from.
- The pinned stb_image (ADR 0003) decodes PNGs in the runtime, as a private static copy.

## Options considered

1. **Where the blur runs**
   1. **A chain of render targets on the window's renderer, with SDL's own bilinear filtering.** Halve the screen texture level by level (bilinear at 2:1 is the mean of the 2 × 2 texels a pixel covers), then double back up a few levels (bilinear at 1:2), and stretch one level over the drawable. No shader, no read-back, any renderer with render targets. Chosen.
   2. **A present pass with a Gaussian shader** (ADR 0066's path). Only with SDL's GPU renderer, so `gfx_backend = software` without a filter (the default) would need a CPU fallback or a plain colour. It also needs a shader build and the descriptor-set checks for three shader formats. Rejected: option 1 is the same on every renderer, and fast enough.
   3. **On the CPU, from the composited screen.** GPU presentation has no CPU copy of the screen (ADR 0068 avoids the read-back), so this works only on the read-back path. Rejected.
2. **How smooth the blur is**
   1. Stretch the smallest level (32 texels wide) straight over the window. One bilinear stretch of 16 to 60 times shows its piecewise-linear diamonds. Rejected.
   2. **Double back up to about 128 texels wide, then stretch.** Repeated 2× bilinear steps approach a quadratic B-spline, so the result is smooth (the read-back test checks it is monotone across a red–blue edge). Two or three extra small draws. Chosen.
3. **Where the blurred frame goes**
   1. **The frame scaled about the picture's centre until it covers the drawable** (`cover_about`). The bars show the picture's own edges, continued. Chosen.
   2. The frame fitted to the whole drawable, ignoring the picture. Its edges don't line up with the picture's. Rejected.
4. **The art's decoder**
   1. **stb_image, a private copy in `border.cpp`**, with limits checked before decoding: the file at most 32 MiB, the header read first (`stbi_info_from_memory`), each side at most 8192 (`STBI_MAX_DIMENSIONS`), at most 7680 × 4320 pixels, and no larger than the renderer's largest texture. Chosen: pinned, already used on the runtime's PNGs, PNG only.
   2. SDL 3.4's `SDL_LoadPNG`. A second PNG decoder in the program, without a way to refuse a size before decoding. Rejected.

## Decision

- **Settings** (Video, game only; `settings.h`'s `BorderMode`):

  | Key | Values | Original | Original + controller | Enhanced |
  |---|---|---|---|---|
  | `border` (`--border`) | `color` (default), `blur`, `image` | `color` | `color` | `blur` |
  | `border_image` (`--border-image`) | a PNG file; empty: none | not in the presets | | |

  `border_color` stays. It is what `color` draws, and what lies under transparent art and the minimised window. A relative `border_image` is from the working directory, as `texture_pack` (#153).
- **The present stage is an ordered list of steps** (`Presenter::draw`, `border.h`), each drawn over the last:
  1. the clear in the border colour (every mode; a minimised window stops here);
  2. the border's step (`Border::draw`): the blur or the art over the whole drawable, nothing for `color`;
  3. the picture, as before: the screen texture into the placement, through SDL's shader with the scale mode's filter or a filter's pass.

  Then the window-only overlays (the focus frame, the frame-time overlay and its notice, the price line, the settings menu). #158's colour filters add a step to the same list.
- **The blur** (`Border::draw_blur`):
  - Levels (`blur_chain`): half the screen texture (rounded up), then half the last, until a level is at most 32 texels wide (`kBlurBaseWidth`). That is 30 × 20 at render scale 1, about 16 screen points a texel. The level stretched over the window is the first at most 128 wide (`kBlurTopWidth`): 120 × 80 at render scale 1. At render scale 3 the chain is 720 × 480 down to 23 × 15, drawn from 90 × 60.
  - Each frame: the screen texture is drawn into level 0 with linear filtering (its own scale mode is put back after), each level into the next, then each level from the base back up into the one above it, up to the top. All targets are `SDL_BLENDMODE_NONE`, `SDL_SCALEMODE_LINEAR`. The renderer's target before the chain (the window, or a test's or a capture's target) is put back.
  - The top level is drawn over `cover_about(placement)` with a colour mod of 150/255 (`kBlurDim`, about 59%), opaque. Dimming keeps the picture's edge visible and the overlays (which draw their own dark boxes) readable over bright scenes.
  - The levels follow the screen texture's size: a new render scale (#152) remakes them at the next draw. They are freed when the mode leaves `blur`.
  - A renderer that cannot make a render target keeps the border colour, logged once (`present: border blur: no WxH render target (...)`).
- **The art** (`load_border_image`, `decode_border_image`, `Border::ensure_art`):
  - It is read at the first draw that needs it. A failure is logged once with its reason (missing, not a PNG, over 32 MiB, too large, broken, larger than the renderer's textures) and keeps the border colour until the file or the renderer changes.
  - It is drawn with linear filtering into `cover_rect(drawable, art)` (the art's aspect ratio, covering, centred, cropped on one axis), blended with straight alpha over the border colour.
  - The texture stays across a new render scale (`Presenter::start` on the same renderer). A new renderer reads the file again.
- **The read-back check** (`--frames`, `Presenter::check_readback`): with `color` (or a fallback to it) unchanged, every bar must have the border colour. Under the blur or the art the bars are not checked, and the report says so (`bars #000000 under the border blur (not checked)`). The picture is checked as before.
- **The settings menu (#89)** can make both rows live with one case in `apply_live_setting`: `Presenter::set_border(s.border, s.border_image)`. The rows stay `Applies::kNextStart` until it does.

## Consequences

- **Evidence: CI (synthetic; `platform.border: *`, `platform.settings: border ...`, `tests/cpp/border_test.cpp`, SDL's dummy driver and software renderer):**
  - `cover_rect` and `cover_about` cover the output: 16:9 art over 21:9, 4:3 over 16:9, the integer 3× placement at 1080p (scaled by 4/3 about the centre), the 21:9 fit, an off-centre placement, and empty sizes.
  - The blur's levels at render scales 1, 3 and 8, and for a 1 × 1 and an empty source.
  - The decoder: a 64 × 36 PNG decodes to its pixels from memory and from a file. Refused with the reason: empty, a JPEG, the signature alone, half a file, garbage after the signature, scrambled image data, headers of 8193 × 10, 8192 × 8192 (over 8K UHD), 0 × 10 and a 7680 × 4320 header with no data, a 32 MiB + 1 file, a missing file and an empty path.
  - `border = color` with black is byte-identical at 1920 × 1080, 2560 × 1080, 2560 × 1440 and 1440 × 900 in every scale mode: a Presenter that never had a border set, one set to `color` and one set back to `color` after a blur frame read back the same bytes. Integer mode's bytes equal a reference built from the image (black bars, each texel k × k pixels), and `check_readback` checks every bar with 0 mismatches.
  - The blur at the same four sizes, every scale mode, render scales 1 and 2: the left bar is the frame's red side at 150 ± 4, the right bar its blue side; no bar pixel is brighter than the dimmed frame or has the border colour; along a row above the picture (integer mode) red falls and blue rises monotonically, over more than 1/32 of the width. The picture passes `check_readback`, the screen texture's scale mode is the placement's again, a caller's render target is kept, and a new render scale remakes the levels (4, then 6).
  - The art at the same sizes and modes: the bars show the PNG's left and right colours where they are opaque and the border colour where the art is transparent. A missing file, a file that is not a PNG and an empty path keep the border colour with every bar checked. A new render scale keeps the texture (the file removed in between); a stopped Presenter reads the file again.
  - The settings rows: parsing, bad values, a path with control characters or over 4096 bytes, empty as none, the round trip, the flags; `presets_test`'s columns.
- **Evidence: local, with a GPU** (`gfx_gpu.present border: *`, Apple M4, Metal, the real `Presenter` on an offscreen SDL GPU renderer):
  - The blur under every filter's pass (`none`, `xbr`, `crt`, `lcd`) in every scale mode at 1920 × 1080 and 2560 × 1080: the bars are the dimmed frame's two sides, the picture passes each filter's check, and the test's render target is kept.
  - GPU time (fence-timed upper bounds, median of 60, a Debug build): what the blur adds to a draw of the picture.

    | Output | render scale 1 | render scale 3 |
    |---|---|---|
    | 1920 × 1080 | 0.39 ms | 0.54 ms |
    | 2560 × 1080 | 0.37 ms | 0.54 ms |
    | 3840 × 2160 | 0.26 ms | 0.50 ms |

    The test asserts under 1 ms. Most of it is the per-render-pass cost of 6 to 9 small target switches, not pixels: the 4K output costs no more than 1080p.
- **Evidence: local, real assets** (Release, arm64, Apple M4; `MapleThief --frames 300 --tap 3,106,165 --capture DIR`, a fresh data directory; the shots stay local):
  - Windows of 1280 × 720 points (16:9, a 2560 × 1440 drawable) and 1680 × 720 (21:9, 3360 × 1440). `color`, `blur` and `image` (a synthetic 1920 × 1080 PNG) in integer mode on both backends; `blur` in fit, smooth and sharp on the software backend; `image` in fit, smooth and sharp on the GPU backend; `blur` under `--filter crt` on the GPU backend; `blur` at render scale 3. 28 runs, every one reaching the character screen.
  - Every run's last frame reads back with 0 mismatched: `color`'s bars checked as before (`bars #000000: 0 mismatched`), the others' picture checked (`bars #000000 under the border blur (not checked): 0 mismatched`). Every GPU-backend run's composed screen is within ADR 0060's tolerance of the CPU's composite.
  - The log names the path: `present: border blur on the metal renderer: the 480x320 frame down to 30x20 in 4 halvings, up to 120x80, stretched over the window at 150/255` (`gpu renderer` with `--gfx-backend gpu`); `present: border image '.../art.png': 1920x1080, scaled to cover the window`.
  - Looked at privately: the blurred bars continue the picture's colours (the title's sky and grass, the character screen's greens) with no visible blocks, and the art fills the bars in each mode.
  - **#137's frame split** at a 1920 × 1080 drawable (960 × 540 points at 2×), real time, 600 window frames, three rounds alternating `color` and `blur` (the machine also ran other workers' builds). The blur is in "upload and draw":

    | backend | `color` | `blur` | added |
    |---|---|---|---|
    | software | 0.11 / 0.16 / 0.45 ms | 0.30 / 0.39 / 0.66 ms | 0.19 / 0.23 / 0.21 ms |
    | gpu | 0.00 / 0.00 / 0.00 ms | 0.06 / 0.15 / 0.07 ms | 0.06 / 0.15 / 0.07 ms |

    SDL_RenderPresent's share did not change (0.06–0.25 ms either way). With the GPU time above (at most 0.54 ms), the blur stays under 1 ms a frame at 1080p.
- **Not verified:** D3D11/12 and Vulkan (Windows, Linux, Steam Deck) have not run the chain; it uses only SDL's documented render-target and scale-mode calls. Judging the look on a real display, and whether 150/255 is the right dimming, is Joris's call.
- **Costs:** 6 to 9 extra small draws a frame with `blur`, and up to 133 MB while a large PNG is decoded (freed once it is a texture).
- **Limits:**
  - The art is not mipmapped. A picture much larger than the window aliases a little when scaled down. A picture near the window's size looks best.
  - The blur shows the frame as composited: an open remap overlay or settings menu shows blurred in the bars.
- **How we'd know this was wrong:**
  - The blur shows in #137's draw phase on weaker GPUs (Steam Deck, Intel Macs). Then it can be recomputed only when the screen texture changed (the game's frame rate, not the display's).
  - Players find the blurred bars distracting during fast scrolling. Then a stronger dimming, or a slower blur (one that follows the frame over several frames), is the fix.
