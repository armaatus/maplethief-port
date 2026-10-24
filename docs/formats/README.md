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
| `.mapprj` tileset projects | 11 in `map/` | – | – | unknown (`u16 tw, u16 th, u8 n, pstr…`?) | [#15](https://github.com/armaatus/maplethief-port/issues/15) |
| `map/back/`, `map/tile/` | 22 PNGs | – | – | standard PNG; usage unknown | [#15](https://github.com/armaatus/maplethief-port/issues/15) |
| `.sprprj` sprite projects | 138 in `sprite/*/` | – | – | unknown (frame name list + records) | [#16](https://github.com/armaatus/maplethief-port/issues/16) |
| `png/**/*.dat` | 40 | – | – | **plain PNG** files; atlas slicing unknown | [#17](https://github.com/armaatus/maplethief-port/issues/17) |
| Data tables `*/data/*.dat` | 8 × 3 languages | – | – | unknown (u16 header, fixed-width names; CP949 for Korean) | [#18](https://github.com/armaatus/maplethief-port/issues/18) |
| `.out` scripts | 307 in `script/` | – | – | unknown (header `1B 46 00 00`, custom Lua-like VM) | [#19](https://github.com/armaatus/maplethief-port/issues/19) |
| `Scripts.strings`, `ScriptExtra.strings` | `en.lproj`, `ko.lproj` | – | – | Apple binary plist (known); key scheme to document | [#19](https://github.com/armaatus/maplethief-port/issues/19) |
| Save data | runtime-created | – | – | unknown (`CDataMgr::SaveSaveData`, NSKeyedArchiver?) | [#20](https://github.com/armaatus/maplethief-port/issues/20) |
| CgBI PNG | 113 (root UI + `English.lproj`) | – | – | known publicly; un-crusher needed | [#24](https://github.com/armaatus/maplethief-port/issues/24) |
| `MainWindow.nib` | 1 | – | – | `NIBArchive` (known publicly) | [#22](https://github.com/armaatus/maplethief-port/issues/22) |
