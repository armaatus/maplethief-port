# 66. Shader filters on the present stage, and the game's text at the render scale

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #88
- **Builds on:** [ADR 0036](0036-coregraphics-and-uikit-drawing.md) (CG text, TextCache_DrawText's bitmap), [ADR 0039](0039-rendering-backend-software-rasterizer.md) (the render scale, the replacement hook), [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (scale modes, settings.ini), [ADR 0060](0060-gpu-rendering-backend.md) (the present stage, the shader build path)

## Context

#88 asks for optional pixel-art filters (sharp-bilinear, xBRZ-style, CRT/LCD), selectable in settings and under 1 ms a frame on the target hardware, and for an option to draw CoreGraphics text at the output resolution.

- **Sharp bilinear exists.** ADR 0043's `scale_mode = sharp` is `SDL_SCALEMODE_PIXELART`, SDL's own sharp-bilinear shader.
- **The present stage exists.** ADR 0060's `PresentPass` draws the screen image through a library fragment shader on SDL's GPU renderer (`SDL_GPURenderState`). Its interface is SDL's vertex colour (location 0), its texture coordinates (location 1), the screen texture at set 2 slot 0, and uniforms in set 3. The pass replaces SDL's texture shader, but the sampler is still the scale mode's.
- **The rendering backends.** `gfx_backend = software` (the default) presents with SDL's default renderer, which runs no custom shaders. SDL's GPU renderer can still present the image when mt_gfx draws on the CPU, because the two are independent.
- **No settings menu yet (#89).** "Selectable" means `settings.ini` plus a flag, as ADR 0043 does it.
- **The game's text.**
  - The engine draws every text texture through `TextCache_DrawText` (`0x9031`). It makes a 512 × 32 (or 32 × 32) context over a global guest buffer, clears it, sends `-drawAtPoint:withFont:`, releases the context and uploads the buffer with `glTexImage2D` (ADR 0036, ADR 0041).
  - At render scale N, mt_gfx's targets are N× and the GL quads are rasterized at N×, but the text texture is still 1×, so its glyphs are magnified.
  - The shell's UIKit labels are drawn by the compositor straight into the N× screen and are sharp already.
  - mt_gfx's replacement hook (ADR 0039) can hand any upload a k-times image, identified by the upload's content hash. Nothing installs a hook yet.

## Options considered

1. **Which filters**
   1. **xBR-style edge smoothing, CRT, LCD**, each a single-pass fragment shader that works at any output scale. Chosen.
      - `xbr` uses Scale2x's corner rule with xBR's 45° edge, anti-aliased over one output pixel.
      - `crt` draws a Gaussian beam per source row (wider when bright), a 3-column aperture mask, and works in linear light.
      - `lcd` is a cell grid, with darker cell edges at least one output pixel wide.
   2. Real xBRZ (or ScaleFX, Super-xBR): several passes or render targets of their own, and a fixed scale factor per pass. The present stage draws one quad through one shader, so these would need intermediate targets SDL's renderer doesn't expose to a render state. Rejected for now; it can follow, keeping the same setting.
   3. A sharp-bilinear shader of our own. That duplicates `scale_mode = sharp`. Rejected.
2. **How shaders read texels**
   1. **Integer loads** (`Texture2D.Load`; MSL `read()`) with the image size and the output rectangle's size in a uniform block. The result doesn't depend on the scale mode's sampler. Chosen.
   2. Sampling at texel centres through the scale mode's sampler. Correct only up to the sampler's rounding. Rejected.
3. **Filters with `gfx_backend = software`**
   1. **Ask for SDL's GPU renderer for the window whenever a filter is set**, while mt_gfx stays on the CPU (on a device of its own). Without one, log it and draw with the scale mode's filter. Chosen: it is one more `SDL_CreateRenderer(window, "gpu")` and keeps the filters off the reference path's pixels.
   2. Filters only with `gfx_backend = gpu`, with a logged fallback otherwise. Simpler, but needlessly ties a presentation choice to the backend choice.
   3. A CPU path. One pass over a 2880 × 1800 output is about 5 million shader evaluations a frame on the CPU. Not cheap. Rejected.
4. **Text at the output resolution**
   1. **A text shadow per bitmap context over guest memory**, matched to the upload by its content hash. Every `-drawAtPoint:withFont:` into such a context also draws, at k = N, into a k-times shadow, with the CTM scaled by k. The 1× hash recorded after that draw ties the shadow to exactly those pixels, and mt_gfx's replacement hook hands the shadow over only for an upload with that hash. Chosen.
   2. Recognize TextCache's buffer address and re-render its string at k× on upload. That ties the runtime to one function's globals and to the string, which the upload doesn't carry. Rejected.
   3. Make the context itself N× and downsample for the guest. The guest reads and uploads its own buffer, so the 1× pixels must stay exact. Rejected.

## Decision

- **Filters** (`settings.h`'s `ShaderFilter`, `gfx_backend.h`'s `PresentPass::open_filter`):

  | `filter` | Shader | Read-back check (`--frames`) |
  |---|---|---|
  | `none` (default) | the scale mode's (SDL's texture shader) | as ADR 0043 |
  | `xbr` | `present_xbr.frag` | each pixel within the range of the texels it covers and their neighbours, ±2 (`PassCheck::kBlend`) |
  | `crt` | `present_crt.frag` | the bars only (`kBarsOnly`) |
  | `lcd` | `present_lcd.frag` | the bars only |

  - Each shader has `present_copy.frag`'s interface plus `cbuffer Sizes` at set 3, b0: the image's size in texels and the placement rectangle's size in output pixels, each as w, h, 1/w, 1/h (`present.h`'s `PresentSizes`). `Presenter::draw` pushes it with `SDL_SetGPURenderStateFragmentUniforms` before each draw, so a resize is followed at once. `Presenter::set_stage` takes the pass's state, whether it wants sizes, and its read-back check.
  - The filter draws into the scale mode's rectangle (integer or fit), between the bars. The scale mode's own filter is replaced. The CRT and LCD looks want about 3 output pixels per texel (integer 3× and up); at 2× they are subtle.
  - Shaders are compiled by `scripts/build-shaders` like ADR 0060's (DXIL, SPIR-V, MSL; set-2 samplers and set-3 uniforms checked).
- **Selection:** `filter = none | xbr | crt | lcd` in `settings.ini` and `--filter`. It is a game option: the placeholder and the prototype stay unfiltered. When a filter is set, `GfxBackendHost::open(window, backend, true)` asks for SDL's GPU renderer for the window on the software path too. Without one, the window gets SDL's default renderer, the pass doesn't open, and the log says `present: filter crt needs SDL's GPU renderer, and the window's renderer is software: the image is drawn with the scale mode's filter (integer)`. The game runs on.
- **`hd_text = on | off`** (`--hd-text`, default off; `maplethief/cg.h`, `runtime/src/cg/hires_text.cpp`):
  - At render scale N > 1 the frontend sets the text scale to N and installs `mt_cg_text_replace` as mt_gfx's replacement hook. At render scale 1 it logs that there is nothing to do.
  - `-drawAtPoint:withFont:` into a bitmap context over guest memory (32-bit pixels, rows of exactly width × 4 bytes) goes to `draw_context_text`.
    - Before the draw, it checks the context's shadow. The shadow counts as the 1× pixels' N× image only if the 1× upload hash equals the one the shadow recorded. Otherwise it is rebuilt from the 1× pixels, each copied to N × N.
    - Then the text is drawn at 1× as always, and again into the shadow with the CTM scaled by N, so the glyphs are rasterized at N× the size.
    - The shadow then records the new 1× hash. The last 4 shadows stay findable after their context is released.
  - The hook replaces a `GL_RGBA` / `GL_UNSIGNED_BYTE` upload only when its size and hash are a shadow's. Any write the game makes to the buffer after the text changes the hash, so the upload is kept as it is: a replacement is always the upload's own content.
  - Not shadowed, and logged once: a draw through a clip, or into a context of another row layout. Host-memory contexts are never shadowed.
  - A later texture-pack hook (HD textures, M10) has to chain to `mt_cg_text_replace`: mt_gfx has one hook slot.
- **Lifecycle:** the filter's pass is opened after the Presenter starts and closed before the renderer goes. Drawing it stays behind #128's `Lifecycle::presenting()` gate, at the top of `present()` (ADR 0063).

## Consequences

- **Evidence: CI (synthetic).**
  - `platform.settings: filter ...`: the names in any case, a bad value reported with the old value kept, `hd_text` on/off, serialise → parse round trips, and the keys appended to an old file.
  - `platform.present: a shader filter without SDL's GPU renderer falls back ...`: on the dummy driver the GPU renderer is refused, no pass opens, and the Presenter draws the scale mode's image, read back with 0 mismatches.
  - `objc.CG: hd text ...`: TextCache's sequence at text scale 3.
    - The 1× buffer is byte for byte what scale 1 drew.
    - The hook's 1536 × 96 image equals the text drawn straight into a 3× canvas, and its inked rows are the 1× rows times 3.
    - Another size, another format, or a buffer changed after the text gets no replacement.
    - A CPU clear between two texts in one context rebuilds the shadow.
    - Of five texts, the last four stay findable. A 32 × 32 one-character texture works.
    - A clip gets no shadow and is logged. Host contexts are not shadowed.
    - Through `mt_gfx_texture_image` the texture keeps its 512 × 32 logical size and samples the 1536 × 96 texels.
- **Evidence: local, with a GPU** (`gfx_gpu.present filters: *`, Apple M4, Metal; each filter drawn by the real Presenter + PresentPass on an offscreen SDL GPU renderer).
  - Every pass opens with its sizes, and the bars keep the border colour.
  - `xbr` at 3×: a 64-step staircase gets 248 smoothed pixels, all in texels next to the diagonal, while texel centres stay exact. An isolated texel, a checkerboard's inside and flat areas are exact. Only the checkerboard block's 4 outer corners are rounded. At a 1001 × 667 fit every pixel is a blend of its neighbours.
  - `crt` at 5×: the scanline peaks at each row's centre, with the same profile every row. The mask runs red, green, blue in 3-pixel steps. A grey of 160 comes out at a mean of 159.9.
  - `lcd` at 3×: the cell centres are the texel ±1, the edges darker, the corners darkest.
  - **GPU time** (fence-timed upper bounds, median of 60, a Debug build on a machine loaded by parallel builds). The *pass alone* is what each further draw of the image adds (a clear plus the pass over the whole output). The *frame* adds the submit and the fence's wake-up, which the copy pass pays too.

    | Output (image) | copy pass / frame | xbr pass / frame | crt pass / frame | lcd pass / frame |
    |---|---|---|---|---|
    | 1920 × 1080 (1620 × 1080) | 0.05 / 0.91 ms | 0.26 / 0.94 ms | 0.19 / 1.01 ms | 0.08 / 0.81 ms |
    | 2880 × 1800 (2700 × 1800) | 0.21 / 1.00 ms | 0.47 / 1.17 ms | 0.53 / 1.28 ms | 0.23 / 1.15 ms |
    | 3840 × 2160 (3240 × 2160) | 0.32 / 1.23 ms | 0.54 / 1.88 ms | 0.73 / 1.90 ms | 0.32 / 1.30 ms |

    Every pass is under 1 ms. The test asserts that on the pass. The frame's fixed part follows the machine's load: on a quieter run, copy took 0.45–0.59 ms a frame, and crt at 1800p 0.94 ms.
- **Evidence: local, real assets** (`MapleThief --window 1440x900 --frames 240 --tap 3,106,165 --filter F --gfx-backend B`):
  - Every filter on both backends starts its pass (`the window's renderer is gpu`, `present: filter crt (present_crt.frag)`), plays to the character screen, and its last frame reads back with 0 mismatches under the filter's check.
  - With `SDL_VIDEO_DRIVER=dummy` the run logs the fallback and reads back exactly.
  - `hd_text`: see the issue's report; the run counts the text draws and replaced uploads at exit (`text: hd_text: ...`).
- **Not verified:** D3D12 and Vulkan run of the filter shaders (#131). They pass DXC, `spirv-val` and the descriptor-set check. Judging the filters' and the text's look on a real display is Joris's call.
- **How we'd know this was wrong:**
  - A filter's frame time shows up in the game's frame time on weaker GPUs (Steam Deck, older Intel Macs). Then the filters need a cheaper variant, or they get gated by the output size.
  - Players want xBRZ proper. Then a multi-pass present stage is needed (intermediate targets), which #130's GPU-resident frame makes natural.
  - Text in some screen is not replaced (the log's replaced count stays low). Then the game changes the buffer between the text and the upload in a way the shadow doesn't follow, and that draw path needs shadowing too.
