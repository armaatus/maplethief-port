# Saves

The game has three save slots, as on the iPhone. You save with the game's own **Save** in the popup menu (Esc, or Start on a gamepad). With **Save on quit** (part of the Enhanced preset) the port also saves for you when you quit or minimise.

## Where your saves are

Everything the game keeps for you is in a **`data` folder next to the game**: beside `MapleThief.exe`, beside `MapleThief.app` on a Mac, beside `MapleThief.sh` on Linux. The game creates it on the first start.

| In `data` | What it holds |
|---|---|
| `saves` | the save files, under the game's own names (`theif0`, `quest0`, `config`, …), and `gamecenter.json` (the achievements) |
| `settings.ini`, `bindings.ini`, `window.ini` | the settings, the keys and buttons, the window's size and place |
| `backups` | the earlier saves of each character ([below](#backups)) |
| `states`, `screenshots` | save states and screenshots |
| `exports`, `import` | for moving saves ([below](#moving-saves-to-another-computer)) |

`game`, beside `data`, holds the game imported from your IPA. It doesn't need a backup: import it again any time.

To open the data folder from the game: settings menu (F1, or Guide on a gamepad) → Saves → **Open data folder**. It opens in Finder, Explorer or your file manager. The Saves tab shows its path at the top, and `MapleThief --print-data-dir` prints it.

- **Back up** by copying `data` somewhere else.
- **Start over** by deleting `data/saves` (only the saves) or all of `data` (the settings too), with the game closed.
- **Move the game, saves and all,** by moving its whole folder: to another place, another computer or a USB stick. Keep `data` next to the game.
- **A new version**: unpack it to a new folder and move `data` (and `game`, to skip the import) into it from the old one. Or start the new version once with `--migrate-from <old folder>/data`.

Each save replaces its file in one step, so a crash or power cut never leaves half a save.

### The user folder instead

You can keep your data in your user account's folder instead, shared by every copy of the game:

| System | User folder |
|---|---|
| Windows | `%APPDATA%\MapleThief` (paste it into Explorer's address bar) |
| Mac | `~/Library/Application Support/MapleThief` (in Finder: Go → Go to Folder…) |
| Linux, Steam Deck | `~/.local/share/MapleThief` |

On the title screen, open the settings menu → Saves → **Data location**. It shows *Game folder* or *User folder*. Choose it to switch to the other one: the game asks *Copy your data to … and restart?* with **No** selected; Right (or **>**) picks **Yes**, choose again. It copies your data across (your data where it was stays as it is), leaves a note `user-data.txt` next to the game when it's the user folder, and closes. Start it again to use the new folder. Choose the row again to go back. If the other folder already holds data, the game says when it was saved and asks whether to use that data instead; nothing is overwritten. From the command line: `MapleThief --switch-data-location user` (or `game`).

Builds from before this change kept everything in the user folder. The first start of a new version copies that data next to the game by itself and tells you so; the old folder is left as it was.

On a Mac, an app dragged alone into Applications uses the user folder (the game says so in the Saves tab): keep `MapleThief.app` in its own folder to have `data` next to it.

### When the game can't write next to itself

In a protected folder (`C:\Program Files`), on a read-only disk, or on a Mac when the freshly downloaded app runs from a temporary read-only copy, the game uses the user folder for the time being. It says so once at the start, and the Saves tab's **Data location** says why and what to do. See [Troubleshooting](troubleshooting.md#the-game-keeps-its-data-in-the-user-folder).

## Backups

After every save, the port keeps a copy of that character in `backups`: the newest 10 per slot. Change the number in the settings menu (Saves) or with `save_backups = 20` (0 turns backups off).

To go back to a backup:

1. Go to the title screen or the main menu, not into the game.
2. Open the settings menu → Saves → **Backups**. The list shows each backup, newest first: the slot, the character's level and job, and when it was saved.
3. Choose one. The game asks *Restore slot 1 from …? The current save is backed up first.* with **No** selected. Press Right (or tap **>**) for **Yes**, then choose again.

The save you replace is kept too: it shows as the newest row (*Replaced: slot 1, …*), so a restore can be undone the same way. With a gamepad: the d-pad moves, LT + up/down jumps a page, A chooses, B goes back. From the command line: `MapleThief --list-backups`, then `MapleThief --import-saves <backup folder>`.

## Moving saves to another computer

- **Export**: settings menu → Saves → **Export saves…**. Your system's save dialog asks where to put the zip; it starts in `exports`. Or run `MapleThief --export-saves saves.zip`.
- **Import**: on the title or main menu, settings menu → Saves → **Import saves…**, then pick the zip in your system's open dialog. Or run `MapleThief --import-saves saves.zip`.

Where no dialog can show, the game uses fixed folders instead: on iOS, on a Steam Deck in Game Mode, in fullscreen on Linux, or when the system has no file dialog. **Export** then writes the zip into `exports`. For **Import**, put the zip (or the save files) into the `import` folder first; the newest zip there is used.

### On the Nintendo Switch

The data folder is `sdmc:/switch/maplethief/data` on the SD card, with the same folders as above. There is no file dialog on the console:

- **Export saves…** writes `exports/MapleThief-saves-<date and time>.zip`.
- **Import saves…** (from the title or the main menu) lists every zip and save folder in `import` and every zip in `exports`, newest first. Choose one with A. The game asks *Import …? The files it replaces are backed up first.* with **No** selected; press Right for **Yes**, then A again. To bring saves from a computer, copy the zip into `sdmc:/switch/maplethief/data/import` first.
- **Backups** works as above.

An import checks every file first. If anything would break the game (a cut-off file, an item the game doesn't have, a character without its quest file) it refuses the whole set, changes nothing and says why. The saves it replaces are backed up first.

## Sharing saves through Dropbox, iCloud or OneDrive

Set `save_dir` in `settings.ini` to a folder in your cloud drive, for example `save_dir = C:\Users\you\Dropbox\MapleThief`. Every computer that points there plays the same saves, and the achievements (`gamecenter.json` sits with the saves). Settings and backups stay on each computer. Don't play the same slot on two computers at once.

## Bringing your iPhone or iPod save over

If the original game is still installed on your device, you can bring your character along. You make a backup of the device with Finder or iTunes and run one command. Step by step: [Saves from your iPhone](iphone-saves.md).
