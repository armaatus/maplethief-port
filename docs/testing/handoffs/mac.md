# Mac hand-offs

The hand-offs that need the Mac. Two builds cover all of them: the game app (`scripts/build mac`) for the first sections, then the player kit (`mac-player`) for the data folder sections at the end. [How to report](README.md#how-to-report) says what to post.

Every issue's older comments name branches that are merged now (`192-saves-menu`, `196-window-size-display`, `epic-201`, `epic-259`) or `main`: build from the epic branch `epic-106-itch-handoffs`, or `dev` once it is merged.

## Setup

1. Your copy of the game at `original/MapleThief-1.5.ipa`, then `just doctor` and `scripts/bootstrap`.
2. **Back up the per-user folder first**, since #263 and #264 write to it: `cp -a ~/Library/Application\ Support/MapleThief ~/mt-userfolder-backup`.
3. Rosetta, for #142: `softwareupdate --install-rosetta --agree-to-license`.
4. A second display for #196, a 144 Hz monitor for #272 if there is one, and a gamepad (optional, #192).
5. **Build 1, the game app:** `scripts/build mac`. It makes `dist/mac/MapleThief.app` (universal, Release, the game inside) with its data folder at `dist/mac/data/`. Below, `G` stands for `dist/mac/MapleThief.app/Contents/MacOS/MapleThief`.
6. **Build 2, the player kit** (for the last two sections): `cmake --workflow --preset mac-player`. It makes `build/mac-player/MapleThief-<version>-macos-universal.zip`, with no game inside, so the first start shows the import screen. Don't use `scripts/release build TAG --players-only` for this: it needs a release tag.

`G --print-data-dir` prints which data folder a start would use and why; use it whenever a step is about the folder.

## #179: Fullscreen first start, modes and Quit

Closes [#179](https://github.com/armaatus/maplethief-port/issues/179). The code merged in #180; this is the macOS check. The comment's `~/Library/Application Support/MapleThief/settings.ini` is out of date: since #259 the data folder is beside the app, so a fresh `MAPLETHIEF_DATA_DIR` is used instead.

1. `export MAPLETHIEF_DATA_DIR=$HOME/mt-fs-test; rm -rf "$MAPLETHIEF_DATA_DIR"`, then `$G`. The preset picker shows; pick **Enhanced**. It opens in borderless fullscreen, no crash.
2. Quit and start again: no crash.
3. F11 off and on again. Then the green button: in borderless it switches to `fullscreen_mode = desktop` (a Space).
4. Settings menu (F1) > Video > **Fullscreen mode**, the other value, restart: the new mode is used.
5. Settings menu > **Quit** at the bottom of a tab: No is focused, Yes quits.
6. Optional, on an idle Mac: `cmake --preset mac-debug && cmake --build --preset mac-debug`, then `ctest --preset mac-debug -R platform.game_speed`.

**Post back:** pass or fail per step, the macOS version and the Mac model.

## #196: Window size hotkeys and the fullscreen display

Closes [#196](https://github.com/armaatus/maplethief-port/issues/196) (a two-display Windows PC would do too). Windowed, from Build 1:

1. Cmd+3: a centred 1440 x 960 point window; the log says `desktop: window 3x: 1440x960 at (x, y)` (or `does not fit`). Move it to the other display, Cmd+2: 960 x 640 there. With `scale_mode = integer` no bars.
2. F11, then Cmd+4: it leaves fullscreen and comes back at 4x (or the largest that fits), no crash. Once with **Fullscreen mode** `desktop`, once `borderless` (restart in between).
3. F1 > Video > **Fullscreen display** lists `current` and both displays. Pick the other one, F11: fullscreen there. F11 again: windowed there, centred.
4. Quit with fullscreen on and the second display chosen, start again: fullscreen on the second display. Then unplug it (or set `fullscreen_display = Nothing` in `dist/mac/data/settings.ini`) and start: the log says `desktop: the display "..." is not connected...` and settings.ini still names the display.

**Post back:** pass or fail per step and the `desktop:` log lines.

## #192: Saves in the menu (Mac part)

Covers the Mac part of [#192](https://github.com/armaatus/maplethief-port/issues/192); [Windows](windows.md#192-saves-in-the-menu-windows-part) has the rest, and the Linux folder check is the agent's. The comment's portable mode (`portable.txt`) is gone since ADR 0127; `exports/` and `import/` are now under `data/`. On the title, F1 > **Saves**:

1. **Export saves...** opens a save dialog in `data/exports/` suggesting `MapleThief-saves-<time>.zip`. Save to the Desktop: `Exported to <name>.zip`. Again with a name without `.zip`: it gets `.zip`. Again and Cancel: `Export cancelled`.
2. **Import saves...** opens in `data/import/` showing only `.zip` files. Pick that zip: `Imported N files from <name>.zip`.
3. Steps 1 and 2 in fullscreen, with **Fullscreen mode** `desktop` and then `borderless`: the dialog shows over the game and works.
4. Load a character, then Import: no dialog, and the status says `Import from the title or the main menu (a character is loaded)`.
5. **Open data folder** opens Finder on `dist/mac/data/`. Then start `$G --data-location user` (a path with a space): it opens `~/Library/Application Support/MapleThief`.
6. Optional, with a gamepad: the Backups list. D-pad moves, LT+down pages, A on a row asks with No selected, Right then A restores, B goes back.

**Post back:** OK or the problem per step.

## #198: Crash dialog and safe start (Mac part)

Covers the Mac part of [#198](https://github.com/armaatus/maplethief-port/issues/198); [Windows](windows.md#198-crash-dialog-and-safe-start-windows-part) has the other.

1. `export MAPLETHIEF_DATA_DIR="$HOME/mt-crash-test"`
2. `$G --preset original --force-crash host`, then the same with `--force-crash trap`. Each time: a "MapleThief crashed" box naming a `crashes/` path, and Open folder works (for `host` the box comes from a second process). The crash file has the version lines, the guest report and the last log lines, and `logs/` has a log with its header.
3. Start again without `--force-crash` (same folder, `--preset original`): the box offers to start with default settings (no texture or audio packs, software renderer). Choose it and play a little; settings.ini is unchanged.
4. `rm -rf ~/mt-crash-test`

**Post back:** steps 2 and 3 per mode.

## #124: Audio listening check

Closes [#124](https://github.com/armaatus/maplethief-port/issues/124); everything else in it is evidenced in the 2026-10-30 comment. Mouse and keyboard work now (M7 is done), so the comment's `--tap` arrays are not needed.

1. `MAPLETHIEF_DATA_DIR=/tmp/mt-listen-menu $G --seconds 75`: the menu track loops with about 3.3 s of silence near 59 to 62 s and no click. Did the device pause the same way?
2. `MAPLETHIEF_DATA_DIR=/tmp/mt-listen-map MAPLETHIEF_TRACE=audio $G --seconds 150`, and play: the select sound and the menu music stopping; the quiet intro track with its pause around the first speech box; the Henesys track starting from its beginning; a water drop for each OK in MacArthur's dialog; any harshness in the 8 kHz effects; walking between maps, a field map resumes its track and a town map restarts it.
3. Step 2 with `--seconds 1800`: no crackle, and the exit line ends with `0 late device callbacks`.
4. Optional: a recording of the menu loop from the device, for comparison.

**Post back:** OK or what differs, per point.

## #142: Performance under Rosetta (Mac part)

Covers the Mac part of [#142](https://github.com/armaatus/maplethief-port/issues/142): an Intel Mac or Rosetta (Apple M4 native was measured on 2026-10-30). The issue's `perf-check` recipe was never built; the exit log line is the measure. The other platforms' parts: [Windows](windows.md#142-performance-windows-part), [Linux / Steam Deck](linux.md#142-performance-steam-deck-part); the iPhone's numbers can come from any iPhone session's console.

1. `lipo -archs $G` names `x86_64 arm64`.
2. `arch -x86_64 $G --perf-session 2>&1 | tee ~/mt-perf-rosetta.log`, at default settings.
3. Load a hero, stay on a map for 5 minutes or more, then F1 > Quit.
4. Read the exit lines `game: ticks: ... N a second; tick interval p50 ... ms, p99 ... ms` and `game: clock: ...`. The criterion is at least 19.5 ticks a second and p99 under 75 ms.
5. Optional: `PYTHONPATH=tools python3 -m mtport.perfsession judge ~/mt-perf-rosetta.log --displays desktop`. Its thresholds are #244's (stricter), so read it as a note only.

**Post back:** the `game: ticks:` and `game: clock:` lines.

## #272: 1 ms grid pacing on a 144 Hz panel

Closes [#272](https://github.com/armaatus/maplethief-port/issues/272). Needs a real 144 Hz display: a 120 Hz ProMotion panel doesn't count, as 120 is a multiple of 20. If the only 144 Hz panel is on the Windows PC, run the same commands there with `dist\win\...\MapleThief.exe`.

1. Fullscreen on the 144 Hz display, for each tick length L of 65, 50, 40, 33 and 25: `$G --preset original --no-audio --game-speed <L> --seconds 26 --perf-csv ~/mt-144-<L>.csv 2>&1 | tee ~/mt-144-<L>.log`, staying on the main menu.
2. Read `game: clock: pacing = display, a 1 ms grid (...), a <L> ms tick ...` and `game: ticks: ... a second`.

**Post back:** each L's ticks a second against its rate (15.385, 20, 25, 30.303, 40), all within 0.5 %, and the clock line.

## #264: Data folder sign-off (Mac part)

Covers the macOS part of [#264](https://github.com/armaatus/maplethief-port/issues/264); [Windows](windows.md#264-data-folder-sign-off-windows-part) and [Linux](linux.md#264-data-folder-sign-off-linux-part) have the others. Uses Build 2. Do step 1 first, while the per-user folder still holds an older build's saves.

1. Unzip the kit to `~/Games/` and clear the quarantine: `xattr -dr com.apple.quarantine ~/Games/MapleThief-<version>-macos-universal`. Below, `M` is `MapleThief.app/Contents/MacOS/MapleThief` in that folder; run `$M --print-data-dir` before and after each step.
2. First start: the IPA import, then the older saves are copied into the empty `data/`, with a notice for 20 s. The old folder is left as it was.
3. Play, save, change a setting and a binding (F1 > Controls > Bindings): only `data/` and `game/` in the kit change; the per-user folder doesn't.
4. Copy the whole kit folder elsewhere (a USB stick) and start it there: the hero, settings and bindings come along.
5. Delete `data/saves/`: empty slots, settings kept. Delete `data/`: a first start.
6. F1 > Saves > **Data location** > User folder, from the title. After the restart the hero is there and the user folder is used. Switch back: the same.

**Post back:** one macOS comment with each step's result and a picture of the Saves tab's text (or the text typed out).

## #263: The macOS kit under App Translocation

Closes [#263](https://github.com/armaatus/maplethief-port/issues/263) (its hand-off part). Uses Build 2. The comment's `log stream` shows nothing (SDL logs to stderr); the player log file is the evidence.

1. Give the zip a quarantine flag: AirDrop it to yourself or download it through a browser. `xattr -l <zip>` shows `com.apple.quarantine`.
2. Double-click to unzip it in `~/Downloads`, open `MapleThief.app` (Privacy & Security > Open Anyway if asked) and import the IPA.
3. Check: the newest `~/Library/Application Support/MapleThief/logs/MapleThief-*.log` says the folder `can't be written (App Translocation runs the app from a read-only copy); using the user folder`; the title notice shows once; F1 > Saves shows `Data: .../Library/Application Support/MapleThief`, Data location says *User folder* and its help names App Translocation; choosing the row is refused; the user folder's settings.ini has `data_fallback_seen = fallback-translocated`.
4. In Finder, drag `MapleThief.app` to the Desktop and back, then open it: `data/` appears beside the app and Saves says *Game folder*.
5. A fresh unzip, then `xattr -dr com.apple.quarantine ~/Downloads/MapleThief-<version>-macos-universal` and open: the same as step 4.
6. Optional: Saves > Data location > Yes on the title: Done, it quits after about 4 s, `user-data.txt` appears beside the app and the next start uses the user folder. The same row switches back.

**Post back:** each step's result, and whether the Finder move alone cleared translocation (if not, the README and [Troubleshooting](../../play/troubleshooting.md) keep only the `xattr` fix).

Afterwards: restore `~/mt-userfolder-backup` if needed, and delete `~/mt-fs-test` and `/tmp/mt-listen-*`.
