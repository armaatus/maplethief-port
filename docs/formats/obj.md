# `.obj`: map objects (portals, NPCs, mob spawns, map script)

**Status: fully specified.** All 90 shipped files decode with zero errors and no trailing bytes, and re-encode byte for byte. Issue [#14](https://github.com/armaatus/maplethief-port/issues/14).

A `.obj` file lists the objects placed on one map, one record per object: a tile position, a one-byte **kind** and a **script name**. Portals, NPCs (shops included) and mob spawn points are all records of the same shape. Only the kind tells them apart. At most one extra record, with position −1, names the **map script**. Portal destinations are **not** in this file: they live in the portal's script (see [Portals](#portals-and-arrival-points)).

## Where the game loads and reads it

Addresses are armv7 VM addresses, and function pointers carry the Thumb bit. The names are descriptive until Ghidra scripts name them ([#7](https://github.com/armaatus/maplethief-port/issues/7)). Offsets such as `+0x48` are into the `CMMap` object.

| Address | What | Evidence |
|---|---|---|
| `0x86699` | map change: calls `.mapprj`, `.map`, then this loader | `bl 0xD259` at `0x867F6` / `0x86862` |
| **`0xD259`** | **`.obj` loader** `CMMap::LoadObj(this, area, map)` | xrefs to `"%s%d%d%s"`, `"/map/"`, `".obj"` (`0xE281A`) |
| `0xB689` | read a whole bundle file (`0xB5DD` without returning the size) | so the loader has **no bounds at all** |
| `0x1BC61` | `ReadI16(buf, off)`, sign-extending | see [map.md](map.md#where-the-game-loads-it) |
| `0x1BD99` | `u8 → int` (identity) | used for `kind` |
| `0x1BDB1` | `StrDup(buf, off, len)`: `new char[len + 1]`, zeroed, copy `len` bytes | used for `name` |
| `0x5BC61`, `0xBD95`, `0xBD75`, `0xBE69` | object count (`+0x44`), `cell`, `kind`, `name` of object *i* | 9-instruction getters |
| `0xBCBD`, `0xBCE5` | tile x = `cell % w`, tile y = `cell / w` (`0xE469`, `0xE489`, `w` at `+0x20`) | |
| `0xBD0D`, `0xBD41` | pixel anchor: x = `tx · tw + 8`, y = `(ty + 1) · th − 1` | bottom-centre pixel of the tile |
| `0xBDB5`, `0xBE31` | "is there an object at this tile / pixel" (compares `cell` with `y · w + x`) | loops over `+0x4C` |
| `0x77791` | map script name (`+0x64`) | |
| `0x86289` | spawns the objects of a new map (sprites, mobs) | [Kinds](#kinds) |
| `0x868D5` | warp: places the hero on the arrival point | [Portals](#portals-and-arrival-points) |
| `0x8572D` | per frame: draws sprite objects, shows the "up" hint near portals and the NPC button near NPCs | `offUpIndicate`, `onUpIndicate`, `setNpcPosition:`, `setIsNearNPC:` selectors |
| `0x63191` | interaction: enter a portal or talk to an NPC, run its script | `"/script/chapter%d/%s.out"` (`0xE50D7`), `"/script/common/%s.out"` (`0xE50F0`), `"lua=%s, num=%d\n"` |
| `0x59BB1` | minimap: blinking dots for portals, dots for visible NPCs | |

The file name is `"%s%d%d%s"` of `"/map/"`, area, map and `".obj"`, like the `.map`.

## Files

- 90 files in `map/`, one per `.map`, with the same `(area, map)` names.
- Sizes run from 1 B (`416.obj`, no records) to 125 B (`10.obj`).
- Endianness: little-endian. `cell` is the only multi-byte field.

## Layout

| Offset | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0 | 1 | u8 | `count` | number of records, map-script record included. Shipped: 0–23 |
| 1 | … | records | `count` × record | back to back; the file ends after the last one |

Each record:

| Offset | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0 | 2 | i16 | `cell` | tile index on the `.map` grid, `y · width + x`. **−1** (`FF FF`) marks the map-script record |
| 2 | 1 | u8 | `kind` | what the object is ([Kinds](#kinds)); `0xFF` on the map-script record. The loader turns `0xFF` into −1 on any record |
| 3 | 1 | u8 | `len` | name length, 0–255 |
| 4 | `len` | char | `name` | the object's script name, ASCII, no terminator; empty for most mobs |

In every shipped file the objects are sorted by `cell` and the map-script record comes first. The loader requires neither.

### The `|` in names

No name contains `|`. The `portal000|` from the first look at `00.obj` comes from reading across a record boundary: `7C` is the low byte of the next record's `cell`. That record is `7C 02` = cell 636, so the bytes `7C 02 07 08 "Darklord"` are cell 636, kind 7, length 8, name `"Darklord"`. The real-asset test checks that no name contains `|`.

## Load routine (`0xD259`)

```c
void CMMap::LoadObj(int area, int map) {                    // 0xD259
    sprintf(path, "%s%d%d%s", "/map/", area, map, ".obj");
    u8 *buf = ReadFile(g_resources, path);                  // 0xB689: no size
    int total = buf[0];
    this->count  = buf[0];                                  // +0x44
    this->script = "";                                      // +0x64, the empty string 0xE271D
    int pos = 1;
    for (i = 0; i < this->count; i++) {                     // pass 1 (0xD2EA): find the map script
        i16 cell = ReadI16(buf, pos);  int len = buf[pos + 3];  pos += 4;
        if (cell == -1) {
            this->script = StrDup(buf, pos, len);           // 0xD336
            this->count -= 1;                               // it is not an object
            break;
        }
        pos += len;
    }
    if (this->count <= 0) { this->count2 = 0; this->six = 6; this->count3 = 0; goto done; }
    this->obj  = new Obj*[count];  each = new Obj (12 bytes); // +0x48
    this->count2 = count;  this->six = 6;                   // +0x4C, +0x50
    this->name = new char*[count];  this->count3 = count;   // +0x54, +0x58
    pos = 1;
    for (j = 0, i = 0; j < total; j++) {                    // pass 2 (0xD436)
        if ((u16)ReadI16(buf, pos) == 0xFFFF) { pos += 4 + buf[pos + 3]; continue; }
        obj[i]->cell  = ReadI16(buf, pos);                  // Obj+0
        obj[i]->kind  = buf[pos + 2] == 0xFF ? -1 : buf[pos + 2];   // Obj+2
        name[i]       = StrDup(buf, pos + 4, buf[pos + 3]);
        obj[i]->state = -1; obj[i]->visible = 1;            // Obj+4, Obj+6
        obj[i]->facing = 0; obj[i]->frame = 0;              // Obj+8, Obj+0xA
        pos += 4 + buf[pos + 3];  i++;
    }
done:
    delete[] buf;
}
```

The 12-byte `Obj` keeps the two file fields and four runtime fields. Scripts and the game change the runtime fields through setters (`0xBE8D`/`0xBEA5` for `+4`, `0xBEC5`/`0xBEF5` for `+6`, `0xBEDD`/`0xBF15` for `+8`, `0xC051`/`0xBF35` for `+0xA`). `0xBF55`/`0xBFD5` move an object by rewriting its `cell`. A hidden object (`visible == 0`) is neither drawn (`0x8572D`) nor interactive (`0x63191`). The renderer flips the sprite when `facing == 2`: sprites face left unflipped, so `facing == 2` faces right (checked in the game, issue #27).

The game doesn't validate any of this, but the parser rejects all of the following:

- **Truncation:** nothing is checked. A short file reads past the end of the buffer.
- **Trailing bytes:** ignored.
- **A second map-script record:** pass 1 stops at the first one and sizes the arrays for `total − 1`. Pass 2 skips every `−1` record, so each extra one leaves an `Obj*` slot that is never written.
- **A cell below −1:** stored as is, and gives a negative tile position.

None of this happens in the shipped files.

## Kinds

| `kind` | What | Game behaviour | Shipped |
|---|---|---|---|
| 0–2 | **portal**, number = `kind` | Sprite `/sprite/mob/<kind>.sprprj` (frames `portal01`–`03`). Shows the "up" hint when the hero is near, blinks on the minimap. Entering it runs its script. It is also an arrival point. | 153 (152 named) |
| 3 | **hidden portal**, number 3 | As a portal, but `0x8572D` skips it (not drawn, no hint) and the minimap leaves it out. It can still be entered, and it can still be an arrival point. | 7 (3 named) |
| 4–59 | **NPC**, NPC id = `kind − 4` | Sprite `/sprite/mob/<kind>.sprprj` (frames `npc<kind−4>.png`). Shows the NPC button when the hero is near. Talking sets the world's current NPC id (`+0xAC = kind − 4`) and runs the script. An NPC without a name is decoration. | 83 (60 named), kinds 4–39 |
| 40, 41 | special NPCs | `0x63191` handles them before the generic path: it sets `+0xAC = kind − 4` and `+0xCC = 0` or `1`, then calls `0x823A1` with no script | none |
| 42 | special marker | `0x82271` looks for an object of this kind and stores its position | none |
| 60–254 | **mob spawn** of `mob.dat` id `kind − 59` (ids are 1-based, and `mob.dat`'s `sprite` = id + 59, so the kind is also the mob's sprite project) | `0x86289` spawns a mob at the anchor with `0x77055(mobs, kind − 59, x, y)`. No sprite object and no script. | 772, kinds 60–128, all unnamed |
| `0xFF` | the map-script record (`cell = −1`) | the name goes to `+0x64` | 6 |
| `0xFF` on an object | inert | the loader stores it as −1, which every kind test above rejects; the parser calls it `inert` | none |

`0x86289` loads the sprite project of every kind below 60 once per map (an array of 60 pointers at world `+0x3C`, filled through `0x20005(spr, "mob", kind)`, which formats `"%s%s%s%d%s"` with `"/sprite/"` and `".sprprj"`). The `mob` sprite set therefore holds the portal and NPC sprites too: `sprite/mob/0`–`3` are portals, and `4` onwards are `npc0`, `npc1`, …. The `.sprprj` format itself is [#16](https://github.com/armaatus/maplethief-port/issues/16). What mob type `n` means comes from the data tables ([#18](https://github.com/armaatus/maplethief-port/issues/18)).

## Names and scripts

`name` is a script name. `0x63191` handles interaction. For an NPC, the hero's tile at x, x − 16 or x + 16 must be the object's tile. For a portal, his tile at x, x − 8 or x + 8 must be the portal's tile, and the routine's second argument must be set (presumably the up key). The game then loads `/script/chapter<c>/<name>.out`, with `c` read from the world's `+0x58 → +4`. If that file doesn't exist, it falls back to `/script/common/<name>.out`. Then it runs the script. An empty name means no script: nothing happens.

Every name in the shipped files has a script: each portal name has a `script/common/` file, and each NPC and map-script name has one in `common/` or a `chapter<n>/` directory. The main update (`0x87501` at `0x879B6`–`0x87B64`) looks up the map-script record's name (`PerionMap`, `BalogMap`, `TutorialMap`, `MelodyMap`, `WolfMap`, `HomunMap`) the same way.

Shops are not a record type. `Equip00`/`Etc00` (kinds 5 and 6) are ordinary NPCs. Judging by their names, their scripts open the equipment and etc. shops ([#19](https://github.com/armaatus/maplethief-port/issues/19)).

## Portals and arrival points

A portal record holds only its own position and number. Where it leads is set by its script: every named portal has `script/common/portal….out` (some with chapter overrides). The script calls the native `changeMap(area, map, portal)` (handler `0x99969`, [script-vm.md](../re/script-vm.md#natives)), whose `CMapleWorld::changeMap` (`0x81369`) stores the target in the world object at `+0x98`/`+0x9C`/`+0xA0`, sets `+0xA4`/`+0xA8` to −1 and raises the warp flag `+0xB4`. The world update `0x83C71` also writes `+0x98`/`+0x9C` directly for a few hard-coded scene exits. The map change (`0x868D5`) then loads the target map and puts the hero at the object whose `kind` equals the target portal number: x = anchor + 16, y = anchor. It then calls `0xC175` (presumably the camera) with x ± 30 px, depending on which way the hero faces. It loops over every object without stopping, so the last match wins. An explicit target position (`+0xA4`/`+0xA8` > −1) skips the search. `0x671BD` (called from `0x8201D` and `0x8D4C9`, probably revive) puts the hero on the `kind 0` object the same way.

Portal names usually spell `portal<area><map><number>`, with the map's file name followed by `kind`: `portal000` and `portal001` in `00.obj`, `portal1031` in `103.obj`. Eight portals don't follow the pattern (for example `24.obj` has `portal242` with kind 0, and `93.obj` has `portal932` with kind 3). The name only picks the script, so the kind is what counts.

## Worked examples

`00.obj` (82 bytes, map 40 × 28 tiles, no map script):

```
00000000: 08 f6 00 10 05 73 6d 69 74 68 0b 01 09 04 41 6c   .....smith....Al
00000010: 65 78 12 01 00 09 70 6f 72 74 61 6c 30 30 30 7c   ex....portal000|
00000020: 02 07 08 44 61 72 6b 6c 6f 72 64 fa 02 05 07 45   ...Darklord....E
```

| Bytes | Meaning |
|---|---|
| `08` | 8 records |
| `F6 00` `10` `05 "smith"` | cell 246 = tile (6, 6), kind 16 = NPC 12, script `smith` |
| `0B 01` `09` `04 "Alex"` | cell 267 = tile (27, 6), NPC 5, script `Alex` |
| `12 01` `00` `09 "portal000"` | cell 274 = tile (34, 6), portal 0, anchor pixel (552, 111) |
| `7C 02` `07` `08 "Darklord"` | cell 636 = tile (36, 15), NPC 3 |
| … | `Equip00` (kind 5), `Etc00` (6), `JM` (8), then `portal001` (portal 1) at cell 996 = tile (36, 24) |

`10.obj` starts with the map-script record:

```
00000000: 0c ff ff ff 09 50 65 72 69 6f 6e 4d 61 70 da 00   .....PerionMap..
```

`0C`: 12 records. `FF FF` `FF` `09 "PerionMap"`: cell −1, kind 0xFF, the map script. Then `DA 00` = cell 218, the first object (`pig`, kind 15).

## Validation

| Check | Result |
|---|---|
| Files decoded | **90 / 90**, zero errors (`tests/test_attobj_real.py`, local only) |
| Trailing bytes | none in any file |
| Re-encode | `encode(decode(f)) == f` for all 90 |
| Positions | every object `cell` is inside its `.map` (`0 ≤ cell < w · h`) |
| Records | 1,021: 6 map scripts, 153 portals, 7 hidden portals, 83 NPCs, 772 mob spawns |
| Names | ASCII, no `|`, no NUL; every mob spawn unnamed; every named portal or hidden portal named `portal…`; every name has a script in `script/common/` or `script/chapter<n>/` (portal names in `common/`) |

CI runs only the synthetic tests (`tests/test_objfile.py`).

## Parser

[`tools/mtport/objfile.py`](../../tools/mtport/objfile.py) is built on the shared [`bytereader`](../../tools/mtport/bytereader.py).

- `decode(data, name) -> ObjFile(records)`: the records in file order, each a `Record(cell, kind, name)`. It raises `FormatError` on truncation, trailing bytes, a non-ASCII name, a name with a NUL, a cell below −1, or a second map-script record.
- `ObjFile.objects` holds the records the game keeps (all but the map-script record), and `ObjFile.map_script` is that record's name. `Record.category` is one of `map_script`, `portal`, `hidden_portal`, `npc`, `mob` or `inert`. `Record.tile(map_width)` and `Record.position(map_width)` give the tile and the pixel anchor.
- `encode(ObjFile) -> bytes`.
- CLI: `python3 -m mtport.objfile info FILE...` and `python3 -m mtport.objfile dump FILE` (it adds tile and pixel positions when the `.map` sits next to the file). Run them with `PYTHONPATH=tools`.

## Unknowns

- `CMMap+0x50`, always set to 6 by the loader; nothing found reads it yet.
- The runtime fields `Obj+4` (set to −1 at load) and `Obj+0xA` (an animation frame?): their setters are called from the script VM, which [#19](https://github.com/armaatus/maplethief-port/issues/19) will map.
- Kinds 40, 41 and 42, which have special code but appear in no shipped file. They may come from scripts that move or create objects.
- When exactly the map script runs (on map entry, or on every revisit), and which chapter number picks the override directory. Both are for #19.
