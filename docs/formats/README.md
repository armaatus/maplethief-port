# File format specs

One Markdown spec per reverse-engineered format. Each spec must have a parser in `tools/` with tests (synthetic fixtures in `tests/`, plus a local "parse every shipped file" check against `work/ipa/`). First-look notes for each format are in [ANALYSIS.md → Data layout](../ANALYSIS.md#data-layout).

## Spec template

Each spec should contain: purpose and where the game loads it (function addresses), file locations and counts, endianness/encoding, a byte-level layout table, worked example(s) from a real file (small hex excerpts are fine), unknown fields clearly marked, the parser location, and validation status (how many shipped files parse cleanly).

## Index

| Format | Files | Spec | Parser | Status | Issue |
|---|---|---|---|---|---|
| `.map` tile layers | 90 in `map/` | – | – | unknown (header `u16 w, u16 h`, RLE-like body) | [#13](https://github.com/armaatus/maplethief-port/issues/13) |
| `.att` attributes/collision | 91 in `map/` (`106.att` orphan) | – | – | unknown (magic `03 "ATT"`) | [#14](https://github.com/armaatus/maplethief-port/issues/14) |
| `.obj` map objects | 90 in `map/` | – | – | unknown (Pascal strings + ints) | [#14](https://github.com/armaatus/maplethief-port/issues/14) |
| `.mapprj` tileset projects | 11 in `map/` | – | – | unknown (`u16 tw, u16 th, u8 n, pstr…`?) | [#15](https://github.com/armaatus/maplethief-port/issues/15) |
| `map/back/`, `map/tile/` | 22 PNGs | – | – | standard PNG; usage unknown | [#15](https://github.com/armaatus/maplethief-port/issues/15) |
| `.sprprj` sprite projects | 138 in `sprite/*/` | – | – | unknown (frame name list + records) | [#16](https://github.com/armaatus/maplethief-port/issues/16) |
| `png/**/*.dat` | 40 | – | – | **plain PNG** files; atlas slicing unknown | [#17](https://github.com/armaatus/maplethief-port/issues/17) |
| Data tables `*/data/*.dat` | 8 × 3 languages | [data-tables.md](data-tables.md) | `tools/mtport/datatables.py` | **specified**: 7 fixed-record tables (`u16 columns, rows, stride` + records) + `buildup.dat` (ASCII int); all 24 files parse, a few columns unnamed | [#18](https://github.com/armaatus/maplethief-port/issues/18) |
| `.out` scripts | 307 in `script/` | – | – | unknown (header `1B 46 00 00`, custom Lua-like VM) | [#19](https://github.com/armaatus/maplethief-port/issues/19) |
| `Scripts.strings`, `ScriptExtra.strings` | `en.lproj`, `ko.lproj` | – | – | Apple binary plist (known); key scheme to document | [#19](https://github.com/armaatus/maplethief-port/issues/19) |
| Save data | runtime-created | – | – | unknown (`CDataMgr::SaveSaveData`, NSKeyedArchiver?) | [#20](https://github.com/armaatus/maplethief-port/issues/20) |
| CgBI PNG | 113 (root UI + `English.lproj`) | – | – | known publicly; un-crusher needed | [#24](https://github.com/armaatus/maplethief-port/issues/24) |
| `MainWindow.nib` | 1 | – | – | `NIBArchive` (known publicly) | [#22](https://github.com/armaatus/maplethief-port/issues/22) |
