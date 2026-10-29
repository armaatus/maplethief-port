# 41. Texture uploads: every GL ES 1.1 format, the game's premultiplied texels kept as uploaded, guard-band clipping

- **Status:** Accepted
- **Date:** 2026-10-29
- **Issue:** #57
- **Builds on:** [ADR 0036](0036-coregraphics-and-uikit-drawing.md) (CoreGraphics bitmap contexts, the PNG decoder), [ADR 0039](0039-rendering-backend-software-rasterizer.md) (`mt_gfx`, its conversion table, clipping), [ADR 0040](0040-gles-fixed-function-pipeline.md) (the fixed-function pipeline, the GL trace file)
- **Amends:** ADR 0039's clipping (x and y now clip to a guard band, see Decision)

## Context

#57 asks two things: that every texture format the game uploads is supported, and that alpha-blended sprites show "no dark or bright fringes". With a game that blends with `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` while its loader premultiplies, the second needs a careful reading. The port's target is the device, so the criterion is read as **no fringe that the device would not draw, and none taken away**.

What the game uploads (static analysis in Ghidra, cross-checked with Capstone, plus the GL trace file with the call site of every upload; [ANALYSIS.md → Rendering](../ANALYSIS.md#rendering)):

- `glTexImage2D` is called from **three sites**, each with the constants `GL_RGBA`, `GL_RGBA`, `GL_UNSIGNED_BYTE`, level 0, border 0:
  - `0x987E` in the texture loader `FUN_00009534`, used for every PNG: bundle files, `.dat` pieces and baked map images.
  - `0x7BD2` and `0x7C84` in the text texture `FUN_00007ad8`, one in each branch of a `glIsTexture` check. It is always 512 × 32 (or 32 × 32), drawn by `TextCache_DrawText`.
- In a 40 s run (the boot, the main menu, Play, the loading screen, Henesys, an NPC dialogue) there were 573 uploads, all `GL_RGBA`/`GL_UNSIGNED_BYTE`: 563 from `0x987E` at sizes up to 1024 × 512, and 10 text uploads at 512 × 32. Every texture gets `GL_LINEAR` for both filters and keeps `GL_REPEAT`.
- The loader decodes the PNG with `-[UIImage initWithData:]`. It draws it 1:1 into a calloc'd power-of-two bitmap context: `kCGImageAlphaPremultipliedLast` when the image's alpha info is 1–4, else `kCGImageAlphaNoneSkipLast`, both with `kCGBitmapByteOrder32Big`. The context is flipped (`TranslateCTM(0, H)`, `ScaleCTM(1, −1)`) and the image drawn at `(0, H − h, w, h)`. The power of two is `FUN_00006558`: n if n is one, else `2 << floor(log2 n)`. The image's top row ends up in texel row H − 1, and the padding stays 0.
- The text texture is a `kCGImageAlphaPremultipliedLast` context with grey fill 1, so its texels are `(a, a, a, a)`.
- The sprite draw `FUN_00007ccc` computes texture coordinates as truncated 16.16 quotients over the power-of-two size, `s = (sx << 16) / W` and `t = ((H − sy) << 16) / H`. Its colour is `(0xFF00, 0xFF00, 0xFF00, alpha << 8)`, with `GL_BLEND` and `GL_ALPHA_TEST` on.
- `FUN_0000949c`, from `CStateTitle::Enter`, makes a texture object with only `GL_TEXTURE_MAG_FILTER` set and no image.

So the game uploads **premultiplied** texels and blends them with **`GL_SRC_ALPHA`**: each edge colour is multiplied by alpha twice. A white text edge texel at alpha 38 is `(38, 38, 38, 38)`; over black it becomes `38 × 38 / 255 = 6`, where a correct "over" would give 38. That darkening is the device's. A port that un-premultiplies the texels or switches to `GL_ONE, GL_ONE_MINUS_SRC_ALPHA` would draw cleaner edges than the device.

## Options considered

1. **Keep the uploaded texels as they are and blend as the game asks.** Each conversion is a pure expansion to RGBA8, exactly as `GL_MODULATE` reads the format.
   - Pro: GL's semantics as written. The device's darkened edges are reproduced, and the texture-replacement hash (ADR 0039) is over the game's own bytes.
   - Con: the device's edge darkening stays. It is the original look, and a later, optional "clean edges" mode could change it (M10).
2. **Un-premultiply at upload** (or blend with `GL_ONE, GL_ONE_MINUS_SRC_ALPHA` for textured draws).
   - Pro: cleaner edges than the device.
   - Con: not the device. Text and every sprite edge would look brighter than on the iPod. Rounding loses colour where alpha is low, and an upload does not say whether its texels are premultiplied (the game's are, a replacement's might not be).
3. **Detect premultiplied uploads by content and correct them.**
   - Con: guesswork. A `(c ≤ a)` test also passes for straight images with dark edges, and the result would still differ from the device.

For clipping, which the check below found to matter:

1. **Clip x and y to the view volume** (ADR 0039 as it was). A sprite that hangs over the screen's edge is cut where it leaves the screen. The new vertices get snapped to 1/16 pixel away from where their texture coordinates were computed, so a 1:1 sprite no longer samples texel centres there, and `GL_LINEAR` mixes in 1–2 levels of the neighbouring texels (`GL_REPEAT` wraps the far edge in).
2. **Clip x and y to a guard band and bound the pixels by the viewport.** Inside the band a triangle keeps its own vertices. The rasterizer then draws exactly the pixels that clipping to the viewport would leave. Hardware rasterizers generally work this way.

## Decision

- **Texels are kept as uploaded.** There is no un-premultiplying, no re-premultiplying and no change of blend function. The pipeline draws `GL_MODULATE` and then the game's `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`, with every product rounded as `(a × b + 127) / 255`, as ADR 0039 already does.
- **Every GL ES 1.1 format/type pair is converted** (`runtime/src/gfx/texture.cpp`): `GL_RGBA`, `GL_RGB`, `GL_ALPHA`, `GL_LUMINANCE` and `GL_LUMINANCE_ALPHA` with `GL_UNSIGNED_BYTE`; `GL_RGB`/`GL_UNSIGNED_SHORT_5_6_5`; `GL_RGBA`/`GL_UNSIGNED_SHORT_4_4_4_4` and `GL_UNSIGNED_SHORT_5_5_5_1`. Each becomes RGBA8 as `GL_MODULATE` reads it:
  - RGB gets alpha 255.
  - `GL_ALPHA` becomes `(255, 255, 255, A)`, because modulation takes its colour from the fragment.
  - `GL_LUMINANCE` becomes `(L, L, L, 255)` and `GL_LUMINANCE_ALPHA` `(L, L, L, A)`.
  - An n-bit component c of a packed type becomes `(c × 255 + max / 2) / max` with max = 2ⁿ − 1, i.e. `round(c × 255 / max)`. The packed words are read little-endian, as the device stores them.
  - Every other pair stays the tracker's `GL_INVALID_ENUM` or `GL_INVALID_OPERATION`, and `mt_gfx_upload_hash` returns 0 for it.
  - The game uses only RGBA/`UNSIGNED_BYTE`; the other pairs exist for completeness and for replacement packs.
- **The loader's CoreGraphics draw is the model** that ADR 0036 implements: the straight RGBA of the PNG decoder, each colour `round(c × a / 255)`, alpha as decoded, 0xFF in the skip byte of an opaque image, image row y in buffer row H − 1 − y, the rest 0. It was checked against Apple's CoreGraphics (Evidence). There is **no colour management**, as on iOS 4. The two PNGs with an `iCCP` profile, `MainMenuCompanyLogo.png` and `sprite/title/res/titled_0.png`, are drawn with their bytes as they are.
- **The GL trace names the call site of every `glTexImage2D`** (`… from 0x987E`): the return address of the guest call minus 4, with the Thumb bit cleared.
- **Clipping uses a guard band** (`runtime/src/gfx/gfx.cpp`). x and y are clipped to `−16w ≤ x, y ≤ 16w`, and w and z as before. The rasterizer keeps the pixels inside the viewport (`Shader`'s bounds) and the target. In exact arithmetic it covers the same pixels as before, and a sprite over the screen's edge keeps its snapped, whole-pixel vertices. Beyond the guard band, triangles are still clipped.

## Evidence

- **Formats** (`tests/cpp/gfx_test.cpp`, CI): every pair through `mt_gfx_texture_image` and drawn with `GL_MODULATE`.
  - The packed types are checked for all 65,536 words against `round(c × 255 / max)`.
  - `GL_ALPHA` and `GL_LUMINANCE` are checked in modulated draws.
  - Invalid pairs are refused and the hash of a pair without a conversion is 0.
  - `tests/cpp/gles_fixed_test.cpp`: 565 and `GL_LUMINANCE` uploads now convert ("could not store" is logged only when memory runs out).
- **The game's texture path, synthetic** (`gles_fixed_test.cpp`, CI). `FUN_00009534` is replayed through the HLE: `UIImage`, `CGImageGet*`, the bitmap context, translate, scale, `CGContextDrawImage`, `glTexImage2D`, then `GL_LINEAR`.
  - Every colour × alpha pair is premultiplied as `round(c × a / 255)`, NoneSkipLast writes 0xFF, and the padding is 0.
  - Under the game's filter sequence the texture is complete (`GL_LINEAR`), and `FUN_0000949c`'s image-less texture draws untextured.
  - `FUN_00007ccc`'s 1:1 sprites hit texel centres in 9 placements over magenta padding, so the padding never bleeds in.
  - Magnified, `GL_LINEAR` with `GL_REPEAT` mixes in what wraps around, with values checked by hand.
  - **The golden `gles_premultiplied_edges.png`** (tolerance 0) draws premultiplied sprites and text texels over black and white under the game's blend, with hand-computed edge pixels. A white texel at alpha 51 gives `(10, 10, 10, 214)` over black and `(214, 214, 214, 214)` over white; at 153 it gives `(91, 91, 91, 194)` / `(194, 194, 194, 194)`. Opaque white gives `(254, 254, 254, 254)` over black (the colour `0xFF00` is 254) and `(255, 255, 255, 254)` over white. Text at alpha 102 gives `(41, 41, 41, 194)` / `(194, …)`.
- **Apple's CoreGraphics as the reference** (`tests/cpp/cg_apple_test.cpp`, `cg.apple_reference`, macOS only):
  - The synthetic part runs always. Every colour × alpha pair is drawn into the loader's 256 × 256 context and, padded, a 200 × 100 image into a 256 × 128 context; an RGBX image with skip byte 0x00 and 0x5A is drawn into the NoneSkipLast context. 0 bytes differ from the model.
  - Locally, every PNG of the bundle goes through ImageIO and Apple's draw, tagged DeviceRGB as iOS 4 drew it: 1,322 files (113 CgBI) and 473 pieces of the 40 `png/**/*.dat` packs. 0 bytes differ from the model.
  - With ImageIO's own colour spaces only the two `iCCP` files differ, by at most 20 levels: that is macOS's colour management.
- **The port's CoreGraphics on every shipped PNG** (`tests/cpp/cg_real_test.cpp`, local): all 1,795 PNGs through `FUN_00009534`'s sequence give the model's buffer, 0 differ. A model that truncates instead of rounding makes 81 of them differ, so the check can fail.
- **A game screen, pixel by pixel** (`tests/cpp/boot_real_test.cpp`, `runtime.launch_real`, local). A command observer recomputes every pixel of every 1:1 textured draw of the 6 s launch: the uploaded texel, modulated by the current colour, blended with `GL_SRC_ALPHA` over a shadow copy of the pixel before the draw.
  - One run had 2,214 draws, 2,076 textured and 2,045 of them 1:1 (checked); 31 stretched or rotated ones are left out. 17.6 M pixels were checked, 71,000 of them from edge texels (0 < alpha < 255). **0 differ.** The counts vary a little with timing.
  - Before the guard band, 11,220 pixels differed by up to 2. All of them were in quads that cross the screen's edge, such as the loading screen's 128 × 128 art at x = 474.
  - Sample edge pixels of the loading screen, for #60: texel `(38, 38, 38, 38)` × colour 254 over black → `(6, 6, 6)`; texel `(45, 45, 45, 45)` → `(8, 8, 8)`; texel `(28, 28, 28, 28)` over `(17, 37, 20)` → `(18, 36, 21)`.
- **Guard band** (`gfx_test.cpp`, "clipping against the view volume"):
  - A triangle with vertices at ±10 is drawn unclipped as one triangle and still covers exactly its sub-viewport.
  - A 32 × 32 `GL_LINEAR` sprite of distinct texels, half off the target on each side, gives every visible pixel its texel exactly. With the old clipping (guard 1), 3 of the 4 placements fail.
  - The existing rules (the huge triangle, watertightness across the boundary, the eye plane) still pass.
  - The golden `gfx_perspective.png` changed in 1,655 of 19,200 pixels. Compared with a ray-cast of the same floor, the new image is closer on both halves: in the nearest half 67 pixels have the wrong texel instead of 86, and in the bilinear half no pixel is off by more than 2, where 163 were before.

## Consequences

- **The device's look is kept**, darkened premultiplied edges included. #60 compares these points with device screenshots:
  - edge and text fringes, using the sample pixels above;
  - `0xFF00` = 254, so sprites are never quite opaque;
  - the `iCCP` title art (`titled_0.png`), drawn without colour management;
  - opaque palette sprites, drawn opaque through NoneSkipLast;
  - the wrap-around bleed at the far edge, which shows only on stretched draws.
- A "clean edges" option (un-premultiplying for display) would be a deliberate deviation for M10, behind a setting, not a default.
- Replacement packs (M10) are hashed over the game's own premultiplied bytes. A replacement should be premultiplied as the game's loader does it, or it will look brighter at its edges.
- **Found on the way, #121:** the game draws `GL_LINES` (`FUN_000082ac`, a Cohen–Sutherland line drawer; 2,544 lines in 40 s, at an NPC dialogue). `mt_gfx` counts lines but does not draw them yet.
- **How we'd know this was wrong:**
  - Device screenshots (#60) show brighter edges than the port. Then the device's GL did not blend as asked, or iOS premultiplied differently, and the model changes in one place.
  - A later game state uploads another format. The trace file names its call site, and the conversion already exists.
