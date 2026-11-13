# iPhone hand-offs

The hand-offs that need an iPhone: the port on a modern iPhone (#73, #128, #194, #197), and the original 2010 game on an old 32-bit device (#149, #60, #118). One iOS build covers the first group; every setting they switch is in the settings menu, so nothing is rebuilt in between. [How to report](README.md#how-to-report) says what to post.

The baseline is done: #83 (closed 2026-11-10) built the app with Xcode and reached the first map by touch on an iPhone 12 with iOS 26.6.1. That build already held the code of #128, #194 and #197.

## Setup

**Once, on the Mac** (already done for #83; [Playing on your iPhone](../../play/iphone.md) has the details):

1. Xcode with the iOS component, selected with `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`, and your Apple ID in Xcode > Settings > Accounts. Note your team id.
2. `just doctor` shows `ok Xcode with the iOS SDK`.
3. Your copy of the game at `original/MapleThief-1.5.ipa` ([Your game file](../../play/your-ipa.md)).
4. On the iPhone: Developer Mode on and the Mac trusted. With a free account, also trust the developer under VPN & Device Management; such a build expires after 7 days.

**For the modern iPhone:** an Xbox controller and a PS controller (DualShock 4 or DualSense) paired over Bluetooth, and someone to call the phone.

**For the old device:** a 32-bit iPhone or iPod touch (iOS 3.1.2 to 10.3.3) with the game 1.5 installed and the system language English, its cable, and Terminal allowed Full Disk Access (or copy the backup to the Desktop).

**The build** (from the epic branch `epic-106-itch-handoffs`, or `dev` once it is merged):

```sh
MAPLETHIEF_IOS_TEAM=<team id> scripts/build ios 2>&1 | tee build/ios-build.log   # dist/ios/MapleThief.app
open build/ios-app/MapleThief.xcodeproj
```

In Xcode: the MapleThief scheme, your iPhone as the destination, Signing & Capabilities with *Automatically manage signing* and your team, then Run. Keep Xcode's console open for the whole session: #128, #194 and #197 read its lines.

While the phone is new to this build, also note the three checks #83 left out: the icon and launch screen, both landscape orientations with no status bar, and a console with no guest memory fault and no Metal or GPU error.

## #194: Key pad haptics

Closes [#194](https://github.com/armaatus/maplethief-port/issues/194). The compile criterion is met by #83's build (it held the #194 merge); only the warnings and the feel are left. The old comment's "configure CMake with `CMAKE_SYSTEM_NAME=iOS`" is obsolete: `scripts/build ios` does that.

1. Warnings: `grep -n "warning:" build/ios-build.log` should show nothing from `platform/ios/keypad_haptics_ios.mm` or `platform/ios/audio_session_ios.mm`. If the build was incremental, `touch` both files and build again first.
2. Go to the first map. The console shows `input: keypad_haptics light`.
3. Hold the d-pad and slide across its arms, then press jump, attack and the quick slots: one tap per new key or arm, nothing while holding, nothing off the keys.
4. Open a popup (the menu, an NPC, the death popup): no taps.
5. Settings > Controls > **Key pad haptics**: `medium` taps stronger, `off` taps never.
6. The first tap after the pad appears doesn't lag.
7. Quit: the console shows `input: keypad_haptics: N taps, M presses muted, K mask reads`.

**Post back:** the warning grep's result, pass or fail for steps 2 to 6, whether `light` is too weak, `medium` too strong or d-pad slides tap too often, and the line of step 7.

## #197: Audio session

Closes [#197](https://github.com/armaatus/maplethief-port/issues/197). The compile criterion is met by #83's build. Leave Settings > Audio > **Audio session** at `original`.

1. Play music in the Music app, then open the game: both are heard. The console shows `audio: the session's category: ambient (ios_audio = original)`.
2. Ring/Silent switch to silent: the game goes quiet.
3. Get called during play (do this together with #128's call): the sound stops with `audio: interrupted (a call, Siri, an alarm): the output is paused` and comes back after it with `audio: the interruption is over (...)`. Siri and an alarm too, if convenient.
4. Last of the session: set **Audio session** to `own` and restart the game. It plays with the switch on silent, and other apps' music is turned down rather than stopped.

**Post back:** pass or fail for steps 1 to 3 with the log lines, the console around the call if the sound didn't come back, and whether you prefer `own` to stop other apps' music instead.

## #128: App lifecycle

Closes [#128](https://github.com/armaatus/maplethief-port/issues/128) (criterion 3, the iPhone; the desktop part merged in #135). Get onto a map with music playing, then:

1. Home gesture, wait about 10 s, reopen.
2. Control Center, about 5 s, dismiss.
3. A call: decline it once; accept it once, talk about 10 s, hang up.
4. Bonus: hold in the app switcher and come back; kill the app from the switcher once.

**Expect:** on leaving, `lifecycle: applicationWillResignActive: at <t> s (SDL_EVENT_WILL_ENTER_BACKGROUND)` and the game's suspend line. For step 1 and the accepted call also `lifecycle: hidden at <t> s: nothing is presented, the guest waits ...`; for Control Center and the declined call no hidden line. On return, `lifecycle: applicationDidBecomeActive: at <t> s (SDL_EVENT_DID_ENTER_FOREGROUND)`, the game's resume line, the music back and the controls working. One resign and one become-active per step, no crash, no Metal or background GPU error, no watchdog kill. On the kill the terminate line may be missing ([ADR 0063](../../adr/0063-app-lifecycle-background-and-minimise.md) allows that).

**Post back:** pass or fail per step, the console lines around any failure, and whether the resign line only showed up after coming back.

## #73: Controllers, touch and the safe area

Closes [#73](https://github.com/armaatus/maplethief-port/issues/73). An iPhone 12 has a notch, not a Dynamic Island; read "notch" for the issue's "island".

1. Touch, no controller: main menu to character select to the first map by touch; hold the d-pad and attack with another finger at once; a tap in the black bars does nothing; the key pad shows.
2. Xbox controller: A is OK and B back in menus; stick or d-pad, A jump, X attack and the quick slots on the field as in [Controls](../../play/controls.md); the key pad fades on the first press and comes back on a touch; turning the pad off mid-walk stops the hero; it rumbles on damage.
3. PS controller: the same four checks.
4. Both orientations: no part of the picture is under the notch or the home indicator (only the bars reach there), the picture is centred or moved only as far as needed, and the game never turns to portrait. Repeat with Settings > Video > **Scale mode** = `fit`.
5. Rotation: walk with a finger on the d-pad and flip the phone: the hero stops, nothing is pressed at the mirrored point and the next touch works at once. Walk with a controller and flip: the walk goes on.

**Post back:** yes or no per step, with the iPhone model and iOS version on any no. No screenshots.

## #149: Import saves from a device backup

Closes [#149](https://github.com/armaatus/maplethief-port/issues/149). Needs the old device with the game and its saves; do it before #60, whose fresh install deletes them. [iPhone saves](../../play/iphone-saves.md) is the player page for the same steps (the issue's old docs path is now that page).

1. Finder > the device > General: *Back up all of the data to this Mac*, **Encrypt local backup** off, Back Up Now.
2. `scripts/build mac` (for step 6), from the repo root.
3. `PYTHONPATH=tools python3 -m mtport.iosbackup find`: note the backup folder and its manifest.
4. `PYTHONPATH=tools python3 -m mtport.iosbackup <backup>`: `theif<n>`, `quest<n>`, `config` and `cupon` are `ok`, the rest `ignored:`. Exit status 3 means a failed check.
5. `PYTHONPATH=tools python3 -m mtport.iosbackup extract <backup> /tmp/device-saves` (a folder that doesn't exist yet), then `MAPLETHIEF_SAVE_DIR=/tmp/device-saves scripts/test --python-only -- tests/test_savedata_real.py -k pulled` and `PYTHONPATH=tools python3 -m mtport.savedata dump /tmp/device-saves/*`.
6. `MAPLETHIEF_DATA_DIR=/tmp/mt-ios PYTHONPATH=tools python3 -m mtport.iosbackup import <backup> --program dist/mac/MapleThief.app`, then start `MAPLETHIEF_DATA_DIR=/tmp/mt-ios dist/mac/MapleThief.app/Contents/MacOS/MapleThief` (not `open`, which drops the variable). The slot screen shows the device's characters and one loads into its map.

**Post back:** the device's iOS version, the manifest (`Manifest.db` or `Manifest.mbdb`), the output of steps 4 to 6 and anything unexpected. Never attach the backup or the save files.

## #60: Golden images from the device

Closes [#60](https://github.com/armaatus/maplethief-port/issues/60) (the capture half of criterion 1; the agent tunes the tolerances afterwards). The issue's latest comment (2026-10-29) asks for **seven** screens, not six. [tests/golden/README.md](../../../tests/golden/README.md) ("Capturing on the device") has the taps and timings; `tests/golden/device_screens.json` lists the names.

1. A fresh install of 1.5 (or every character deleted and the options at their defaults), language English.
2. In one session, Home + Sleep/Wake for each: `title`, `main_menu`, `intro_dialog`, `npc_dialog`, `first_map`, `popup`, `character_select`.
3. Copy them as unmodified PNG (Image Capture, Photos' *Export Unmodified Original*, or AirDrop): not JPEG or HEIC, not resized. Save them as `work/golden/device/<name>.png` in this checkout. They stay in git-ignored `work/`.
4. `scripts/build mac debug`, then `ctest --preset mac-debug -R platform.device_golden --output-on-failure`.

**Post back:** the device model and iOS version, whether it was a fresh install, and the test's per-screen output as text. No images.

## #118: Font calibration references

Closes [#118](https://github.com/armaatus/maplethief-port/issues/118) once the agent has compared and you have signed off. The issue has no prepared steps; this is the capture its body asks for, on the old device right after #60.

1. Screenshots of the text-heavy screens: title and main menu, an NPC dialogue, the inventory and an item description, the shop, a popup, and the terms of service. Then set the device language to Korean and capture the NPC dialogue and the terms of service again.
2. Keep them in git-ignored `work/golden/text/`, as unmodified PNG.
3. Later, when the agent has compared the bundled font against them: judge the look in the port and sign off, or say what to change.

**Post back:** that the references exist, the device and iOS version, and which screens are missing. No images in git or in the issue.
