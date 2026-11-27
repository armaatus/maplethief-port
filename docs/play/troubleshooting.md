# Troubleshooting and questions

## The game won't start

### Mac: "Apple could not verify 'MapleThief' is free of malware"

The port isn't notarised by Apple (that needs a paid developer account), so macOS blocks it the first time it was downloaded. To allow it:

- **macOS 15 Sequoia and later:** open the app once and close the alert with *Done*. Open System Settings → Privacy & Security, scroll down to *Security*, click **Open Anyway** next to "'MapleThief' was blocked…", then confirm with *Open* and your password.
- **macOS 13 Ventura and 14 Sonoma:** right-click (or Control-click) the app, choose **Open**, then *Open* again in the dialog.
- **Terminal, any version:** `xattr -dr com.apple.quarantine` followed by the unpacked folder (drag it onto the Terminal window), for example `xattr -dr com.apple.quarantine ~/Downloads/MapleThief-1.0.0-macos-universal`. Only do this for a copy you trust. It also lets the game keep its data next to itself ([below](#the-game-keeps-its-data-in-the-user-folder)).

An app you built yourself isn't affected.

### Windows: "Windows protected your PC"

Windows SmartScreen warns about programs it hasn't seen often. Click **More info**, then **Run anyway**.

### The import screen says "THAT FILE CANNOT BE USED"

The game checked the file you chose and refused it. The line below the title says why: "This file is not an IPA", "This is the IPA of another app", "This is MapleThief 1.x; the port needs version 1.5", "This IPA's game executable is encrypted", or "This is MapleThief 1.5, but not the known MapleThief-1.5.ipa", with the file's size and the start of its SHA-256. Nothing is kept from a refused file. Choose another file, or drop one on the window. [Your game file](your-ipa.md#if-it-doesnt-match) says what each case means.

When you build it yourself, the build checks the file in `original/` instead, and says what is wrong in its own words ([Your game file](your-ipa.md)).

### Linux: "version `GLIBC_2.xx' not found"

Your system is older than the build needs (glibc 2.35, e.g. Ubuntu 22.04). Update your distribution or SteamOS, or use the Flatpak bundle, which brings its own runtime. More Linux tips: [Linux and Steam Deck](linux-and-steam-deck.md#if-it-doesnt-start).

## The game keeps its data in the user folder

The game keeps your saves and settings in a `data` folder next to itself ([Saves](saves.md#where-your-saves-are)). When it can't write there, it uses your user folder for the time being (`%APPDATA%\MapleThief`, `~/Library/Application Support/MapleThief` or `~/.local/share/MapleThief`), says so once at the start, and the settings menu's Saves → **Data location** row shows why. Nothing is lost: once the cause is fixed, the next start copies that data next to the game by itself.

- **"Can't write next to the game"**: the game's folder is protected (`C:\Program Files`, `/opt`) or on a read-only disk (a locked SD card, a network share, a mounted disk image). Move the whole folder somewhere you can write, such as Documents or a `Games` folder, and start it from there.
- **Mac, "macOS runs MapleThief from a temporary read-only copy (App Translocation)"**: macOS runs a freshly downloaded app from a hidden read-only copy until its quarantine is gone. In Terminal run `xattr -dr com.apple.quarantine` followed by the unpacked folder (drag it onto the Terminal window). Without Terminal: in the Finder, drag `MapleThief.app` out of its folder (to the Desktop, say) and back in. A move with `mv` in Terminal doesn't count. Then start it again.
- **Mac, "MapleThief.app is in Applications"**: in Applications the game always uses the user folder. That's fine; to have `data` next to the game instead, keep the app in a folder of its own.
- **"Set by MAPLETHIEF_DATA_DIR"**: that environment variable names the folder; unset it to use the game's folder.

`MapleThief --print-data-dir` prints the folder in use and the reason.

## While playing

**The picture is blurry or has black bars.** Change *how the picture fills the window* in the settings menu (Video). `sharp` fills the window and stays crisp; `integer` keeps perfect pixels but may leave bars. See [Settings → Display](settings.md#display).

**It runs slowly.** Lower the render scale or set it to `auto`, or try the GPU renderer (`gfx_backend = gpu`). F3 shows how long each frame takes.

**No sound.** Check the volumes in the settings menu (Audio), and that `mute_unfocused` isn't muting it because the window lost focus. On Linux, try starting it with `SDL_AUDIO_DRIVER=pulseaudio`.

**The controller does nothing.** Press a button on it once: the first controller you press is the one that plays. Steam can take over controllers; on the Deck use Steam's Gamepad layout.

**The music pauses each time it loops.** The original did that too. Turn on gapless music (settings menu → Audio).

**The menus have buttons that do nothing (More Games, the banner).** Those opened Nexon's web pages, which are gone. `hide_online_ui = on` hides them (part of the Original + controller and Enhanced presets).

## On the iPhone

**The app stopped opening after a week.** With a free Apple account the app is signed for 7 days. Build and install it again ([Playing on your iPhone](iphone.md#3-install-it-with-xcode)); your saves stay as long as you don't delete the app.

**"Untrusted Developer" when it starts.** On the iPhone open Settings > General > VPN & Device Management, tap your account under Developer App and choose **Trust**.

**The build or Xcode's signing fails.** [Playing on your iPhone](iphone.md#when-something-goes-wrong) lists each message and its fix.

**No sound.** The Ring/Silent switch silences the game, as it did the original. Turn the switch to ring, or set **Audio session** to `own` in the settings menu's Audio tab and start the game again ([Settings](settings.md#sound)).

**The on-screen buttons disappeared.** They fade while a controller is used. Touch the screen and they come back. If they stay away, check Controls → **Hide the key pad** in the settings menu ([iPhone controls](iphone-controls.md#a-controller)).

**The key pad is in an odd place.** Settings menu → Controls → **Edit the touch key pad**, then **Reset all** and **Save** ([iPhone controls](iphone-controls.md#moving-the-key-pad)).

**The controller does nothing.** Pair it in the iPhone's Settings → Bluetooth first, then press a button on it while the game runs.

**My saves are gone.** Deleting the app deletes its saves. Copy them out with the Files app before you delete it ([Uninstall and reset](uninstall-and-reset.md#iphone)).

**Seeing the log.** Start the game from Xcode with your iPhone connected ([step 3](iphone.md#3-install-it-with-xcode)): the game's log shows in Xcode's debug area at the bottom. Copy it into your report.

## On the Nintendo Switch

**"Running short of memory", or it closes on a map.** You started the Homebrew Menu from the Album. Start it through a game instead: hold R while you start one ([Playing on the Nintendo Switch](switch.md#start)).

**"No IPA found".** The file isn't in `sdmc:/switch/maplethief/`. Copy it there, next to `MapleThief.nro`, and press A to look again.

**"That file cannot be used".** The line below says why: not an IPA, another version, an encrypted copy, or a damaged one. Check its SHA-256 on a computer ([Your game file](your-ipa.md)).

**The import screen comes back on every start.** The import didn't finish, or `game/` was deleted. Let it run to the end with MapleThief open and the console awake.

**The touch screen does nothing.** It works in handheld mode only; docked there is no touch screen.

**The wrong button jumps.** Buttons go by their Nintendo labels: B jumps and A confirms. Settings menu (`+`) → Controls → **Bindings** → **Reset to defaults** brings back the defaults ([Nintendo Switch controls](../controls/switch.md)).

**The settings menu is missing a row.** The console's menu leaves out what doesn't work there, such as fullscreen and the output device ([Settings](settings.md#on-the-nintendo-switch)).

**Seeing the log.** Each run's log is in `sdmc:/switch/maplethief/data/logs/`, and a crash report in `sdmc:/switch/maplethief/crash.txt` and `data/crashes/`. Put the card in a computer to copy them. More in [Playing on the Nintendo Switch](switch.md#troubleshooting).

## Seeing what went wrong

The game writes a log of each run to `logs/` in its data folder: settings menu → Saves → **Open logs folder**. Each file is named by the date and time the run started, so the newest is the latest run. You don't need a terminal for it.

When you [report a problem](https://github.com/armaatus/maplethief-port/issues/new?template=bug.yml), attach that log, and say which system you play on and what you did just before. [Known issues](../KNOWN_ISSUES.md) lists what we already know about.

## Questions

**Is this legal?** The port is our own code, and you play with your own copy of the game, which this project doesn't include or hand out. [What's included, and what isn't](../legal.md) has the details.

**Is this the online MapleStory?** No. Thief Edition is a single-player game made for the iPhone in 2010. Nothing in it connects to a server.

**Can I use my iPhone save?** Yes, if the game is still installed on your device: [Saves from your iPhone](iphone-saves.md).

**Will Cygnus Knights Edition or other versions work?** No. The port is built for Thief Edition 1.5 only.

**Is the game changed?** No. The original game logic runs unchanged. Everything the port adds is optional, and the Original preset turns it all off.

**Can I help?** Yes. See [CONTRIBUTING.md](../../CONTRIBUTING.md).

**My question isn't here.** Ask in [Discussions](https://github.com/armaatus/maplethief-port/discussions/categories/q-a). Its [FAQ](https://github.com/armaatus/maplethief-port/discussions/418) sums up this page.

## A crash or a bug to report

The game writes a log to `logs/` in the data folder (settings menu → Saves → **Open logs folder**) and, after a crash, a report in `crashes/`; a dialog names the file. Attach both to a [bug report](https://github.com/armaatus/maplethief-port/issues/new?template=bug.yml). After a crash the next start offers to use default settings for one run (no texture or audio packs, the software renderer); your settings file is not changed. With **Crash recovery** on, it also offers to resume from a recovery point taken a few minutes before the crash ([Saves](saves.md#after-a-crash-recovery-points)).
