# 68. GPU presentation: the GL frame stays on the GPU, UIKit's views as an overlay texture

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #130
- **Builds on:** [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (the compositor's full and overlay modes), [ADR 0042](0042-frontend-game-loop-and-presentation.md) (the present hook, the Presenter), [ADR 0060](0060-gpu-rendering-backend.md) (the GPU backend, "Presentation"), [ADR 0066](0066-shader-filters-and-text-at-the-render-scale.md) (the present stage's filters)

## Context

ADR 0060 put mt_gfx's draws on SDL3 GPU but kept presentation on pixels. At each `presentRenderbuffer:`, `Screen::take_gl_frame` calls `mt_gfx_target_cpixels`. That runs the queued draws, downloads the target (a copy pass and a fence wait) and copies it. Then `mt_uikit_composite` (full mode) draws UIKit's views over the frame on the CPU, and the Presenter uploads the composited screen again. At render scale 4 that is about 10 MB down and 10 MB up a frame, plus a CPU-GPU sync. The full composite also redraws the GL frame itself through the CG rasterizer at the render scale.

Things already in place:

- `mt_gfx_gpu_target_texture` hands out a target's GPU texture.
- The window's renderer is SDL's GPU renderer on mt_gfx's device.
- The compositor has an overlay mode (ADR 0037): UIKit's views only, with the GL view's slot transparent.

Constraints:

- The software path stays the default and the reference. Its output and the goldens don't change.
- The GPU path has to match software within ADR 0060's tolerance.
- #88's filters keep working.
- `--shot`, `--shot-gl`, `--capture` and the `--frames` check still produce pixels.
- ADR 0042's tear-free copy at the present stays. The game draws its next frame into the same renderbuffer.

## Options considered

1. **Where the layers meet**
   1. **On the GPU, into the Presenter's screen texture.** That texture becomes a render target. Each present clears it to opaque black, draws a GPU copy of the GL frame into the GL view's rectangle, and draws the overlay over it with premultiplied "over". The Presenter then draws the texture into the window exactly as it drew the uploaded one. The scale modes, the filter passes, the bars, the focus highlight and the read-back check all stay as they are. Chosen.
   2. Draw the GL frame and the overlay straight into the window, each through the scale mode. The filters run on one image, so they would see the GL frame without the UI over it. The two layers' scaling would also have to agree pixel for pixel. Rejected.
   3. A render pass of our own on the device, outside SDL's renderer. The ordering against the renderer's command buffer would then be ours to manage. Rejected while SDL's renderer can do it.
2. **The GL frame**
   1. **A GPU-to-GPU copy at the present** (`SDL_CopyGPUTextureToTexture`, cycled) into a texture of the window's renderer. It is the GPU form of `Screen::take_gl_frame`'s copy: later draws into the target can't tear the frame shown, and a turn-hook present without a new GL frame shows the last one presented. Chosen.
   2. Sample the target's texture at present time. That is cheaper by one copy, but a present from the turn hook would show draws the game hasn't presented yet. Rejected.
3. **The overlay's upload**
   1. **Upload only when it changed** (a memcmp against the last upload). In the device golden run, 38 of 1,265 composites changed. Chosen.
   2. Upload every frame: 10 MB at 4x for nothing, most frames.
4. **The test aids**
   1. **Download on demand, through the frontend's own downloads.** `--shot` reads the composed screen texture back (what the window shows). `--shot-gl` reads the GL copy. `--capture` reads the screen until the first lit frames are found. The `--frames` check reads both. mt_gfx's `readbacks` counter stays 0. Chosen.
   2. Read back through mt_gfx at each shot. That would read back the target as it is now, not the frame shown. Rejected.

## Decision

- **`GpuScreen`** (`platform/src/gpu_screen.{h,cpp}`) is used when mt_gfx draws on the GPU and the window's renderer is SDL's GPU renderer (`GfxBackendHost` puts both on one device). The log says `present: the GL frame stays on the GPU: UIKit's views are composited as an overlay over it`. `MAPLETHIEF_GPU_READBACK=1` keeps ADR 0060's read-back path for comparisons.
  - `take_gl_frame` (the present hook): `mt_gfx_gpu_target_texture(target)` runs the queued draws and returns the texture. A command buffer of our own copies it into the GL copy texture, an `ABGR8888` (R8G8B8A8_UNORM) texture of the renderer. That command buffer is submitted after mt_gfx's flush and before the renderer's (at `SDL_RenderPresent`), and SDL GPU runs command buffers in submission order.
  - `compose_uikit` (every present): `mt_uikit_composite(OVERLAY)` at 480 × 320 × render scale on the CPU. The overlay is uploaded if it changed. Then, into the screen texture:
    - clear to opaque black (the full composite's start);
    - the GL copy in `mt_uikit_gl_rect` × render scale, nearest, with a colour-only blend (src colour, dst alpha), so the frame's own alpha never shows and the screen stays opaque;
    - the overlay, premultiplied "over".
  - The Presenter's texture is a render target (`Presenter::start(..., true)`). `Presenter::draw` and everything after it is unchanged.
- **`mt_gfx_gpu_target_texture`** now always returns the target's current pixels:
  - a target the GPU hasn't drawn gets its GPU side;
  - pixels the CPU wrote since the GPU last drew go up first (`target_uploads`);
  - nothing is read back.
  
  It takes `const mt_gfx_target *`, like `mt_gfx_target_cpixels`. Nothing else in `gfx.h` or `gfx_gpu.h` changes.
- **Equivalence.** Full mode draws the views over the GL frame one layer at a time. Here the views are drawn over transparent, and the result over the frame. Over is associative, so the two differ only by rounding: at most 1 per layer with the compositor's `(x · (255 − a) + 127) / 255` against the GPU's round-to-nearest. ADR 0060's tolerance covers it.
- **The `--frames` end check** with GPU presentation:
  - The window is checked against the downloaded screen texture, as before (`check_readback`, exact under nearest).
  - The CPU's full composite at the same time, from the downloaded GL frame, must match the GPU's screen within ADR 0060's tolerance: `game: GPU presentation of window frame N ... against the CPU's composite`.
- **Frame-time log:** `game: per window frame:` now also times `SDL_RenderPresent` and taking the GL frame (where the read-back sat). `game: frame time: X ms per window frame in the frontend` sums taking, compositing, uploading and drawing, without the vsync wait. GPU presentation adds `game: GPU presentation: N GL frames copied on the GPU, ... overlay uploads (... unchanged), ... downloads`.
- **Lifecycle:** all of it stays behind #128's `presenting()` gate at the top of `present()`. The GL frame is still taken while hidden (a GPU copy), as Screen still copied it.

## Consequences

- **Evidence: CI (synthetic, every OS):**
  - `platform.present: GPU presentation's layers`: the compositor's overlay mode over a GL frame, with an opaque view and two overlapping translucent panels, against its full mode. Within tolerance, max channel delta ≤ 2.
  - `platform.present: diff_screens ...`: the tolerance's counting.
  - `mtport.gpucompare`'s tests: the read-back and frame-time lines, and the 4x screens.
- **Evidence: local, with a GPU** (`gfx_gpu.GPU presentation: *`, Apple M4, Metal):
  - A GL frame (stripes plus a blended textured quad) in a GL view smaller than the screen, under a synthetic overlay (an opaque shell, a translucent panel, glyphs, a faint button): 0 of 153,600 pixels beyond ±2 at 1x and 0 of 2,457,600 at 4x, max channel delta 0, against the CPU reference. `readbacks` stays 0 until the test itself reads the target.
  - The window's read-back check: 0 mismatches. The screen is opaque everywhere.
  - A frame taken and then drawn over (queued, then flushed) still shows the frame taken. A CPU fill after GPU draws, and a target the GPU never drew, are what the next take shows. Without a GL view the slot is black.
  - An unchanged overlay is uploaded once.
  - The xbr, crt and lcd passes draw the composed screen with 0 mismatches under their checks.
- **Evidence: local, real assets** (`platform.gpu_compare`, Release, Apple M4; the device golden list's fresh run, 1,206 GL frames, 1,259 window frames):
  - The GPU runs log `0 read-backs`, against 1,202 before. At 4x that was 11.8 GB of read-back over the run.
  - Every GL frame and composited screen matches the software run within tolerance, now at 4x as well: the screens are the GPU's composites, read back. At 1x, title and main menu are identical; the intro's speech box has 5 pixels beyond ±2 (the GL_LINES ends of ADR 0060), the rest max delta 1. At 4x the intro has 32 pixels beyond ±2, the NPC dialogue 5, the rest 0.
  - The GPU time per frame stays under ADR 0060's bound: 0.69 ms median at 4x.
  - Frame time, the same binary with `MAPLETHIEF_GPU_READBACK=1` (before) and without (after), one after the other on a quiet machine:

    | | read-back (before) | GPU presentation (after) | software |
    |---|---|---|---|
    | **4x**, frontend time per window frame | **69.25 ms** | **44.79 ms** (−35%) | 68.91 ms |
    | 4x: taking the GL frame (per GL frame) | 1.44 ms | 0.26 ms | 0.90 ms |
    | 4x: compositing / upload and draw | 66.93 / 0.94 ms | 44.55 / 0.00 ms | 67.83 / 0.22 ms |
    | 4x: the whole run (wall clock) | 91.4 s | 59.9 s | 142.1 s |
    | **1x**, frontend time per window frame | 4.01 ms | 2.27 ms (−43%) | 3.74 ms |
    | 1x: the whole run | 11.1 s | 10.9 s | 21.1 s |

    The base build (#120 + #88, before this change) on a busier machine: 95.7 ms per window frame at 4x (94.45 compositing + 1.28 upload and draw, the read-back untimed), 130 s for the run.
  - The 4x "taking" time on the GPU path includes `--capture`'s downloads until the first lit GL frame. Without `--capture` it is 0.06 ms.
  - Overlay uploads: 38 of 1,265 composites changed the overlay.
  - `--frames 240` runs (1440 × 900 window), each 0 mismatches in the window's read-back check, 0 of N pixels beyond ±2 (max delta 0) in the GPU-against-CPU check, and `0 read-backs`:
    - 1x;
    - 4x;
    - 3x with `--filter crt`;
    - 2x fit with `--filter xbr`;
    - 2x with `--hd-text on` and `--minimise 2,1` (4 text textures replaced, 0 presents while hidden).
- Not verified here: D3D12 and Vulkan (#131). The ordering argument (submission order, SDL GPU's own barriers) holds for every SDL GPU driver, but it hasn't run there.
- What is left of the frame time is the CPU compositor at the render scale: 44.6 ms per window frame at 4x on the GPU path. A profile puts about 60% of the main thread in `mt_cg::Canvas::draw_image` for one view's image. Making that cheap is #134. Then the GPU path's 4x frame should come down to a few milliseconds.
- How we'd know this was wrong:
  - The GPU's screen and the CPU's full composite drift apart beyond the tolerance in some screen (the `--frames` check logs it). That would mean a view under the GL view, a GL view with alpha, or a GL frame of another size than its rectangle, which full mode draws differently.
  - A driver shows the frame from before the copy (an ordering bug). Then the copy would go into mt_gfx's own command buffer.

## Addendum: the remap overlay on the GPU, and its hold under the lifecycle (#70 with #130 and #128, 2026-10-30)

M7's remap overlay ([ADR 0054](0054-bindings-file-remap-overlay-and-button-glyphs.md)) was written for the read-back path: `shown_pixels` copies the CPU's composite, `Remap::draw` draws over the copy, and that copy is uploaded. With GPU presentation the composite never reaches the CPU, so the overlay needs a place on the GPU too.

- **Options.**
  1. Download the screen texture, draw over it, upload it. Simple, but it brings back a read-back per redraw, and the download after the first redraw holds the overlay already, so the base has to be kept anyway.
  2. Draw the overlay as a window layer after `Presenter::draw`, like the focus frame. It would skip the shader filter and the scale mode's filter, unlike the CPU path, and it needs its own placement and texture.
  3. **Draw it over UIKit's overlay layer and compose again (chosen).** The overlay layer is premultiplied, transparent in the GL slot. `Canvas` blends "over" it, and the GPU then puts it over the GL frame. "Over" is associative, so this equals the CPU path's image up to rounding.
- **`GpuScreen::compose_over(draw)`** keeps the last compose's overlay (UIKit's) and GL rectangle. It copies that overlay, lets `draw` draw over the copy, uploads it and composes the screen texture again. It makes no guest call, so `run_remap` can redraw while it holds the turn. The next `compose_uikit` or `compose` has UIKit's layer only, so a `--shot` composed after it doesn't have the overlay, as on the CPU path. The base moves out of `uploaded_` (a swap, no copy) the first time it is drawn over. There is one 480 × 320 × N² copy per redraw, and only while the overlay is open.
- **`Canvas::blend`** now composites alpha too: `a + dst_a · (255 − a) / 255`. An opaque image stays at 255, so its bytes are as before (the overlay's capture hashes are unchanged).
- **game.cpp.** `present()` with GPU presentation and the overlay open calls `draw_remap_gpu` after `compose`, and `run_remap` calls it instead of the upload. The `--frames` end check compares the GPU's screen with `shown_pixels` (the CPU's composite with the overlay when it is open). The focus frame stays off over the overlay.
- **The lifecycle's gate** stays first in `present()`. `run_remap` keeps it too:
  - Minimised or in the background while the overlay is open: the overlay stays open and the turn stays held. Nothing is drawn while hidden (iOS allows no GPU work in the background), and the overlay is drawn again when shown.
  - The resign and become-active messages wait in `Lifecycle`'s queue. `on_turn` now runs `run_remap` before the lifecycle's delivery, so when the overlay closes they go out in the same turn, as for a minimise between two turns. The guest was held all along, so it never saw a turn while hidden.
  - A quit (`SDL_EVENT_QUIT`, the close button) no longer sets `g.quit` directly (#128). So `run_remap` also ends on `Lifecycle::terminating()`. The turn hook then sends `applicationWillTerminate:` and the run ends.
- **Evidence.**
  - `gfx_gpu.GPU presentation: the remap overlay over the composed screen, and the shot without it` (Metal; local only, like every `gfx_gpu` case) runs at 1x and 2x. The screen texture with the overlay against the CPU path's image (the reference composite with `Remap::draw` over it): 0 of 153,600 and 0 of 614,400 pixels beyond ±2, max channel delta 1. The window's read-back matches it. A redraw without a compose gives the same image. The next compose is the game's screen again, bytes equal. mt_gfx's `readbacks` stay 0.
  - `gfx_gpu.GPU presentation: compose_over needs a compose first`.
  - `platform.remap: drawn over a premultiplied layer, then composited, as over the composite` (CI, no GPU): the CPU arithmetic of both paths agrees within 2, and the glyph-set hashes (`platform.remap: a capture of the overlay per glyph set, by hash`) are unchanged.
