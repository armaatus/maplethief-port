## What's new since 0.1.0

- **Your saves and settings sit in one `data` folder next to the game.** Move or back up the game folder and everything comes along. Saves from 0.1.0 are copied in on the first start; the old ones stay where they were. If you'd rather keep them in your user account's folder, the settings menu → Saves → **Data location** switches ([Saves](https://github.com/armaatus/maplethief-port/blob/main/docs/play/saves.md)).
- **Nintendo Switch.** A homebrew build for a Switch running custom firmware, as a preview ([Playing on the Nintendo Switch](https://github.com/armaatus/maplethief-port/blob/main/docs/play/switch.md)).
- **iPhone.** You can build the game as an iPhone app on a Mac with Xcode and install it on your own phone; there is no download for iOS ([Playing on your iPhone](https://github.com/armaatus/maplethief-port/blob/main/docs/play/iphone.md)).
- **Hear about new versions**, if you want to: the first launch asks once, and the game then checks GitHub once a day and shows one line on the title screen when a new version is out. It downloads nothing and is off unless you turn it on ([Settings](https://github.com/armaatus/maplethief-port/blob/main/docs/play/settings.md)).
- **After a crash** a dialog names the crash report to attach to a bug report, and the next start offers safe settings for one run ([Troubleshooting](https://github.com/armaatus/maplethief-port/blob/main/docs/play/troubleshooting.md#a-crash-or-a-bug-to-report)).
- **Play without touch.** Everything the touch screen does now works with a mouse, a keyboard or a gamepad, and you can switch between them at any time ([Controls](https://github.com/armaatus/maplethief-port/blob/main/docs/play/controls.md)).

**Going back to an older version?** Versions before this one don't read the `data` folder, so they won't see what you saved since. Your saves from before the update are still where that version keeps them.

## Downloads

**To play**, download the file for your system:

| System | File |
|---|---|
| Windows 10/11 (64-bit) | `MapleThief-<version>-windows-x64.zip` |
| macOS 13+ (Apple silicon and Intel) | `MapleThief-<version>-macos-universal.zip` |
| Linux x86_64, Steam Deck (glibc 2.39+) | `MapleThief-<version>-linux-x86_64.tar.gz` |
| Nintendo Switch, homebrew (a preview) | `MapleThief-<version>-switch.zip` |

These contain the program only: none of the game's art, music, maps or text. **You need your own copy of the game**, `MapleThief-1.5.ipa` (version 1.5, decrypted). On first launch the game asks for it, checks it and copies the game's files into a `game` folder next to itself. We can't provide the file. [Playing MapleThief](https://github.com/armaatus/maplethief-port/blob/main/docs/play/README.md) has the steps for each system and explains how to check your copy.

**To build or change the port**, the `MapleThief-kit-<version>-<os>-<arch>` files hold our own code only: the platform shell without the game, the runtime and SDL3 libraries, the recompiler, the tools and the docs ([Build from source](https://github.com/armaatus/maplethief-port/blob/main/docs/build/README.md)).

`SHA256SUMS` lists every file's SHA-256: `sha256sum -c SHA256SUMS` (Linux) or `shasum -a 256 -c SHA256SUMS` (Mac).

MapleThief is an unofficial fan project, not affiliated with or endorsed by Nexon. MapleStory is a trademark of Nexon. [What's included, and what isn't](https://github.com/armaatus/maplethief-port/blob/main/docs/legal.md).
