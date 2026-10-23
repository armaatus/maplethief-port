# File format specs

One Markdown spec per reverse-engineered format. Each spec must have a parser in `tools/` with tests (synthetic fixtures in `tests/`, plus a local "parse every shipped file" check against `work/ipa/`). First-look notes for each format are in [ANALYSIS.md → Data layout](../ANALYSIS.md#data-layout).

## Spec template

Each spec should contain: purpose and where the game loads it (function addresses), file locations and counts, endianness/encoding, a byte-level layout table, worked example(s) from a real file (small hex excerpts are fine), unknown fields clearly marked, the parser location, and validation status (how many shipped files parse cleanly).

## Index

| Format | Files | Spec | Parser | Status | Issue |
|---|---|---|---|---|---|
| `.map` tile layers | 90 in `map/` | – | – | unknown (header `u16 w, u16 h`, RLE-like body) | M1-07 |
| `.att` attributes/collision | 91 in `map/` (`106.att` orphan) | – | – | unknown (magic `03 "ATT"`) | M1-08 |
| `.obj` map objects | 90 in `map/` | – | – | unknown (Pascal strings + ints) | M1-08 |
| `.mapprj` tileset projects | 11 in `map/` | – | – | unknown (`u16 tw, u16 th, u8 n, pstr…`?) | M1-09 |
| `map/back/`, `map/tile/` | 22 PNGs | – | – | standard PNG; usage unknown | M1-09 |
| `.sprprj` sprite projects | 138 in `sprite/*/` | – | – | unknown (frame name list + records) | M1-10 |
| `png/**/*.dat` | 40 | – | – | **plain PNG** files; atlas slicing unknown | M1-11 |
| Data tables `*/data/*.dat` | 8 × 3 languages | – | – | unknown (u16 header, fixed-width names; CP949 for Korean) | M1-12 |
| `.out` scripts | 307 in `script/` | – | – | unknown (header `1B 46 00 00`, custom Lua-like VM) | M1-13 |
| `Scripts.strings`, `ScriptExtra.strings` | `en.lproj`, `ko.lproj` | – | – | Apple binary plist (known); key scheme to document | M1-13 |
| Save data | runtime-created | – | – | unknown (`CDataMgr::SaveSaveData`, NSKeyedArchiver?) | M1-14 |
| CgBI PNG | 113 (root UI + `English.lproj`) | – | – | known publicly; un-crusher needed | M2-01 |
| `MainWindow.nib` | 1 | – | – | `NIBArchive` (known publicly) | M1-16 |
