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

**The checks** (`maplethief/saves.h`, next to `mt_saves_sanitise`): `mt_saves_check(name, bytes, size, why, why_size)` returns the first rule a file breaks, or `MT_SAVES_OK`, with the reason in plain words. The verdicts are `bad_name`, `bad_size`, `bad_job`, `bad_level`, `bad_item`, `bad_skill`, `bad_map`, `bad_quest`, `bad_config`, `bad_receipts` and `bad_counters` (the port's cash shop counters `maplethief-cashshop<n>`, #156), listed with their limits in [save.md](../formats/save.md#checking-a-save). The limits are facts of the 1.5 data (74 equipment ids, 138 other item ids, levels 1–100, skill levels up to 20, 11 areas with their map counts). `mt_saves_is_game_file` says which names are save files: the game's, and the cash shop's counters, which export, import and the backups carry with their slot. An import of `theif<n>` without its counters removes the old counters, which belonged to the character it replaces. The checks refuse and never repair. The sanitiser keeps clamping `config` at read time for files that are already in place. `tools/mtport/savecheck.py` has the same rules written on `savedata.py`'s records, plus 50 synthetic fixture sets. CTest `saves.check_parity` runs `MapleThief --check-saves` on each fixture, as a folder and as a deflated zip from Python's `zipfile`, and compares the verdicts. `tests/test_savecheck.py` compares the limits, the codes and their order with `saves.h` and `saves.cpp`.

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

## Addendum: the saves in the menu, with a backups list, system file dialogs and the data folder (#192, 2026-11-05)

**Context.** The settings menu's Saves tab (ADR 0076) wrote exports to `exports/` and imported the newest zip in `import/`. *Backups* only counted the backups. To restore one, players had to use `--list-backups` and copy files by hand. ADR 0076 left the file dialog out because SDL 3's dialogs are asynchronous, don't exist on iOS, and might sit behind a fullscreen window.

**Options considered.**

- **Where to restore from.** Option 1: a screen of its own, like the remap overlay. Option 2: **a list in the menu, in place of the tab's rows** (chosen). It reuses the menu's drawing, input, pointer and glyph hints, so it stays small. `SettingsMenu::open_list(MenuList)` is generic: rows (label, value), a question per row, and a choose callback.
- **The dialogs.** Option 1: native code per platform. Option 2: **SDL 3's `SDL_ShowOpenFileDialog` / `SDL_ShowSaveFileDialog`** (chosen). They are already linked, and they become xdg-desktop-portal or zenity on Linux. They answer on any thread, so the wrapper queues the answer and hands it over at `poll()` on the frontend's thread. #182's IPA import can reuse the same wrapper.
- **When a dialog can't be used.** Never on iOS (the Files app route, ADR 0074). Never under gamescope (a Steam Deck's Game Mode, `XDG_CURRENT_DESKTOP = gamescope` or `SteamDeck = 1`): a portal dialog there opens behind the game, with no mouse. Never on a fullscreen Linux window: the portal's dialog isn't the window's child. On macOS (a sheet) and Windows (an owned dialog), the dialog shows over the window, so fullscreen keeps it. When the dialog fails (SDL's callback gets no list), the action falls back to the folders. A cancelled dialog does nothing.

**Decision.**

- **`platform/src/file_dialog.{h,cpp}`** (reusable):
  - `FileDialogs::show(window, request, done)`: one dialog at a time (`busy()`). `poll()` hands the answer to `done` on the caller's thread. An answer that comes after the wrapper is gone is dropped.
  - `FileDialogs::open_folder(dir)`: `SDL_OpenURL` of `file_url(dir)`, a percent-encoded `file://` URL, with Windows drive paths kept.
  - `dialog_route(dialog_place(window))`: the dialog, or the folder, as listed under *When a dialog can't be used*.
  - The test seam is `set_backend` and `set_url_opener`.
- **`platform/src/save_menu.{h,cpp}`**, the tab's actions:
  - **Backups** opens a list of every backup, newest first across the slots and the imports. A row shows `Slot 1  Lv 23 Hermit` and the local time; an import's row shows `Replaced: slot 2, Lv 9 Beginner`.
    - The character comes from the backup's `theif<n>`: branch, job tier and level at `0x0`–`0x2`. Job names: Beginner; Rogue; Assassin, Hermit, Night Lord; Bandit, Chief Bandit, Shadower.
    - Choosing a row asks "Restore slot N from <time>? The current save is backed up first." with No focused, like #179's Quit. Yes runs `restore_backup`: checked, the replaced files to `backups/import/<time>/`.
    - Like an import, a restore runs only with no character loaded (`mt_input_panel() < 0`). Anywhere else the row says so at once, without asking.
    - Slots are numbered 1–3 here (the folders stay `0`–`2`).
  - **Export saves...** opens the save dialog at `exports/MapleThief-saves-<time>.zip`. `.zip` is added to a chosen name that lacks it.
  - **Import saves...** opens the open dialog for a `.zip` at `import/`, from the title only. The panel is checked again when the dialog answers.
  - **Open data folder** (not on iOS) opens the data directory.
- **The list's controls** (`SettingsMenu`, ADR 0051 / 0102 / 0114):
  - Up / Down move over the rows. LT held with Up / Down, Shift with the arrows, Page Up / Down and LB / RB page by 10.
  - Enter / South choose. Left / Right pick No or Yes.
  - Esc / Backspace / East cancel a question, then go back to the tab with *Backups* selected. A tab change or closing the menu closes the list too.
  - A tap or click on a row asks, and on `>` picks Yes. The hints show the glyphs of the pad in use.
- **Test aid:** `--restore-backup T,N` chooses and confirms row N (1 = the newest) of the menu's list at T seconds, through the same `backups_list`.

**Consequences.**

- A regretted save is three presses away at the title: Backups, the row, Yes. The save it replaced is itself a backup (the list's newest row), so a restore can be undone.
- Players on macOS and Windows pick where exports go and which zip to import. iOS, the Deck's Game Mode and fullscreen Linux keep the folders, and so does any desktop where SDL can't show a dialog.
- The menu stays open, and the game paused, while a dialog is up. The answer shows on the status line at the next turn of the overlay loop (within 100 ms). If the menu was closed meanwhile, the import or export still runs and its line is logged.
- **Tests (CI):**
  - `file_dialog_test.cpp`: URLs; the route table; an answer from another thread handed over only at `poll`, once; a refused backend; an answer after the wrapper is gone.
  - `save_tools_test.cpp` (`save menu: …`):
    - Job names and the character text.
    - Stamps as times.
    - The list's rows from synthetic backups, newest first, with an import's row.
    - A restore replacing the slot after backing up the current files; refused with a character loaded, with nothing written; a broken backup refused.
    - Export and Import through the seam: chosen, cancelled, failed (fallback), the folder route, a panel that changes before the answer.
    - Open data folder through the URL seam.
  - `settings_menu_test.cpp`: the list's moves, pages, question (No first), back, tab, pointer, an empty list, and its drawing by hash.
- **Real assets (local):** CTest `platform.game_restore_backup` (`platform/restore_backup_test.cmake`, after `play.save_quit`):
  1. Save; walk 548 points left and save again.
  2. Restore the first backup through `--restore-backup` in the field: refused, nothing changed.
  3. Restore it at the title: `theif0` / `quest0` are the first save's bytes, and `backups/import/` holds the second save.
  4. Reload: the game saves the hero at the first save's level, area, map and x.
- **needs-human (#192):** the dialogs on macOS and Windows (windowed and fullscreen), and *Open data folder* on macOS, Windows and Linux.

## Addendum: the data folder next to the game by default (ADR 0127, #260, 2026-11-07)

[ADR 0127](0127-data-folder-next-to-the-game.md) replaces this ADR's *Where* bullet on the desktop. The data directory is `<base>/data` **by default**, without a marker file. `<base>` is the same as here, plus the Linux game folder: the parent of `bin/` when it holds `MapleThief.sh`. The per-user folder is an option (`<base>/user-data.txt`, `--data-location user`, the Saves tab's *Data location* row) and the fallback when `<base>/data` can't be written (Program Files, a read-only volume, macOS App Translocation); `MAPLETHIEF_DATA_DIR` still wins over all of it. `portable.txt` is still accepted and no longer does anything. `Documents/` becomes `saves/` and holds `gamecenter.json` as well, so `save_dir` now moves the saves with the achievements; settings, bindings and the backups stay in the data directory as before. One resolver in `platform/src` replaces `data_dir()`, `portable_data_dir()` and the separate reads of `MAPLETHIEF_DATA_DIR`; `configure_save_location()` takes its answer. Export, import, the backups and their checks are unchanged, and the first-start copy from the old per-user folder and `--migrate-from` reuse the import's checks.

## Addendum: a folder of saves is read without following links (#413, 2026-11-13)

[ADR 0154](0154-threat-model-for-player-supplied-files.md) (b): a save export unpacked from an archive can hold symbolic links, and the import's folder source followed them, so a shared folder could make the port read any local file as a save. The folder source (`read_save_source` in `platform/src/save_tools.cpp`) now looks at each entry with `symlink_status`: a linked file, or a `Documents/` that is a link, is not read, and is listed among the ignored files as "a symbolic link: not followed" and logged. The player's own `saves/` and `backups/` are read as before (a link a player made there is their choice). **Verification** (CI): `save tools: a symbolic link in a folder of saves is not followed (#413)`, which failed before the change; the test skips itself where links cannot be made (Windows without the privilege).
