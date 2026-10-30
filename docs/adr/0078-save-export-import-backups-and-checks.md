# 78. Save export and import, rotating backups, checks before a file replaces a save, and where the saves live

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #148
- **Builds on:** [ADR 0074](0074-save-files-atomic-writes-sanitising-and-ios-documents.md) (`maplethief/saves.h`: the Documents directory, `mt_saves_replace_file`, the sanitiser), [ADR 0069](0069-guest-function-hooks.md) (guest function hooks), [ADR 0053](0053-observed-functions-game-events-and-rumble.md) (observed functions), [ADR 0070](0070-settings-registry-and-presets.md) (the settings registry), [ADR 0035](0035-foundation-subset.md) (the guest home mapped to the data directory), [ADR 0003](0003-dependencies.md) (dependencies)

## Context

The game keeps each character in two raw files, `theif<n>` and `quest<n>`, with no version and no checksum ([formats/save.md](../formats/save.md)). Since ADR 0074 a crash can no longer leave half a file. Players still had no way to:

- **Move their saves.** Moving a save meant finding `~/Library/Application Support/MapleThief/Documents` and copying files by hand.
- **Undo a bad save.** The game overwrites the slot on every save. It saves on menu actions, rewards and purchases, never on a timer. A character saved in a bad state, or a file broken outside the game, has nothing to go back to.
- **Bring in a file safely.** The loader trusts item ids, the area and map, skill levels, quest keys and two `config` bytes ([the checks](../formats/save.md#checking-a-save)). A copied, edited or damaged file can make it read past a table or load a map that does not exist. Players have no Python, so `savedata.py` cannot check a file for them.
- **Keep the saves somewhere else.** Examples are a cloud-synced folder, or a folder beside the executable on a USB stick.

#149 (importing an iPhone backup) and #150 (autosave) build on whatever this decides. The settings menu (#89) is built at the same time and needs entry points for Export, Import and the backups.

## Options considered

**When to back up:**

1. **A timer, or the frontend watching the files.** It would back up files halfway through the game's save pair, and it would need a polling thread.
2. **An observer on `SaveSaveData` `0x38711`** (ADR 0053). It runs at the entry of the pair, so it sees the save before this one. An observer may not touch the files from inside the guest's call.
3. **A hook that wraps `CDataMgr::SaveQuestData` `0x38F55`** (ADR 0069). Chosen. In the generated code, all 8 calls of `SaveQuestData` come right after a call of `SaveSaveData`. When the wrapper's call to the original returns, both files are on the disk (`SaveFile_Write` closes each handle, so `mt_saves_replace_file` has run).

**Which slot:** reading `CDataMgr+0x4E4` (the slot in use) from guest memory needs a check of the guest's committed memory. Comparing each slot's files with its newest backup needs no guest state at all. Chosen: only the slot just saved differs, and an unchanged save costs no backup.

**Export and import format:**

1. **A folder only.** It is simple, but a folder cannot be attached to a message. The Finder and Explorer zip folders anyway, so the import would have to read zips regardless.
2. **A zip through a new dependency** (miniz, libzip, zlib). This adds a library under ADR 0003 for about 25 KB of files.
3. **A small zip reader and writer of our own.** Chosen. The writer stores entries (method 0), since the saves are small. The reader takes stored and deflated entries. Deflate goes through the raw inflate of the pinned `stb_image.h` (`stbi_zlib_decode_noheader_buffer`), which the runtime already has for PNG, compiled as a private static copy. No new dependency.

**The checks:** in Python only (the players lack it), in C only (nothing checks the checks), or **in C and again in Python on the same fixtures**. Chosen: the third.

## Decision

**The checks** (`maplethief/saves.h`, next to `mt_saves_sanitise`): `mt_saves_check(name, bytes, size, why, why_size)` returns the first rule a file breaks, or `MT_SAVES_OK`, with the reason in plain words. The verdicts are `bad_name`, `bad_size`, `bad_job`, `bad_level`, `bad_item`, `bad_skill`, `bad_map`, `bad_quest`, `bad_config` and `bad_receipts`, listed with their limits in [save.md](../formats/save.md#checking-a-save). The limits are facts of the 1.5 data (74 equipment ids, 138 other item ids, levels 1–100, skill levels up to 20, 11 areas with their map counts). `mt_saves_is_game_file` says which names are the game's. The checks refuse and never repair. The sanitiser keeps clamping `config` at read time for files that are already in place. `tools/mtport/savecheck.py` has the same rules written on `savedata.py`'s records, plus 42 synthetic fixture sets. CTest `saves.check_parity` runs `MapleThief --check-saves` on each fixture, as a folder and as a deflated zip from Python's `zipfile`, and compares the verdicts. `tests/test_savecheck.py` compares the limits, the codes and their order with `saves.h` and `saves.cpp`.

**The save tools** (`platform/src/save_tools.{h,cpp}`, `save_zip.{h,cpp}`): every write goes through `mt_saves_replace_file`, and `*.mt-tmp` files are skipped.

- **Export** copies every game file in `Documents/` as it is (unchecked: it is a copy) to a zip, when the name ends in `.zip`, or to a folder.
- **Import** reads a folder, the folder's `Documents/`, or a zip (any folder inside it; `__MACOSX` and dot files are left out). It checks every file and the `theif`/`quest` pairs (`bad_pair`). On any problem it writes nothing. Otherwise it copies the files it will replace to `backups/import/<time>/` and then writes. A failed write puts back the old bytes (or removes a new file), so `Documents/` ends as it was. Files the import does not hold stay. Untrusted input is capped: 64 MiB for a zip, 16 MiB per entry, 4096 entries. Zip64, encryption and other methods are refused, and the CRC-32 is checked.
- **Backups:** after each save pair, every slot whose files differ from its newest backup is copied to `<data dir>/backups/<slot>/<time>/`. The newest `save_backups` are kept (default 10; 0 = off). Imports keep the newest `max(save_backups, 1)` of `backups/import/`. `<time>` is UTC `YYYYMMDD-HHMMSS-mmm`, plus `-N` within one millisecond. Only folders with such names are ever removed. A restore is an import of the backup's folder, so it is checked and backs up what it replaces.
- **The hook:** `CDataMgr::SaveQuestData` `0x38F55` is in `recomp/hooks.py` (`MT_HOOK_FN_SAVE_QUEST_DATA`). `install_save_backups()` registers the wrapper at game start, on or off as `save_backups` says. The wrapper never reads or writes guest state, and an exception in it never reaches the generated code.
- **Where:** the data directory is `MAPLETHIEF_DATA_DIR`. Otherwise it is `<base>/data` when `<base>/portable.txt` exists, where `<base>` is the executable's folder or, inside an app bundle, the folder around `MapleThief.app` (not on iOS). Otherwise it is SDL's per-user folder. `save_dir` (an absolute path, or empty) moves `Documents/` alone. Settings, bindings, Game Center and the backups stay in the data directory, so a synced save folder carries only the saves. `configure_save_location()` applies all of this for the game and for the command line.
- **Settings** (Saves group, `kNotInPresets`, `Applies::kNextStart`): `save_dir` (`--save-dir`, empty by default) and `save_backups` (`--save-backups`, 0–100, step 5, default 10). An empty value is written as `save_dir =`.
- **Command line:** `--export-saves ZIP|DIR`, `--import-saves ZIP|DIR`, `--check-saves ZIP|DIR` and `--list-backups` run before SDL starts. They print to stdout and exit with 0, or 3 when a check fails or nothing was done. They take `--save-dir` and `--save-backups` and no game options.
- **The settings menu (#89)** gets `export_saves(dest, &why)`, `import_saves(source, &why)`, `list_backups()` and `restore_backup(backup, &why)`, all on the configured location.

## Consequences

- A character survives a save the player regrets. The last 10 distinct saves of each slot are kept, about 60 KB per slot. An unchanged save costs no backup.
- A file that would crash the game, or load a missing map, never replaces a working save. Importing a set always leaves `Documents/` either as it was or with the whole set in place.
- The generated code gains one hooked function. With backups off, or with the hook unregistered, the pair runs exactly as before.
- **An import while the game runs:** the game reads the slots into memory at the slot screen (`CDataMgr::LoadSlots` `0x37A01`, called from `0x96845` and `0x9462D`). The menu should import from the title or main menu, where the next slot list loads the new files. A handle the game has open keeps its own bytes until it closes (ADR 0074).
- Rules that change with the data (for example, another table row) need both `saves.h` and `savecheck.py` changed. `test_savecheck.py` and `test_savecheck_real.py` fail until they agree with each other and with the bundle.
- `save_dir` on a synced folder is only as safe as the sync. Two machines that play the same slot at once can overwrite each other. The local backups do not sync, by design.
- **Tests (CI):** `objc_test`'s `saves_test.cpp` covers every rule of `mt_saves_check`. `platform_test`'s `save_tools_test.cpp` covers: export to a folder and a zip and import back byte for byte; hostile sets (truncated, oversized, ids past the tables, speed byte 3, a bad area, a missing pair) refused, with `Documents/` unchanged and no backup taken; the import's backup and its rotation; a write failing part way leaving every file as it was; backups rotating at N, skipping an unchanged save, a millisecond collision and N = 0; a restore; the hook through `MT_FN_HOOK` (the original runs once and the saved slot is backed up); folders and zips (a deflated one from Python, `__MACOSX`, a name in two folders); hostile zips; portable mode; the two settings. `saves.check_parity` and `saves.cli_round_trip` run the program on savedata.py's fixtures. `tests/test_savecheck.py` covers the Python rules and the sync with C.
- **Real assets (local):** `tests/test_savecheck_real.py` checks the limits against the shipped tables and `map/`, and every `Scripts.strings` key (English and Korean) against the key rule. `play.save_quit` installs the backups as the frontend does and checks that the real game's save, through the hooked `SaveQuestData`, left one backup of slot 0 with the bytes saved.
