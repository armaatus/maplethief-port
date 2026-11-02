# `.att`: attribute (collision) grid

**Status: fully specified.** All 90 packed files decode with zero errors, match their `.map` (a 2w × 2h grid), and re-encode byte for byte. The orphan `106.att` is a headerless raw grid that the parser also reads (see [`106.att`](#106att-a-leftover-raw-grid)). Issue [#14](https://github.com/armaatus/maplethief-port/issues/14).

A `.att` file holds the collision grid of one map: one byte of flags for every 8 × 8-pixel cell, so a cell is a quarter of a 16 × 16 tile. Footholds, ladders, ladder tops and walls are all flags in this grid; nothing about collision is stored in the tiles of the [`.map`](map.md). Portals, NPCs and spawn points are in the [`.obj`](obj.md).

## Where the game loads and reads it

Addresses are armv7 VM addresses, and function pointers carry the Thumb bit. The names are descriptive until Ghidra scripts name them ([#7](https://github.com/armaatus/maplethief-port/issues/7)). Offsets such as `+0x3C` are into the `CMMap` object.

| Address | What | Evidence |
|---|---|---|
| `0xD6D1` | `.map` loader: sets `attW = 2·w` (`+0x28`) and `attH = 2·h` (`+0x2C`), allocates `att = new u8[attW·attH]` (`+0x3C`, size at `+0x40`), fills it with 1, then calls the loader below | `0xD864`, see [map.md](map.md#decode-routine-0xd6d1) |
| `0xE13D` | `.mapprj` loader: attribute cell size `+0x0C = tw / 2`, `+0x10 = th / 2` (8 × 8 for every shipped project) | `0xE1C2`–`0xE1E0` |
| **`0xD5B1`** | **`.att` loader** `CMMap::LoadAtt(this, area, map)` | xrefs to `"%s%d%d%s"`, `"/map/"`, `".att"` (`0xE281F`) |
| `0xE4C9` | `AttIndex(x, y) = y · attW + x` | 7 instructions |
| `0xBBFD` | `GetAttCell(x, y)`: `AttIndex ≥ +0x40` gives **2**, otherwise `att[AttIndex]` | the only reader of `+0x3C` after load |
| `0xBC51` | `GetAttAt(px, py) = GetAttCell(px / +0x0C, py / +0x10)` | 23 call sites, listed under [Values](#values) |
| `0xE3F1`, `0xE411` | pixel → cell: `px / +0x0C`, `py / +0x10` (`__divsi3`) | |
| — | `CStateGame +0x2C` holds the current `CMMap`: the port's minimap reads the loaded grid from there ([ADR 0098](../adr/0098-port-drawn-minimap.md); equal to the bundle's `23.att` on map 2.3, `play.minimap`) | `runtime/src/hooks/widescreen.cpp`, `platform/src/minimap.h` |

The file name is `"%s%d%d%s"` of `"/map/"`, area, map and `".att"`, like the `.map`. The buffer comes from `0xB5DD` (whole file plus its size).

## Files

- 91 files in `map/`, the same `(area, map)` names as the 90 `.map` files plus `106.att`.
- Packed files run from 32 B (`76.att`) to 1,328 B (`62.att`). `106.att` is 2,112 B.
- Endianness: little-endian. The cell count in the header is the only multi-byte field.

## Layout

| Offset | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0 | 4 | bytes | magic | `03 41 54 54`, a Pascal string `"ATT"`. **The game never reads it** |
| 4 | 4 | u32 | `cells` | number of cells the runs fill; always `2w × 2h` of the matching `.map`. **The game never reads it** |
| 8 | 2 | u8, u8 | run | `value`, `count`: `count` cells of `value`, row-major from cell 0 |
| … | | | runs | one pair after another until the end of the file |

There is no padding and no terminator: the decoder stops at the end of the buffer. Cells are row-major, `index = y · attW + x` with `attW = 2 · map width`, top row first. Runs freely cross row boundaries.

## Decode routine (`0xD5B1`)

```c
void CMMap::LoadAtt(int area, int map) {                    // 0xD5B1
    int size = 0;
    sprintf(path, "%s%d%d%s", "/map/", area, map, ".att");  // 0xD608
    u8 *buf = ReadFile(g_resources, path, &size);           // 0xD620, 0xB5DD
    int pos = 8, i = 0;                                     // 0xD62A: skip magic and count
    while (pos < size) {                                    // 0xD698
        u8  v = buf[pos++];
        int n = buf[pos++];
        if (v == 1) { i += n; continue; }                   // 0xD658: 1 is already there
        for (int k = 0; k < n; k++) att[i++] = v;           // 0xD672..0xD696
    }
    delete[] buf;
}
```

The game doesn't validate any of this, but the parser rejects all of the following:

- **Magic or count wrong:** ignored, because the loader starts at offset 8 without reading them.
- **Runs past the grid:** `i` is never compared with `attW · attH`, so a long file overflows the heap buffer.
- **Runs that stop short:** the rest of the grid keeps its fill value 1 (air).
- **An odd final byte:** `buf[pos++]` reads one byte past the buffer for the missing count.

None of this happens in the shipped files.

## Values

Each cell is a set of flags. The game always tests single bits (`and r0, r0, #bit` right after `0xBC51`), apart from the minimap, which compares whole values. The meanings below come from what the code does when a bit is set.

| Bit | Name | Meaning | Code |
|---|---|---|---|
| `0x01` | `AIR` | Nothing to stand on. If the cell under an object's feet (`y + 1`) has it, the object starts falling. Mobs turn back before walking into it, so they never walk off a ledge. | `0x6D9E1` (start falling: vertical speed `+0x1C` = the global at `0x11A9D0`, state `|= 1`), `0x6F29D` and `0x72A09` (mob ledge check) |
| `0x02` | `FOOTHOLD` | One-way floor. A falling object lands on the first such cell its path crosses on the way down, at `y = cell top − 1`. Moving up through it does nothing. Spawning and "find the ground below" also snap to it. | `0x6DA65` (landing sweep: rows `ceil8(y0)`…`floor8(y1)`, only when `y1 > y0`), `0x65979` and `0x6F695` (snap down to the next foothold) |
| `0x04` | `LADDER` | A climbable ladder or rope, always 2 cells (one tile) wide. "Grab" searches ±4 px around the hero for it and snaps him to the middle of the pair. Leaving it at the bottom (body middle in air while going down) makes him fall. | `0x61475` (ladder search and snap: `x = (cx & ~7) + 7` or `− 1`), `0x6D759` (ladder movement) |
| `0x08` | `LADDER_TOP` | Stops upward climbing: if the hero's body middle moves into such a cell while climbing up, his motion is cancelled. Only used where a ladder or rope hangs from nothing (areas 4, 7 and 10, and `106.att`). | `0x6D759` (`(mid_old & LADDER) && (mid_new & LADDER_TOP) && vy < 0` → `vx = vy = 0`, `y1 = y0`) |
| `0x10` | `WALL` | Blocks movement in every direction. The sweep over each 8-px cell between the old and new position fails on the first `WALL`. Mobs also turn back at it. | `0x6D3BD` (6 tests, one loop per direction of motion), `0x6F29D` (mob front check) |

`0x6D759` also handles the top of a ladder: with the feet on a `FOOTHOLD|LADDER` cell (`0x06`) and air above, climbing up ends standing on the floor (`y = floor8(y1) + 7`, state cleared). Climbing down onto a `FOOTHOLD` ends the climb.

Looking a cell up past the end of the grid returns **`FOOTHOLD` (2)**, not air (`0xBBFD`), so the bottom edge of every map acts as an invisible floor. A negative index (above the top row) is not checked and reads before the buffer. The parser's `AttFile.cell()` raises `IndexError` there instead.

### Shipped values

Only seven values occur, in all 91 files:

| Value | Flags | Cells (files) in the 90 packed files | Typical use |
|---|---|---|---|
| `0x01` | `AIR` | 425,341 (90) | empty space; also the fill value |
| `0x02` | `FOOTHOLD` | 21,001 (90) | platforms and the ground |
| `0x04` | `LADDER` | 8,368 (83) | ladder and rope shafts |
| `0x06` | `FOOTHOLD\|LADDER` | 736 (82) | where a ladder passes through a platform (the top step) |
| `0x08` | `LADDER_TOP` | 68 (14 files, areas 4, 7 and 10); 8 more in `106.att` | the cap above a ladder or rope that hangs from nothing |
| `0x10` | `WALL` | 1,621 (45) | vertical walls, usually below the edge of a raised floor |
| `0x12` | `FOOTHOLD\|WALL` | 169 (40) | the corner on top of a wall, and the steps of diagonal staircases (`00.att` rows 40–47) |

The minimap (`0x59BB1`) compares whole values: it draws `0x02`, `0x06` and `0x12` in the floor colour, and `0x04` in the ladder colour.

## Worked example: `00.att` (278 bytes, map 40 × 28 → 80 × 56 cells)

```
00000000: 03 41 54 54 80 11 00 00 01 ff 01 ff 01 ff 01 ed   .ATT............
00000010: 02 04 01 76 02 0e 06 02 02 12 01 0a 02 0b 06 02   ...v............
```

| Bytes | Meaning |
|---|---|
| `03 41 54 54` | magic `03 "ATT"` |
| `80 11 00 00` | 4,480 cells = 80 × 56 |
| `01 FF` × 3, `01 ED` | 1,002 cells of air (rows 0–11, then row 12 up to column 41). The game skips these. |
| `02 04` | 4 cells of foothold: row 12, columns 42–45 (a small ledge) |
| `01 76` | 118 cells of air |
| `02 0E` `06 02` `02 12` | row 14: a 14-cell floor, a 2-cell ladder top, and 18 more floor cells |

The rows below that carry the ladder (`04 02` at the same two columns). Row 50 is one 80-cell `FOOTHOLD` run, which is the ground.

## `106.att`: a leftover raw grid

`106.att` would be area 10, map 6, but there is no `106.map` or `106.obj`. It is also not in the format above. It has no magic and no count: it is **2,112 raw bytes, one flag byte per cell**. Laid out 44 cells wide (a 22 × 24-tile map of 44 × 48 cells) it reads cleanly. Its ladders are unbroken 2-cell columns capped with `LADDER_TOP`, as in the shipped maps of areas 4, 7 and 10. Row 42 is a full-width floor, and every byte is one of `01 02 04 06 08`. No other width divides 2,112 with that result: at 88 or 66 wide the ladders break up.

The game would never load it correctly. The run decoder would skip the first 8 bytes and then read the raw cells as `(value, count)` pairs. It is most likely an early version of a map exported in an older uncompressed format and left in the bundle. Whether any script warps to (10, 6) is for the script work to confirm ([#19](https://github.com/armaatus/maplethief-port/issues/19)). With no `.map` for it, that warp could not work anyway.

## Validation

| Check | Result |
|---|---|
| Files decoded | **91 / 91**: 90 packed with zero errors, plus `106.att` as a raw grid (`tests/test_attobj_real.py`, local only) |
| Header | magic `03 "ATT"` in all 90 packed files; `cells` equals the run total and `4 · w · h` of the matching `.map` in every file |
| Trailing bytes | none: the run pairs end exactly at the end of every file |
| Re-encode | `encode(decode(f)) == f` for all 90 packed files (runs of at most 255 equal cells, value 1 included) |
| Values | exactly the seven listed above |
| Runs | 15,182 runs, 7,468 of them value-1 skips; no zero-length runs |

CI runs only the synthetic tests (`tests/test_attfile.py`).

## Parser

[`tools/mtport/attfile.py`](../../tools/mtport/attfile.py) is built on the shared [`bytereader`](../../tools/mtport/bytereader.py).

- `decode(data, name) -> AttFile(cells, packed=True)`: the strict run decoder. It raises `FormatError` on a bad magic, a truncated pair, runs that overshoot the header count, or runs that stop short of it.
- `decode_raw(data)` reads the headerless grid of `106.att`; it accepts only bytes that are known flag sets. `decode_any(data)` picks between the two by the magic.
- `AttFile.rows(width)` and `AttFile.cell(width, x, y)` (game semantics: past the end gives `FOOTHOLD`). The width comes from the `.map`: `grid_size(map_w, map_h)`.
- `encode(AttFile)` writes the original tool's run choices. `flag_names(v)` and the constants `AIR`, `FOOTHOLD`, `LADDER`, `LADDER_TOP`, `WALL` name the flags.
- CLI: `python3 -m mtport.attfile info FILE...` and `python3 -m mtport.attfile dump FILE [--width W]` (one character per cell: `.` air, `=` foothold, `#` ladder, `H` foothold+ladder, `^` ladder top, `|` wall, `+` foothold+wall). Run them with `PYTHONPATH=tools`.

## Unknowns

- Bits `0x20`–`0x80`: no code tests them and no file uses them.
- How the hero walks up the `FOOTHOLD|WALL` staircases despite the wall sweep. A step-up rule in the movement code (`0x66189`, `0x69361`) is not traced yet.
- Which object classes own `+0x6C`…`+0x7C` (old and new position, velocity) in `0x6D3BD`, `0x6D759`, `0x6DA65`. Both the hero and mobs go through these helpers, and the class layout is for the runtime work.
