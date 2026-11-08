# 127. The data folder next to the game by default, the per-user folder as an option

- **Status:** Accepted
- **Date:** 2026-11-07
- **Issue:** #260 (epic #259)
- **Revises:** [ADR 0078](0078-save-export-import-backups-and-checks.md) (*Where*: portable mode becomes the default; `portable.txt` no longer does anything), [ADR 0074](0074-save-files-atomic-writes-sanitising-and-ios-documents.md) (on the desktop, the guest's `Documents/` maps to `saves/`, and `gamecenter.json` sits there too), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (the IPA import writes `<base>/game/`; the bundle search uses `<base>`)
- **Builds on:** [ADR 0076](0076-settings-menu.md) (the settings menu's Saves tab), [ADR 0100](0100-public-repository-and-player-releases.md) (the IPA is read-only input; the bundle is imported), [ADR 0065](0065-linux-and-steam-deck-build.md) (the Linux game folder and `MapleThief.sh`)

## Context

Everything the port writes for a player goes to one data directory. Today it is chosen in `data_dir()` (`platform/src/game.cpp`):

1. `MAPLETHIEF_DATA_DIR` if it is set and not empty.
2. `<base>/data` if `<base>/portable.txt` exists (ADR 0078). `portable_base_dir()` (`platform/src/save_tools.cpp`) turns `SDL_GetBasePath()` into `<base>`: the executable's folder, or the folder around `MapleThief.app` inside an app bundle.
3. Otherwise SDL's per-user folder `SDL_GetPrefPath("", "MapleThief")`: `~/Library/Application Support/MapleThief`, `%APPDATA%\MapleThief`, `~/.local/share/MapleThief`.

`configure_save_location()` hands the result to the runtime (`mt_stubs_set_data_dir`, `mt_saves_set_documents_dir`), and `save_dir` moves `Documents/` alone. The runtime also reads `MAPLETHIEF_DATA_DIR` itself (`mt_stubs_data_dir`, `runtime/include/maplethief/stubs.h`), and so does `save_tools.cpp`. Under the data directory are `Documents/` (the game's files, ADR 0074), `settings.ini`, `bindings.ini`, `window.ini`, `gamecenter.json`, `backups/`, `exports/`, `import/`, `states/`, `screenshots/` and `game/` (the imported bundle, ADR 0125). #198 adds `logs/` and `crashes/`.

The player kits (ADR 0125) are a folder `MapleThief-<version>-<os>-<arch>/` holding `MapleThief.exe`, `MapleThief.app`, or `MapleThief.sh` with `bin/MapleThief` on Linux. They don't ship `portable.txt`, so every player's saves go to a hidden per-user folder that they have to look up to back up, reset or move. The epic (#259) wants the opposite: one `data` folder next to the game that a player copies to back up, deletes to reset, and carries to another machine. The per-user folder stays available as a choice.

Three things can stop the game from writing next to itself:

- **A protected folder.** `C:\Program Files` is writable only by administrators. UAC's file virtualisation applies only to 32-bit applications (Microsoft, [*UAC architecture*](https://learn.microsoft.com/windows/security/application-security/application-control/user-account-control/architecture): non-elevated 64-bit applications get *access denied*), so our x64 build's write there fails rather than silently landing in `VirtualStore`. On Linux the same holds for `/opt` or `/usr`.
- **A read-only volume:** a locked SD card, a network share, a mounted image.
- **macOS App Translocation** (Gatekeeper path randomisation, macOS 10.12 and later). A quarantined app opened from where it was unpacked runs from a random read-only mount. See [What was checked about App Translocation](#what-was-checked-about-app-translocation).

## Options considered

### Default location

1. **Keep the per-user folder as the default, `portable.txt` to opt in** (ADR 0078 as it is). Rejected: this is what the epic replaces. The folder is hidden (`~/Library` is hidden in the Finder, `%APPDATA%` in Explorer), and a game copied to another machine or a USB stick loses its saves.
2. **`<base>/data` by default, no marker file.** **Chosen.**
3. **Ask on first start.** Rejected: one more screen before the first-launch picker and the import screen, and a question most players can't answer yet. The menu offers the choice later (below).

`<base>` is `portable_base_dir()` with one more rule for the Linux game folder: when the executable's folder is named `bin` and its parent holds `MapleThief.sh`, `<base>` is that parent. So `<base>` is:

- Windows: the folder of `MapleThief.exe`.
- macOS: the folder around `MapleThief.app`. Never inside the bundle, because writing there would break its seal.
- Linux: the game folder, the one that holds `MapleThief.sh` and `bin/`.

**Dev builds** (run from `build/<preset>/…`): **the same rule.** Their data lands in `<build dir>/data`, beside the executable or the `.app`. Tests already set `MAPLETHIEF_DATA_DIR`, so no test changes behaviour. The alternative, keeping the per-user folder for builds with `MT_DEV_BUNDLE_DIR`, was rejected: it would put back the split this ADR removes, and the paths players use would never run on the developer's machine. A developer who wants data that survives `rm -rf build/` sets `MAPLETHIEF_DATA_DIR` or passes `--data-location user`.

**`portable.txt`**: still accepted and has no effect. Once per start it is logged as "portable.txt is no longer needed: the data folder is next to the game by default". Nothing removes it. Treating it as an error was rejected, because old kits and players' folders hold it.

**macOS, an app in Applications.** A player who drags `MapleThief.app` alone into `/Applications` or `~/Applications` would get `/Applications/data` and `/Applications/game`. An admin account can write there, so the probe passes. When `<base>` is one of those two folders, the default is the per-user folder instead. This is a normal state, not a fallback. The reason is `applications-folder`, the Saves tab says so, and `--data-location game` still overrides it. Applying the rule to every system folder was rejected: on Windows and Linux the probe already catches those.

### The per-user option

How the choice is stored. It must be readable before the data folder is known:

1. **A setting in `settings.ini`.** Rejected: `settings.ini` lives in the data folder, so the choice would have to be known before it can be read.
2. **A file in the per-user folder that points to the game folder.** Rejected: it couples the two folders and breaks when the game is moved.
3. **A marker file next to the game, `<base>/user-data.txt`.** **Chosen**, with a one-run command-line override. Only its presence counts. The menu writes it with one line of text saying what it does and how to undo it. On macOS it sits beside `MapleThief.app`, never inside it.

**The command line:**

- `--data-location game|user`: this run only. It beats the marker, writes nothing and copies nothing.
- `--switch-data-location game|user`: does exactly what the menu's switch does (below), prints what it did and exits: 0 when done, 1 when refused, with the reason.

**The menu's switch** (Saves tab, the *Data location* row: *Game folder* or *User folder*, #263):

- **When it can run.** Only with no character loaded (`mt_input_panel() < 0`), like an import (ADR 0078's addendum). Then the autosave on quit (ADR 0089) can't write the old folder after the copy.
- **The question.** "Copy your data to `<dest>` and restart?" with No focused.
- **The copy.** The frontend first writes out its own files (`settings.ini`, `bindings.ini`, `window.ini`). Then the player data (the [layout](#layout)'s player part) goes into the destination. The bundle goes to the destination's bundle folder when that has none. The source is never changed or deleted. The copy is staged in `<dest>.tmp-<pid>/` and renamed into place, as the IPA import stages `game/`. Then the marker is written (to *User folder*) or removed (to *Game folder*).
- **The restart.** The game says "Done. MapleThief will close; start it again to use <dest>." and quits. Restarting itself was rejected: it means a different exec per platform (`open -n` for the bundle, Steam's launcher, the gamescope wrapper), and the player is right there anyway.
- **A destination that already holds data.** If the destination already has `saves/` with files, or `settings.ini`, nothing is copied and nothing is overwritten. The question becomes "`<dest>` already has data (saved `<date>`). Switch and use that data? Your data here stays here." Yes switches: marker only, then restart. Overwriting is left to `--migrate-from DIR --force`, a deliberate command.
- **When the game folder can't be used** (any [fallback](#fallback), or `MAPLETHIEF_DATA_DIR`), switching to *Game folder* is refused, and the row says why: "Set by MAPLETHIEF_DATA_DIR" or the fallback's text.

**How the player sees which folder is in use:**

- The Saves tab shows the folder (shortened to fit, in full in the log) and the reason (#263).
- The start log has one line: `data: <dir> (<reason>)`.
- `--print-data-dir` prints the resolver's answer without starting SDL's video, then exits 0:

```text
data: /Users/x/Games/MapleThief-1.0-macos-universal/data
location: game
reason: default
saves: /Users/x/Games/MapleThief-1.0-macos-universal/data/saves
game: /Users/x/Games/MapleThief-1.0-macos-universal/game
```

The value of `location`, and the `reason` it can come with:

| `location` | What it means | `reason` values |
| --- | --- | --- |
| `game` | `<base>/data` | `default`, `flag` |
| `user` | the per-user folder | `flag`, `marker`, `applications-folder`, `fallback-translocated`, `fallback-not-writable`, `fallback-no-base` |
| `env` | `MAPLETHIEF_DATA_DIR` | `env` |
| `ios` | ADR 0074, unchanged | `platform` |
| `switch` | the SD card | `platform` |
| `none` | no data folder could be found | `none` |

A fallback line adds `why: <the probe's error text>`.

### The resolver

**One resolver** in `platform/src` (#261) decides the data folder for everyone. It returns:

- the folder;
- the location and the reason above;
- the fallback's `why`;
- `<base>`;
- the paths of every subfolder, and of the bundle folder.

Every writer takes its path from it: `settings.ini`, `bindings.ini`, `window.ini`, the saves, `gamecenter.json`, the backups, `states/`, `screenshots/`, `exports/`, `import/`, the IPA import, the bundle search, and #198's logs. It is the only code that reads `MAPLETHIEF_DATA_DIR` or calls `SDL_GetPrefPath`, and a lint or test enforces that.

The runtime's own default in `mt_stubs_data_dir` is still there for builds without the frontend (the recompiler harness). The frontend always sets the folder explicitly. `--data-location` beats the marker, the marker beats the `applications-folder` rule, and that rule beats the default. Since `MAPLETHIEF_DATA_DIR` beats them all, the first-start copy, the marker and the probe all stay out of tests.

Rejected: keeping `data_dir()`, `configure_save_location()` and the runtime each deciding part of it, as now. Three readers of one environment variable are how the per-user default and the portable mode drifted apart.

### Fallback

**The probe**, which runs on every start when the location would be `game`:

1. Create `<base>/data` (with its parents).
2. Create `<base>/data/.mt-probe-<pid>` exclusively, write one byte, flush, close.
3. Remove it.

Any failure means a fallback to the per-user folder, with the error text as `why`. It costs a few file operations per start.

Rejected:

- **Checking permissions** (`access(W_OK)`, ACL queries). They lie on network shares and under Windows ACL inheritance; a real write doesn't.
- **Probing once and remembering the answer.** A folder becomes writable when the player moves it, and the next start must notice.

**The fallback reasons**, checked in this order:

- `fallback-no-base`: `SDL_GetBasePath()` gave nothing.
- `fallback-translocated` (macOS): `SDL_GetBasePath()` contains `/AppTranslocation/`. This is decided by the path, before the probe; the probe would fail anyway, since the mount is read-only.
- `fallback-not-writable`: the probe failed.

An existing `<base>/data` that can't be written is not read either. The game never runs with half its data read-only.

**What the player is told:**

- **The log, once:** `data: <base>/data can't be written (<why>); using the user folder <dir>`.
- **The Saves tab:** the *Data location* row shows the folder, the reason in words, and how to fix it (#263).
- **A title notice the first time a fallback reason is seen.** The per-user folder's `settings.ini` keeps `data_fallback_seen = <reason>`, so the notice doesn't nag on every start.

The words, which #263 may polish:

- **Not writable:** "MapleThief can't write next to itself in `<base>` (`<why>`), so your data is in `<dir>`. To keep it next to the game, move the game's folder somewhere you can write, such as Documents or a Games folder."
- **Translocated:** "macOS is running MapleThief from a temporary read-only copy (App Translocation), so your data is in `<dir>` for now. To keep it next to the game, see the README's *macOS: first start*."

**Getting back:** when the cause is fixed, the next start uses `<base>/data`. That folder is new and empty, so the [first-start copy](#migration) brings the fallback's data across. Nothing is lost, and the per-user folder stays as it was.

**The macOS kit's README** (#263) gives two fixes:

- **The dependable one:** remove the quarantine from the unpacked folder with `xattr -dr com.apple.quarantine <folder>` in Terminal. That also ends Gatekeeper's prompt for this unsigned build.
- **Without Terminal:** move `MapleThief.app` in the Finder, out of its folder and back. Apple's note ([App Translocation Notes](https://developer.apple.com/forums/thread/724969)) says moving the app clears the state that caused translocation. Joris checks this on a real kit in #263's `needs-human` step, because a Terminal `mv` does not count (see below).

Detecting translocation with `SecTranslocateIsTranslocatedURL` was rejected: it is private SPI. Writing the data next to the original app through `SecTranslocateCreateOriginalPathForURL` was rejected too: also private SPI, and Apple says "there is no supported way" to find the original path. Both worked in the local test, but neither is something to ship. The path check is the same kind of implementation detail. It only chooses the text of the message, and the probe makes the fallback happen anyway, so if Apple changes the path, the player sees *not writable* instead.

#### What was checked about App Translocation

**The local test** ran on macOS 26.6.2 (build 25G83), Apple silicon, on 2026-11-07, with a throwaway `Probe.app`:

- **The app.** One C file, ad hoc signed like the macOS kit (ADR 0024: no Developer ID, no notarisation). It logs its executable path, `realpath`, the `statfs` of that path, the result of creating `data/` and a probe file in the folder around the `.app`, and the answers of `SecTranslocateIsTranslocatedURL` and `SecTranslocateCreateOriginalPathForURL` (looked up with `dlsym`).
- **The unpacking.** `Probe-kit/Probe.app` was zipped with `ditto -c -k --keepParent`. The zip got a browser-style `com.apple.quarantine` (`0081;<time>;Safari;`) and was unpacked with `ditto -x -k`. That spread the quarantine to the folder and the app as `0281;00000000;;`.
- **Not quarantined** (the built copy, run directly):
  - the path seen is where the app sits, on the APFS data volume, writable;
  - the probe succeeds;
  - `translocated=0`.
- **Quarantined, opened with `open`:**
  - LaunchServices runs it from `/private/var/folders/<..>/T/AppTranslocation/<UUID>/d/Probe.app/Contents/MacOS/Probe`, a new UUID on every launch.
  - `mount` shows a `nullfs, read-only, nobrowse` mount of that one `Probe.app` on `…/AppTranslocation/<UUID>`. Nothing else from the unpacked folder is visible: no README, no `data/`, no `game/`.
  - The folder around the `.app` is the mount's `d/`. Both `mkdir data` and creating the probe file fail with `EROFS` ("Read-only file system").
  - `SecTranslocateIsTranslocatedURL` says `translocated=1`, and `SecTranslocateCreateOriginalPathForURL` gives the path of the unpacked app.
  - On the first launch Gatekeeper held the process before `main` (`spctl --assess` rejects an ad hoc app), and the path was read from outside, from `ps` and the mount table. After that the app ran and wrote these values itself.
- **After `mv` (Terminal) of the whole unpacked folder elsewhere:** the app was translocated again, from a new mount of the moved app, with the same `EROFS`. So moving the folder with a shell tool does not end translocation.
- **Not done:** a move by the Finder. Driving the Finder through AppleScript needs the Automation permission, which this session doesn't have. It is #263's hand-off.
- **Clean-up:** the test app, the zip and the folders were deleted afterwards, and nothing of it is in the repository.

**The sources**, for macOS 13 to 15 (the test machine is newer):

- Apple Developer Forums, Quinn "The Eskimo!", [*App Translocation Notes*](https://developer.apple.com/forums/thread/724969), tested on macOS 13.0:
  - translocation runs a newly downloaded app from a randomised path;
  - "the exact circumstances … [are] not documented and [have] changed over time";
  - moving the app (to the Desktop, in the Finder) "has cleared the state that triggered app translocation";
  - "there is no supported way to detect if your app is being run translocated", nor to find the original path;
  - an app should work either way.
- Howard Oakley, [*What causes App Translocation?*](https://eclecticlight.co/2023/05/09/what-causes-app-translocation) (2023):
  - the rules (Jeff Johnson, 2016) are a quarantine attribute, a launch through LaunchServices rather than a shell, and an app not moved from where it was unpacked;
  - since macOS 13.3.1 translocation also happens after a move into `/Applications`;
  - only removing the quarantine (and provenance) attributes reliably stops it.
- [*Lost in Translocation*](https://eclecticlight.co/2024/05/14/lost-in-translocation/) and [*Check if an app is stuck in translocation*](https://eclecticlight.co/2025/10/16/check-if-an-app-is-stuck-in-translocation/), the same author, for macOS 14 and 15.
- Apple, [*Updates to runtime protection in macOS Sequoia*](https://developer.apple.com/news/?id=saqachfa) (2024): from macOS 15 the Control-click *Open* no longer gets past Gatekeeper for an app that isn't notarised. The player approves it in System Settings > Privacy & Security instead. That approval does not stop translocation: in the local test, an approved quarantine flag still translocated.

**Conclusion:**

- A player who unzips the macOS kit and double-clicks the app runs translocated on every macOS this port targets (13 and later).
- In that state the game can neither read nor write anything beside the app.
- The fallback and the message are needed, and so is the README's fix.

### Layout

The player's data is kept apart from what can be rebuilt:

```text
<base>/                       the game folder (the kit's folder)
  MapleThief.exe | MapleThief.app | MapleThief.sh + bin/MapleThief
  README.md, LICENSE, ...
  user-data.txt               only when the player chose the user folder
  game/                       the imported bundle (regenerable from the IPA; not backed up)
  game/MapleThief.app/        or a bundle placed by hand (the Linux personal folder, ADR 0065)
  data/                       everything of the player's:
    settings.ini  bindings.ini  window.ini
    saves/                    the game's files (was Documents/) and gamecenter.json
    backups/                  ADR 0078's rotating backups and backups/import/
    states/                   save states (ADR 0105)
    screenshots/
    exports/  import/         the Saves tab's folders (ADR 0078)
    logs/  crashes/           #198
```

**`saves/`** is the guest's `<home>/Documents` (`mt_saves_documents_dir`). It holds the game's files and `gamecenter.json`, so one folder holds all of the progress. The frontend sets the Game Center path explicitly to `<saves>/gamecenter.json` (#261; the runtime gets a setter beside `mt_stubs_set_data_dir`).

The game never lists `Documents/`: `-enumeratorAtPath:` has no live caller ([re/imports.md](../re/imports.md)). It only opens its own file names, so an extra file there is invisible to it. Export, import and the backups still carry only `mt_saves_is_game_file` names, so `gamecenter.json` isn't part of a save set. It is backed up by copying `data/`.

**`save_dir`** still moves this one folder, and now that means the saves and `gamecenter.json` together. A synced `save_dir` therefore carries the achievements too. Settings, bindings and backups stay in `data/`.

**`game/`** sits in `<base>`, outside `data/`. It is large, regenerable from the player's IPA, and not worth backing up. The bundle folder depends on the location:

| Location | Bundle folder |
| --- | --- |
| `game` | `<base>/game/` |
| `user` | `<per-user folder>/game/`, as today |
| `env` | `<MAPLETHIEF_DATA_DIR>/game/`, so tests are unchanged |
| `switch` | `sdmc:/switch/maplethief/game/` |

The IPA import writes the bundle folder, staged as now (`game.tmp-<pid>`, `game.old-<pid>`).

**The bundle search** looks in this order:

1. `--bundle`
2. `MAPLETHIEF_BUNDLE_DIR`
3. the build's default
4. `<base>/game/MapleThief.app`, a hand-placed bundle. This used to be `<executable dir>/…`; `<base>` is the same folder on Windows, and on Linux it is the game folder, which the launcher points at anyway.
5. the location's bundle folder
6. the import screen

Rejected:

- **The bundle inside `data/`**, as today. It makes a backup of `data/` 17 MB bigger for nothing, and a reset by deleting `data/` would mean a re-import.
- **The import writing `<base>/game/MapleThief.app/`.** On macOS the Finder would show a second, broken "MapleThief" app in the folder.

**Ship an empty `data/` in the kits:** no. The game creates it, and an empty folder in a zip is easy to lose. The README describes it.

### Migration

**(a) An old-layout folder, migrated in place at start.** This runs after the resolver and before anything opens a file (before `load_settings`). Each step checks its own precondition, so a second start does nothing, and a start after an interrupted one finishes the job.

1. `Documents/` exists and `saves/` doesn't: rename `Documents/` to `saves/` (one rename in one folder, atomic). If both exist, `saves/` is used and `Documents/` is left alone, with a warning in the log.
2. `<data>/gamecenter.json` exists and `<saves>/gamecenter.json` doesn't: move it into `saves/`. That is a rename, or a copy through `mt_saves_replace_file` followed by removing the source when the rename crosses volumes (a `save_dir` elsewhere).
3. Location `game` only: `<base>/data/game/` exists and `<base>/game/` doesn't: rename it to `<base>/game/`. Under `user` and `env`, `game/` already sits where the bundle folder is.

This applies to whichever folder is in use. That includes the per-user folder when the player chose it, and a `MAPLETHIEF_DATA_DIR`. A build from before this ADR, started later on that folder, sees no saves. This is the one downgrade cost, and it is listed in the release notes.

Rejected: keeping the name `Documents/`. It is the iOS name of a folder the player now opens by hand, and *saves* says what is in it.

**(b) The first-start copy from the old per-user folder (#262).**

- **When.** On a start where all of these hold:
  - the location is `game`, by default or by the flag;
  - `<base>/data` has neither `saves/` with files nor `settings.ini`;
  - the per-user folder has either.

  It doesn't run under `MAPLETHIEF_DATA_DIR`. That keeps it out of tests, and a test's empty `HOME` has nothing to copy anyway.
- **Just copy, don't ask.** **Chosen.** The copy is harmless: the source is never changed, and the destination is empty. A question before the window exists would need its own UI path for the keyboard, the pad and touch. A player who doesn't want the old saves deletes `data/saves/`. Asking first was rejected for those reasons.
- **What is copied.** The source may be in the old layout (`Documents/`, `gamecenter.json` at the top) or the new one. It goes into the new layout:
  - the save set, through the import's checks (`read_save_dir`, `mt_saves_check`, the pairs);
  - `gamecenter.json`, `settings.ini`, `bindings.ini` and `window.ini`;
  - `backups/`, `states/`, `screenshots/`, `exports/` and `import/`, byte for byte.

  `*.mt-tmp` files and names the port doesn't know are skipped.
- **The staging.** Everything goes to `<base>/data.tmp-<pid>/`, then `data/` (empty but for the probe) is replaced by a rename. Leftover `data.tmp-*` folders from an interrupted copy are removed at the start of the next one.
- **The bundle.** It is copied too, into `<base>/game/`, when that has none and the old folder has an imported bundle (`has_imported_game`). Its executable's SHA-256 is checked as the import does, and it is staged the same way. Then a game folder is complete on its own, with no re-import. Reading the old bundle in place was rejected: the game folder would quietly depend on the per-user folder and break on another machine.
- **A save set that fails the checks.** Nothing at all is copied, so a half-migrated folder is never made. The notice gives the reason, and the copy is tried again on each start until the new folder has data: the player plays, imports, or runs `--migrate-from`.
- **The old folder** is never changed: no `MOVED.txt`, no renames, no layout migration. A player who switches to *User folder* later finds it as it was, and the switch's *already has data* question applies.
- **The one-time notice.** A title-screen notice and a log line: "Your saves and settings were copied from `<old>` to `<new>`. The old folder was left as it is." It is shown on the start that copied, through a test seam. No marker file is needed, because the new folder isn't empty afterwards.

**`--migrate-from DIR [--force]`** (#262) does the same copy from any folder: a backup, another machine's `data/`, an old per-user folder, or an old kit's `data/`. It accepts the old or the new layout. Without `--force` it refuses a destination that holds data. With `--force`, every file it replaces is first moved to `backups/import/<time>/`, as an import does, so nothing is lost. It prints what it did and exits.

**Updating to a new kit.** The new version unpacks to a new folder. The README tells the player to move `data/` and `game/` across, or to use `--migrate-from <old kit>/data`. Searching sibling folders for an older kit was rejected as guesswork.

### Other platforms

- **iOS: unchanged** (ADR 0074). The data directory is the app's per-user folder, `Documents/` is the app's own, shared with the Files app, and `gamecenter.json` stays in the data directory. There is no game folder, no probe, no marker and no switch. The location is `ios` with the reason `platform`.
- **Nintendo Switch** (#233): the data folder is `sdmc:/switch/maplethief/data/`, with the same layout (`saves/` and the rest). The bundle goes to `sdmc:/switch/maplethief/game/`. There is no per-user option, no marker and no fallback: the SD card is the only place, and a failed write is an error on screen. The location is `switch` with the reason `platform`. #236 (atomic saves on the SD card) and #241 (the menu and export or import) build on this.

## Decision

The desktop builds (macOS, Windows, Linux, dev builds included) keep the player's data in `<base>/data/`, without a marker file.

**Where `<base>` is:**

- the folder around `MapleThief.app`;
- the folder of `MapleThief.exe`;
- the Linux game folder (the parent of `bin/` when it holds `MapleThief.sh`).

`portable.txt` is accepted and does nothing.

**How the folder is chosen,** one resolver in `platform/src` for everyone. The first rule that applies wins:

1. `MAPLETHIEF_DATA_DIR`
2. `--data-location game|user` (this run)
3. `<base>/user-data.txt` (the user folder)
4. macOS with `<base>` in an Applications folder (the user folder)
5. the game folder, if the probe can create and remove a file in `<base>/data`
6. otherwise the per-user folder, with the reason `fallback-translocated`, `fallback-not-writable` or `fallback-no-base`

A fallback is logged once and shown in the Saves tab, with a one-time title notice per reason. The resolver is the only reader of `MAPLETHIEF_DATA_DIR` and the only caller of `SDL_GetPrefPath`. `--print-data-dir` prints its answer: `data`, `location`, `reason`, `why`, `saves` and `game`.

**The layout:**

- `data/` holds `settings.ini`, `bindings.ini`, `window.ini`, `saves/` (the guest's `Documents/` plus `gamecenter.json`), `backups/`, `states/`, `screenshots/`, `exports/`, `import/`, `logs/` and `crashes/`.
- The imported bundle is in `<base>/game/` (beside `data/`) for the game folder, and in the folder's own `game/` otherwise.
- `save_dir` moves `saves/`.

**Changing location:**

- The *Data location* row in the Saves tab, and `--switch-data-location game|user`, copy the player's data (and a missing bundle) to the other folder, never delete the source, write or remove `user-data.txt`, and close the game so it starts on the new folder.
- A destination that already has data is used as it is, never overwritten.

**Migration:**

- An old-layout folder is migrated in place at start: `Documents/` → `saves/`, `gamecenter.json` into `saves/`, `data/game/` → `<base>/game/`. Each step is idempotent.
- A new, empty game folder gets the old per-user folder's data copied in on its first start, without asking, through the import's checks, all or nothing, with a one-time notice. The bundle is copied too, and the old folder is never touched.
- `--migrate-from DIR [--force]` does the same from any folder.

**Other platforms:**

- iOS is unchanged.
- The Switch uses `sdmc:/switch/maplethief/data/` with the same layout and `sdmc:/switch/maplethief/game/`.

## Consequences

- **Backing up, resetting and moving get simple.** A player backs up by copying `data/`, resets by deleting `data/` (or `data/saves/`), and moves the game, data included, by moving its folder. #82's Windows zip and #198's logs build on this.
- **Unpacking a kit somewhere writable is now the normal case.** The fallback and its message cover Program Files, read-only volumes and translocation, and the data follows automatically once the cause is fixed.
- **A macOS player who double-clicks the freshly unzipped app always lands in the fallback first** (translocation). The README's first-start section and the in-game message are not optional. If Apple changes the translocation path, the reason degrades to *not writable* with the same outcome.
- **Downgrading** to a build from before this ADR on a migrated folder finds no saves (`saves/` instead of `Documents/`). A player can rename it back. The release notes say so.
- **Dev builds write `data/` into `build/`**, and a clean build deletes it. Tests are unaffected (`MAPLETHIEF_DATA_DIR`).
- **Two installs** of the game now have two separate data folders. That is intended, and the per-user option exists for a player who wants one shared folder.
- **Follow-up work:**
  - #261: the resolver, the layout, the probe and fallback, the in-place migration, `--print-data-dir`, `--data-location`, the lint for `SDL_GetPrefPath` and `MAPLETHIEF_DATA_DIR`.
  - #262: the first-start copy, `--migrate-from`.
  - #263: the kits' README, the *Data location* row, `--switch-data-location`, the player docs, the macOS Finder-move check.
  - #264: sign-off on real machines.
  - #236 and #241: the Switch.
- **How we'd know this is wrong:**
  - players report data in the user folder without having chosen it, on writable folders (the probe or the base rule is wrong);
  - or saves are lost in a switch or a migration (the copy rules are wrong);
  - or the Applications rule surprises players.

## Addendum: the resolver as built (#261, 2026-11-08)

Details the implementation settled, within the decision above:

- **Where it lives.** `platform/src/data_location.{h,cpp}`. `resolve_data_location(DataInputs)` applies the rules to injected inputs (the variable, the flag, `SDL_GetBasePath()`'s answer, the home folder, the OS, a lazy per-user folder getter and the probe), so `platform_test` covers every case without touching the real folders. `data_location()` resolves once per process and logs it; `set_data_location_for_test()` is the seam that injects an answer (a fallback, say) for the Saves tab and the notices of #262 and #263. `user_data_dir(keep)` names the old per-user folder for #262's copy; `SDL_GetPrefPath` creates it to answer, so with `keep` false an empty one is removed again.
- **The per-user folder is asked for only when it is used.** The default location never calls `SDL_GetPrefPath`, so a player of the game folder never gets an empty `~/Library/Application Support/MapleThief`.
- **gamecenter.json (migration step 2) moves in `configure_save_location()`**, not before `load_settings`: its destination is the folder `save_dir` names, which is in `settings.ini`. Steps 1 and 3 run before `load_settings`; step 2 runs right after, still before the guest or the save tools open a file. The runtime got `mt_stubs_set_gamecenter_path` for it; on iOS `gamecenter.json` stays in the data folder.
- **The runtime's own default** (`mt_stubs_data_dir` reading `MAPLETHIEF_DATA_DIR`, then the per-user folder, and `<data>/Documents`) stays for builds without the frontend, as decided. The frontend always sets the data folder, `saves/` and the Game Center file explicitly. `scripts/lint` fails on any C, C++ or Objective-C source outside the resolver that reads the variable or calls `SDL_GetPrefPath`, with `runtime/src/stubs/stubs.cpp` as the one listed exception; tests that set the variable for the program are not readers.
- **Tests did change**, contrary to *Consequences*: under `MAPLETHIEF_DATA_DIR` the game's files are now in `saves/`, so every check of `<data>/Documents/` became `<data>/saves/`. The real-asset game driver (`tests/cpp/game_driver.h`) configures the frontend's layout, so `play.save_quit` and `play.save_relaunch` run on it. A copied old save still works: the migration renames its `Documents/`.
- **`--print-data-dir` and `--data-location`** run before anything else but `--version`; the placeholder (`--smoke-frames`, the prototype, the tone) never resolves the folder, so the smoke tests don't create `build/.../data`.
- **`platform.data_folder`** (CI, every OS) copies the program into a kit folder of its own and runs it with `HOME`, `APPDATA`, `LOCALAPPDATA` and `XDG_DATA_HOME` at an empty folder: `--print-data-dir`, an import through the real write path after migrating an old layout, and the empty per-user folder. `platform.game_data_folder` (real assets) does the same with the game saving a new game. Windows' `SDL_GetPrefPath` ignores `APPDATA`, so there the check rests on the resolver never calling it, which the unit tests count.
