# itch.io page text

The text of the itch.io page `armaatus.itch.io/maplethief` ([ADR 0146](../adr/0146-distribution-on-itch-io.md)). itch.io has no API for project pages, so Joris pastes everything below the line into the page's description in the web editor. The short fields of the form (tagline, genre, tags, platforms) are in [metadata.md](metadata.md).

Before pasting: replace `https://buymeacoffee.com/REPLACE_ME` with the Buy Me a Coffee link. Links are absolute, because the text lives on itch.io. It is our own words only: no game text, and no image in the description; the screenshots are uploaded in the page's screenshot section (gameplay only, from a local build, never committed).

---

## MapleThief

**MapleStory Thief Edition, the 2010 iPhone game, running natively on Windows, macOS and Linux.** The game is unchanged; the port adds a big window or fullscreen, keyboard and controller support, sharper graphics and save backups.

This is an unofficial fan port. It contains none of the game's art, music, maps or text: you play it with **your own copy** of the game.

### You need your own copy

The download is the program. The game itself comes from your copy of MapleStory Thief Edition **version 1.5**, the iPhone app file:

- name: `MapleThief-1.5.ipa`
- size: 17,872,861 bytes
- SHA-256: `a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387`

It has to be a decrypted copy. We can't provide the file, link to one, or explain how to get or decrypt one.

To check your file, open a terminal in its folder and compare the long string it prints with the one above:

- Windows (PowerShell): `Get-FileHash MapleThief-1.5.ipa`
- Mac: `shasum -a 256 MapleThief-1.5.ipa`
- Linux: `sha256sum MapleThief-1.5.ipa`

On the first start the game asks for this file, checks it and copies the game's files from it. You do this once. More in [Your game file](https://github.com/armaatus/maplethief-port/blob/main/docs/play/your-ipa.md).

### Which download

| Your system | Download |
|---|---|
| Windows 10 or 11, 64-bit | `MapleThief-<version>-windows-x64.zip` |
| Mac, macOS 13 or later (Apple silicon or Intel) | `MapleThief-<version>-macos-universal.zip` |
| Linux x86_64 with glibc 2.39 or later, Steam Deck | `MapleThief-<version>-linux-x86_64.tar.gz` |

The itch app installs and updates the right one for you. Each file is the same as on the project's [GitHub Releases page](https://github.com/armaatus/maplethief-port/releases), where `SHA256SUMS` lists its checksum.

### Install

Unpack the download somewhere you can write, such as Documents or a Games folder, and keep its folder together: the game keeps your saves in a `data` folder next to itself.

**Windows**

1. Right-click the zip, choose **Extract All...** and pick a folder (not Program Files).
2. Double-click **MapleThief.exe**.
3. If Windows shows "Windows protected your PC", click **More info**, then **Run anyway**. The port isn't signed with a paid certificate, so SmartScreen asks once.

**Mac**

1. Double-click the zip. Move the folder with **MapleThief.app** where you want to keep the game, for example into Documents. Keep the app in its folder.
2. Open Terminal, type `xattr -dr com.apple.quarantine ` (with the space), drag the folder onto the window and press Return. This lets the game keep its saves next to itself.
3. Open the app. If macOS still blocks it (Gatekeeper: the port isn't notarised by Apple, which needs a paid developer account):
   - macOS 15 and later: close the alert, open System Settings, Privacy & Security, and click **Open Anyway** next to "MapleThief was blocked".
   - macOS 13 and 14: right-click the app, choose **Open**, then **Open** again.

It runs natively on Apple silicon and Intel Macs.

**Linux**

1. Unpack the tarball, for example: `tar -xzf MapleThief-<version>-linux-x86_64.tar.gz -C ~/Games`
2. Run `./MapleThief.sh` in the unpacked folder.
3. To add it to your application menu, run `./MapleThief.sh --install-desktop` once.

**Steam Deck**

In Desktop Mode, do the Linux steps above, including `--install-desktop`. Then in Steam choose **Add a Non-Steam Game to My Library** and tick MapleThief. Steam hands the Deck's controls to the game as an Xbox-style gamepad. Details in [Linux and Steam Deck](https://github.com/armaatus/maplethief-port/blob/main/docs/play/linux-and-steam-deck.md).

### Controls

Mouse or touch works like your finger on the iPhone. Keyboard and gamepad work too, and every key and button can be changed in the settings menu.

| | Keyboard | Gamepad (Xbox names) |
|---|---|---|
| Move | Arrow keys | D-pad or left stick |
| Jump | X or Space | A |
| Attack | Z or Ctrl | X or right trigger |
| Potions (quick slots) | A, S, D, F | Y, B, LB, RB |
| Popup menu | Esc | Start |
| Map | M | Back / View |

The full list: [Controls](https://github.com/armaatus/maplethief-port/blob/main/docs/play/controls.md).

### Known issues

- Text uses an open font instead of Apple's, so it looks a little plainer.
- The cash shop can't take real money. A setting lets it pay in the game's meso instead.
- The download accepts only the exact game file above; a repacked copy is refused. Building from source accepts it.
- It hasn't been checked on a real Steam Deck yet.
- macOS and Windows warn the first time, because the port isn't signed or notarised with paid certificates.

The current list: [Known issues](https://github.com/armaatus/maplethief-port/blob/main/docs/KNOWN_ISSUES.md). Found something else? [Open an issue](https://github.com/armaatus/maplethief-port/issues).

### Credits

- The port (recompiler, runtime, frontends): Joris de Jong and contributors, MIT-licensed, at [github.com/armaatus/maplethief-port](https://github.com/armaatus/maplethief-port).
- Built on SDL3 and other open-source libraries, listed with their licences in [THIRD_PARTY.md](https://github.com/armaatus/maplethief-port/blob/main/THIRD_PARTY.md) and in every download.
- MapleStory Thief Edition was made by Nexon Mobile (2010).

### Support

The download is free. If you enjoy the port, you can leave a tip here, or [buy me a coffee](https://buymeacoffee.com/REPLACE_ME). It is entirely optional.

### Legal

MapleThief is an unofficial fan project, **not affiliated with, endorsed by or connected to Nexon**. MapleStory and MapleStory Thief Edition are trademarks of Nexon. The download contains the program, with the game's code recompiled for your system, and **none of the game's art, music, maps or text**; those come from your own copy. Rights holders with a concern: please [open an issue](https://github.com/armaatus/maplethief-port/issues), and we will respond quickly. More in [What's included, and what isn't](https://github.com/armaatus/maplethief-port/blob/main/docs/legal.md).
