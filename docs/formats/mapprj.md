# `.mapprj`: tileset projects, and the `map/tile/` and `map/back/` folders

**Status: fully specified.** All 11 shipped projects parse with zero errors and no trailing bytes, and the parser re-encodes each one byte for byte. Every PNG they reference exists, and every tile index in every `.map` falls inside its area's sheet. Issue [#15](https://github.com/armaatus/maplethief-port/issues/15).

A `.mapprj` file is the tileset list of one **area**. It gives the tile size and the PNG sheets in `map/tile/` that the area's maps index into. The tile grids themselves are in `.map` files ([map.md](map.md)), and collision and objects are in `.att`/`.obj` ([#14](https://github.com/armaatus/maplethief-port/issues/14)). This spec also covers the full-screen backdrops in `map/back/`, and what `SkyCanvas` is (it is not a sky layer).

## Where the game loads it

Addresses are armv7 VM addresses. Function pointers carry the Thumb bit. Names are descriptive until Ghidra scripts name them ([#7](https://github.com/armaatus/maplethief-port/issues/7)). Class names come from RTTI.

| Address | What | Evidence |
|---|---|---|
| `0x868D5` | `CStateGame` map change: `newArea = (nextArea +0x98 != curArea +0x84)`, then calls `0x86699` | `ldr [r0, #0x98]` / `#0x84`, `cmp`, `bl 0x86698` with `r3 = 1` or `0` |
| `0x86699` | load a map: `(this, area, map, newArea)`. When `newArea` is set, it deletes the old `CMMap`, allocates a new one (`0x9C` bytes) and calls the project loader. It then calls the `.map` and `.obj` loaders. Without `newArea` it clears the map (`0xC2B1`) and keeps the tilesets | `bl 0xe13c` at `0x867AE`, only on the `newArea` path |
| **`0xE13D`** | **`.mapprj` loader**: `CMMap::LoadProject(this, area)` | `sprintf(buf, "%s%d%s", "/map/", area, ".mapprj")`: `0xE2829`, `0xE2814`, `0xE2830` |
| `0xB689` | read a whole bundle file into `new u8[]`, dropping the size | wraps `0xB5DD` |
| `0x1BC61` | `i16 ReadI16(buf, off)`, little-endian, sign-extended | shared with `.map` |
| `0x1BDB1` | `char *ReadString(buf, off, len)`: `new char[len + 1]`, zero it, copy, NUL-terminate | `operator new[]`, `memset`, byte loop |
| **`0xD019`** | **`CMMapTileSet` constructor** `(this, name, first, tw, th)`. The object is `0x3C` bytes and derives from `CMUtil` | vtable `0x119600`, typeinfo `0x119608` = `12CMMapTileSet` |
| `0xC271` | `CMMapTileSet::LoadImage`: `sprintf("%s%s", "/map/tile/", name)` and then `CMImage::Load` (`0xBB49`) into the member at `+0x04` | `0xE27E5`, `0xE27EA` |
| `0xB951` / `0xB965` | `CMImage` width / height (`+0x18` / `+0x1C` of the image) | one `ldr` each |
| `0xD6D1` | `.map` loader. Its bake (`0xD9E4`–`0xDE1C`) turns tile indices into sheet pixels | see [map.md](map.md#rendering-at-load-time) |
| `0x86109` | `CStateGame::LoadBack(this, area)`: loads `map/back/<n>.png` into the `CMImage` at `CStateGame+0x454` | `sprintf("%s%d%s", "/map/back/", n, ".png")`: `0xE2829`, `0xE59B2`, `0xE5052`. Callers `0x87318`, `0x8790A` pass `CStateGame+0x84` (the current area) |
| `0x884C4` | inside the `CStateGame` virtual `0x87501` (vtable slot `+0x24`, presumably the per-frame update/draw): the back image at screen (0, 0), then the map | `bl 0x24dc8` with `r1 = r2 = 0` on `this + 0x454` |
| `0xD225` | `CMMap::Draw`: draws the baked map image (`+0x78`) at `(+0x68 − camX, +0x6C − camY)` | `bl 0x247dc` |
| `0xC069` / `0xC175` / `0xC225` | camera clamp / follow a point / scroll by a delta (`CMMap+0x70`, `+0x74`) | see [Resolution step 7](#resolution-end-to-end) |
| `0x26529` | `SkyCanvas` constructor | vtable `0x119780` (typeinfo name `9SkyCanvas`), see [SkyCanvas](#skycanvas) |

All of this is unoptimized (`-O0`) code with SjLj exception frames.

## Files

| Folder | Files | Contents |
|---|---|---|
| `map/` | `0.mapprj` … `10.mapprj` (11) | one project per area, 15 to 25 bytes each |
| `map/tile/` | 11 PNGs | one tile sheet per area, all 320 px wide, 64 to 192 px tall, 8-bit palette + `tRNS` |
| `map/back/` | 11 PNGs: `0`–`4`, `6`, `7`, `9`–`12` | full-screen backdrops, all 480 × 320, opaque (RGB, or palette without `tRNS`) |

None of these are CgBI-crushed: they are standard PNGs (checked with [`pnginfo`](../../tools/mtport/pnginfo.py)). Endianness of the `.mapprj` header is little-endian. Names are ASCII.

## Layout

| Offset | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0 | 2 | i16 | `tile_width` | read with `0x1BC61` into `CMMap+0x04`. Shipped: 16 |
| 2 | 2 | i16 | `tile_height` | into `CMMap+0x08`. Shipped: 16 |
| 4 | 1 | u8 | `count` | number of sheets, into `CMMap+0x18`. Shipped: 1 |
| 5 | 1 | u8 | `len[0]` | length of the first name |
| 6 | `len[0]` | char | `name[0]` | a file in `map/tile/`. No terminator, no padding |
| … | | | `len[i]`, `name[i]` | repeated `count` times, back to back |
| end | | | | the file ends right after the last name |

There is no magic number, version or checksum. The loader also stores half the tile size at `CMMap+0x0C` / `+0x10` (signed `/ 2`, so 8 and 8). Given that the `.att` grid is `2w × 2h` cells, that is presumably the 8 × 8 attribute cell size ([#14](https://github.com/armaatus/maplethief-port/issues/14)).

## Load routine (`0xE13D`, `0xD019`)

```c
void CMMap::LoadProject(int area) {                                   // 0xE13D
    char path[...];
    sprintf(path, "%s%d%s", "/map/", area, ".mapprj");                // 0xE190
    u8 *buf = ReadFile(g_resources, path);                            // 0xE1A6 -> 0xB689 (size dropped)
    this->tw = ReadI16(buf, 0);  this->th = ReadI16(buf, 2);           // +0x04, +0x08
    this->tw2 = tw / 2;          this->th2 = th / 2;                   // +0x0C, +0x10
    this->sheetCount = buf[4];                                        // +0x18
    this->sheets = new CMMapTileSet*[sheetCount];                     // +0x1C
    int pos = 5, first = 0;
    for (int i = 0; i < sheetCount; i++) {                            // 0xE266..0xE334
        int len = buf[pos++];
        char *name = ReadString(buf, pos, len);                       // 0x1BDB1
        sheets[i] = new CMMapTileSet(name, first, tw, th);            // 0x3C bytes, 0xD019
        pos += len;
        first += sheets[i]->count;                                    // +0x34
    }
    delete[] buf;
}

CMMapTileSet::CMMapTileSet(char *name, int first, int tw, int th) {   // 0xD019
    // CMUtil base (0x24EA1), then the CMImage member at +0x04 (0xB90D)
    this->name  = name;                                               // +0x28
    LoadImage();                                                      // 0xC271: "/map/tile/" + name
    this->cols  = image.width  / tw;                                  // +0x2C (signed division)
    this->rows  = image.height / th;                                  // +0x30
    this->count = cols * rows;                                        // +0x34
    this->first = first;                                              // +0x38
}
```

### `CMMapTileSet` fields

| Offset | Field | Set by |
|---|---|---|
| `+0x00` | vtable `0x119600` | constructor |
| `+0x04` | `CMImage` (the decoded sheet). Its file buffer is at `+0x0C`/`+0x10` (pointer, size), width at `+0x1C`, height at `+0x20` | `0xB90D`, `0xBB49` |
| `+0x28` | `char *name` (heap copy, NUL-terminated) | `0xD0D4` |
| `+0x2C` | `cols` = sheet width ÷ `tw` | `0xD100` |
| `+0x30` | `rows` = sheet height ÷ `th` | `0xD11C` |
| **`+0x34`** | **`count` = `cols × rows`**, the number of tiles in the sheet | `0xD12A` |
| `+0x38` | `first`: the sum of `count` over the sheets before this one, i.e. the global index of this sheet's tile 0 | `0xD130` |

This answers the question left open in [map.md](map.md#unknowns): `+0x34` is the exact tile count, not count + 1.

### Malformed input

The loader never learns the file size, so it checks nothing. A short file reads past the buffer. A `count` of 0 makes the `.map` bake read `sheets[0]` anyway. A tile size of 0 divides by zero in the constructor. A name containing `/` or `..` is joined to `/map/tile/` as it is, and trailing bytes are ignored. A missing sheet only logs `"(m_nResID == 0 by [%s]"` (`0xE27CE`) in `CMImage::Load`; the load then continues with no file (not traced further). The parser rejects all of these. None occur in the shipped files.

## Resolution, end to end

How a cell on screen gets its pixels, for area `a`, map `m`:

1. **Map → area.** The game state holds the current area and map (`CStateGame+0x84`, `+0x88`) and the next ones (`+0x98`, `+0x9C`). On a map change, `0x868D5` passes `newArea = (next area ≠ current area)` to `0x86699`.
2. **Area → project.** Only on `newArea`, `0xE13D` loads `map/<a>.mapprj`. Maps in the same area share the already-loaded sheets.
3. **Project → sheets.** Each name becomes a `CMMapTileSet` that loads `map/tile/<name>` and cuts it into `cols = W / tw` by `rows = H / th` tiles (integer division, so a partial tile at the right or bottom edge is unreachable). Sheet `k` gets `first[k] = Σ count[j]` for `j < k`.
4. **Map → cells.** `0xD6D1` decodes `map/<a><m>.map` (`"%d%d.map"`, [map.md](map.md)) into layers of cell values: −1 is empty, otherwise a tile index `v` from 0 to 254.
5. **Tile index → sheet.** With one sheet (every shipped project), `v` indexes sheet 0 directly. With more than one, the bake walks the sheets (`0xDC02`–`0xDC4C`):
   ```c
   k = 0;
   while (k < sheetCount && sheets[k]->count <= v) { v -= sheets[k]->count - 1; k++; }
   ```
   The `- 1` disagrees with `first` from step 3: with two 160-tile sheets, global index 160 lands on tile **1** of sheet 1, not tile 0, and tile 0 of every later sheet is unreachable. `first` (`+0x38`) is never read by the bake. This is a game bug on a path no shipped data takes. A port that wants the original behaviour must keep the `- 1` (the parser's `TileLayout.locate` does). If `v` runs past the last sheet, `k` ends one past the array (out of bounds).
6. **Tile → pixel rect.** `src = ((v % cols[k]) · tw, (v / cols[k]) · th)`, size `tw × th` (`0xDC4E`–`0xDCB2`). With one sheet, `cols` is taken from sheet 0 up front (`0xDA22`). There is **no check** that `v < count`: a larger index reads past the end of the sheet's pixels. Every shipped index is in range (see [Validation](#validation)). The rect is copied to `(tw · x, th · y)` in the map's baked bitmap, with an alpha test and higher layers on top. The copy loop is hard-coded to 16 × 16, so `tw = th = 16` is required in practice ([map.md](map.md#rendering-at-load-time)).
7. **Bitmap → screen.** In the frame routine, `CMMap::Draw` (`0xD225`) draws the baked image at `(+0x68 − camX, +0x6C − camY)`. `+0x68`/`+0x6C` are zeroed by the constructor (`0xCF70`), and `camX`/`camY` are `CMMap+0x70`/`+0x74`. The camera is clamped by `0xC069` to `0 … mapPixels − screen` on each axis, or to 0 when the map is smaller than the screen. The screen size is `SkyCanvas+0x88`/`+0x8C`. `0xC175` follows a point: `target = p − screen/2`, then either `cam = target` or `cam += (target − cam) / 3` (smoothed). `CStateGame` feeds it the position of the object at `CStateGame+0x30` (presumably the hero), offset ±30 px horizontally by its facing. It uses 32 px higher for area/map (4, 17), (9, 5), (7, 4) and (10, 5).

So for the shipped data the whole chain is: `tile v of map a·m` → `map/tile/<name of a>.png`, rect `(16·(v mod 20), 16·(v div 20), 16, 16)`. Every shipped sheet is 320 px wide, so `cols = 20`.

## `map/back/`: the area backdrop

There is **no parallax**. Each area has one 480 × 320 backdrop, the size of the landscape iPhone screen, drawn at screen (0, 0) before the map (`0x884C4`: `CMImage` at `CStateGame+0x454`, anchor 0 = top-left), and never scrolled.

`CStateGame::LoadBack` (`0x86109`) picks the file from the area. It reloads only when the area differs from the cached one at `CStateGame+0x90`, which the constructor (`0x940D9`) sets to −1:

```c
n = area;
if (area == 4 || area == 5) n = 4;     // 0x8613A
if (area == 7) n = 11;                 // 0x86158
if (area == 8) n = 12;                 // 0x8616E
sprintf(path, "%s%d%s", "/map/back/", n, ".png");
```

| Area | Tileset | Backdrop |
|---|---|---|
| 0 | `Kerningcity.png` | `0.png` |
| 1 | `Perion.png` | `1.png` |
| 2 | `Henesys.png` | `2.png` |
| 3 | `Dungeon.png` | `3.png` |
| 4 | `Orvis.png` | `4.png` |
| 5 | `Orvis-dungeon-2.png` | `4.png` (shared with area 4) |
| 6 | `Magatia.png` | `6.png` |
| 7 | `Magatia-field-2.png` | `11.png` |
| 8 | `Magatia-field-1.png` | `12.png` |
| 9 | `elnas.png` | `9.png` |
| 10 | `Zakum.png` | `10.png` |

**`map/back/7.png` is never loaded**: area 7 is remapped to `11.png`, and `"/map/back/"` has no other reference in the binary. There is no `5.png` or `8.png`, which matches the remap. The sheet names suggest the area is a MapleStory region (Kerning City, Perion, Henesys, Orbis, Magatia, El Nath, Zakum).

## `SkyCanvas`

Despite the name, `SkyCanvas` has nothing to do with skies or backgrounds. It is the engine's **screen canvas singleton**:

- An initialisation function (`0x5C2D`) allocates it (`0xFC` bytes) and constructs it (`0x26529`). The constructor stores `this` in `0x11A950`, which `SkyCanvas::Instance` (`0x2567D`) returns. Right after, `0x5C2D` stores `Instance()` in a second global, `0x11A958` (`0x5CF8`). The map, camera and image-drawing code read the canvas through that one.
- `+0x88` / `+0x8C` hold the screen width and height, and `+0x90` / `+0x94` half of each. They come from an object at `+0x58` (`0x62ED` / `0x6301`). The constructor also looks up `"PHONEMODEL"` and `"HEIGHT"`.
- It takes its font name from `ScriptExtra.strings`: the key `SkyCanvas.SkyCanvas.1` is `"Gulim"` in English and `"굴림"` in Korean. (`SkyCanvas.SkyCanvas.2` is `"Red"` / `"홍"`.)

For this format, it supplies the screen size that the camera clamp and the follow logic use (step 7 above). The rest of the class belongs with the renderer ([M5 – Graphics, #100](https://github.com/armaatus/maplethief-port/issues/100)).

## Worked example: `0.mapprj` (21 bytes)

```
00000000: 10 00 10 00 01 0f 4b 65 72 6e 69 6e 67 63 69 74   ......Kerningcit
00000010: 79 2e 70 6e 67                                    y.png
```

| Bytes | Meaning |
|---|---|
| `10 00` `10 00` | 16 × 16 px tiles |
| `01` | one sheet |
| `0F` `"Kerningcity.png"` | a 15-byte name, so the sheet is `map/tile/Kerningcity.png` (320 × 128 → 20 × 8 = 160 tiles, `first` = 0) |

The first tile of `00.map` (area 0, map 0) is `0x8D` = 141 at row 2, column 34 ([map.md](map.md#worked-example-00map-1193-bytes)). It resolves to `141 mod 20 = 1`, `141 div 20 = 7`, so it is the rect `(16, 112, 16, 16)` of `Kerningcity.png`, and it is drawn at `(544, 32)` in the map's 640 × 448 bitmap. Area 0's backdrop is `map/back/0.png`.

## All areas

From `python3 -m mtport.mapprj areas work/ipa/Payload/MapleThief.app` (with `PYTHONPATH=tools`):

| Area | Tileset | Sheet (px) | Cols × rows | Tiles | Maps | Highest index used |
|---|---|---|---|---|---|---|
| 0 | `Kerningcity.png` | 320 × 128 | 20 × 8 | 160 | 0–5 | 159 |
| 1 | `Perion.png` | 320 × 112 | 20 × 7 | 140 | 0–4 | 139 |
| 2 | `Henesys.png` | 320 × 128 | 20 × 8 | 160 | 0–11 | 159 |
| 3 | `Dungeon.png` | 320 × 112 | 20 × 7 | 140 | 0–8 | 136 |
| 4 | `Orvis.png` | 320 × 176 | 20 × 11 | 220 | 0–18 | 219 |
| 5 | `Orvis-dungeon-2.png` | 320 × 96 | 20 × 6 | 120 | 0–3 | 117 |
| 6 | `Magatia.png` | 320 × 192 | 20 × 12 | 240 | 0–3 | 231 |
| 7 | `Magatia-field-2.png` | 320 × 128 | 20 × 8 | 160 | 0–6 | 151 |
| 8 | `Magatia-field-1.png` | 320 × 64 | 20 × 4 | 80 | 0–6 | 79 |
| 9 | `elnas.png` | 320 × 128 | 20 × 8 | 160 | 0–10 | 159 |
| 10 | `Zakum.png` | 320 × 144 | 20 × 9 | 180 | 0–5 | 176 |

Every sheet in `map/tile/` is used by exactly one project. The highest index is below the tile count everywhere (the largest sheet has 240 tiles), and many areas use their sheet's last tile.

## Validation

| Check | Result |
|---|---|
| Projects parsed | **11 / 11**, zero errors, strict end check (`tests/test_mapprj_real.py`, local only) |
| Re-encode | `encode(decode(f)) == f` for all 11 |
| Tile size, sheet count | 16 × 16 and 1 sheet in every project |
| Referenced sheets | 11 / 11 exist in `map/tile/`, and every file there is referenced |
| Referenced backdrops | 10 distinct files for 11 areas, all present. Unreferenced: `map/back/7.png` only |
| PNG format | all 22 `map/` PNGs are standard (not CgBI). All backdrops are 480 × 320 |
| Tile indices | every non-empty cell of all 90 `.map` files resolves inside its area's sheet (`TileLayout.tile_rect`) |
| Map names | every `.map` stem splits into exactly one (area, map) given the 11 areas. Map numbers are contiguous from 0 in each area |

CI runs only the synthetic tests (`tests/test_mapprj.py`, `tests/test_pnginfo.py`). The real-asset test skips when `work/ipa/` is absent.

## Parser

[`tools/mtport/mapprj.py`](../../tools/mtport/mapprj.py), on the shared [`bytereader`](../../tools/mtport/bytereader.py):

- `decode(data, name) -> MapProject(tile_width, tile_height, tilesets)` and `encode(MapProject)`. `decode` raises `FormatError` (with file name and offset) on truncation, trailing bytes, a non-positive tile size, zero sheets, or a name that is empty, non-printable-ASCII, `.`/`..`, or contains `/` or `\`.
- `layout(project, sheet_sizes) -> TileLayout` cuts the sheets like `CMMapTileSet` (`cols`, `rows`, `count`, `first`). `TileLayout.locate(v)` / `tile_rect(v)` resolve a cell value like the bake, including the multi-sheet `- 1`, and raise `IndexError` where the game would read out of bounds.
- `project_path(area)`, `map_stem(area, map)`, `split_map_stem(stem, areas)` (the `"%d%d"` names), and `back_image(area)` / `back_path(area)` (the backdrop remap).
- CLI: `python3 -m mtport.mapprj info FILE...` and `python3 -m mtport.mapprj areas APPDIR` (the table above). Run it with `PYTHONPATH=tools`.

[`tools/mtport/pnginfo.py`](../../tools/mtport/pnginfo.py) reads a PNG's `IHDR` (size, bit depth, colour type, CgBI flag) without decoding pixels. It is used here for the sheet sizes.

## Unknowns

- How the bake's 4-bytes-per-pixel read of a **palette** PNG works: it goes through `UIImage` → `CGImage` → `CGDataProviderCopyData` and assumes RGBA with alpha in byte 3. Presumably ImageIO expands palette + `tRNS` to RGBA. The port should decode the sheets to RGBA8 and keep the "copy if alpha ≠ 0" rule. Whether the source alpha is premultiplied does not matter for that test.
- The scripted camera modes in the `CStateGame` frame (`+0x47D`, `+0x47E`, `+0x480`–`+0x490`, which look like a camera shake from a random offset). They are not traced; they only move `camX`/`camY`.
- `CMMap+0x0C`/`+0x10` (half tile size): who reads them. Presumably the `.att` code ([#14](https://github.com/armaatus/maplethief-port/issues/14)).
