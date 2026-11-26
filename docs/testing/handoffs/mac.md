# Mac hand-offs

The hand-offs that need the Mac, then the checks from closed issues (#59 to #204, [Checks from closed issues](README.md#checks-from-closed-issues)). Two builds cover all of them: the game app (`scripts/build mac`) for the first sections, then the player kit (`mac-player`) for the data folder sections at the end. [How to report](README.md#how-to-report) says what to post.

Every issue's older comments name branches that are merged now (`192-saves-menu`, `196-window-size-display`, `epic-201`, `epic-259`) or `main`: build from the epic branch `epic-106-itch-handoffs`, or `dev` once it is merged.

## Setup

1. Your copy of the game at `original/MapleThief-1.5.ipa`, then `just doctor` and `scripts/bootstrap`.
2. **Back up the per-user folder first**, since #263 and #264 write to it: `cp -a ~/Library/Application\ Support/MapleThief ~/mt-userfolder-backup`.
3. Rosetta, for #142: `softwareupdate --install-rosetta --agree-to-license`.
4. A second display for #196, a 144 Hz monitor for #272 if there is one, and a gamepad (optional, #192). For the checks from closed issues: the 120 Hz MacBook display, a 16:9 or 21:9 external one, a picture of your own for #157, and an Intel Mac for #81 if one is around.
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

## #59: Integer scaling on a Retina display

From closed [#59](https://github.com/armaatus/maplethief-port/issues/59) (PR #123's hand-offs). The HiDPI path was checked only by arithmetic tests and a headless 2880 x 1800 drawable, since the agent's display is 1x. On the Mac's own Retina display, windowed:

1. `$G --window 1440x900 --scale-mode integer 2>&1 | grep "game: drawable"`: the line reads `game: drawable 2880x1800 pixels (density 2x2), integer` with 5x (a smaller drawable if the window didn't fit; then say which).
2. The picture is crisp, not blurred, centred, with bars round it.

**Post back:** the `game: drawable` line, crisp yes or no, and the Mac model.

## #64: Mute when the window loses focus

From closed [#64](https://github.com/armaatus/maplethief-port/issues/64) (PR #126's hand-offs). `mute_unfocused` is on in every preset.

1. `$G 2>&1 | grep "audio: "`, on the main menu with its music. Click another app: the game goes silent and the log says `audio: muted (the window lost focus)`. Click back: `audio: unmuted (the window gained focus)` and the music again.
2. The same in fullscreen with Cmd+Tab away and back.
3. F1 > Audio > **Mute in the background** off, then click another app: the music goes on.

**Post back:** pass or fail per step with the log lines.

## #145: Window behaviour (Mac part)

From closed [#145](https://github.com/armaatus/maplethief-port/issues/145) (PR #178's hand-offs); [Windows](windows.md#145-window-behaviour-windows-part) and [Linux](linux.md#145-window-behaviour-linux-part) have the same steps. Fullscreen and its modes are #179's and #196's sections above, so this is the rest. Since #259 `window.ini` and `screenshots/` are in the data folder, `dist/mac/data/`.

1. Windowed, move and resize the window, quit, start `$G` again: the same place and size. Zoom it (maximised), quit, start: maximised. With `--window 960x640` it starts at that size and `window.ini` isn't touched.
2. The mouse pointer hides on a key or pad press and comes back when the mouse moves.
3. Mouse wheel: in a shop list or the popup menu, one notch moves one row; on the field it does nothing.
4. F12 (and a pad's Share or Capture button) writes `dist/mac/data/screenshots/MapleThief_<date>_<time>.png`: the game's picture without the port's overlays. Look at it, don't post it.
5. F1 > Gameplay > **Pause in the background** on (it is in Original + controller and Enhanced): walk, click another app, and the game holds; click back and it goes on. Off: it keeps running behind.
6. F1 > Video > **Vsync** off, then on: the picture keeps going both times, without tearing when on.

**Post back:** pass or fail per step, and the macOS version.

## #144: The first-start preset picker (Mac part)

From closed [#144](https://github.com/armaatus/maplethief-port/issues/144) (PR #178's hand-offs); the touch half is on the [iPhone page](iphone.md#144-the-first-start-preset-picker-iphone-part). The picker shows when the data folder has no `settings.ini`. #179's step 1 sees it once; this tries every input.

1. `export MAPLETHIEF_DATA_DIR=$HOME/mt-picker; rm -rf "$MAPLETHIEF_DATA_DIR"`, then `$G`: the picker lists Original, Original + controller, Enhanced and Custom.
2. Mouse: hover and click Enhanced. The game starts with it (F1 shows Preset Enhanced). Quit, `rm -rf "$MAPLETHIEF_DATA_DIR"`.
3. Keyboard: arrows and Return pick Original. Again with a pad: d-pad and A pick Original + controller.
4. Start once more without deleting: no picker. Then delete the `preset = ...` line from `$MAPLETHIEF_DATA_DIR/settings.ini` (an older settings file) and start: the picker offers **Keep my settings** first, and choosing it keeps the file's values.
5. `rm -rf ~/mt-picker` and `unset MAPLETHIEF_DATA_DIR`.

**Post back:** pass or fail per input, and anything unclear in the picker's text.

## #89: The settings menu with each input (Mac part)

From closed [#89](https://github.com/armaatus/maplethief-port/issues/89) (PR #178's hand-offs); touch and the gear are on the [iPhone page](iphone.md#89-the-settings-menu-with-each-input-iphone-part). The menu's Gameplay > Game options is checked by `play.game_options`.

1. Keyboard: F1 opens the menu over the game, which pauses. The arrows change Video > **Scale mode** and the picture changes at once. Esc closes it; `dist/mac/data/settings.ini` has the new `scale_mode`.
2. Pad: the Guide button (and L3 + R3 together) opens it. LB and RB switch tabs, the d-pad changes Controls > **Hide the key pad**, B closes it.
3. Mouse: in fullscreen or a wide window, click the gear in the bars. Click a tab, then `<` and `>` on Audio > **Volume**: the sound changes at once.
4. Controls > **Bindings** opens the remap overlay; Save or Cancel goes back to the menu. Controls > **Edit the touch key pad** opens the editor, Cancel goes back.
5. Move through a few rows of each tab: each has a help line that says what it does.

**Post back:** pass or fail per input, and any row whose label or help is unclear.

## #88: How the filters and the HD text look

From closed [#88](https://github.com/armaatus/maplethief-port/issues/88) (PR #135's hand-offs). The filters are tested on synthetic images, timed under 1 ms and read back exact; only the look is left. Fullscreen on the Mac's display (and on an external one if you have it), in the first map and in a dialogue:

1. `$G --filter xbr`, then `--filter crt`, then `--filter lcd` (F1 > Video > **Filter** does the same live).
2. `$G --render-scale 3 --hd-text on`, then `--hd-text off`: in the first dialogue the name and the lines are sharp with `on`, and the layout is the same.

**Post back:** per filter, what looks off (a halo, a moiré, too dark, a seam at the picture's edge) or "fine"; and whether HD text should be on by default (Enhanced has it on).

## #170: Filtered widescreen wings

From closed [#170](https://github.com/armaatus/maplethief-port/issues/170) (PR #178's hand-offs). The wings, the picture and the pad strips are now one wide picture under the filter (no seam, checked byte for byte); only the look on a real display is left.

1. Fullscreen on a 16:9 display, go to the first hunting field (2.3): `$G --widescreen auto --filter crt`. The field fills the width.
2. The same with `--filter xbr`, `--filter lcd` and `--filter none`, and once with `--colorblind-filter protanopia`.

**Post back:** whether the wings read as one picture with the middle under each filter, or where they don't.

## #157: Blur and image borders

From closed [#157](https://github.com/armaatus/maplethief-port/issues/157) (PR #178's hand-offs). Read back exact and timed; the D3D12 and Vulkan half is [#131's run](windows.md#131-the-gpu-backend-on-d3d12-and-vulkan). Fullscreen on a 16:9 display, and on a 21:9 one if you have it (or a window dragged wide):

1. `$G --border blur`: the game blurred behind the picture, dimmed to 150 of 255. Too bright, too dark, or distracting while things move?
2. `$G --border image --border-image ~/border.png` with a picture of your own (not the game's art): it fills the bars, not stretched out of shape.
3. `$G --border color --border-color '#204060'` (or a colour of your choice): plain bars of that colour.

**Post back:** the look of each, and whether 150 of 255 should change.

## #87: Smooth frames on 60, 120 and 144 Hz

From closed [#87](https://github.com/armaatus/maplethief-port/issues/87) (PR #178's hand-offs). Smooth frames need the GPU renderer in real time; on software the log says `present: frame_rate = smooth needs gfx_backend = gpu in real time` and keeps the game's frames. The D3D12 and Vulkan half is [#131's run](windows.md#131-the-gpu-backend-on-d3d12-and-vulkan).

1. On each display you have (60 Hz, the MacBook's 120 Hz ProMotion, a 144 Hz one): `$G --gfx-backend gpu --frame-rate smooth 2>&1 | tee ~/mt-smooth-<Hz>.log`, walk and jump on the first map for a few minutes, quit.
2. Then `--frame-rate original` on the same display, for comparison.
3. Read the exit lines `game: frame_rate = smooth: ...`: ticks, interpolated frames and the latency after the tick's present.

**Post back:** per display, the two `game: frame_rate = smooth` lines, whether smooth looks smoother, and whether the extra latency is felt in a jump or an attack.

## #151: Game speeds and fast-forward at 60, 120 and 144 Hz

From closed [#151](https://github.com/armaatus/maplethief-port/issues/151) (PR #178's hand-offs). Each speed is exact in fixed-step; this measures it in real time on each display. The 144 Hz runs at every tick length are [#272](#272-1-ms-grid-pacing-on-a-144-hz-panel); here 60 and 120 Hz.

1. Fullscreen on a 60 Hz display, idle on the main menu, for each `S` of 65, 50, 40, 33 and 25: `$G --no-audio --game-speed <S> --seconds 26 2>&1 | grep "game: ticks"`. The rate is 1000 / S ticks a second, within 1 %.
2. The same on the 120 Hz display.
3. Play with `$G --fast-forward x2`, holding Tab (or R3) on a field; then `x3`. The exit `game: clock:` line names the time fast-forwarded.

**Post back:** the ticks lines per display and speed, and how fast-forward feels (music pitch is kept on purpose).

## #153: Texture packs

From closed [#153](https://github.com/armaatus/maplethief-port/issues/153) (PR #178's hand-offs). A pack replacing every upload is tested; the try is the player's path in [Settings, Texture packs](../../play/settings.md#texture-packs). Dumps and packs are the game's art: keep them in your home folder, never in the repository or a post.

1. `$G --dump-textures ~/mt-dump`, play the tutorial and Henesys, quit.
2. `PYTHONPATH=tools python3 -m mtport.texpack build ~/mt-dump ~/mt-pack --scale 2`.
3. `$G --texture-pack ~/mt-pack --render-scale 2`, the same maps. The exit log counts the replaced textures. Then add `--texture-filter nearest`.
4. Edit one PNG in `~/mt-pack` (paint it a colour), start again: that texture shows the edit.
5. `rm -rf ~/mt-dump ~/mt-pack`

**Post back:** the replace counts, how the 2x pack looks against the game, and whether the steps on the settings page were enough.

## #81: The app on a real Intel Mac

From closed [#81](https://github.com/armaatus/maplethief-port/issues/81) (PR #135's hand-offs). Rosetta is checked (#142 above); real Intel hardware isn't. Its other hand-off, Open Anyway on a quarantined copy, is [#263](#263-the-macos-kit-under-app-translocation). Skip this if there is no Intel Mac at hand.

1. Copy `dist/mac/MapleThief.app` and `dist/mac/data/` (or the `mac-player` kit) to the Intel Mac; `lipo -archs` on the binary names `x86_64 arm64`.
2. Open it, reach the first map, play a few minutes, quit.
3. `MapleThief.app/Contents/MacOS/MapleThief --perf-session 2>&1 | tail -5`: the `game: ticks:` line, 19.5 or more a second.

**Post back:** the Mac model, the macOS version, pass or fail, and the `game: ticks:` line.

## #174: How the scene skip feels (optional)

From closed [#174](https://github.com/armaatus/maplethief-port/issues/174), optional. The skip runs a scene at x8 and taps OK, keeping the state.

1. A new game with `$G --skip-intros on`: the intro and the tutorial's scenes run fast and end where watching them does.
2. Balrog's entrance, if a save near it is at hand.

**Post back:** too fast, too slow, or fine; anything that looked wrong while skipping.

## #175: How the auto-loot reach feels (optional)

From closed [#175](https://github.com/armaatus/maplethief-port/issues/175), optional. With it on, a drop within 160 x 32 points of the hero is picked up through the game's own pick-up.

1. `$G --auto-loot on`, hunt on the first field for a few minutes.

**Post back:** whether the reach feels too short, too long or right.

## #204: Quick-save and quick-load in real time (optional)

From closed [#204](https://github.com/armaatus/maplethief-port/issues/204) (PR #229's hand-offs), optional. The save and load are tested on the virtual clock only; the notice on screen and the clock after a load are not.

1. `$G --save-states on`, on a field: F5 shows a notice and writes `dist/mac/data/states/slot<N>.state`. Walk away, F9: back where F5 was, the music and the mobs going on smoothly.
2. F5 in a scene or with a popup open: the notice refuses it and says why.

**Post back:** pass or fail per step, and anything odd right after a load.

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

## #411: Where presents land against the display's vblanks

Closes [#411](https://github.com/armaatus/maplethief-port/issues/411). Needs no IPA and no game build. Logged in, the screen on, nothing covering the probe's window:

1. `xcrun swiftc -O tools/vblank-probe/vblank_probe.swift -o build/vblank-probe && build/vblank-probe 60 | tee vblank-probe.txt`. It takes about 4 minutes.
2. The two last blocks (`grid`, `vblank`) print the refreshes each frame was shown for; they must not say "no numbers".
3. The same on a MacBook Pro with ProMotion, and on a VRR (Adaptive-Sync) monitor if one is at hand.

**Post back:** `vblank-probe.txt` for each display, with the Mac model and the display.

## #342: The self-hosted runner for the nightly run

Closes [#342](https://github.com/armaatus/maplethief-port/issues/342)'s runner criterion. **Waits on** the agent's part of #342: the ADR 0006 addendum, the nightly workflow and its set-up page. Registering a runner needs your GitHub account, so it is yours:

1. Read the set-up page and the addendum: the runner runs on schedule and dispatch only, never for a pull request, and uploads nothing but logs and counts.
2. In the repository's Settings > Actions > Runners, *New self-hosted runner* (macOS, ARM64), and follow its commands in a folder of its own, with the labels the set-up page names. Run it as a service if the page says so.
3. Ask the agent to dispatch the nightly workflow on `dev`, or start it from the Actions tab.

**Post back:** the run's address, and that the runner shows as idle afterwards. Never paste the registration token.
