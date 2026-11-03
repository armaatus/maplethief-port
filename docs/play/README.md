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

## Getting the game running

There are two ways. Pick the one that suits you.

### Option A: download and play (easiest)

> **Not available yet.** Ready-to-play downloads are being built ([#182](https://github.com/armaatus/maplethief-port/issues/182)). Until then, use option B. This section describes how it will work.

1. Go to the [Releases page](https://github.com/armaatus/maplethief-port/releases) and download the file for your system.
2. Unzip it anywhere, for example your Desktop or `Applications`.
3. Start **MapleThief**. On a Mac, the first start needs one extra click: see [Troubleshooting → "Apple could not verify"](troubleshooting.md#mac-apple-could-not-verify-maplethief-is-free-of-malware).
4. The first time, the game asks for your `MapleThief-1.5.ipa`. Pick it, and the game checks it and copies what it needs. You only do this once.

The download contains the program, never the game's art, music or text. Those come from your own copy.

### Option B: build it yourself

This takes about 15 minutes the first time, mostly installing tools. You run a handful of commands; [Build from source](../build/README.md) walks you through each one for Windows, Mac and Linux. In short:

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
