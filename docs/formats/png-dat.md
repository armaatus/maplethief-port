# `png/**/*.dat`: PNG packs, and how the engine slices them

**Status: fully specified.** All 40 shipped files split into 473 complete PNGs with zero errors and no stray bytes. The engine's own length table in the binary matches the PNG framing of every file, and every sub-image decodes. Issue [#17](https://github.com/armaatus/maplethief-port/issues/17).

A `.dat` file under `png/` is **not an atlas**. It is a *pack*: 2 to 109 complete, ordinary PNG files stored back to back with no header, padding or trailer. This is why each file "is a PNG": the first image starts at offset 0. The engine never looks at pixel rectangles to slice a pack. It cuts the file into its PNGs with a **table of byte lengths compiled into the binary**. Each piece then becomes an ordinary image (`CMImage`), exactly as if it had been loaded from its own `.png`, and is drawn whole.

Only two packs are cut further when drawn, and in both cases the cut is computed from the image size inside a drawing routine. No rect table is involved. `num.dat` holds digit strips (10 equal glyphs each), and `box.dat` holds 2 × 2 corner sheets for UI boxes. See [Parts](#parts-the-only-sub-rects).

## Where the game loads it

Addresses are armv7 VM addresses. Function pointers carry the Thumb bit, code addresses (call sites) don't. Names are descriptive (not yet in Ghidra). The decompiler listing behind this spec comes from [`tools/ghidra/scripts/pngdat_calls.py`](../../tools/ghidra/scripts/pngdat_calls.py) on the #7 project, and the addresses are re-checked against the code by `tests/test_pngdat_real.py`.

| Address | What | Evidence |
|---|---|---|
| `0xF143C` | pointer (`__DATA,__nl_symbol_ptr`) to the global **resource manager** object. All pack loaders are methods on it, and the image arrays are its fields | every caller passes `*(0xF143C)` as `this` |
| `0x1359D` | resource manager: load the shared "static" UI images (7 packs, plus loose PNGs) | called from `0x98751` (start-up, before the title state) |
| `0x16AF1` | resource manager: load the in-game images (14 packs) | called from `0x86C1D` (`CStateGame` set-up) |
| `0x14C59` | resource manager: load the pause/popup menu images (11 packs) | called from `0x8D4C9` |
| `0x12E6D` | resource manager: load the world map images (2 packs) | called from `0x8D4C9` |
| `0x1267D` | resource manager: load the logo images (1 pack) | called from `0x980B5` (title state set-up, state 0) |
| `0x823A1` | the egg-hatching sequence: loads **one** image of `item/etc.dat` | `0x82D3C` (index 85), `0x82DB2` (index 86) |
| **`0xF5A9`** | **`LoadPack(mgr, path, CMImage img[count], count, id)`** | see [Load routine](#load-routine) |
| **`0xF6A5`** | **`LoadPack2D(mgr, path, CMImage *img[rows], rows, cols, id)`** | |
| **`0xF445`** | **`LoadPack3D(mgr, path, CMImage **img[a], a, b, c, id)`** | only `game/storm.dat` |
| **`0xF34D`** | **`LoadPackOne(mgr, path, index, count, id, CMImage *dst)`** | only `item/etc.dat` from `0x823A1` |
| **`0xFC740`** | **length table** `u32 lengths[40][255]` in `__DATA,__data`, row stride `0x3FC` | formed in each helper by `ldr rN, [pc, #…]` + `add rN, pc` at `0xF5F4`, `0xF6FE`, `0xF4AC`, `0xF3A0`/`0xF3D2` (`0xED148 + 0xF5F8 = 0xFC740`) |
| `0xB689` | read a whole bundle file into `new u8[]` (path relative to the bundle) | shared with `.map`/`.mapprj` loaders |
| `0xBABD` | `CMImage::LoadFromMemory(img, buf, offset, length)`: copies `length` bytes from `buf + offset` into the image's own buffer and decodes it as a PNG | same calls as `CMImage::Load` (`0xBB49`) after the copy: `0x9535`, `0x62A5` |
| `0xBB49` | `CMImage::Load(img, path)`: the single-file loader that the loose `.png` files use | |
| `0x247DD` | `DrawImage(img, x, y, anchor)`: draws the **whole** image; `anchor` 0–8 picks the corner/centre that lands on `(x, y)` | the only call to the textured-quad blit `0x7CCD` |
| `0x24B3D` | `DrawImageClipped(x, y, img, sx, sy, w, h)`: sets a scissor rect `(x, y, w, h)` (`0x233D5`) and draws the whole image at `(x − sx, y − sy)`. This is how *every* sub-rect in the game is drawn | 14 callers |
| `0x3A07D`, `0x3A4D9` | draw a decimal number from digit strips | see [Parts](#parts-the-only-sub-rects) |
| `0x3A899` | draw a UI box from one corner sheet | see [Parts](#parts-the-only-sub-rects) |

All of this is unoptimized (`-O0`) code with SjLj exception frames.

## Files

40 files in 11 folders of `png/`, 224 to 28,872 bytes each (137,859 bytes in total). They hold 473 images, all palette PNGs: 465 at 8 bits and 8 at 4 bits (`static/arrow.dat` images 0, 1, 8–11, 18, 19). 457 have a `tRNS` chunk. The 16 opaque ones are `logo.dat` 0–10, `scrollbar.dat` 0–1, `gameqa.dat` 0 and 3, and `box.dat` 19. None are interlaced or CgBI-crushed. A pack is an exact concatenation: the file size is the sum of its PNG sizes.

## Layout

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | `lengths[id][0]` | `png[0]` | a complete PNG: signature `89 50 4E 47 0D 0A 1A 0A`, `IHDR`, `PLTE`, `tRNS`, `IDAT`…, `IEND` |
| `Σ lengths[id][0..i)` | `lengths[id][i]` | `png[i]` | the next PNG starts right after the previous `IEND` CRC |
| file size | | | ends right after the last `IEND`. No count, no trailer |

Nothing in the file says how many images it holds. The engine gets the count from the call site and the lengths from the table. A reader without the binary can split a pack by walking the PNG chunks to each `IEND` (what `mtport.pngdat.split` does); the real-asset test shows the two methods give identical pieces for all 40 files.

### Length table

`u32 lengths[40][255]` at `0xFC740` (`__DATA,__data`), little-endian, one row of 255 entries (`0x3FC` bytes) per **pack id**. Entry `i` is the byte length of image `i`. The rest of the row is zero. The ids are the order in which the packs were built: folder by folder (`static`, `mainmenu`, `skill`, `item`, `game`, `ui`, `minimap`, `popmenu`, `logo`), alphabetical inside a folder, except that `popmenu/picon.dat` comes last (38) in `popmenu`. The table ends at `0x1066A0`, after row 39.

Row 4 (`static/num.dat`) at `0xFD730`, the first 14 entries then a zero:

```
0xFD730  b7 03 00 00  3c 04 00 00  c4 02 00 00  5a 02 00 00   951, 1084, 708, 602
0xFD740  b4 00 00 00  b4 00 00 00  b5 02 00 00  52 02 00 00   180, 180, 693, 594
...                                                            (sum 7,053 = the file size)
```

### Worked example: `static/acceptrefuse.dat` (pack 0, 606 bytes)

Row 0 at `0xFC740` is `99 00 00 00 99 00 00 00 96 00 00 00 96 00 00 00 00 00 00 00`: four images of 153, 153, 150 and 150 bytes. The file:

```
00000000: 8950 4e47 0d0a 1a0a 0000 000d 4948 4452  .PNG........IHDR    png[0], 20×11, 8-bit palette
00000010: 0000 0014 0000 000b 0803 0000 0048 d75d
...
00000090: 0049 454e 44ae 4260 8289 504e 470d 0a1a  .IEND.B`..PNG...    png[0] ends at 0x99 = 153,
000000a0: 0a00 0000 0d49 4844 5200 0000 1400 0000  .....IHDR.......    png[1] starts there
```

It is loaded by `LoadPack2D(mgr, "/png/static/acceptrefuse.dat", mgr->+0xF0, 2, 2, 0)` (`0x141D8`), so image `i` lands in `img[i / 2][i % 2]`.

## Load routine

```c
// 0xF5A9: the flat case. 0xF6A5, 0xF445 are the same loop nested 2 and 3 deep.
void LoadPack(Mgr *mgr, const char *path, CMImage img[], int count, int id) {
    u8 *buf = ReadFile(gBundle, path);                 // 0xB689; the size is not kept
    int off = 0;
    for (int i = 0; i < count; i++) {
        u32 len = lengths[id][i];                      // 0xFC740 + id*0x3FC + i*4
        CMImage_LoadFromMemory(&img[i], buf, off, len);  // 0xBABD
        off += len;
    }
    delete[] buf;
}

// 0xF6A5: img is an array of `rows` pointers to arrays of `cols` CMImages (0x24 bytes each).
//   image k = i*cols + j goes to img[i][j], then DrawImage(&img[i][j], 50*i, 50*j, 0) (0x24DC9).
// 0xF445: image k = (i*b + j)*c + l goes to img[i][j][l].
// 0xF34D: walks the same row, skipping `index` lengths, and loads only image `index` into dst.
```

The 2-D loader draws every image once, at `(50·i, 50·j)`, right after loading it. Nothing else happens there, so this is probably a texture warm-up. It has no effect on slicing.

**No checks.** The loader never compares the lengths with the file size or looks at the PNG framing. A short file reads past the buffer, and a wrong length gives a broken PNG to the decoder. The parser rejects both: a pack must be an exact concatenation of valid PNGs, every chunk CRC must match, and with the binary given the pieces must equal the table row.

## Parts: the only sub-rects

Every image is drawn whole through `0x247DD`. The one way the game draws part of an image is `0x24B3D`: scissor to `(x, y, w, h)` and draw the whole image shifted by `(−sx, −sy)`. Its 14 callers mostly cut the large loose UI sheets (`*(mgr) + 0x6F0/0x6F4/0x6F8`, not packs). Among pack images, the rects come from the image size, never from a table:

| Pack | Routine | Cut |
|---|---|---|
| `static/num.dat` (all 14) | `0x3A07D DrawNumber(value, x, y, strip, spacing, anchor)`, `0x3A4D9` (first digit from a second, larger strip) | glyph `w = width / 10`; digit `d` is the rect `(d·w, 0, w, height)`. All 14 widths divide by 10. Strips 0–4, 6–9, 12 and 13 are passed directly (`*(mgr)+0x8 + 0x24·k`); 5, 10 and 11 may be reached through computed indices |
| `static/box.dat` (corner sheets) | `0x3A899 DrawBox(kind, style, x, y, w, h)` with `kind` = image index | `s = width >> 1`; the four `s × s` quadrants are the corners, drawn at the four corners of the box. Edges and fill are solid-colour rects (`0x23D95`). Odd sizes (9×9 … 13×13) leave the middle row and column unused |

Everything else (`0x24A65` mirrored draw, list viewports in `0x4E499`, `0x4ED75`, `0x4F3D1`, `0x4FAD9`, `0x56E91`) clips the screen, not the source image.

## The 40 files

*Slicing* is how the engine finds each sub-image: **table** = cut by the length-table row at the address given (all 40), then the image is drawn whole unless a part is noted. *Shape* is the array the loader fills (`[rows×cols]` = 2-D: image `k` → `[k / cols][k % cols]`). *Field* is the image array's offset in the resource manager. Loader `A @ B` = loader function `A`, call instruction `B`.

| id | File | Images | Sizes | Table row | Loader | Shape | Field | Content, uses |
|---|---|---|---|---|---|---|---|---|
| 0 | `static/acceptrefuse.dat` | 4 | 20×11, 21×11 | `0xFC740` | `0x1359D` @ `0x141D8` | 2-D `[2×2]` | `+0xF0` | accept / refuse buttons × 2 states; `0x49481` |
| 1 | `static/arrow.dat` | 22 | 4×7 … 33×17 | `0xFCB3C` | `0x1359D` @ `0x141B2` | 2-D `[11×2]` | `+0xEC` | 11 arrows × 2 states (8 are 4-bit); `0x32BBD`, `0x49481`, `0x4AB01`, `0x4D315` |
| 2 | `static/box.dat` | 49 | 4×4 … 20×20 | `0xFCF38` | `0x1359D` @ `0x14154` | flat `[49]` | `+0xC` | square box-corner sheets; **parts**: 4 corners each via `0x3A899` |
| 3 | `static/keyinfo.dat` | 4 | 36×8 … 62×12 | `0xFD334` | `0x1359D` @ `0x1421C` | flat `[4]` | `+0x148` | key-help labels |
| 4 | `static/num.dat` | 14 | 60×7 … 180×19 | `0xFD730` | `0x1359D` @ `0x1418C` | flat `[14]` | `+0x8` | digit strips; **parts**: 10 glyphs each via `0x3A07D` / `0x3A4D9` |
| 5 | `static/scrollbar.dat` | 2 | 7×6 | `0xFDB2C` | `0x1359D` @ `0x14170` | flat `[2]` | `+0x10` | scroll-bar ends (no `tRNS`) |
| 6 | `static/yesno.dat` | 6 | 7×5, 12×5, 28×7 | `0xFDF28` | `0x1359D` @ `0x141FE` | 2-D `[3×2]` | `+0xF4` | yes / no labels × 2 states; `0x49481` |
| 7 | `mainmenu/cmenu.dat` | 10 | 58×22 … 74×26 | `0xFE324` | – | – | – | **not loaded** (no path string, no call) |
| 8 | `mainmenu/gameqa.dat` | 4 | 115×37 … 144×8 | `0xFE720` | – | – | – | **not loaded** |
| 9 | `mainmenu/hmenu.dat` | 10 | 60×20 … 69×24 | `0xFEB1C` | – | – | – | **not loaded** |
| 10 | `mainmenu/menu.dat` | 10 | 45×20 … 71×22 | `0xFEF18` | – | – | – | **not loaded** |
| 11 | `mainmenu/point.dat` | 3 | 20×20 … 26×24 | `0xFF314` | – | – | – | **not loaded** |
| 12 | `skill/skillico.dat` | 22 | 20×20, 22×22 | `0xFF710` | `0x16AF1` @ `0x17E62` | flat `[22]` | `+0x2B0` | skill icons: index = `skill.dat` `icon` (0–21, [data-tables.md](data-tables.md)); `0x7D1AD` |
| 13 | `item/equip.dat` | 73 | 14×18 … 20×19 | `0xFFB0C` | `0x16AF1` @ `0x17EB6` | flat `[73]` | `+0x2D8` | equipment icons: index = `item_equip.dat` `icon` (0–72); `0x31EC1`, `0x3243D`, `0x69A51` |
| 14 | `item/etc.dat` | 109 | 9×17 … 20×20 | `0xFFF08` | `0x16AF1` @ `0x17EE8` | flat `[109]` | `+0x2DC` | item icons: index = `item_etc.dat` `icon` (0–108); `0x31EC1`, `0x3243D`, `0x4CAED`, `0x699A1`. Also `LoadPackOne` (`0xF34D`) of image 85 or 86 for the hatching egg (`0x823A1`) |
| 15 | `game/miss.dat` | 2 | 58×23 | `0x100304` | `0x16AF1` @ `0x17F6E` | flat `[2]` | `+0x308` | "miss" popup; `0x36611` |
| 16 | `game/spear.dat` | 2 | 14×13, 18×17 | `0x100700` | `0x16AF1` @ `0x17F1A` | flat `[2]` | `+0x2E0` | thrown-weapon images; `0x6C095` |
| 17 | `game/storm.dat` | 12 | 28×28 … 40×131 | `0x100AFC` | `0x16AF1` @ `0x17FB0` | 3-D `[3×2×2]` | `+0x30C` | effect frames; `0x6E2C9` |
| 18 | `game/yeti.dat` | 4 | 44×55 … 56×30 | `0x100EF8` | `0x16AF1` @ `0x17FE2` | flat `[4]` | `+0x310` | effect frames; `0x6E2C9` |
| 19 | `ui/bossface.dat` | 5 | 24×24 | `0x1012F4` | `0x16AF1` @ `0x18162` | flat `[5]` | `+0x408` | boss portraits; `0x5A465`, `0x87501`, `0x8D4C9` |
| 20 | `ui/mktext.dat` | 4 | 20×7 … 54×11 | `0x1016F0` | `0x16AF1` @ `0x18130` | flat `[4]` | `+0x394` | market labels (loose `ui/mktext0…3.png` also exist) |
| 21 | `ui/npcnotify.dat` | 5 | 12×12 … 25×26 | `0x101AEC` | `0x16AF1` @ `0x180CC` | flat `[5]` | `+0x38C` | NPC quest markers; `0x8572D` |
| 22 | `ui/qnline.dat` | 2 | 4×3 | `0x101EE8` | `0x16AF1` @ `0x180FE` | flat `[2]` | `+0x390` | quest-list line pieces; `0x77C19` |
| 23 | `ui/reqmark.dat` | 2 | 14×14, 17×9 | `0x1022E4` | `0x16AF1` @ `0x18068` | flat `[2]` | `+0x33C` | "required" marks; `0x3243D` |
| 24 | `ui/rewmark.dat` | 2 | 14×14, 20×10 | `0x1026E0` | `0x16AF1` @ `0x1809A` | flat `[2]` | `+0x364` | "reward" marks; `0x31EC1` |
| 25 | `ui/ui.dat` | 11 | 10×10 … 111×24 | `0x102ADC` | `0x16AF1` @ `0x18036` | flat `[11]` | `+0x338` | HUD pieces (image 7 = `mainmenu/rank1.png`, byte-identical); `0x5A465`, `0x87501` |
| 26 | `minimap/continent.dat` | 4 | 24×22 … 84×13 | `0x102ED8` | `0x12E6D` @ `0x131B4` | flat `[4]` | `+0x528` | world-map continent labels |
| 27 | `minimap/worldmap.dat` | 4 | 82×70 … 220×180 | `0x1032D4` | `0x12E6D` @ `0x13196` | flat `[4]` | `+0x500` | world-map pieces |
| 28 | `popmenu/class.dat` | 5 | 17×7 … 45×7 | `0x1036D0` | `0x14C59` @ `0x161CC` | flat `[5]` | `+0x460` | class names; `class0…4.png` load into the parallel array `+0x464` |
| 29 | `popmenu/classMark.dat` | 2 | 13×13 | `0x103ACC` | `0x14C59` @ `0x161F4` | flat `[2]` | `+0x468` | class marks; `0x56321` |
| 30 | `popmenu/optIcon.dat` | 8 | 14×20 … 20×19 | `0x103EC8` | `0x14C59` @ `0x160D0` | 2-D `[4×2]` | `+0x440` | option icons × 2 states |
| 31 | `popmenu/optText.dat` | 8 | 48×11 … 55×16 | `0x1042C4` | `0x14C59` @ `0x160A0` | 2-D `[4×2]` | `+0x438` | option labels × 2 states; `optText00…31.png` load into the parallel `+0x43C` |
| 32 | `popmenu/ptext.dat` | 6 | 22×7 … 37×7 | `0x1046C0` | `0x14C59` @ `0x16070` | flat `[6]` | `+0x444` | pause-menu labels |
| 33 | `popmenu/skillup.dat` | 2 | 11×12 | `0x104ABC` | `0x14C59` @ `0x16154` | flat `[2]` | `+0x454` | skill-up button × 2 states |
| 34 | `popmenu/statInfo.dat` | 8 | 22×8 … 48×12 | `0x104EB8` | `0x14C59` @ `0x161A4` | flat `[8]` | `+0x45C` | stat labels; `statInfo4…7.png` load into `+0x4FC` |
| 35 | `popmenu/stattext.dat` | 3 | 15×5 … 20×5 | `0x1052B4` | `0x14C59` @ `0x1617C` | flat `[3]` | `+0x458` | stat names; `0x56321` |
| 36 | `popmenu/sticon.dat` | 3 | 17×19, 18×19 | `0x1056B0` | `0x14C59` @ `0x16104` | flat `[3]` | `+0x448` | stat icons |
| 37 | `popmenu/sttext.dat` | 3 | 49×10 … 52×10 | `0x105AAC` | `0x14C59` @ `0x1612C` | flat `[3]` | `+0x44C` | status labels; `ui/sttext0…3.png` load into `+0x450` |
| 38 | `popmenu/picon.dat` | 12 | 12×15 … 25×17 | `0x105EA8` | `0x14C59` @ `0x16038` | 2-D `[6×2]` | `+0x430` | pause-menu icons × 2 states; `0x5A465`, `0x87501` |
| 39 | `logo/logo.dat` | 12 | 21×21 … 155×49 | `0x1062A4` | `0x1267D` @ `0x12794` | flat `[12]` | `+0x4` | title/logo pieces |

Notes on the table:

- **Not loaded.** The 5 `mainmenu/*.dat` packs still have their table rows (7–11), but no path string and no loader call refers to them. They are leftovers (the 1.5 main menu uses the root-level `MainMenu*.png` images instead). The port can ignore them.
- **Uses** are functions found reading the field (decompiled code, `*(mgr) + field`). Fields with none listed had no direct read found. They may be reached through a computed offset, or the parallel array of loose PNGs may be what the UI draws (`class`, `optText`, `statInfo`, `sttext`). Which of each pair is drawn belongs to the UI work, not to this format.
- **States.** Every 2-D pack has a second dimension of 2. Matching loose files are named the same way (`static/yesno00.png` … `yesno11.png`, `popmenu/optText00.png` … `optText31.png`), so `[i][j]` is presumably item `i` in state `j` (e.g. normal / pressed).
- Content descriptions come from file names, sizes and the code that reads each field. They are labels for inspection, not verified by playing.

## Parser and extractor

[`tools/mtport/pngdat.py`](../../tools/mtport/pngdat.py), standard library only:

- `split(data)`: cut a pack by its PNG framing (`pnginfo.png_end`), with every chunk CRC-checked. Rejects empty files, bytes that are not a PNG, truncation and more than 255 images.
- `length_row(macho, id)` / `split_by_lengths(data, lengths)`: the engine's way, reading the table out of the armv7 slice.
- `PACKS`: the table above as data (id, path, shape, loader, call, helper, field, parts).
- `decode_indexed` / `encode_indexed`: a small palette-PNG codec (bit depths 1/2/4/8, all five filter types), used only to cut the parts.
- Command line:

  ```sh
  PYTHONPATH=tools python3 -m mtport.pngdat list
  PYTHONPATH=tools python3 -m mtport.pngdat extract --out DIR
  ```

  The asset pipeline (`scripts/pipeline`, [ADR 0011](../adr/0011-asset-pipeline-manifest-and-incremental-runs.md)) writes the packs into `assets-out/png/<folder>/<stem>/` through `pack_files`, checked against the length table in the bundle's own `MapleThief` binary, with one `index.json` per pack (the entry below plus `length_table_checked`). `extract` is the standalone equivalent. It writes `DIR/<folder>/<stem>/<name>.png` for every image, byte for byte as stored. Names follow the loader's array: `NN` (`NNN` for `etc.dat`) for flat packs, `NN_i_j` / `NN_i_j_k` for 2-D / 3-D ones. It also writes the parts: `num/NN_d0` … `NN_d9` and `box/NN_tl`, `_tr`, `_bl`, `_br`. And it writes `index.json` with each image's offset, length, size and coordinates, and each pack's table row, loader and field. By default it checks every pack against the length table in `work/slices/MapleThief.armv7`; pass `--no-binary` to skip that.

## Validation

- **Unit tests** (CI): `tests/test_pngdat.py`, on synthetic PNGs built in code (split, malformed packs, table rows, naming, the codec over every filter type and bit depth, part rects, extraction, CLI), and `tests/test_pnginfo.py` for `png_end`.
- **Real assets** (local only, skipped without `work/ipa/`): `tests/test_pngdat_real.py`.
  - The 40 shipped files are exactly `PACKS`, and each splits into the loader's image count.
  - Every length-table row equals the PNG framing of its file.
  - Path strings exist for the 35 loaded packs and not for the 5 others.
  - All 473 images decode (465 8-bit, 8 4-bit). Every digit-strip width divides by 10, and every box sheet is square.
  - Every recorded call site is a `bl` to the recorded helper, and every helper's `ldr`/`add pc` pair resolves to `0xFC740` (capstone).
  - `extract` runs into a temp dir with `40/40 packs, 473 sub-images, 336 parts, 40 checked against the length table, 0 errors`.
  - When `work/re/pngdat_calls.txt` exists (`tools/ghidra/run-script pngdat_calls.py work/re/pngdat_calls.txt`), it agrees with `PACKS`.

## Unknowns

- Which index of the multi-image packs means what, beyond the item/skill icon indices and the two drawing routines above. That needs the UI and effect code (later milestones).
- Why `LoadPack2D` draws each image at `(50·i, 50·j)` while loading (probably texture warm-up; harmless).
- Whether the `.dat` arrays or the parallel loose-PNG arrays are drawn for `class`, `optText`, `statInfo` and `sttext`.
