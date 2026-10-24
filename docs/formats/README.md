# File format specs

One Markdown spec per reverse-engineered format. Each spec must have a parser in `tools/` with tests (synthetic fixtures in `tests/`, plus a local "parse every shipped file" check against `work/ipa/`). First-look notes for each format are in [ANALYSIS.md → Data layout](../ANALYSIS.md#data-layout).

## Spec template

Each spec should contain: purpose and where the game loads it (function addresses), file locations and counts, endianness/encoding, a byte-level layout table, worked example(s) from a real file (small hex excerpts are fine), unknown fields clearly marked, the parser location, and validation status (how many shipped files parse cleanly).

## Index

| Format | Files | Spec | Parser | Status | Issue |
|---|---|---|---|---|---|
| `.map` tile layers | 90 in `map/` | [map.md](map.md) | [`tools/mtport/mapfile.py`](../../tools/mtport/mapfile.py) | **specified**: `i16 w, i16 h, u8 layers` + byte RLE per layer; 90/90 decode, byte-exact re-encode | [#13](https://github.com/armaatus/maplethief-port/issues/13) |
| `.att` attributes/collision | 91 in `map/` (`106.att` orphan) | [att.md](att.md) | [`tools/mtport/attfile.py`](../../tools/mtport/attfile.py) | **specified**: `03 "ATT"`, u32 cells, `(value, count)` runs over a 2w×2h grid of flag bytes; 90/90 packed decode and re-encode byte-exact, `106.att` is a raw 44×48 grid | [#14](https://github.com/armaatus/maplethief-port/issues/14) |
| `.obj` map objects | 90 in `map/` | [obj.md](obj.md) | [`tools/mtport/objfile.py`](../../tools/mtport/objfile.py) | **specified**: `u8 count` + `i16 cell, u8 kind, pstr script`; portals, NPCs, mob spawns, map script; 90/90 decode, byte-exact re-encode | [#14](https://github.com/armaatus/maplethief-port/issues/14) |
| `.mapprj` tileset projects | 11 in `map/` | [mapprj.md](mapprj.md) | [`tools/mtport/mapprj.py`](../../tools/mtport/mapprj.py) | **specified**: `i16 tw, i16 th, u8 n` + `n` Pascal-string sheet names; 11/11 parse, byte-exact re-encode; map → tile → pixel rect resolution | [#15](https://github.com/armaatus/maplethief-port/issues/15) |
| `map/back/`, `map/tile/` | 22 PNGs | [mapprj.md](mapprj.md#mapback-the-area-backdrop) | [`tools/mtport/pnginfo.py`](../../tools/mtport/pnginfo.py) (header only) | **specified**: standard PNGs; `tile/` = one 16×16 sheet per area, `back/` = fixed 480×320 backdrop per area (no parallax; `7.png` unused) | [#15](https://github.com/armaatus/maplethief-port/issues/15) |
| `.sprprj` sprite projects | 138 in `sprite/*/` | [sprprj.md](sprprj.md) | [`mtport/sprprj.py`](../../tools/mtport/sprprj.py) | **specified**; 138/138 parse with no leftover bytes, every frame's PNGs exist | [#16](https://github.com/armaatus/maplethief-port/issues/16) |
| `png/**/*.dat` PNG packs | 40 in `png/` | [png-dat.md](png-dat.md) | [`tools/mtport/pngdat.py`](../../tools/mtport/pngdat.py) | **specified**: 2–109 complete PNGs back to back, cut by the length table `u32[40][255]` at `0xFC740`; 40/40 split (473 images), all match the table; 5 `mainmenu/` packs never loaded | [#17](https://github.com/armaatus/maplethief-port/issues/17) |
| Data tables `*/data/*.dat` | 8 × 3 languages | [data-tables.md](data-tables.md) | `tools/mtport/datatables.py` | **specified**: 7 fixed-record tables (`u16 columns, rows, stride` + records) + `buildup.dat` (ASCII int); all 24 files parse, a few columns unnamed | [#18](https://github.com/armaatus/maplethief-port/issues/18) |
| `.out` scripts | 307 in `script/` | – | – | unknown (header `1B 46 00 00`, custom Lua-like VM) | [#19](https://github.com/armaatus/maplethief-port/issues/19) |
| `Scripts.strings`, `ScriptExtra.strings` | `en.lproj`, `ko.lproj` | – | – | Apple binary plist (known); key scheme to document | [#19](https://github.com/armaatus/maplethief-port/issues/19) |
| Save data | runtime-created in `Documents/` | [save.md](save.md) | [`tools/mtport/savedata.py`](../../tools/mtport/savedata.py) | **specified**: raw little-endian files via `NSFileHandle` (no NSUserDefaults/NSKeyedArchiver/checksum); `theif0..2` (1,124 B, every `SaveSaveData` field), `quest0..2` (4,781 B), `config`, `cupon`, `CTerms`, receipt lists; synthetic saves round-trip byte-exact, no device save checked yet | [#20](https://github.com/armaatus/maplethief-port/issues/20) |
| CgBI PNG | 113 (root UI + `English.lproj`) | – | – | known publicly; un-crusher needed | [#24](https://github.com/armaatus/maplethief-port/issues/24) |
| `MainWindow.nib` | 1 | – | – | `NIBArchive` (known publicly) | [#22](https://github.com/armaatus/maplethief-port/issues/22) |
