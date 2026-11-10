# NRO and NACP: the Nintendo Switch homebrew executable

**Status: specified** for the parts we read and write. This is not a game format: it is the container our own Switch build writes ([ADR 0138](../adr/0138-nintendo-switch-homebrew-target.md), [#235](https://github.com/armaatus/maplethief-port/issues/235)). devkitPro's `elf2nro` (through `nx_create_nro` in `Switch.cmake`) writes `MapleThief.nro`; we read it back to check what the build produced and to run the release guard on it. The layout follows the public descriptions on switchbrew ([NRO](https://switchbrew.org/wiki/NRO), [NACP](https://switchbrew.org/wiki/NACP)).

- **Parser**: [`tools/mtport/nro.py`](../../tools/mtport/nro.py) (`parse`, `parse_nacp`, `members`, `check`; `build` and `build_nacp` write small files for the tests). CLI: `python3 -m mtport.nro info|check|unpack`.
- **Tests**: [`tests/test_nro.py`](../../tests/test_nro.py): a synthetic NRO laid out field by field from switchbrew, NACPs, malformed headers, the release guard on NROs. `test_switch_job_nro` reads the `MapleThief.nro` that `scripts/build-switch` leaves in `build/switch-*/` and skips when there is none. The Switch build runs CTest `platform.switch_nro` (`mtport.nro check`) and `platform.switch_guard` (`mtport.release guard`) on its own NRO.
- **Where it is used**: the `switch` job (CI and `scripts/local-ci`), `mtport.release` (the guard splits an NRO into members, so each rule sees the code, the icon, the NACP and the RomFS separately).

All integers are little-endian. Offsets are from the start of the file unless the table says otherwise.

## Layout

An NRO is three parts back to back:

1. `NroStart` (0x10 bytes) and the NRO header (0x70 bytes), so the code starts at 0x80;
2. the text, ro and data segments, up to the header's `size`;
3. optionally, at offset `size`, the **asset section**: its header, the icon, the NACP and a RomFS.

### NroStart and header

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0x00 | 4 | unused | a branch instruction in practice |
| 0x04 | 4 | MOD0 offset | not read |
| 0x08 | 8 | padding | |
| 0x10 | 4 | magic | `NRO0` |
| 0x14 | 4 | version | 0 |
| 0x18 | 4 | size | the NRO without the asset section; must lie inside the file |
| 0x1C | 4 | flags | |
| 0x20 | 8 | text segment | u32 offset, u32 size |
| 0x28 | 8 | ro segment | u32 offset, u32 size |
| 0x30 | 8 | data segment | u32 offset, u32 size |
| 0x38 | 4 | bss size | |
| 0x3C | 4 | reserved | |
| 0x40 | 0x20 | build id | |
| 0x60 | 0x20 | reserved, then the dynamic/API-info sections | not read |

The reader checks the magic, that `size` lies inside the file, and that each segment ends at or before `size`.

### Asset section

At offset `size`, if the file goes on:

| Offset (from the asset header) | Size | Field |
|---|---|---|
| 0x00 | 4 | magic `ASET` |
| 0x04 | 4 | version, 0 |
| 0x08 | 16 | icon: u64 offset, u64 size |
| 0x18 | 16 | NACP: u64 offset, u64 size |
| 0x28 | 16 | RomFS: u64 offset, u64 size |

The header is 0x38 bytes. Each offset is relative to the asset header; a size of 0 means the part is absent. Bytes after the NRO that do not start with `ASET` are an error; bytes after the last asset are allowed and ignored.

The **icon** is a JPEG. The Homebrew Menu expects 256 x 256; `mtport.nro check --icon-size 256` requires a baseline JPEG of exactly that size ([`tools/mtport/jpeg.py`](../../tools/mtport/jpeg.py) reads its frame header). Ours is generated at build time from our own drawing (`python3 -m mtport.appicon switch-icon`, the iOS icon redrawn at 256 x 256 and encoded deterministically), so its SHA-256 is on the release guard's icon allowlist.

### NACP

The NACP is 0x4000 bytes. We read and write only:

| Offset | Size | Field |
|---|---|---|
| 0x0000 + 0x300·i | 0x200 | title *i*: application name, NUL-terminated UTF-8 |
| 0x0200 + 0x300·i | 0x100 | title *i*: author |
| 0x3060 | 0x10 | display version, NUL-terminated |

There are 16 title entries, one per language, American English first. The Homebrew Menu shows the first language that has a name. `nacptool` (through `nx_generate_nacp`) fills every language with the same name and author; ours are `MapleThief`, `armaatus` and the project version (`MAPLETHIEF_VERSION`).

## Members (release guard)

`mtport.nro.members` and the release guard split an NRO into `code.bin` (the bytes up to `size`), `icon.jpg`, `control.nacp` and `romfs.bin` (the guard names the RomFS `data.romfs`, so its archive rule refuses one: the player build carries no assets, ADR 0100). A guard path for a member is `<nro name>/<member>`, for example `MapleThief.nro/icon.jpg`.

## Validation

- Synthetic NROs in CI: the handmade one, `build`'s round trip, truncated or wrong-magic headers, out-of-range sizes, trailing bytes without `ASET`.
- The `switch` job's `MapleThief.nro` (CI and local): `platform.switch_nro` passes (NACP `MapleThief` by `armaatus`, the project version, a 256 x 256 baseline JPEG icon) and `platform.switch_guard` passes.
