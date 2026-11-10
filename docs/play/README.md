# Playing MapleThief

This guide is for anyone who wants to play. You don't need to know anything about programming.

MapleThief is MapleStory Thief Edition, the 2010 iPhone game, running natively on your computer. The game itself is unchanged. The port adds things the iPhone never had: a big window or fullscreen, keyboard and controller support, sharper graphics and save backups.

## What you need

1. **Your own copy of the game**: the file `MapleThief-1.5.ipa`. We can't give it to you. [Your game file](your-ipa.md) explains which file it has to be and how to check it.
2. **A computer** with one of these:

   | System | Minimum |
   |---|---|
   | Windows | Windows 10 or 11, 64-bit |
   | Mac | macOS 13 Ventura or later (Apple silicon or Intel) |
   | Linux | x86_64 with glibc 2.39 or later (Ubuntu 24.04, Fedora 40, current SteamOS) |
   | Steam Deck | Yes, as a non-Steam game ([Linux and Steam Deck](linux-and-steam-deck.md)) |
| iPhone | iOS 16 or later, built yourself on a Mac with Xcode ([Playing on your iPhone](iphone.md)), not checked on a device yet |

## Download and play

1. **Check your game file.** It must be `MapleThief-1.5.ipa`, 17,872,861 bytes, with this SHA-256:

   `a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387`

   To see your file's SHA-256, open a terminal in its folder and run `Get-FileHash MapleThief-1.5.ipa` (Windows PowerShell), `shasum -a 256 MapleThief-1.5.ipa` (Mac) or `sha256sum MapleThief-1.5.ipa` (Linux). [Your game file](your-ipa.md) says what to do if it doesn't match.
2. **Download** the file for your system from the [Releases page](https://github.com/armaatus/maplethief-port/releases), under *Assets*:

   | System | File |
   |---|---|
   | Windows | `MapleThief-<version>-windows-x64.zip` |
   | Mac | `MapleThief-<version>-macos-universal.zip` |
   | Linux, Steam Deck | `MapleThief-<version>-linux-x86_64.tar.gz` |

   The `MapleThief-kit-…` files are for developers; you don't need them.
3. **Start it**, the first time as described for your system below.
4. **Import your game file.** On the first start the game shows an import screen. Choose your `MapleThief-1.5.ipa` in the file dialog, or drag the file onto the window. The game checks the file and copies the game's files into `game/`, next to itself. You only do this once. If the game refuses the file, it tells you why: not an IPA, another version, or an encrypted copy.

The download contains the program, never the game's art, music or text. Those come from your own copy.

Your saves and settings go into a `data` folder next to the game, which the game creates on the first start. So unpack the download somewhere you can write, such as Documents or a `Games` folder, and keep the folder together: copy `data` to back up, move the whole folder to move the game with its saves. [Saves](saves.md#where-your-saves-are) has the details, and how to keep them in your user folder instead.

To import again later (another copy, or after deleting the `game/` folder), start the game once with `--import-ipa` and the file's path, for example `MapleThief.exe --import-ipa C:\Games\MapleThief-1.5.ipa`. Your saves and settings are kept.

### Windows

1. Right-click the zip, choose **Extract All…**, and pick a folder, for example `Documents\MapleThief`. Not `C:\Program Files`: the game can't write its `data` folder there.
2. Double-click **MapleThief.exe**.
3. If Windows shows "Windows protected your PC" (SmartScreen), click **More info**, then **Run anyway**. Windows asks only once. The port isn't signed with a paid certificate, so SmartScreen doesn't know it yet.

### Mac

1. Double-click the zip in Finder. It unpacks to a folder with **MapleThief.app** in it. Move the whole folder where you want to keep the game, for example into Documents. Keep the app in its folder: in `Applications` on its own it keeps its data in your user folder instead.
2. Clear the download's quarantine, so macOS runs the app from this folder rather than from a temporary read-only copy ("App Translocation", where the game can't keep `data` next to itself). In Terminal, type `xattr -dr com.apple.quarantine ` and drag the folder onto the window, then press Return. Without Terminal: in the Finder, drag `MapleThief.app` out of its folder (to the Desktop, say) and back in. Skip this and the game still runs, but keeps your data in your user folder until you do it ([Troubleshooting](troubleshooting.md#the-game-keeps-its-data-in-the-user-folder)).
3. Open the app. If you used Terminal in step 2, it simply opens. Otherwise, the first time, macOS blocks it, because the port isn't notarised by Apple (that needs a paid developer account):
   - **macOS 15 Sequoia and later:** close the alert with *Done*. Open System Settings → Privacy & Security, scroll down to *Security*, click **Open Anyway** next to "'MapleThief' was blocked…", then confirm with *Open* and your password.
   - **macOS 13 Ventura and 14 Sonoma:** right-click (or Control-click) the app, choose **Open**, then *Open* again in the dialog.

   After that it opens normally. More in [Troubleshooting](troubleshooting.md#mac-apple-could-not-verify-maplethief-is-free-of-malware).

The app runs natively on Apple silicon and on Intel Macs.

### Linux and Steam Deck

1. Unpack the tarball, for example into `~/Games`: `tar -xzf MapleThief-<version>-linux-x86_64.tar.gz -C ~/Games`.
2. Run `./MapleThief.sh` in the unpacked folder.
3. To add it to your application menu, and through that to Steam's "Add a Non-Steam Game" list, run `./MapleThief.sh --install-desktop` once.

It needs glibc 2.39 or later (Ubuntu 24.04, Fedora 40, current SteamOS). The Steam Deck steps and controller layout are in [Linux and Steam Deck](linux-and-steam-deck.md).

## Build it yourself

If you want to change the port, or there's no download for your system, you can build it from source. This takes about 15 minutes the first time, mostly installing tools. You run a handful of commands; the [build guide](../BUILDING.md) walks you through each one for Windows, Mac and Linux. In short:

1. Install the tools listed for your system.
2. Download this project and put your `MapleThief-1.5.ipa` in its `original/` folder.
3. Run `just build`.

What it makes is yours to play on your own computers. **Don't share it**: it contains the game's files.

## The first start

Before the game starts, the port asks how you want to play:

- **Original**: the iPhone game as it was, with the on-screen touch buttons always shown.
- **Original + controller**: the same game, made comfortable for a keyboard or gamepad. The touch buttons hide while you use them.
- **Enhanced**: every improvement that keeps the gameplay the same: sharper text, music that loops without gaps, save on quit, and more.

Choose with the arrow keys and Enter, a gamepad, or the mouse. You can change it later in the settings menu (**F1**).

## Next

- [Controls](controls.md): keyboard, mouse, gamepad, Steam Deck
- [Settings](settings.md): the settings menu, display, sound, graphics and gameplay options
- [Saves](saves.md): where they are, backups, moving them between computers, bringing your iPhone save over
- [Troubleshooting](troubleshooting.md): when something doesn't work, and common questions
