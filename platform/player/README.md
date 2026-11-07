# MapleThief

MapleThief runs MapleStory Thief Edition, the 2010 iPhone game, natively on your computer. It is an unofficial fan project, not affiliated with or endorsed by Nexon. MapleStory is a trademark of Nexon.

This download contains the program only: none of the game's art, music, maps or text. You need **your own copy** of the game, the file `MapleThief-1.5.ipa` (version 1.5, decrypted, 17,872,861 bytes, SHA-256 `a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387`). We can't provide it.

## Start

- **Windows**: double-click `MapleThief.exe`. If SmartScreen says "Windows protected your PC", click **More info**, then **Run anyway**.
- **Mac**: move `MapleThief.app` to Applications and open it. The app isn't notarised, so the first time macOS blocks it. On macOS 15 and later, open System Settings → Privacy & Security and click **Open Anyway**. On macOS 13 and 14, right-click the app, choose **Open**, then **Open** again.
- **Linux, Steam Deck**: run `./MapleThief.sh`. `./MapleThief.sh --install-desktop` adds it to the application menu, and through that to Steam's "Add a Non-Steam Game".

## First launch: your game file

The first time, MapleThief shows an import screen. Pick your `MapleThief-1.5.ipa` with the file dialog or drop it on the window. MapleThief checks the file and copies the game's files into its data folder, in `game/`:

| System | Data folder |
|---|---|
| Windows | `%APPDATA%\MapleThief` |
| Mac | `~/Library/Application Support/MapleThief` |
| Linux, Steam Deck | `~/.local/share/MapleThief` |

You only do this once. To import again (another copy, or after deleting `game/`), start it with `--import-ipa FILE`, for example `MapleThief.exe --import-ipa C:\Games\MapleThief-1.5.ipa`. Your saves and settings live in the same folder and are kept.

## More

- Playing, controls, settings, saves and troubleshooting: https://github.com/armaatus/maplethief-port/tree/main/docs/play
- `MapleThief --version` prints the version. The command-line options are listed in the repository's README.
- Licences: `LICENSE` (MIT, the port's own code), `THIRD_PARTY.md` and `licenses/` (the libraries and fonts it includes).
