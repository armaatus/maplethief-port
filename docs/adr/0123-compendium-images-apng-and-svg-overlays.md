# 123. Compendium images: APNG animations, SVG map overlays, the hero dressed as the game does

- **Status:** Accepted
- **Date:** 2026-11-06
- **Issue:** [#216](https://github.com/armaatus/maplethief-port/issues/216) (epic #210)
- **Builds on:** [ADR 0010](0010-asset-pipeline-and-viewers.md) (the sprite and map viewers), [ADR 0087](0087-game-speed-and-fast-forward.md) (the tick lengths), ADR 0121 (what the compendium may publish), ADR 0122 (the page generator that reads the manifest)

## Context

Every compendium page needs its picture: monsters and NPCs (a still and an animation per action), item and skill icons, equipment as worn, and every map with its portals, NPCs and spawn points. The viewers already draw sprites and maps as the game does (checked against the game in M2). The export must be stdlib-only Python, lossless and deterministic, and the generator (#215) must find each file without knowing how it was made.

Three choices were open: the animation format, how the map's markers sit on the map so a page can toggle them, and how to show an item worn, since `item_equip.look` was documented only as an "avatar sprite index".

## Options considered

**Animation format.**

1. **Animated WebP.** Supported by every current browser (Chrome 32+, Firefox 65+, Safari 14+ on macOS 11+). Lossless WebP is VP8L, and the stdlib has no encoder: we would write and test one. A browser without WebP shows nothing.
2. **GIF.** One bit of alpha and 256 colours a frame, and delays in hundredths of a second: the sprites' half-transparent layers and their palettes swapped per mob would not survive.
3. **APNG.** Supported by every current browser (Chrome 59+, Firefox, Safari 8+, Edge 79+). It is a PNG with three extra chunk types (`acTL`, `fcTL`, `fdAT`), so it reuses `mtport.png`'s filter and zlib stream. Delays are a fraction (`num / den` seconds), so a tick of 50 ms is exact. Frame 0 is the ordinary `IDAT` image, so anything without APNG support shows the first frame. **Chosen.**

**Map markers.**

1. Draw them into a second PNG the map's size. Big, blurry at 2×, and the labels would be our pixel font.
2. **An SVG in map pixel coordinates** (`viewBox` = the 1× map). It scales with the 1× or the 2× image, its text is real text, and a page shows it as `<img>`, `<object>` or inline. **Chosen.** A spawn point is the monster's still standing on the point; the still is embedded once per map as a `data:` URI and placed with `<use>`, so the SVG needs no other file (an SVG in `<img>` loads no external images).

**Worn equipment.**

1. Show the item's own image files (`png/cab`, `png/arm`, `png/costume`) alone. Easy, but not how the item looks on the hero.
2. **Dress the hero as the game does.** `0x62571` (called with the hero's sprite list and an item) reads `look` through `0x694C1` and, by slot, loads `/png/cab/cab_<look>_<j>.png` (hat, 2 files), `/png/arm/arm_<look>_<j>.png` (weapon, 11) or `/png/costume/costume_<look>_<j>.png` (overall, 13) over the image of `hero/0` named at entry `j` of the i32 tables at `0xEABE0`, `0xEAC00` and `0xEAC90`. Each table has three rows (one per hero project), but the loop stops after row 0, so only `hero/0` is dressed. Row 0 names the hat images `cab_0_0`, `cab_1_0`, the weapon images `do_a_01`…`do_a_11` and the body images `a_body_*`. Shoes and earrings (`look = -1`) are never drawn. **Chosen**: the hero's standing frame (`hero/0` action 2, `CHero::SetAction` 2) with that one item swapped in.

## Decision

- `tools/mtport/compendium/images.py` writes `build/compendium/site/img/` (the directory is its own: it refuses to replace a non-empty directory without its manifest):
  - `monster/<id>/<action>.png` (still: frame 0) and `<action>.anim.png` (APNG, actions of two or more frames). Mob actions are named from the code that sets them: 0 `move` (`CMob::Flush` `0x7458D`), 1 `hit` (`CMob::TakeHit` `0x73715`, a hit the mob survives), 2 `die` (`CMob::Die` `0x73349`); a boss's other actions are `action<n>`.
  - `npc/<id>.png`, `npc/<id>.anim.png` (sprite `mob/<id + 4>`).
  - `item/<equip|etc>/<id>.png` and `@2x.png`, `skill/<id>.png` and `@2x.png`: the pack image at the table's `icon`, decoded to RGBA, 2× by nearest neighbour.
  - `item/equip/<id>.worn.png`: the dressed hero.
  - `map/<area>-<map>.png`, `@2x.png` (the viewer's export: backdrop and tile layers), `.svg` (the overlay).
  - `manifest.json`: `{"schema": 1, "images": {"<kind>:<id>": [{path, w, h, alt, role, ...}]}, "missing": {"<kind>:<id>": reason}}`, every list sorted by path, the file sorted by key. Sprites carry `action` and `anchor` (the sprite position in the image), animations `delays_ms`.
- Sprite frames face as drawn (left), all frames of an action share one canvas cropped to the union of their pixels, so the anchor is one point for the action. Every APNG frame is whole (blend `source`, dispose `none`) and loops; a frame lasts `max(delay, 1)` ticks of 50 ms, the speed `LoadConfig` sets.
- `check` (`python3 -m mtport.compendium.images check [--kb FILE]`) compares the manifest with the files (each listed, none extra, sizes, frame delays read back from the APNG) and with the entities: from the knowledge base when given (`mtport.compendium.kb.load_kb`), else from the tables and maps. An entity without its image is an error unless the manifest gives a reason.

## Consequences

- On the shipped game: 1,207 files, 26 MB, in about 10 s: 69 monsters, 39 NPCs, 212 items (50 of them worn), 24 skills and 90 maps, none missing (`tests/test_compendium_images_real.py`). The stills equal the sprite viewer's frames and the map renders equal `mapview export`, pixel for pixel.
- Animations show at the game's default speed. The 40 ms and 65 ms speeds would be the same frames with other delays; one speed is enough for a reference site.
- The skill icons are framed tiles in the game's art (only their corners are transparent); the export keeps them as they are.
- The overlay is our own drawing, not game art: its colours and labels can change without touching an image.
- Nothing is deployed until ADR 0121 is accepted.
