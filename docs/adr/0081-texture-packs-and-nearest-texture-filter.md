# 81. Texture packs, texture dumps and a nearest texture filter

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #153
- **Builds on:** [ADR 0003](0003-dependencies.md) (stb_image, no new dependency), [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (the compositor draws UIKit's images), [ADR 0039](0039-rendering-backend-software-rasterizer.md) (the replacement hook, FNV-1a), [ADR 0041](0041-texture-formats-and-premultiplied-alpha.md) (premultiplied texels), [ADR 0066](0066-shader-filters-and-text-at-the-render-scale.md) (hd_text uses the hook), [ADR 0070](0070-settings-registry-and-presets.md) (the registry), [ADR 0080](0080-render-scale-auto-and-frame-budget-guard.md) (N changes while the game runs)

## Context

#153 asks for HD texture and sprite packs that players make from their own copy, and for sprites that stay sharp at high render scales.

- **The seam exists.** mt_gfx offers each upload with pixels to one host hook with its FNV-1a 64 content hash, and takes back an image k times the upload's size (ADR 0039). hd_text's `mt_cg_text_replace` already sits in that one slot (ADR 0066), so a pack cannot simply take it.
- **The game's uploads** (ADR 0041) are all `GL_RGBA`/`GL_UNSIGNED_BYTE`: power-of-two CoreGraphics buffers, premultiplied (`round(c × a / 255)`), zero-padded, and the 512 × 32 text texture with texels `(a, a, a, a)`. A replacement that is not premultiplied the same way has brighter edges.
- **Menu art bypasses mt_gfx.** The shell's buttons and backgrounds are `+[UIImage imageNamed:]` images that the compositor draws straight into the screen (ADR 0037). The GL texture loader reads PNGs through `-[UIImage initWithData:]`, by bytes, not by name.
- **At N > 1** every texture keeps the game's `GL_LINEAR`: a magnified sprite is blurred over N pixels, and at its edges the bilinear footprint reaches the texture's padding.
- **Pack files are untrusted.** A player downloads packs, or makes them with tools we don't control.
- **N changes live** (ADR 0080): `mt_gles_set_render_scale` remakes the renderbuffers' targets between two guest calls. Textures are not touched.

## Options considered

**The second hook.**

1. **A chain of hooks in mt_gfx, by order.** `mt_gfx_add_replace_hook(order, fn, user)`; the first valid replacement counts, a wrong-sized one is rejected and the next hook asked. Chosen: the dump, hd_text and the pack are independent and each owns its entry.
2. A second global slot, or one platform function that calls hd_text, then the pack. Either couples the pack to hd_text, and a third client (mods) would need yet another.

**Which goes first, text or pack.**

1. **hd_text, then the pack.** hd_text's replacement is the very text the upload holds, re-drawn at N and matched to its exact 1x bytes. A pack can only offer an upscale of those bytes. The game's text textures also change with every string, so a pack cannot name them in a useful way. Chosen.
2. The pack first. It would replace good text with a worse upscale whenever a dump caught the same string.

The dump goes before both: it must see every upload, including those hd_text takes.

**Naming files.**

1. **`<hash>_<W>x<H>.png`.** The hash covers the bytes only: two uploads of the same bytes in another shape (an all-transparent 64 × 32 and 32 × 64) have the same hash. The size in the name tells them apart, and it also tells a pack's tool what k a file has. Chosen.
2. `<hash>.png` alone. Ambiguous across shapes, as above.

The format and type are not in the name: the replacement is RGBA8 whatever the upload's format, and the game uploads one format only.

**Alpha in the files.**

1. **Straight alpha in dumps and packs, premultiplied at load as the game's loader does.** A PNG is straight alpha by definition, so editors and upscalers treat it right. Un-premultiplying the game's texels, `round(c × 255 / a)`, is exactly undone by ADR 0041's `round(c × a / 255)` for every c ≤ a (`objc.texpack: a 1x pack made from the dump ...` checks all 32,896 pairs). A texture with a colour above its alpha, which the game never uploads, goes to `.raw.png` as stored. Chosen.
2. Premultiplied PNGs. Exact without the round trip, but every tool would see dark fringes and treat them as colour.

**The pack builder's scaler.**

1. **Scale2x / Scale3x (AdvMAME, EPX), composed for 4x, 6x and 8x, in pure Python on `mtport.png`.** No dependency (ADR 0003). The rules only compare and copy whole pixels, so alpha needs no special case and nothing is blended. Chosen, with `nearest` as the other choice.
2. xBRZ. No pinned pure-Python implementation; binding the C++ one would be a new dependency for a tool that a player runs once. It can follow behind the same command (`--scaler`).

**Menu art.** A hook keyed by the bundle file name at `+imageNamed:` and `+imageWithContentsOfFile:` (the only by-name loaders), whose replacement only the compositor draws. The guest keeps the original CGImage and size, so layout and hit-testing are the game's.

**The nearest filter.** A global override in mt_gfx (`mt_gfx_set_texture_filter(MT_GFX_NEAREST)`), read by the software rasterizer's filter choice and the GPU backend's sampler key. The game's texture parameters stay as set, and turning it off gives them back.

## Decision

- **mt_gfx** (`maplethief/gfx.h`): the replacement hook is a chain of up to `MT_GFX_MAX_REPLACE_HOOKS` (8), run by order: `MT_GFX_REPLACE_ORDER_DUMP` 0, `MT_GFX_REPLACE_ORDER_TEXT` 100, `MT_GFX_REPLACE_ORDER_PACK` 200. `mt_gfx_set_replace_hook` empties the chain and adds one hook at order 0 (the old behaviour). `mt_gfx_upload_rgba8` converts an upload as mt_gfx stores it, for the dump. The frontend's hd_text adds and removes its own entry.
- **Dumps and packs** (`maplethief/texpack.h`, `runtime/src/texpack/texpack.cpp`):
  - `--dump-textures DIR` writes each upload once as `DIR/<hash>_<W>x<H>.png` (straight) or `.raw.png`, each UIKit image loaded by name as `DIR/ui/<bundle path>`, and a line per file to `DIR/manifest.jsonl` (`{"hash", "file", "width", "height", "format", "type", "alpha"}`; `{"ui", "file", "width", "height", "alpha"}`). Files are written atomically (`mt_saves_replace_file`); a second dump into the same directory writes only what is new.
  - `texture_pack = DIR` (`--texture-pack DIR`; Video, not in the presets: a path) indexes the file names when the game starts. A file is read when its upload comes: at most 80 MiB, the PNG signature and the header's size checked (k × the upload's size, k 1 to 8, at most 4096) before stb_image (PNG only) decodes it, then the decoded size again. A straight file is premultiplied (`(c × a + 127) / 255`). A rejected file is logged once (`texpack: texture DIR/<name> 4x6 is not k times 2x2 (k 1..8, at most 4096): rejected, the game's texture kept`) and never read again. Other names in DIR are ignored; `ui/` paths must be plain parts of `[A-Za-z0-9 _-+.@]`.
  - Both are opened before the boot and closed at the end; the exit log counts: `texpack: pack: 1342 uploads, 1342 replaced, 0 without a file, 0 rejected; 31 UIKit images, 31 replaced, 0 rejected`.
- **The UIKit image hook** (`maplethief/cg.h`, `mt_cg_set_image_replace_hook`): asked once per bundle file and hook with the decoded straight RGBA; a k × replacement becomes a host CGImage that `-[UIImage CGImage]` never returns. The compositor draws `uiimage_display_cgimage` into the view's rectangle. The texpack module installs one such hook for the dump and the pack.
- **`texture_filter = game | nearest`** (`--texture-filter`; Video; Original **game**, Original + controller **game**, Enhanced **nearest**): the frontend sets `mt_gfx_set_texture_filter(texture_filter_override(filter, N))`, which is `MT_GFX_NEAREST` for nearest at N > 1 and 0 otherwise, at the start and in `apply_render_scale` at every change of N (ADR 0080). At 1x the game's filters stay: there they are the device's image.
- **`python3 -m mtport.texpack build DUMP PACK --scale K [--scaler scale|nearest] [--jobs N] [--no-ui]`** writes every file of the dump's manifest, K times larger, under its name into PACK. Manifest lines it cannot trust (a bad name, a size that disagrees with the name, a `ui/` path with `..`) are skipped and reported. Dumps and packs are the player's copies of the game's art: they live outside git (`work/`, the test output directory).

## Evidence

- **CI (synthetic):**
  - `gfx.texture replacement: hooks form a chain by order, ...`: the order whatever the order of adding, a move instead of a duplicate, a wrong size rejected and the next hook asked, removal, the full chain. `gfx.texture replacement: mt_gfx_upload_rgba8 ...`: a padded RGB 5-6-5 upload equals the stored texels.
  - `gfx.texture_filter: the nearest override ...`: the 2 × 1 black-white pair magnified 4x gives `0, 0, 32, 96, 159, 223, 255, 255` with the game's `GL_LINEAR`, two flat halves with the override, and the ramp again without it; minified it is nearest too; only 0 and `GL_NEAREST` are taken.
  - `objc.texpack: *` (`tests/cpp/texpack_test.cpp`): the dump's names, straight values by hand, the `.raw.png`, the manifest line, nothing written twice (also across two dumps); the 1x round trip of every c ≤ a pair, byte for byte; a 2x pack premultiplied at load, `.raw.png` as stored, GL's size kept; four bad files (two axes of different k, text, a PNG cut in its data, k = 9) each logged once, uploaded twice and kept, and a file for another shape of the same hash not used; the chain with a text hook in front; a pack that is no directory.
  - `objc.CG: the image hook ...`, `objc.CG: a texture pack's ui/ files ...`, `objc.compositor: an image view draws the image hook's replacement ...`: the guest keeps the 12 × 10 image while the compositor gets 24 × 20, once per file, a wrong size rejected and logged, the dump's `ui/` file equal to the decoded image, and an image view composited at 2x shows the replacement.
  - `platform.present: a texture pack and texture_filter survive a live render scale change`: a pack replacement and the nearest override stay through 1x → 2x → 3x → 1x of `mt_gles_set_render_scale`, while the frames draw as before.
  - `platform.presets: *`: the two rows' columns in `kExpected`.
  - `test_texpack.py`: Scale2x and Scale3x worked by hand on a diagonal, flat areas and straight edges as blocks, the factors' passes, the 4096 limit, alpha compared like colour, the builder's names, sizes, skipped lines, a broken file, `--no-ui`, one process equal to two.
- **Local, with a GPU:** `gfx_gpu.texture_filter nearest: the GPU's samplers are nearest ...`: the prototype scene at 2x with the override, GPU against software within ADR 0060's tolerance, and different from the bilinear one.
- **Local, real assets** (EVIDENCE).
- **Goldens unchanged** with no pack and `texture_filter = game` (EVIDENCE).

## Consequences

- A player can dump, upscale and play with a pack in three commands, and edit any texture in an image editor in between. A pack made for 2x works at any render scale; at 1x a 4x pack is minified with the game's filters.
- A pack file is read and decoded at each upload of its texture (a map change re-uploads). The decode of a 2048 × 1024 PNG is a few milliseconds in a Release build, inside the map's loading. If that shows in the frame time, a decoded cache with a byte budget is the next step.
- Text textures in a dump are the strings the run showed: a pack replaces them only when the same string is drawn again. hd_text is the way to sharp text.
- The `ui/` replacements are asked once per file when the image is first loaded: a pack is chosen before the game starts (`Applies::kNextStart`), and so is the filter's setting, though its effect follows N live.
- **Found on the way:** CG's caches (the `+imageNamed:` cache among them) followed the ObjC runtime by its address only, so a second runtime at the same address (tests) saw the first one's images. CG now also compares CF's binding counter, as audio, Foundation and UIKit already did.
- How we'd know this is wrong: uploads that a dump-built pack does not replace on the same run (`without a file` above 0: the game's bytes are not deterministic there), packs that look brighter or darker at their edges than the game (the alpha round trip), or a frame-time spike at map loads with a pack.
