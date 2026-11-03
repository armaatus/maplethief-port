# Troubleshooting and questions

## The game won't start

### Mac: "Apple could not verify 'MapleThief' is free of malware"

The port isn't notarised by Apple (that needs a paid developer account), so macOS blocks it the first time it was downloaded. To allow it:

- **macOS 15 Sequoia and later:** open the app once and close the alert with *Done*. Open System Settings → Privacy & Security, scroll down to *Security*, click **Open Anyway** next to "'MapleThief' was blocked…", then confirm with *Open* and your password.
- **macOS 13 Ventura and 14 Sonoma:** right-click (or Control-click) the app, choose **Open**, then *Open* again in the dialog.
- **Terminal, any version:** `xattr -dr com.apple.quarantine MapleThief.app`. Only do this for a copy you trust.

An app you built yourself isn't affected.

### Windows: "Windows protected your PC"

Windows SmartScreen warns about programs it hasn't seen often. Click **More info**, then **Run anyway**.

### "MapleThief-1.5.ipa missing" or "SHA-256 mismatch"

The game can't find your copy of the game, or it's a different file. See [Your game file](your-ipa.md).

### Linux: "version `GLIBC_2.xx' not found"

Your system is older than the build needs (glibc 2.39, e.g. Ubuntu 24.04). Update your distribution or SteamOS. More Linux tips: [Linux and Steam Deck](linux-and-steam-deck.md#if-it-doesnt-start).

## While playing

**The picture is blurry or has black bars.** Change *how the picture fills the window* in the settings menu (Video). `sharp` fills the window and stays crisp; `integer` keeps perfect pixels but may leave bars. See [Settings → Display](settings.md#display).

**It runs slowly.** Lower the render scale or set it to `auto`, or try the GPU renderer (`gfx_backend = gpu`). F3 shows how long each frame takes.

**No sound.** Check the volumes in the settings menu (Audio), and that `mute_unfocused` isn't muting it because the window lost focus. On Linux, try starting it with `SDL_AUDIO_DRIVER=pulseaudio`.

**The controller does nothing.** Press a button on it once: the first controller you press is the one that plays. Steam can take over controllers; on the Deck use Steam's Gamepad layout.

**The music pauses each time it loops.** The original did that too. Turn on gapless music (settings menu → Audio).

**The menus have buttons that do nothing (More Games, the banner).** Those opened Nexon's web pages, which are gone. `hide_online_ui = on` hides them (part of the Original + controller and Enhanced presets).

## Seeing what went wrong

Start the game from a terminal to see its log:

- Windows: open PowerShell in the game's folder and run `.\MapleThief.exe`
- Mac: `MapleThief.app/Contents/MacOS/MapleThief`
- Linux: `./MapleThief.sh`

When you [report a problem](https://github.com/armaatus/maplethief-port/issues/new), include that log, your system, and what you did just before.

## Questions

**Is this legal?** The port is our own code, and you play with your own copy of the game, which this project doesn't include or hand out. [What's included, and what isn't](../legal.md) has the details.

**Is this the online MapleStory?** No. Thief Edition is a single-player game made for the iPhone in 2010. Nothing in it connects to a server.

**Can I use my iPhone save?** Yes, if the game is still installed on your device: [Saves from your iPhone](iphone-saves.md).

**Will Cygnus Knights Edition or other versions work?** No. The port is built for Thief Edition 1.5 only.

**Is the game changed?** No. The original game logic runs unchanged. Everything the port adds is optional, and the Original preset turns it all off.

**Can I help?** Yes. See [CONTRIBUTING.md](../../CONTRIBUTING.md).
