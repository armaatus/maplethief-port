# Saves

The game has three save slots, as on the iPhone. You save with the game's own **Save** in the popup menu (Esc, or Start on a gamepad). With **Save on quit** (part of the Enhanced preset) the port also saves for you when you quit or minimise.

## Where your saves are

| System | Folder |
|---|---|
| Windows | `%APPDATA%\MapleThief` (paste it into Explorer's address bar) |
| Mac | `~/Library/Application Support/MapleThief` (in Finder: Go → Go to Folder…) |
| Linux, Steam Deck | `~/.local/share/MapleThief` |

The save files themselves are in its `Documents` folder, under the game's own names (`theif0`, `quest0`, `config`, …). The same folder holds `settings.ini`, `bindings.ini`, `backups`, `screenshots` and `exports`.

Each save replaces its file in one step, so a crash or power cut never leaves half a save.

## Backups

After every save, the port keeps a copy of that character in `backups`: the newest 10 per slot. Change the number in the settings menu (Saves) or with `save_backups = 20` (0 turns backups off).

To go back to a backup: settings menu → Saves → Import, or `MapleThief --import-saves <backup folder>`.

## Moving saves to another computer

- **Export**: settings menu → Saves → Export saves. This writes a zip into `exports`. Or run `MapleThief --export-saves saves.zip`.
- **Import**: put the zip (or the save files) into the `import` folder and choose Import saves in the settings menu, on the title or main menu. Or run `MapleThief --import-saves saves.zip`.

An import checks every file first. If anything would break the game (a cut-off file, an item the game doesn't have, a character without its quest file) it refuses the whole set, changes nothing and says why. The saves it replaces are backed up first.

## Sharing saves through Dropbox, iCloud or OneDrive

Set `save_dir` in `settings.ini` to a folder in your cloud drive, for example `save_dir = C:\Users\you\Dropbox\MapleThief`. Every computer that points there plays the same saves. Settings and backups stay on each computer. Don't play the same slot on two computers at once.

## Bringing your iPhone or iPod save over

If the original game is still installed on your device, you can bring your character along. You make a backup of the device with Finder or iTunes and run one command. Step by step: [Saves from your iPhone](iphone-saves.md).

## Portable mode (USB stick)

Put an empty file named `portable.txt` next to the game (next to `MapleThief.exe`, or next to `MapleThief.app` on a Mac). Saves and settings then go into a `data` folder beside it, so the whole thing can live on a USB stick.
