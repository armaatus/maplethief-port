# Nintendo Switch sign-off checklist

The hardware sign-off of the Switch build ([#245](https://github.com/armaatus/maplethief-port/issues/245), epic [#233](https://github.com/armaatus/maplethief-port/issues/233), [ADR 0138](../adr/0138-nintendo-switch-homebrew-target.md)). It gathers the hardware checks of the epic's children into one session on a real console, run on the **player build's** `MapleThief.nro`, the one inside `MapleThief-<version>-switch.zip`. It is also the Switch's row of the per-platform playthrough checklists ([#92](https://github.com/armaatus/maplethief-port/issues/92)).

Every row has an ID, the steps, what to expect, the evidence to attach and what it signs off. [How to report](#how-to-report) says how to tick it. [The epic's exit criteria](#the-epics-exit-criteria) maps each criterion of #233 to its rows.

Nothing from this run goes into git: no NRO, no log with your own paths, no screenshot or photo of the game's picture. Pictures are judged by eye and described in words. Logs go into the #245 comment as attachments.

## Before you start

**What you need**

- A Switch (original, OLED or Lite; not the Switch 2) running custom firmware with the Homebrew Menu. This project doesn't explain how to set that up.
- The dock and a TV for the docked rows, both Joy-Con, the grip or wrist straps, and a Pro Controller. A Lite has no dock and no detachable Joy-Con, so on a Lite only the handheld rows apply.
- The Mac on the same network as the console, with Docker (OrbStack) running. The firewall must let the console reach the Mac on TCP 28771 (nxlink's output port).
- Your own copy of the game, `MapleThief-1.5.ipa`, the one in [`original/SHA256SUMS`](../../original/SHA256SUMS) ([Your game file](../play/your-ipa.md)).
- An existing save with a character that can walk onto a field map, as a zip exported from the desktop build (Saves → Export saves…), for the import row G4. You can also play a new character up to a field map.
- About 2 hours. The 30-minute play (P1) and the performance session (F1, about 15 minutes) are the long rows.

**Builds** (on the Mac, from `dev`):

```sh
scripts/bootstrap && python3 -m recomp all          # work/ipa and work/gen, if not there yet
MAPLETHIEF_VERSION=0.9.245-rc.1 scripts/build-switch --preset switch-player --gen work/gen
#   -> build/switch-player/MapleThief-0.9.245-rc.1-switch.zip   (the player zip; rows I1 to G5)
mkdir -p build/switch-release && cp build/switch-player/MapleThief.nro build/switch-release/MapleThief.nro   # the game, for scripts/switch-perf (row F1)
```

`scripts/build-switch --preset switch-release` without `--gen` builds the hello NRO (#235), not the game, and with `--gen` its kit guard fails the run by design (#243). So F1 sends the player build's NRO: `scripts/switch-perf` reads `build/switch-release/MapleThief.nro`, hence the copy. Don't use `scripts/switch-perf --build`, which rebuilds the hello NRO there.

If a release with a Switch zip has been drafted or published by then, download `MapleThief-<version>-switch.zip` from it instead and use that. Rows I1 and I2 then check the release's own file.

**Where the evidence is**

| What | Where |
|---|---|
| The player log of each run | `sdmc:/switch/maplethief/data/logs/MapleThief-<time>.log` (the newest). It writes the data folder as `<data>` and the game folder as `<game folder>` |
| A run sent with nxlink | the Mac's terminal; `scripts/switch-perf` keeps it in `build/test-output/switch/MapleThief-perf-<time>.log` |
| A crash | `sdmc:/switch/maplethief/crash.txt` and `sdmc:/switch/maplethief/data/crashes/` |
| The perf judge | the `perfsession:` lines at the end of `scripts/switch-perf`'s output |

The log lines quoted below are the code's own text. A `...` stands for the part that varies.

**Defaults.** Run everything at the defaults, except where a row says otherwise. Since #244 ([ADR 0145](../adr/0145-switch-performance-session-and-defaults.md)) the Switch draws with `gfx_backend = gpu` (deko3d) at `render_scale = 1`, with `scale_mode = sharp` and the touch key pad hidden until touched ([Settings on the Switch](../play/settings.md#on-the-nintendo-switch)). If the default renderer was changed back to software on #244, the `gfx:` line says so; note which one ran.

**Opening the settings menu and quitting.** `+` opens the settings menu (both stick clicks do too; on one Joy-Con alone, its stick click). L / R change tabs, A chooses, B closes. **Quit** is the bottom row of every tab: Right for Yes, then A. It goes back to the Homebrew Menu.

**Two ways to start the Homebrew Menu.** *Applet mode*: from the Album. *Title takeover*: hold R while you start any game. Use title takeover unless a row says applet mode.

## I. Installing from the zip, and the import

Signs off [#243](https://github.com/armaatus/maplethief-port/issues/243)'s hardware criterion and part of [#235](https://github.com/armaatus/maplethief-port/issues/235)'s.

### I1. The zip holds the program and no file of the game (Mac)

- **Steps:**
  1. `unzip -l build/switch-player/MapleThief-<version>-switch.zip`
  2. `PYTHONPATH=tools python3 -B -m mtport.release guard --mode player --dispatch work/gen/dispatch.c build/switch-player/MapleThief-<version>-switch.zip`
  3. Unzip it to a scratch folder and run `PYTHONPATH=tools python3 -B -m mtport.nro check --name MapleThief --author armaatus --version <version> <scratch>/switch/maplethief/MapleThief.nro`
- **Expect:** the zip holds only `switch/maplethief/MapleThief.nro`, `README.md`, `LICENSE`, `THIRD_PARTY.md` and `licenses/`. The guard passes in player mode. The NRO check passes: NACP name `MapleThief`, author `armaatus`, the version, a 256 x 256 JPEG icon.
- **Evidence:** the three commands' output, pasted.
- **Signs off:** #233 *No assets in the release*.

### I2. A fresh folder on the SD card

- **Steps:**
  1. Console off, SD card in the Mac (or over FTP). Rename `sdmc:/switch/maplethief/` to `maplethief-old` if it exists, so that this run starts with no data, no game folder and no settings.
  2. Unzip the zip and copy its `switch` folder to the root of the card, as [Playing on the Nintendo Switch](../play/switch.md#download-and-copy) says.
  3. Copy your `MapleThief-1.5.ipa` into `sdmc:/switch/maplethief/`, then rename it `MapleThief-1.5.ipa.keep` (for I3).
- **Expect:** `sdmc:/switch/maplethief/` holds the zip's five entries and the `.keep` file.
- **Evidence:** a directory listing of the folder (`ls -la` on the card), pasted.
- **Signs off:** #243 (installing from the release zip).

### I3. The Homebrew Menu entry, and a wrong file refused

- **Steps:**
  1. Put any other file in the folder, named `MapleThief-1.5.ipa`: a renamed zip, a text file, or another app's IPA.
  2. Start the Homebrew Menu (title takeover). Look at MapleThief's entry.
  3. Start MapleThief.
- **Expect:** the entry shows the port's own icon, the name **MapleThief**, the author **armaatus** and the zip's version. The import screen says the file was refused and why (not an IPA or not MapleThief's, another version, encrypted, or damaged), with A to look again and + to return to the Homebrew Menu.
- **Evidence:** the refusal's text, typed out. The console's system version and the Homebrew Menu's version.
- **Signs off:** #233 *starts from the Homebrew Menu*; #243 (the refusal).

### I4. The import

- **Steps:**
  1. Leave MapleThief on the import screen. Over FTP, or with the card out and back, delete the wrong file and rename `MapleThief-1.5.ipa.keep` back to `MapleThief-1.5.ipa`. If you took the card out, start MapleThief again.
  2. Press A. Time the import.
  3. At the question, check that **Keep** is focused. Press Right for **Delete**, then A.
- **Expect:** a progress bar while it extracts, then the Keep/Delete question with Keep focused. After Delete, the IPA is gone from the card and `sdmc:/switch/maplethief/game/` exists. The game starts.
- **Evidence:** pass or fail, how long it took, and the player log of this run.
- **Signs off:** #233 *imports the player's IPA from the SD card*; #243.

### I5. The first map

- **Steps:**
  1. The first start shows the preset picker: pick **Original + controller**.
  2. Go through the title, the main menu and character creation (a new character) until the first map shows. Play a minute there.
  3. Quit from the settings menu.
- **Expect:** each screen reaches the picture. In the log: a `data: ...` line naming `sdmc:/switch/maplethief/data`, a `game: ...` line naming the imported game folder, `gfx: mt_gfx draws with ...; the window's renderer is ...`, the `video driver switch, renderer ..., window 1280x720 ...` line (handheld), and no `unimplemented report:` line. A player run prints the report only when the game called something the port doesn't implement ([ADR 0030](../adr/0030-runtime-diagnostics.md)), so a clean run has none.
- **Evidence:** the player log; the output of `grep -i unimplemented` on it (empty when nothing is missing).
- **Signs off:** #233 *reaches the first map*; #238's hardware criterion.

### I6. The second start goes straight to the game

- **Steps:** start MapleThief again from the Homebrew Menu.
- **Expect:** no import screen and no preset picker; the title screen comes up, and loading a game shows the character from I5.
- **Evidence:** pass or fail.
- **Signs off:** #243; #233 *keeps a save across a restart* (first half; L4 is the full one).

## M. Applet mode and title takeover

Signs off [#236](https://github.com/armaatus/maplethief-port/issues/236)'s and [#240](https://github.com/armaatus/maplethief-port/issues/240)'s run-mode criteria. Put your own save in place first if you want a stronger character: G4 imports it, or do G4 now and come back.

### M1. Applet mode

- **Steps:**
  1. Start the Homebrew Menu **from the Album**, then MapleThief.
  2. Load the character and walk through 3 or 4 maps (a town and two fields).
  3. Quit from the settings menu.
- **Expect:** the game starts and plays. The log has `switch: applet mode; the memory is logged at each map load and each new peak`, a `memory: map ... loaded at ... (applet mode): guest committed ..., heap ... of ... in use, process ... of ...; headroom ...` line per map, and at the end `memory: the run's peak (applet mode): ...` with *fits* or *does not fit*. If it doesn't fit, the notice *MapleThief is short of memory in applet mode ...* shows once on screen: that is the designed behaviour, not a failure, but note it.
- **Evidence:** the player log (or every `memory:` and `switch:` line); whether the notice showed.
- **Signs off:** #233 *starts from the Homebrew Menu, in applet mode*; #240 (applet-mode memory, which sets `kAppletMarginBytes`).

### M2. Title takeover

- **Steps:** the same as M1, with the Homebrew Menu started through a game (hold R).
- **Expect:** `switch: title takeover; ...` and the `memory: the run's peak (title takeover): ...` line. No short-of-memory notice.
- **Evidence:** the `memory:` and `switch:` lines.
- **Signs off:** #233 *starts from the Homebrew Menu, in title takeover*; #240.

## C. Controllers and touch

Signs off [#239](https://github.com/armaatus/maplethief-port/issues/239)'s hardware criteria. The full table of buttons is [Nintendo Switch controls](../controls/switch.md). Start with no `sdmc:/switch/maplethief/data/bindings.ini` (it isn't there after I2), so the Switch's defaults apply. Play the rows in one run, started with the Joy-Con attached; quit at the end of C8 and keep that run's log.

### C1. What SDL reports

- **Steps:** start MapleThief handheld; load the character onto a field map.
- **Expect:** 8 lines `input: pad N: 'Switch Controller', SDL type standard ...`; pad 1 ends in `handheld, shown as Nintendo (joyconpair)`.
- **Evidence:** every `input: pad` line.
- **Signs off:** #239 (the controller log).

### C2. Handheld, Joy-Con attached

- **Steps:** on the field map, then in a shop:
  1. Move with the stick and the d-pad. B jumps, Y attacks, X / A are quick slots 1 / 2, ZL / ZR quick slots 3 / 4, L the map, R the cash shop, `-` the game's menu.
  2. In the shop and its popups: A confirms, B goes back, L / R change tabs.
  3. `+` opens the settings menu; A chooses, B closes. Controls → Controls card shows A / B / X / Y, L R, ZL ZR, `-` `+` in their places.
- **Expect:** each button does what step 1 to 3 say.
- **Evidence:** pass, or which button misbehaved.
- **Signs off:** #233 *playable with Joy-Con (attached)*; #239.

### C3. A detached pair

- **Steps:** detach both Joy-Con while the game runs; play C2's steps in two hands, then in the grip.
- **Expect:** `input: controller changed: ... Joy-Con pair` in the log; the game goes on (the console may show its controller screen; confirm it). C2's buttons work.
- **Evidence:** the `input: controller changed` lines; pass or what failed.
- **Signs off:** #233 *playable with Joy-Con (detached)*; #239.

### C4. One Joy-Con on its side

- **Steps:** hold one Joy-Con on its side (the console asks via its controller screen). Then the other one.
- **Expect:** `input: controller changed: ... left Joy-Con` / `right Joy-Con`. The bottom button jumps (back in menus), the right one is quick slot 2 (OK), the left one attacks, the top one quick slot 1. SL / SR are quick slots 3 / 4. The stick moves; its click opens the settings menu; its `-` or `+` opens the game's menu. In menus left / right change tabs.
- **Evidence:** the log lines; pass or what failed, per Joy-Con.
- **Signs off:** #239.

### C5. Pro Controller

- **Steps:** connect a Pro Controller while the game runs; play C2's steps with it.
- **Expect:** `input: controller changed: ... Pro Controller`; C2's buttons work.
- **Evidence:** the log line; pass or what failed.
- **Signs off:** #233 *playable with a Pro Controller*; #239.

### C6. Touch only

- **Steps:** handheld, Joy-Con attached but untouched. From the title screen: tap through the title and the character select; on the field map tap the key pad's buttons and drag on its d-pad to walk. Then press a button.
- **Expect:** the key pad is hidden at the start; the first touch shows it and already works; taps and drags work everywhere; a button press hides the key pad again.
- **Evidence:** pass, or what didn't respond. If touch does nothing at all, say so first (that is the fix of the pinned SDL's touch code, `cmake/patches/sdl3-nx-touch.cmake`).
- **Signs off:** #233 *playable with the touch screen*; #239.

### C7. Glyphs and prompts

- **Steps:** Settings → Controls → Button prompts on. Open the shop and a popup with the Joy-Con, then with one Joy-Con on its side.
- **Expect:** Nintendo glyphs everywhere: A for OK, B for back; SL SR and the stick's click for a Joy-Con on its side. No Xbox labels.
- **Evidence:** pass, or which glyph was wrong (in words).
- **Signs off:** #239.

### C8. Remap, rumble, and a restart

- **Steps:**
  1. `+` → Controls → Bindings → In the field: attack → press ZR → Save.
  2. Take a hit from a monster with each controller in turn (attached, pair, one Joy-Con, Pro Controller). Then Controls → Rumble strength 30, and take another hit.
  3. Quit from the settings menu. Start again, load the character: ZR attacks, Y doesn't.
  4. Controls → Bindings → Reset to defaults → Save.
- **Expect:** each controller rumbles (both sides of a pair or Pro Controller), weaker at 30. The remap survives the restart.
- **Evidence:** pass or what failed, per controller; this run's player log.
- **Signs off:** #239.

## D. Handheld and docked

Signs off [#238](https://github.com/armaatus/maplethief-port/issues/238)'s picture criterion, with [#242](https://github.com/armaatus/maplethief-port/issues/242)'s dock change on the GPU renderer.

### D1. Handheld picture

- **Steps:** start MapleThief handheld; go through the title, the main menu and onto a field map.
- **Expect:** `video driver switch, renderer ..., window 1280x720 (1280x720 pixels)` and the `gfx:` line in the log. The picture fills the height, centred with bars left and right (1080 x 720 of the 1280 x 720 screen, `sharp`), not stretched or cut. Text and sprites are sharp, colours right, no flicker or missing layers.
- **Evidence:** the two log lines; the picture judged in words.
- **Signs off:** #233 *shows the right picture handheld*; #238.

### D2. Docked picture

- **Steps:** start MapleThief with the console in the dock.
- **Expect:** `window 1920x1080 (1920x1080 pixels)`. The picture is 1620 x 1080, centred, bars left and right; as D1 otherwise. The touch screen is off.
- **Evidence:** the log line; the picture judged in words.
- **Signs off:** #233 *shows the right picture docked*; #238.

### D3. A dock change mid-game

- **Steps:** on a field map, while walking: put the console in the dock; play 30 s; take it out; play 30 s; do it once more. Also do it once during a map load.
- **Expect:** `applet: operation mode docked` / `handheld` lines. Each time the picture follows within a moment: the new size, still centred, not stretched, cropped or drawn at the old size. The game, the music and the controls go on. No error line after the change (`grep -i -E "error|fail"` near the operation-mode lines).
- **Evidence:** the `applet: operation mode` lines and the 20 lines after each; pass or what went wrong.
- **Signs off:** #233 *shows the right picture handheld and docked*; #238; #242 (the swapchain follows a dock).

## L. HOME, sleep, and a quit and restart that keeps the save

Signs off [#240](https://github.com/armaatus/maplethief-port/issues/240)'s lifecycle criteria. Before L1, set Settings → Gameplay → Save on quit (`autosave = on_quit`), or pick the Enhanced preset. Title takeover.

### L1. HOME mid-map, then back

- **Steps:** on a field map, press HOME. Wait 10 s. Pick the game again.
- **Expect:** the game goes on, on the same map, with the music back. In the log, in this order: `applet: focus Background`, `lifecycle: applicationWillResignActive: ...`, an `autosave: slot N saved at ...` line (or `not saved ...` with the reason), `applet: focus InFocus`, `lifecycle: applicationDidBecomeActive: ...`.
- **Evidence:** those lines.
- **Signs off:** #233 *keeps the game and its saves through HOME*; #240.

### L2. Sleep and resume

- **Steps:** on a field map, press the power button. Wait at least 60 s. Wake the console and return to the game.
- **Expect:** the game resumes on the same map. The same lines as L1, and `applet: resumed` around the wake.
- **Evidence:** those lines; whether a `focus Background` came for the sleep.
- **Signs off:** #233 *keeps the game and its saves through sleep and resume*; #240.

### L3. Closed from the HOME menu

- **Steps:** on a field map, note the map. HOME, then close the software (X, Close). Start MapleThief again and continue that slot.
- **Expect:** `applet: exit requested (closed from the HOME menu)`, an `autosave:` line, `lifecycle: applicationWillTerminate: ...`. On the restart, you are on the map where you closed it.
- **Evidence:** those lines; the map before and after.
- **Signs off:** #233 *keeps a save across a restart*; #240.

### L4. Quit from the settings menu, then restart

- **Steps:** on a field map, gain some meso or EXP and note the values. Settings → Quit, confirm. Start MapleThief again and continue.
- **Expect:** back in the Homebrew Menu with no error screen. The log has `settings: menu: quit`, an `autosave:` line and `applicationWillTerminate:`. After the restart the character has the noted values, on the same map.
- **Evidence:** the log lines; the values before and after.
- **Signs off:** #233 *keeps a save across a restart*; #240.

### L5. CPU boost

- **Steps:** none extra; read any run's log from M or L.
- **Expect:** a `switch: CPU boost on (a long load)` / `switch: CPU boost off` pair at the start and one per map load, none while walking. Not `switch: no CPU boost (appletSetCpuBoostMode: 0x...)`.
- **Evidence:** the `switch: CPU boost` lines (or the refusal's code, and the run mode).
- **Signs off:** #240.

## P. Thirty minutes of play

### P1. Thirty minutes, with sound

- **Steps:** title takeover, sound on, default settings. Play at least 30 minutes: fight, pick up drops, cross several map changes (Henesys and the fields around it), open the game's menus and a shop. Press HOME once halfway and come back. Dock or undock once if you can. Then quit from the settings menu.
- **Expect:** no crash, no freeze, no wrong picture. Steady music and effects, without crackle or gaps at map changes. In the log: `audio: driver ..., device ...: 48000 Hz, 2 channels, ...`; `game: audio: N frames rendered in M calls (longest X us), K late device callbacks` with **K = 0**; `game: audio: S suspensions of the process (T s), not counted as late` (about 1, for the HOME press); no `unimplemented report:` line.
- **Evidence:** the whole player log of the run; whether you heard a crackle or a gap, and where.
- **Signs off:** #245 (30 minutes of play); #240 (audio over 30 minutes); #233 *Hardware run* as a whole.

## G. The settings pages

Signs off [#241](https://github.com/armaatus/maplethief-port/issues/241)'s hardware criterion ([ADR 0142](../adr/0142-platform-capabilities-and-the-switch-frontend.md)). The rows the Switch hides are listed in [Settings on the Switch](../play/settings.md#on-the-nintendo-switch).

### G1. Setup

- **Steps:** console off, on the SD card: make `sdmc:/switch/maplethief/texture-packs/Test/` (empty is enough) and put any PNG of your own (not a game image) at `sdmc:/switch/maplethief/borders/test.png`. Copy your exported save zip into `sdmc:/switch/maplethief/data/import/`.
- **Expect:** the folders are there.
- **Evidence:** none; tick it.
- **Signs off:** setup for G2 to G5.

### G2. Every row, handheld and docked

- **Steps:** handheld, open the settings menu from the title screen. On each tab (Video, Audio, Controls, Gameplay, Assists, Saves, Other, and the preset row on top), go down every row with the d-pad: Right or Left changes a setting; A runs an action or asks (Right for Yes, A). Put back anything you changed. Then do the same docked.
- **Expect:** every row changes or runs; none does nothing or says it is *not available*. These rows are **absent**: Fullscreen, Fullscreen mode, Fullscreen display (Video); Output device (Audio); Right mouse button, Touch key pad layout (Controls); Save folder, Data location, Open data folder, Open logs folder (Saves); Check for new versions, Port update (Other).
- **Evidence:** pass, or each dead or unexpected row by tab and name; handheld and docked.
- **Signs off:** #241.

### G3. Packs and borders, and export

- **Steps:** Video → Texture pack: Right. Video → Border image: Right. Put them back to none. Saves → Export saves….
- **Expect:** the texture pack row offers `Test`, then `none`; the border image row offers `test.png`. Export's status line says `Exported to exports/MapleThief-saves-<time>.zip`, and the file is in `sdmc:/switch/maplethief/data/exports/` afterwards.
- **Evidence:** the status line; the file's name on the card.
- **Signs off:** #241.

### G4. Import a desktop save

- **Steps:** from the title screen, Saves → Import saves…. Choose your zip (from `import/`), Right for Yes, A. Close the menu and load the slot.
- **Expect:** the list shows your zip and G3's export, newest first. The status line says `Imported N files from ...`. The slot is your desktop character.
- **Evidence:** the status line; pass or fail.
- **Signs off:** #241; #233 *keeps ... its saves* (saves move between the Switch and a computer).

### G5. Backups

- **Steps:** Saves → Backups.
- **Expect:** the list shows the slots and the import's *Replaced* row.
- **Evidence:** pass or fail.
- **Signs off:** #241.

## F. The performance log

Signs off [#244](https://github.com/armaatus/maplethief-port/issues/244)'s hardware criterion and #233 *Pacing*. Details: [Switch performance](switch-performance.md).

### F1. The logged session, handheld and docked

- **Steps:**
  1. Stock clocks: no overclocking tool running. Title takeover. A character that can reach a busy map.
  2. On the console: Homebrew Menu, Y (NetLoader). On the Mac:
     ```sh
     export MAPLETHIEF_SWITCH_IP=<the address the NetLoader shows>
     scripts/switch-perf
     ```
     It sends `build/switch-release/MapleThief.nro` with `--perf-session --fast-forward x2 --gfx-gpu-timing`. Change no setting in the game.
  3. **Handheld:** load the character and play on a map for at least 5½ minutes. Avoid HOME and sleep.
  4. **Fast-forward:** on a map, hold R3 (the right stick's click) for about 20 s.
  5. **Busiest scene:** the busiest map you know, fighting, for a minute.
  6. **Docked:** put the console in the dock and play on a map for at least 5½ minutes. Hold R3 for another 20 s.
  7. `+`, Quit. The judge runs on the log.
- **Expect:** `perfsession: verdict: PASS`: for handheld and docked, at least 300 s on a map, 20.00 ticks a second and at least 99 % of frames held 3 vblanks. Fast-forward close to 40 ticks a second.
- **Evidence:** every `perfsession:` line and the `gfx: GPU time per frame: ...` line, pasted; the log `build/test-output/switch/MapleThief-perf-<time>.log` attached. If nxlink dropped out: copy the player log from `sdmc:/switch/maplethief/data/logs/` and run `scripts/switch-perf --judge <that file>`. Also fill in the table under *To be measured on the console* in [switch-performance.md](switch-performance.md), or ask the agent to.
- **Signs off:** #233 *Pacing*; #244. A FAIL goes to [#279](https://github.com/armaatus/maplethief-port/issues/279) (profile and tune), with the verdict's lines.

### F2. The software renderer, handheld (optional)

- **Steps:** `scripts/switch-perf --displays handheld -- --gfx-backend software`; 2 minutes on a map, then Quit.
- **Expect:** probably a FAIL; it shows whether the software rasterizer can keep up, which is the estimate behind the `gpu` default.
- **Evidence:** the `perfsession:` lines.
- **Signs off:** #244 (the default's evidence); optional.

## Extra runs from the children (optional, same session)

These checks stay on their own issues, with the steps in the issue's hand-off comment. They fit in the same session; report them there.

| Issue | What | Command |
|---|---|---|
| [#236](https://github.com/armaatus/maplethief-port/issues/236) | the runtime's test NRO in applet mode and title takeover, and the forced guest fault | `scripts/switch-test --ip <IP>`, `scripts/switch-test --ip <IP> --crash-test` |
| [#242](https://github.com/armaatus/maplethief-port/issues/242) | the GPU picture against the software one, every filter, the GPU time per frame | `NRO=build/switch-player/MapleThief.nro`, then `scripts/switch-run "$NRO" -- --gfx-prototype --gfx-backend gpu --smoke-frames 120`, and the rest of its comment |
| [#235](https://github.com/armaatus/maplethief-port/issues/235) | the hello NRO's Homebrew Menu entry and an nxlink run | not covered by I3 and F1, which run the game: see [the Switch hand-offs](handoffs/switch.md#235-the-hello-nro-on-the-console) |

## How to report

1. Copy the tick sheet below into a new comment on [#245](https://github.com/armaatus/maplethief-port/issues/245).
2. Tick each row that passed. For a row that failed, leave it unticked and write what happened after it; the agent opens a follow-up issue and links it there. Skip a row that doesn't apply (a Lite has no dock) with *n/a* and the reason.
3. Paste the short evidence (log lines, status lines, the `perfsession:` lines) under the row, and attach the logs to the comment: drag the `.log` files into it (rename to `.txt` if GitHub refuses one). Name them by row, for example `I4-I5-import.log`, `C-controllers.log`, `L-lifecycle.log`, `P1-30min.log`, `F1-perf.log`. Edit out paths of your own if you like; the player log already writes the data and game folders as `<data>` and `<game folder>`.
4. No screenshots, photos or video of the game's picture: describe it in words. A photo of a system error screen (no game art) is fine.
5. Note the console model, its system version and the Homebrew Menu's version, and which zip you used (its version, or the release it came from).

```markdown
Console: <model>, system <version>, Homebrew Menu <version>. Zip: MapleThief-<version>-switch.zip (<built locally | release>). Renderer: <gpu | software>.

**I. Install and import**
- [ ] I1 zip contents, guard (player mode), NRO check
- [ ] I2 fresh folder from the zip
- [ ] I3 Homebrew Menu entry; wrong file refused
- [ ] I4 import, Keep/Delete, IPA deleted (took … s)
- [ ] I5 first map; no `unimplemented report:` line
- [ ] I6 second start straight to the game

**M. Run modes**
- [ ] M1 applet mode (peak: …, fits / does not fit)
- [ ] M2 title takeover (peak: …)

**C. Controllers and touch**
- [ ] C1 SDL's report (8 pads, pad 1 handheld)
- [ ] C2 Joy-Con attached
- [ ] C3 detached pair
- [ ] C4 one Joy-Con on its side (left, right)
- [ ] C5 Pro Controller
- [ ] C6 touch only
- [ ] C7 glyphs and prompts
- [ ] C8 remap survives a restart; rumble on each controller

**D. Handheld and docked**
- [ ] D1 handheld picture
- [ ] D2 docked picture
- [ ] D3 dock change mid-game (both ways, and during a load)

**L. HOME, sleep, quit and restart**
- [ ] L1 HOME and back
- [ ] L2 sleep 60 s and resume
- [ ] L3 closed from HOME; restart on the same map
- [ ] L4 settings Quit; restart keeps the save
- [ ] L5 CPU boost lines

**P. Thirty minutes**
- [ ] P1 30 minutes with sound (K = …, crackle: none / …)

**G. Settings pages**
- [ ] G1 setup
- [ ] G2 every row, handheld and docked; hidden rows absent
- [ ] G3 packs, border, export
- [ ] G4 import a desktop save
- [ ] G5 backups

**F. Performance**
- [ ] F1 switch-perf: handheld …, docked …, verdict …
- [ ] F2 software renderer (optional)
```

## The epic's exit criteria

Each exit criterion of [#233](https://github.com/armaatus/maplethief-port/issues/233), and where its evidence comes from. The agent-side criteria are checked in the epic's pull request; the hardware ones by the rows above.

| Exit criterion | Evidence |
|---|---|
| **Build.** `cmake --workflow --preset switch-release` builds `MapleThief.nro` with the generated code in the pinned devkitA64 container; the `switch` job of `scripts/local-ci` and of CI is green, without the IPA | Agent: the `switch` job on the epic PR's CI and `scripts/local-ci --only switch`; `scripts/build-switch --gen work/gen` locally (the builds of *Before you start*) |
| **Hardware run:** starts from the Homebrew Menu, in applet mode and in title takeover | I3, M1, M2 |
| **Hardware run:** imports the player's IPA from the SD card and reaches the first map | I2, I4, I5, I6 |
| **Hardware run:** playable with Joy-Con (attached and detached), a Pro Controller and the touch screen | C1 to C8 (C2 attached, C3 and C4 detached, C5 Pro Controller, C6 touch) |
| **Hardware run:** shows the right picture handheld and docked | D1, D2, D3 |
| **Hardware run:** keeps the game and its saves through HOME, sleep and resume | L1, L2, P1 |
| **Hardware run:** keeps a save across a restart | L3, L4, C8, I6 |
| **Pacing.** 20 ticks a second on a map, ≥ 99 % of frames held 3 vblanks, handheld and docked, default settings; ADR 0071's telemetry over nxlink | F1 (`perfsession: verdict: PASS` on both screens) |
| **No assets in the release.** The player build's NRO carries no file of the IPA's bundle; the guard's player mode passes | I1; agent: `platform.switch_guard` in the `switch-player` test preset and `tests/test_release.py` |
| **Docs agree.** An accepted ADR, ARCHITECTURE, ROADMAP, CLAUDE.md's platform targets, THIRD_PARTY.md and `docs/play/switch.md` say the same | Agent: the epic PR's review of those files |
| Every child is closed, split or `needs-human` | Agent: the epic PR's list of children |

Rows outside the exit criteria still sign off their child: G1 to G5 (#241), L5 (#240), C7 (#239), F2 (#244).
