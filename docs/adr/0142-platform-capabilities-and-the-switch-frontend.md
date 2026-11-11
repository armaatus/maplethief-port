# 142. Platform capabilities: what each platform's settings menu shows, and the Switch's saves and packs

- **Status:** Accepted
- **Date:** 2026-11-11
- **Issue:** [#241](https://github.com/armaatus/maplethief-port/issues/241) (epic [#233](https://github.com/armaatus/maplethief-port/issues/233))
- **Builds on:** [ADR 0070](0070-settings-registry-and-presets.md) (the registry and its platform rows), [ADR 0076](0076-settings-menu.md) (the settings menu), [ADR 0077](0077-desktop-conveniences.md) and [ADR 0099](0099-fullscreen-modes-and-the-first-start-crash.md) (fullscreen, the hotkeys), [ADR 0103](0103-ui-scaling-window-text-veil-and-minimum-window.md) (the window), [ADR 0078](0078-save-export-import-backups-and-checks.md) (export, import, backups; #192's dialogs), [ADR 0081](0081-texture-packs-and-nearest-texture-filter.md), [ADR 0106](0106-audio-packs.md) and [ADR 0108](0108-text-packs.md) (packs), [ADR 0085](0085-borders-around-the-picture.md) (border images), [ADR 0127](0127-data-folder-next-to-the-game.md) (the data folder), [ADR 0138](0138-nintendo-switch-homebrew-target.md) (the Switch target)

## Context

The settings menu shows every registry row and every action row. On the Nintendo Switch many of them would do nothing: there is no window to make fullscreen, one display, no mouse, no file dialog and no file manager, one audio output, no other place for the data folder, and no HTTP fetcher for the update notice (`update_fetch.cpp` has none there). The path rows (texture, audio and text packs, the border image, the save folder) offer no values in the menu, because on the desktop the player types them into `settings.ini`. A controller can't do that. Export and import ask through the system's dialogs (#192), which the console doesn't have.

Until now the menu hid rows in two ways: `SettingInfo::ios_only` for two rows, and `#if !defined(SDL_PLATFORM_IOS)` around four action rows. A third platform would have added more `#if`s, and nothing would have made a new row decide whether it works on every platform.

## Options considered

1. **More `#if`s and a `switch_hidden` flag on the rows.** Rejected. It says *where* a row is hidden but not *why*, and every new platform would need another flag.
2. **A capability table, with rows naming what they need.** **Chosen.** Each platform lists what it has. Each row names the capabilities it depends on, and the menu hides a row on a platform that lacks one of them. The reason is in the code, and a new platform is one table row.
3. **Hiding nothing and making every row do something on the Switch.** Rejected. A fullscreen switch with nothing to switch, or a list of one display, would still be dead.

For the folders the issue named `sdmc:/switch/maplethief/exports/` and `.../texture-packs/`:

- **Exports and imports** stay where ADR 0127's layout puts them, in `sdmc:/switch/maplethief/data/exports/` and `data/import/`, not `sdmc:/switch/maplethief/exports/`. The layout is the same on every platform (ADR 0138 already says it holds on the SD card), the existing export and import code is reused unchanged, and a copy of `data/` carries the exports with it.
- **Packs** go beside `game/` and outside `data/`: `sdmc:/switch/maplethief/texture-packs/`, as the issue says, plus `audio-packs/`, `text-packs/` and `borders/` for the other path rows. Packs are large and made from the player's own copy of the game. ADR 0127 keeps `game/` outside `data/` for the same reason, so that a backup of `data/` stays small.

## Decision

**The table** is `platform/src/platform_caps.h`: pure and fixed at compile time. `SettingsPlatform` and `kSettingsPlatform` moved there from `settings.h`, unchanged. `kPlatformCaps` has one row per platform with a mask of `Capability` bits. `platform_caps(p)`, `platform_has(needs, p)` and `platform_name(p)` read it.

| Capability | What needs it | Desktop | iOS | Switch |
|---|---|---|---|---|
| `kCapWindow` | `fullscreen`, `fullscreen_mode`, `fullscreen_display`; with `kCapKeyboard`, the fullscreen hotkeys and the window size keys | yes | no | no |
| `kCapDisplays` | `fullscreen_display` | yes | no | no |
| `kCapCursor` | `right_touch`, the cursor's auto-hide | yes | yes | no |
| `kCapKeyboard` | the screenshot key F12; the window's hotkeys with `kCapWindow` | yes | yes | no |
| `kCapFileDialogs` | Export and Import through the system's dialogs | yes | no | no |
| `kCapOpenFolder` | Open data folder, Open logs folder | yes | no | no |
| `kCapTypedValues` | `save_dir`, `keypad_layout`: rows that offer no values and only show what `settings.ini` says | yes | yes | no |
| `kCapDataLocationChoice` | Data location | yes | no | no |
| `kCapAudioDevices` | `audio_device` | yes | yes | no |
| `kCapQuit` | Quit | yes | no | yes |
| `kCapUpdateFetch` | `update_check`, Port update | yes | yes | no |
| `kCapFilesApp` | the iOS import: the newest zip the Files app put in `import/` | no | yes | no |
| `kCapPackFolders` | the pack rows offer the pack folders; Import is a list | no | no | yes |
| `kCapIpaOnSdCard` | the import screen's SD-card route: the IPA beside the game, no dialog or drop (#243, ADR 0138's addendum) | no | no | yes |

**The rows.** `SettingInfo::needs` and `MenuActionInfo::needs` hold the bits a row needs (0 means every platform). `setting_shown(row, platform)` adds the needs to the `ios_only` test, and `action_shown(a, platform)` is the actions' version. The menu builds its tabs and rows for its platform: `SettingsMenu::set_platform` lets a test build another platform's menu, and the build's own is the default. The iOS `#if`s around the action rows are gone; their `needs` give the same result. The file and the command line still take every key on every platform.

**The desktop** has every capability except the iOS Files app and the Switch's pack folders, so nothing changes there: every row shows, the drawings' hashes are unchanged, and the routes are the same. **iOS** loses the three fullscreen rows, which did nothing there (ADR 0055: the app is always fullscreen), and the window hotkeys, which did nothing either. Everything else on iOS stays as it was.

**The Switch** hides `fullscreen`, `fullscreen_mode`, `fullscreen_display`, `right_touch`, `audio_device`, `save_dir`, `keypad_layout` (its editor row, Edit the touch key pad, stays) and `update_check`, plus the Data location, Open data folder, Open logs folder and Port update rows. The iOS-only rows stay hidden as before. `Desktop::handle` ignores the fullscreen hotkeys, the size keys and F12 there, and leaves the cursor alone.

**Every row is decided.** The test `settings menu: every row is supported or hidden on the Nintendo Switch (#241)` has one line per registry row and per action row: shown on the Switch or hidden, and why. It fails on a row missing from that table, so a new row has to be decided. It also builds the Switch's menu and walks every tab with the pad alone. Each shown setting must change with Left / Right, and each shown action must run with South (Right first for a question). The set of rows it meets must be exactly the decided ones.

**Packs on the Switch** (`platform/src/pack_folders.{h,cpp}`): `offer_pack_folders(<base>)` runs once at the menu's start. It makes `texture-packs/`, `audio-packs/`, `text-packs/` and `borders/` in `<base>` (so the player sees where packs go) and gives the rows their entries through `set_pack_choices`: subfolders, or `.png` files for the borders, as whole paths sorted by name. The menu offers "" (none) and those entries, and shows a value by its last part. An empty folder makes the row say where its packs go. Without `set_pack_choices` a row offers nothing, as before, so the desktop is unchanged.

**Saves on the Switch.** `dialog_place` sets `no_dialogs` where the platform has no dialogs, so `dialog_route` gives `kFolder` and Export writes `data/exports/MapleThief-saves-<time>.zip` as it does wherever no dialog can show. Import, with neither dialogs nor the Files app, is a list in the menu (`save_menu.h`'s `import_saves_list_action`, `import_list`). It holds every zip and folder in `import/` and every zip in `exports/`, newest first. Choosing one asks first (No focused), then imports it through the same checked, backed-up `import_saves`. As before, this works only with no character loaded. Backups are unchanged. The test `save menu: on the Switch, export then import on a fresh data folder restores the saves, with the controller (#241)` runs the whole path on the host: it exports from one data folder, copies the zip to a fresh folder's `import/`, imports through the Switch's menu with pad events, and compares the files byte for byte. It is also in `platform_test.nro`.

**Presets.** The Switch's defaults are already a platform override of the registry (`kPlatformDefaults`, ADR 0070's #238 addendum). A hidden row keeps its platform value (`fullscreen = off`), and the presets still set it.

## Consequences

- Every new registry row or action row must be decided for the Switch in the test, and must state its `needs` when it depends on something a platform may lack.
- #239 and #240 build on the table. A row that only works with a capability names it in `needs`, and frontend code asks `platform_has(...)`, not `__SWITCH__`.
- On the Switch, `gfx_backend = gpu` and `filter` stay shown. They need #242's deko3d backend, and until then they fall back as on any machine without SDL's GPU renderer. If #242 is dropped, they get a capability bit and are hidden.
- `audio_pack`, `text_pack`, `texture_pack` and `border_image` gain an `offer` hook (empty unless set). The desktop menu, file and flags are as before.

**How we'd know this was wrong:**

- A row the Switch hides turns out to be wanted there, for example a USB keyboard player who wants F12. Then the capability is split, or the Switch row gains it.
- Joris's walk through every page on the console finds a dead row the test missed. The decision table and the walk then get that case.
- Players want to name a pack outside these folders on the console. Then the folders become a list of places rather than one.
