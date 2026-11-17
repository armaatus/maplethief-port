# 74. Save files: atomic replacement, sanitising on read, and the iOS app's own Documents

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #78
- **Builds on:** [ADR 0035](0035-foundation-subset.md) (the guest home mapped to the data directory, NSFileManager and NSFileHandle), [ADR 0034](0034-offline-services.md) (the data directory, `gamecenter.json`), [ADR 0042](0042-frontend-game-loop-and-presentation.md) (the frontend sets the data directory from `SDL_GetPrefPath`)

## Context

The game keeps its state in raw files in `Documents/` ([formats/save.md](../formats/save.md)): `theif0`–`theif2`, `quest0`–`quest2`, `config`, `cupon`, `CTerms` and the receipt lists. There is no version and no checksum. `SaveIO_Open` `0x6F65` opens a file with `fileHandleForUpdatingAtPath:` (after `createFileAtPath:contents:nil` if it is missing), and the write `0xB729` sends `writeData:` at offset 0, `synchronizeFile` and `closeFile`. It never truncates: a shorter write would keep the old tail. ADR 0035 mapped `<home>/Documents` to `<data dir>/Documents` and wrote through a `FILE *` in place, as the device did.

Three problems remained (#78, research 2026-10-30):

- **A crash or a kill during a save can corrupt it.** Writing in place means a process stopped part way leaves half a new file over half an old one. With no checksum, the game then loads garbage. A new slot has a second window: `createFileAtPath:` makes an empty `theif<n>` before the write, and on the next start `CDataMgr::LoadSlots` `0x37A01` reads 0 bytes into a buffer that `CHero::LoadSave` reads 1,124 bytes from.
- **`config` is trusted.** `ApplyGameSpeed` `0x37C41` indexes the tick lengths `{65, 50, 40}` at `0x108C58` with byte 1 unchecked. 3 reads the next word, the popup id `0x108C64` (−1 at start), so the timer fires on every 1 ms poll; larger values read whatever follows. Byte 5 indexes the 3-entry table `{0, 1, 3}` at `0x108C4C` the same way (`0x37B85`, `CStateGame::LoadGame` `0x86C1D`). The options menu only writes valid values, but a hand-edited, copied or damaged file does not have to.
- **On iOS the saves would be invisible.** `SDL_GetPrefPath` is `Library/Application Support/MapleThief/` inside the app's container, which the Files app cannot see, so a player cannot copy a save off the device or bring one in.

#148 (export, import, backups, a validator) and #149 (importing a real iPod save) build on whatever this decides, so the file I/O should sit behind one small API.

## Options considered

**Atomic writes:**

1. **Write in place, as now.** Faithful, and a kill corrupts the save.
2. **Write in place, keep a backup copy** (`theif0.bak`) and fall back to it when the file looks wrong. Without a checksum, "looks wrong" is only the size; a half-written file of the right size passes. It also doubles the files that #148 has to handle.
3. **Keep the handle's bytes in memory and replace the file in one step** (temporary file in the same directory, flushed to the disk, renamed over the target). The visible file is always a whole old or a whole new version. The game's semantics stay: the copy starts as the file's bytes, `writeData:` overwrites at the offset and extends, and the old tail stays.
4. **Journal the writes** (a log of `writeData:` calls replayed at start). More code than the problem needs, for files of at most 5,962 bytes.

**The config clamp:**

1. **Patch the game's code** at `0x37C41`. The recompiled code is generated ([ADR 0021](0021-generated-code-partition-and-build.md)), so a patch means a guest function hook on each reader (#143), and byte 5 has two more readers. Each hook replaces a whole function to fix one table index.
2. **Sanitise the bytes as the game reads the file.** One host-side function, applied where the file layer hands the bytes to the guest. Valid files pass unchanged, and the file on the disk is left alone.
3. **Rewrite the file at start.** It changes the player's file without the player doing anything.

**iOS Documents:**

1. **Data directory = the app's container root**, so `<home>/Documents` is the real `Documents/`. The other files (`gamecenter.json`, `settings.ini`, `bindings.ini`) would then sit in the container root, where iOS allows no writes.
2. **A separate Documents directory**, settable, by default `<data dir>/Documents`. The iOS frontend sets it to the app's own `Documents/`, and `UIFileSharingEnabled` with `LSSupportsOpeningDocumentsInPlace` shows it in the Files app and the Finder.

## Decision

**Atomic writes: option 3. The config clamp: option 2. iOS: option 2.** All three are in one module, `maplethief/saves.h` (`runtime/src/foundation/saves.cpp`):

- `mt_saves_documents_dir()` / `mt_saves_set_documents_dir()`: where the guest's `<home>/Documents` maps (`host_path` in `files.cpp`). By default it is `<mt_stubs_data_dir()>/Documents`, so macOS, Windows and Linux are unchanged (`~/Library/Application Support/MapleThief/Documents`, `%APPDATA%\MapleThief\Documents`, `~/.local/share/MapleThief/Documents`). On iOS, without `MAPLETHIEF_DATA_DIR`, the frontend sets `SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)` (`documents_dir_override` in `platform/src/game.cpp`).
- `mt_saves_replace_file(path, bytes, n)`: the bytes go to `<path>.mt-tmp`, which is flushed to the disk and then renamed over `path`. The flush is `F_FULLFSYNC` on Apple (falling back to `fsync`), `fsync` on Linux and `_commit` on Windows. The rename is `rename(2)` followed by an `fsync` of the directory, or `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows. The parent directories are created. A `.mt-tmp` left by a killed process is never read and is overwritten by the next save of that file. GameKit's `gamecenter.json` uses the same call: it used to remove the old file before the rename on Windows.
- **NSFileHandle** keeps an `OpenFile` (its bytes, the offset, a dirty flag) instead of a `FILE *`. `writeData:` and `truncateFileAtOffset:` change the copy. `synchronizeFile`, `closeFile` and the handle's dealloc save a dirty copy with `mt_saves_replace_file`. A failed save is logged ("cannot save …: it keeps its last saved bytes") and never raises: the original ignored `synchronizeFile`'s result, and an uncaught exception would end the game.
- **`createFileAtPath:contents:nil` of a missing file** is remembered (per runtime) instead of written. The file exists for `fileExistsAtPath:` and the handles, and it appears on the disk at the first save of a handle on it, even an empty one at `closeFile`. A kill between the create and the write therefore leaves no file and so no corrupt slot. With contents, or over an existing file, it is replaced at once. The only caller is `SaveIO_Open` `0x6F65`, which opens the file for updating right after (`0x7048` → `0x7068`).
- `mt_saves_sanitise(name, bytes, n)` runs on the bytes a handle reads from a file directly in Documents. For `config`, bytes 1 and 5 above 2 become 2: the nearest valid value, the fastest speed. That changes behaviour only where the original read out of bounds, and it is logged ("config holds values past the game's tables"). The file is not rewritten; the options menu writes the valid values back when the player next changes an option. #155 lists the original bug.
- **iOS bundle:** `platform/ios/Info.plist.in`, which the bundle uses when `CMAKE_SYSTEM_NAME` is iOS, has `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace` set. #83 builds and signs the app.

The bytes on the disk are the same as before for every file the game writes in full (all of them). A device's `Documents/` can still be copied in as is.

## Consequences

- The game's saves survive a crash, a kill or a power loss during a save: the slot holds the last complete save. Losing the save in progress is the only loss.
- A handle's changes are visible to other handles only after `synchronizeFile` or `closeFile`. On the device they were visible after `writeData:`. The game never reads a file while a write handle on it is open, so nothing it does can tell.
- A save costs a flush to the disk (`F_FULLFSYNC` takes a few milliseconds on an SSD). The game saves on a menu action, a reward or a purchase, never every frame.
- Save files carry a `*.mt-tmp` sibling only after a crash. #148's export, import and backups use `mt_saves_replace_file` and skip `MT_SAVES_TMP_SUFFIX`; the validator can add rules to `mt_saves_sanitise`.
- A config with speed 3 now runs at speed 2 instead of a tick every millisecond. Anyone who relied on that bug as a fast-forward loses it; it was not reachable from the options menu.
- `saves.cpp` is plain C++ like the stubs, so the recompiler's C test harness links it with them (`tests/recomp_harness.py`).
- **Tests.** In CI: `tests/cpp/saves_test.cpp` covers the replacement, a fault at each stage through the `mt_saves_set_fault` seam (temporary file opened, half written, written but not renamed: the old file is whole and the next save cleans up), and, on POSIX, a child process killed with `SIGKILL` at random moments while it saves two 1 MiB versions in turn (40 runs; the file is always exactly one of them; 37 of 40 kills landed mid-save on an M-series Mac). The same file tests the sanitiser and the Documents directory. `tests/cpp/foundation_test.cpp` covers the game's own write sequence killed at each stage then read by a new runtime, a new slot killed before its first save (no file afterwards), the deferred create, the config clamp through the game's read sequence, and a Documents directory apart from the data directory. `platform_test` covers the iOS override, and `tests/test_ios_info_plist.py` the iOS template. Locally, with real assets: `play.save_quit` and `play.save_relaunch` are two processes on one data directory. The first plays the tutorial, drinks two potions, saves, records the hero and the files, and ends. The second checks that the files are byte for byte as saved, loads slot 0, compares 119 facts of the hero's state, saves again and compares the bytes (the heap bytes the game never writes aside). `play.config_speed` boots the real game on a config with speed byte 3 and measures 25 ticks a second (40 ms); without the clamp it measured 608.

## Addendum: `saves/` next to the game (ADR 0127, #260, 2026-11-07)

On macOS, Windows and Linux, [ADR 0127](0127-data-folder-next-to-the-game.md) moves the data directory to `<base>/data/` beside the game and renames the folder the guest's `<home>/Documents` maps to from `<data dir>/Documents` to `<data dir>/saves` (an old `Documents/` is renamed in place at start). `gamecenter.json` moves into that folder too, so `save_dir` carries it with the saves; the frontend sets its path. The mechanism of this ADR is unchanged: `mt_saves_documents_dir` / `mt_saves_set_documents_dir`, `mt_saves_replace_file`, the remembered `createFileAtPath:` and the sanitiser. **iOS is unchanged:** the app's own `Documents/`, shared with the Files app, and `gamecenter.json` in the data directory.

## Addendum: the two-step replacement on the Nintendo Switch (#236, 2026-11-11)

On the Switch the saves live on the SD card (`sdmc:/switch/maplethief/data/saves/`, ADR 0138), which libnx reaches through its fsdev devoptab. There, `rename` goes to the file system's own rename (`fsFsRenameFile`), which refuses an existing target (the path already exists), and no directory opens as a file, so neither the rename-over nor the directory `fsync` of this ADR works. This comes from reading libnx's fsdev; the hardware run of `MapleThiefTests.nro` confirms it, and the two-step path is right whether or not the rename would have replaced.

**Decision.** `mt_saves_replace_file` takes two steps on Horizon: the flushed `<path>.mt-tmp` is renamed to `<path>.mt-ready` (`MT_SAVES_READY_SUFFIX`, a new name, so the rename works), the old `path` is removed, and the ready file is renamed to `path`. After each rename the device is committed (`fsdevCommitDevice`, which makes a save-data mount durable; on the SD card it has nothing to commit, and its result is ignored), in place of the directory `fsync`. A ready file only ever holds a whole, flushed new version, so a process stopped between the steps leaves one that the recovery puts in place: `mt_saves_recover_file(path)` (the ready file replaces `path`) runs first in every replacement, and `mt_saves_recover_dir(dir)` runs in `mt_saves_set_documents_dir`, which the frontend calls at start before the game reads a save. After the recovery, `path` is the old or the new file, whole. A `.mt-tmp` is never promoted: it may be half written. Files outside the saves folder (`settings.ini`, the states) are recovered at their next replacement; until then their reader sees the old version or, in the window between the removal and the last rename, none.

**Desktop is unchanged:** one rename over the file, then the directory `fsync` (Windows: `MoveFileExW` with write-through). The two-step path is a mode (`mt_saves_set_replace_mode`, AUTO by default) so that the desktop tests run it too, with two more fault points: `MT_SAVES_FAULT_READY` (renamed to the ready name, the old file still there) and `MT_SAVES_FAULT_REMOVED` (the old file gone, the ready one not renamed yet).

**Verification.** On every desktop (CI), `tests/cpp/saves_test.cpp`: the two-step replacement of a new and an existing file; a replacement stopped at each of the five fault points, over an old file and as a new file, followed by "the next start" (`mt_saves_set_documents_dir`), after which the save is exactly the old bytes, the new bytes, or (for a new file stopped before its ready name) absent, never torn, and the next save goes through; and the directory recovery (ready files only, not subdirectories, never a `.mt-tmp`). On a console (`needs-human`): the same cases on the SD card in `MapleThiefTests.nro`, plus `horizon_test.cpp`'s interrupted save in the platform's own mode.

## Addendum: the guest path rules on every system, and the save import's totals (#324, 2026-11-13)

[ADR 0154](0154-threat-model-for-player-supplied-files.md) promises that nothing a file contains reaches a host path outside the data and game folders. A read of the code found three ways the file layer and the save import fell short of that, all closed here:

- **A component is refused, not only `.` and `..`.** `host_path` (`runtime/src/foundation/files.cpp`) split a guest path on `/` only, so a component holding `\` passed and became a separator on Windows, and `C:` or `x:y` named a drive or a stream there. A component that holds `\`, `:` or NUL is now refused on every system, so a test on macOS or Linux proves what Windows does. No file of the game's bundle and no path the game makes holds one (checked against the shipped bundle). The same rule, with no empty component allowed, is `mt_ns::is_safe_relative_path`, which a save state's restored paths go through (ADR 0105's addendum of #324).
- **The home and Documents folders cannot be removed.** `<home>` maps to the data folder and `<home>/Documents` to the saves folder, so `-removeItemAtPath:` of either would have emptied it. Both are refused (logged, `NO`, an error set); a file in them can still be removed, as the game does. The game never removes either.
- **A save zip's names come first, then its total.** The import (ADR 0078) inflated every entry before it looked at the names, with no cap on the whole. `zip_read` now checks every name, then inflates only the files the caller keeps (the save import keeps the game's file names, and lists the others as ignored without inflating them), and refuses more than `kMaxSaveSetBytes` (64 MiB) in the kept files together, from their stated sizes, before any is inflated. Each entry still inflates into exactly its stated size, so the stated total bounds what is allocated.

**Verification** (CI): `Foundation: a guest path with a backslash, a drive letter or NUL is refused, and the home and Documents folders cannot be removed (#324)` (the data folder's files are still there after the refused removals); `save tools: a zip's names are filtered before anything is inflated, and a set is capped in total (#324)` (an entry with a broken CRC that is no save no longer refuses the zip; five 16 MiB saves, half a megabyte deflated, are refused and nothing is read). Both failed before the fix.

## Addendum: the full check when the game reads a save (#413, 2026-11-13)

[ADR 0154](0154-threat-model-for-player-supplied-files.md) (a) decided that a save the game reads gets ADR 0078's checks, not only `config`'s clamp: a `theif` or `quest` copied into `saves/` by hand, by a sync or through the iOS Files app never passed the import. The file layer (`runtime/src/foundation/files.cpp`) now runs `mt_saves_check` when the guest finds `theif<N>` or `quest<N>` directly in `Documents/`, through `fileExistsAtPath:` or an `NSFileHandle`, before the guest sees a byte. A file over 64 KiB is refused unread. One that fails is moved to `<data dir>/backups/refused/<time>/` with `reason.txt` (`<name>: <verdict>: <why>`) and logged, and the guest sees no file: the slot reads as empty, and the game's next save there writes a new character without touching the moved file. The frontend hears of each refusal through `mt_saves_set_refused_hook` and shows it once as a data notice, during play as well as at the title. A file the game wrote itself always passes, so nothing changes for a player who never copies saves; `config` keeps its clamp and is never moved; the other files (`cupon`, the receipts) are not checked at read time, as before.

**Verification** (CI): `Foundation: a theif or quest copied in that fails the checks is moved aside, and the game sees no file (#413)` (a valid copied character and the game's own write pass; a level-0 character, an 11-byte quest and a 1 MiB file are each moved with their reason; the data folder is as it was apart from the moved file; it failed before the change); `data notice: a save refused as the game read it is shown once, with where it went (#413)`.
