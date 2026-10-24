# `.map`: tile layers

**Status: fully specified.** All 90 shipped files decode with zero errors and no trailing bytes, and the encoder in the parser reproduces every one of them byte for byte. Issue [#13](https://github.com/armaatus/maplethief-port/issues/13).

A `.map` file holds the background tile grid of one map: its size in tiles, a layer count, and each layer as a run-length-encoded stream of one-byte tile indices into the map's tileset. Collision (`.att`), objects (`.obj`) and the tileset itself (`.mapprj`) are separate files ([#14](https://github.com/armaatus/maplethief-port/issues/14), [#15](https://github.com/armaatus/maplethief-port/issues/15)).

## Where the game loads it

Addresses are armv7 VM addresses. Function pointers carry the Thumb bit. Names are descriptive until Ghidra scripts name them ([#7](https://github.com/armaatus/maplethief-port/issues/7)).

| Address | What | Evidence |
|---|---|---|
| `0x86699` | map change: builds or resets the `CMMap`, then calls the loaders below in order | `bl` to the `.mapprj`, `.map` and `.obj` loaders at `0x867AE`–`0x86862` |
| `0xCE61` | `CMMap` constructor | stores vtable `0x11961C`, whose typeinfo `0x119624` names `5CMMap` (`0xE2804`) |
| `0xE13D` | `.mapprj` loader, `sprintf("%s%d%s", "/map/", area, ".mapprj")` | xrefs to `0xE2829`, `0xE2814`, `0xE2830` ([#15](https://github.com/armaatus/maplethief-port/issues/15)) |
| **`0xD6D1`** | **`.map` loader**: `CMMap::Load(this, area, map)`; decodes the layers and bakes them into one bitmap | xrefs to `"%s%d%d%s"` (`0xE280B`), `"/map/"` (`0xE2814`), `".map"` (`0xE2824`) |
| `0xD5B1` | `.att` loader, called *from* the `.map` loader at `0xD864` | xref to `".att"` (`0xE281F`) ([#14](https://github.com/armaatus/maplethief-port/issues/14)) |
| `0xD259` | `.obj` loader, called after the `.map` loader | xref to `".obj"` (`0xE281A`) ([#14](https://github.com/armaatus/maplethief-port/issues/14)) |
| `0xA095` | `sprintf`-style path formatter (`buf, fmt, ...`) | called with the format strings above |
| `0xB5DD` | read a whole bundle file into a new `u8[]`; returns the buffer and stores its size through the third argument | `0xA2C1` opens and sizes the file, `0x8F71` reads it, then it is copied into an `operator new[]` buffer |
| `0x1BC61` | `i16 ReadI16(const u8 *buf, int off)`: `buf[off] \| buf[off+1] << 8`, sign-extended | 9 instructions, `sxth`; shared by the `.mapprj`, `.obj` and many other loaders |

The loader is unoptimized (`-O0`) code: every local lives on the stack, so the walk-through below quotes stack slots only where they matter.

## Files

- 90 files in `map/`, named `"%d%d.map"` of (*area*, *map*). The area picks the tileset project `map/<area>.mapprj`. Neither number is zero-padded, so the name alone is ambiguous in general. In the shipped set it is not: `100`–`105` are area 10, `210`/`211` are area 2 maps 10–11, `410`–`418` are area 4 maps 10–18, and `910` is area 9 map 10. Every area's map numbers are then contiguous from 0.
- Sizes range from 369 B (`28.map`) to 2,541 B (`415.map`).
- Endianness: little-endian. The only multi-byte fields are the two in the header.

| Area | Tileset (`<area>.mapprj`) | Tiles in sheet (320 px wide ÷ 16, × rows) | Maps | Highest tile index used |
|---|---|---|---|---|
| 0 | `Kerningcity.png` | 160 | `00`–`05` | 159 |
| 1 | `Perion.png` | 140 | `10`–`14` | 139 |
| 2 | `Henesys.png` | 160 | `20`–`29`, `210`, `211` | 159 |
| 3 | `Dungeon.png` | 140 | `30`–`38` | 136 |
| 4 | `Orvis.png` | 220 | `40`–`49`, `410`–`418` | 219 |
| 5 | `Orvis-dungeon-2.png` | 120 | `50`–`53` | 117 |
| 6 | `Magatia.png` | 240 | `60`–`63` | 231 |
| 7 | `Magatia-field-2.png` | 160 | `70`–`76` | 151 |
| 8 | `Magatia-field-1.png` | 80 | `80`–`86` | 79 |
| 9 | `elnas.png` | 160 | `90`–`99`, `910` | 159 |
| 10 | `Zakum.png` | 180 | `100`–`105` | 176 |

Every tile index in every map is inside its area's tile sheet, which confirms the (area, map) reading of the names. (The orphan `106.att` would be area 10 map 6.)

## Layout

| Offset | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0 | 2 | i16 | `width` | in tiles; read with `0x1BC61`. Shipped: 30–60 |
| 2 | 2 | i16 | `height` | in tiles. Shipped: 20–52 |
| 4 | 1 | u8 | `layers` | Shipped: 2 (11 files), 3 (48), 4 (31) |
| 5 | … | tokens | layer 0 | RLE tokens until `width × height` cells are filled |
| … | … | tokens | layer 1 … `layers − 1` | each starts right after the previous one; no separators, no padding |
| end | | | | the file ends exactly after the last token of the last layer |

There is no magic number, no version and no checksum. Cells are row-major: cell `y * width + x` is column `x` of row `y`, top row first. Runs and literals freely cross row boundaries.

### Tokens

Each layer is a sequence of tokens. The first byte `n` of a token says what follows:

| `n` | Followed by | Meaning |
|---|---|---|
| `0x00`–`0xF0` | 1 byte `v` | a **run**: `n` cells of value `v`. `n = 0` consumes `v` and writes nothing (legal, never shipped) |
| `0xF1`–`0xFF` | `n − 0xF0` bytes | a **literal**: 1 to 15 cells, one byte each |

### Cell values

| Byte | Meaning |
|---|---|
| `0x00`–`0xFE` | tile index into the area's tile sheet: tile `i` is the 16×16 square at column `i % cols`, row `i / cols` of the sheet, `cols` = sheet width ÷ tile width (20 for every shipped sheet) |
| `0xFF` | empty cell: nothing is drawn. The game stores it as the `i16` −1 (`0xFFFF`) |

There are no flip, rotation or palette bits: a cell is a plain index.

## Decode routine (`0xD6D1`)

In C-like pseudocode, with the `CMMap` fields it writes. Field offsets are bytes into the `CMMap` object; `tw`/`th` (`+0x04`/`+0x08`, both 16) and the tileset list (`+0x18` count, `+0x1C` array) were set by the `.mapprj` loader before this runs.

```c
void CMMap::Load(int area, int map) {                       // 0xD6D1
    char path[...]; int size = 0;
    sprintf(path, "%s%d%d%s", "/map/", area, map, ".map");  // 0xD73A
    u8 *buf = ReadFile(g_resources, path, &size);           // 0xD752, 0xB5DD
    this->w      = ReadI16(buf, 0);                         // +0x20
    this->h      = ReadI16(buf, 2);                         // +0x24
    this->layers = buf[4];                                  // +0x14 (copied to +0x34)
    this->attW = 2 * w;  this->attH = 2 * h;                // +0x28, +0x2C: .att grid is 8x8 px
    this->pixW = tw * w; this->pixH = th * h;               // +0x5C, +0x60
    int cells = w * h;                                      // copied to +0x38
    this->cell = new i16*[layers];                          // +0x30
    for (l = 0; l < layers; l++) this->cell[l] = new i16[cells];   // not zeroed
    this->att = new u8[attW * attH]; memset(this->att, 1, attW * attH);   // +0x3C, +0x40
    LoadAtt(area, map);                                     // 0xD864 -> 0xD5B1 (#14)

    int pos = 5;                                            // 0xD868
    for (l = 0; l < layers; l++) {                          // 0xD872..0xD9E0
        for (i = 0; i < cells; ) {
            if (pos >= size) { delete[] buf; return; }      // 0xD87C: abort the whole load
            int n = buf[pos++];
            if (n > 0xF0) {                                 // 0xD8D0: literal
                for (k = 0; k < n - 0xF0; k++, i++) {
                    cell[l][i] = buf[pos++];
                    if (cell[l][i] == 0xFF) cell[l][i] = -1;
                }
            } else {                                        // 0xD94A: run
                i16 v = buf[pos++];
                for (k = 0; k < n; k++, i++) cell[l][i] = (v == 0xFF) ? -1 : v;
            }
        }
    }
    // 0xD9E4..0xDE1C: bake every layer into one bitmap (see Rendering)
    // 0xDE20..0xE0B8: wrap it as a CGImage -> UIImage -> PNG data, hand it to +0x78
    delete[] buf;
}
```

What the game does with malformed input (the parser rejects all of these instead):

- **Truncation**: the only bounds check is `pos >= size` before each token. If it fires, the load stops: no bitmap is built and the rest of the cells stay uninitialized. A run value or literal past the end of the buffer is read anyway (out of bounds).
- **A token that overshoots the layer**: `i` is only compared with `cells` between tokens, so a run or literal crossing the end of a layer writes past that layer's `new i16[cells]` (a heap overflow). The overshoot does **not** carry into the next layer, which restarts at cell 0.
- **Trailing bytes** after the last layer are ignored.
- **`width` or `height` ≤ 0**: not checked; `new[]` gets a nonsense size.

None of these occur in the shipped files.

## Rendering at load time

The loader does not keep the layers for drawing. At `0xD9E4`–`0xDE1C` it composes them into one RGBA bitmap of `(16·w) × (16·h)` pixels (`w·h·1024` bytes, allocated with `0x9009`):

- For each tileset, the loader copies its decoded pixels (`CGImageGetDataProvider` → `CGDataProviderCopyData`, 4 bytes per pixel, byte 3 = alpha). The sheet's width in pixels is at `CMMapTileSet+0x1C` and its tiles per row at `+0x2C`.
- It walks `x` over columns, `y` over rows, and then layer 0 to `layers − 1`. It skips cells < 0 (empty). Otherwise it copies the 16×16 tile from the sheet to pixel (`tw·x`, `th·y`). A pixel is copied **only if its source alpha is non-zero**, and when it is, it overwrites the destination completely, alpha included (alpha test, no blending). Higher layers therefore cover lower ones wherever they are opaque.
- The tile copy loop (16 rows of 64 bytes) and the destination stride (`16·w` pixels) are hard-coded, so tiles must be 16×16. Every `.mapprj` declares 16×16.
- With more than one tileset, the index is walked across sheets at `0xDC02`–`0xDC4C`: `while (sheet[k].count (+0x34) <= v) { v -= sheet[k].count - 1; k++; }`. Note the `- 1`. No shipped project has more than one tileset, so this path is dead. `+0x34` is the sheet's tile count; see [mapprj.md](mapprj.md#resolution-end-to-end) for the whole tileset side.

The finished bitmap becomes a `CGImage` (format copied from tileset 0), then a `UIImage`, then PNG data (`UIImagePNGRepresentation`). The loader then resets the member at `CMMap+0x78` (`0xB979`) and loads the PNG bytes into it (`0xBABD`); this is presumably the map's texture. The port can skip the PNG round trip and upload the composed pixels directly. Because the layers are baked together, **no layer is drawn above the sprites** by this path. Whether some other code reads `CMMap+0x30` again (for example, for foreground tiles) is **unknown** until the rest of `CMMap` is mapped.

## Encoder used for the shipped files

The game never writes `.map` files, but the parser's `encode()` reproduces the original tool's choices exactly (all 90 files, byte for byte). That proves every byte is accounted for. It is also a reference if maps are ever edited. Per layer, from the first cell:

1. If the cell equals the next cell, or it is the last cell of the layer, emit a **run** of all equal cells from here, **at most 239** (`0xEF`). The decoder accepts 240, but the tool never uses it.
2. Otherwise emit a **literal**. It takes this cell, then keeps taking cells while it holds fewer than 15 *and* the next cell does not start a repeat (`cell[i] != cell[i+1]`). The last cell of the layer counts as not starting a repeat.
3. Quirk: a literal that already holds **14** cells always takes one more, even the first cell of a repeat. That is why no shipped literal has length 14 and why a repeat sometimes continues as a literal. The 4 shipped runs of length 1 come from rule 1: a token that starts on the last cell of a layer.

## Worked example: `00.map` (1,193 bytes)

```
00000000: 28 00 1c 00 03 72 ff f5 8d 8e 8f 8e 90 0e ff f5   (....r..........
00000010: 8d 8e 8f 8e 90 11 ff f3 93 94 95 10 ff f3 93 94   ................
```

| Bytes | Meaning |
|---|---|
| `28 00` `1C 00` | 40 × 28 tiles (1,120 cells per layer) |
| `03` | 3 layers |
| `72 FF` | run: 114 empty cells (rows 0–1 and row 2, columns 0–33) |
| `F5 8D 8E 8F 8E 90` | literal of 5: tiles 141, 142, 143, 142, 144 at row 2, columns 34–38 |
| `0E FF` | run: 14 empty cells (row 2 column 39, row 3 columns 0–12) |
| `F5 8D 8E 8F 8E 90` | the same 5 tiles at row 3, columns 13–17 |
| `11 FF` `F3 93 94 95` … | 17 empty, then tiles 147–149, and so on |

Layer 0 ends at cell 1,120 and layer 1 starts with the next token. The last token of layer 2 ends at offset 1,193 = the file size. In this map, layer 0 holds the ground and platforms (its bottom rows are solid tiles), and layers 1–2 hold sparser overlay tiles that are composed on top of it.

## Validation

| Check | Result |
|---|---|
| Files decoded | **90 / 90**, zero errors (`tests/test_mapfile_real.py`, local only) |
| Trailing bytes | none in any file (strict end check) |
| Cells per layer | exactly `width × height` in every layer (no token crosses a layer end) |
| Re-encode | `encode(decode(f)) == f` for all 90 |
| Tile indices | all < tiles in the area's sheet (table above) |
| Tokens | 15,208 runs and 15,735 literals, which fill 376,276 cells in 290 layers; no `n = 0` or `n = 0xF0` runs |

CI runs only the synthetic tests (`tests/test_mapfile.py`). The real-asset test skips when `work/ipa/` is absent.

## Parser

[`tools/mtport/mapfile.py`](../../tools/mtport/mapfile.py), on top of the shared bounds-checked reader [`tools/mtport/bytereader.py`](../../tools/mtport/bytereader.py) (meant for the `.att`/`.obj`/`.mapprj` parsers as well).

- `decode(data, name) -> MapFile(width, height, layers)`, where `layers[l][y * width + x]` is a tile index or `EMPTY` (−1). It raises `FormatError` (with file name and offset) on truncation, a token crossing a layer end, a non-positive size, or trailing bytes.
- `encode(MapFile) -> bytes` with the original tool's token choices.
- CLI: `python3 -m mtport.mapfile info FILE...` (size, layers, token counts, highest tile) and `python3 -m mtport.mapfile dump FILE` (hex grid per layer, `..` = empty). Run it with `PYTHONPATH=tools`.

## Unknowns

- What `area` and `map` mean in game terms (world and zone?). The map-change caller at `0x86699` still needs to be traced back to the script/portal side.
- Whether any code besides the loader reads the per-layer cells (`CMMap+0x30`) after load, for example for foreground drawing or tile queries.
- ~~`CMMapTileSet+0x34` and the `- 1` in the multi-tileset walk~~: answered in [mapprj.md](mapprj.md#cmmaptileset-fields). `+0x34` is `cols × rows`, and the `- 1` is a game bug on a path no shipped data takes.
