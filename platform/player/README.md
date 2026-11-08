# MapleThief

MapleThief runs MapleStory Thief Edition, the 2010 iPhone game, natively on your computer. It is an unofficial fan project, not affiliated with or endorsed by Nexon. MapleStory is a trademark of Nexon.

This download contains the program only: none of the game's art, music, maps or text. You need **your own copy** of the game, the file `MapleThief-1.5.ipa` (version 1.5, decrypted, 17,872,861 bytes, SHA-256 `a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387`). We can't provide it.

## Start

Put this folder where you want to keep the game, somewhere you can write: your Documents, a `Games` folder, a USB stick. Not `C:\Program Files`. MapleThief keeps your saves and settings in a `data` folder next to itself (see [Your data folder](#your-data-folder)).

- **Windows**: double-click `MapleThief.exe`. If SmartScreen says "Windows protected your PC", click **More info**, then **Run anyway**.
- **Mac**: read [macOS: first start](#macos-first-start) first. Keep `MapleThief.app` in this folder.
- **Linux, Steam Deck**: run `./MapleThief.sh`. `./MapleThief.sh --install-desktop` adds it to the application menu, and through that to Steam's "Add a Non-Steam Game".

## macOS: first start

The app isn't notarised, so macOS treats it with care:

1. **Clear the download's quarantine** so macOS runs the app from this folder. Open Terminal and run, with the path of this folder:

   ```sh
   xattr -dr com.apple.quarantine ~/Downloads/MapleThief-1.0.0-macos-universal
   ```

   (Type `xattr -dr com.apple.quarantine ` and drag this folder onto the Terminal window to fill in its path.) Without Terminal: in the Finder, drag `MapleThief.app` out of this folder (to the Desktop, say) and back in. A move with `mv` in Terminal doesn't count.
2. **Open the app.** After the Terminal command it simply opens. Otherwise the first time macOS blocks it. On macOS 15 and later, open System Settings → Privacy & Security and click **Open Anyway**. On macOS 13 and 14, right-click the app, choose **Open**, then **Open** again.

Without step 1, macOS runs the app from a temporary read-only copy ("App Translocation"), and MapleThief can't see or write the folder around it. It still runs, but keeps your data in `~/Library/Application Support/MapleThief` for the time being and tells you so. Do step 1 and start it again: your data is then copied next to the game by itself.

Don't drag `MapleThief.app` alone into Applications: in Applications MapleThief keeps its data in `~/Library/Application Support/MapleThief`. Move the whole folder instead.

## First launch: your game file

The first time, MapleThief shows an import screen. Pick your `MapleThief-1.5.ipa` with the file dialog or drop it on the window. MapleThief checks the file and copies the game's files into `game/` next to itself.

You only do this once. To import again (another copy, or after deleting `game/`), start it with `--import-ipa FILE`, for example `MapleThief.exe --import-ipa C:\Games\MapleThief-1.5.ipa`. Your saves and settings are kept.

## Your data folder

Everything MapleThief keeps for you is in `data/`, next to the game. The game creates it on the first start:

| In `data/` | What it holds |
|---|---|
| `saves/` | your characters and the game's own files, and `gamecenter.json` (the achievements) |
| `settings.ini`, `bindings.ini`, `window.ini` | the settings, the keys and buttons, the window's size and place |
| `backups/` | the earlier saves of each character, and what an import replaced |
| `states/` | save states |
| `screenshots/` | screenshots |
| `exports/`, `import/` | the settings menu's export and import of saves |
| `logs/`, `crashes/` | logs, if the game writes them |

`game/`, beside `data/`, is the game imported from your IPA. It isn't yours to back up: import it again from your IPA any time.

- **Back up** by copying `data/` somewhere else. To restore, put the copy back in its place while MapleThief is closed.
- **Start over** by deleting `data/saves/` (only the saves) or all of `data/` (the settings too), while MapleThief is closed.
- **Keep the folder together.** Moving or copying this whole folder, to another place or another computer, takes your data with it. Moving `MapleThief.exe` or `MapleThief.app` out of it leaves the data behind.
- **Updating to a new version**: unpack the new download to a new folder, then move `data/` (and `game/`, to skip the import) from the old folder into it. Or start the new one once with `--migrate-from <old folder>/data`.
- **Where it is**: the settings menu's Saves tab shows the data folder and opens it (*Open data folder*). `MapleThief --print-data-dir` prints it.

**The user folder instead.** If you'd rather keep your data in your user account's folder (`%APPDATA%\MapleThief`, `~/Library/Application Support/MapleThief`, `~/.local/share/MapleThief`), shared by every copy of the game, choose Saves → *Data location* → *User folder* in the settings menu, from the title screen. MapleThief copies your data there, leaves a note `user-data.txt` next to itself, and closes. Start it again. The way back is the same row (*Game folder*). The command line does the same: `--switch-data-location user` or `--switch-data-location game`.

**When MapleThief can't write next to itself** (a protected folder such as `C:\Program Files`, a read-only disk, or macOS's App Translocation above), it uses the user folder for the time being, says so once when it starts, and shows why in the Saves tab. Move this folder somewhere you can write; the next start copies your data next to the game.

## More

- Playing, controls, settings, saves and troubleshooting: https://github.com/armaatus/maplethief-port/tree/main/docs/play
- `MapleThief --version` prints the version. The command-line options are listed in the repository's README.
- Licences: `LICENSE` (MIT, the port's own code), `THIRD_PARTY.md` and `licenses/` (the libraries and fonts it includes).
