# Android phone hand-offs

The hand-offs that need a real Android phone. The Android epic ([#292](https://github.com/armaatus/maplethief-port/issues/292), [ADR 0150](../../adr/0150-android-target.md)) runs on the Mac's arm64 emulator, the agent's device; a phone is needed only for what the emulator can't show: real display pacing (#358), a real controller and the vibrator (#360), and the release APK signed with your key (#361). Each section **waits on** its issue's emulator part, which the agent finishes and posts on the issue first. They follow each other: #358, then #360, then #361. [How to report](README.md#how-to-report) says what to post.

## Setup

1. An arm64 phone with Android 8.0 or later, and its USB cable. On the phone: Settings > About > tap the build number seven times, then Developer options > **USB debugging** on.
2. On the Mac: `scripts/android-sdk install` (the pinned SDK with `adb`, in `build/_android-sdk/`). Connect the phone and allow the Mac: `build/_android-sdk/platform-tools/adb devices` lists it as `device`. Below, `adb` means that path.
3. Your copy of the game at `original/MapleThief-1.5.ipa`, then `scripts/bootstrap && python3 -m recomp all`.
4. The APK, from `dev` once the issue's emulator part is merged: `cmake --workflow --preset android-release`, which makes `build/android-release/MapleThief.apk`. Install it with `adb install -r build/android-release/MapleThief.apk`.
5. For #360: a Bluetooth or USB controller (an Xbox or PlayStation pad), and someone to call the phone.

## #358: The first map on a real phone

Closes [#358](https://github.com/armaatus/maplethief-port/issues/358)'s phone criterion: real display pacing. Until the import of #359 exists, the agent's notes on #358 say how the game's files reach the phone (pushed with `adb`).

1. Start MapleThief on the phone, reach the title, the main menu, character creation and the first map.
2. The five-minute map run with `--perf-session`, by the one command the agent names on #358.
3. `adb logcat -d > logcat.txt`, and the run's log from the app's storage as the agent's notes say.

**Post back:** the phone's model, Android version and display refresh rate, the `game: ticks:` and `game: clock:` lines, and `logcat.txt` attached.

## #360: A controller and the vibrator

Closes [#360](https://github.com/armaatus/maplethief-port/issues/360)'s phone criterion. The emulator covers touch, home, back, a call and the app being killed; the phone covers what it can't.

1. Pair the controller. On the title, menus work with it, and on the field the key pad fades on the first press.
2. Play the first map and a shop with the controller only.
3. Take damage with the game's vibration option on: the phone vibrates.
4. A real call in the middle of a map, accepted and hung up: the game comes back where it was, with sound.

**Post back:** pass or fail per step, the controller's model, and the phone's model and Android version.

## #361: The signed release APK

Closes [#361](https://github.com/armaatus/maplethief-port/issues/361)'s human part: your release key and the real-phone half of the Android checklist the agent writes. The key is yours: never in the repository or CI.

1. Once: generate the release key as #361 says (`keytool`, RSA 4096, `CN=MapleThief port`), kept outside the repository with a backup.
2. Build and sign the release APK with `scripts/release` as the agent's notes on #361 say.
3. Uninstall the debug APK, then install the signed one from the release page on the phone itself, as the player docs say.
4. Walk the checklist's real-phone half: import, the first map, touch, a controller, background and a restart.

**Post back:** the checklist's tick sheet for the phone half, the phone's model and Android version. Never post the key or its password.
