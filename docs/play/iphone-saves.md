# Bringing your iPhone or iPod save into the port

The original game keeps its saves in its own `Documents/` folder on the device, and it doesn't share that folder with the Files app or iTunes file sharing. To get your saves out without a jailbreak, make an **unencrypted** backup of the device on a Mac or PC. Then the game itself reads its files out of the backup and imports them: settings menu → Saves → **iPhone backup**. The import checks every file before anything replaces a save, and backs up what it replaces ([ADR 0078](../adr/0078-save-export-import-backups-and-checks.md)). Nothing else is needed: no Python, no command line. The backup format is in [formats/ios-backup.md](../formats/ios-backup.md).

Before you start:

- **Keep the game installed on the device.** A backup holds the data of installed apps only. The game is no longer in the App Store, so if you delete it, its saves are gone from the next backup.

## 1. Make an unencrypted backup

An encrypted backup's files can't be read without its password, so the game refuses one and says so. Turning encryption off asks for the backup password. If you've forgotten it, iOS 11 and later can remove it: Settings > General > Transfer or Reset iPhone > Reset > Reset All Settings. This resets settings such as wallpaper and Wi-Fi, and leaves your data alone. An unencrypted backup doesn't include saved passwords, Health or Wi-Fi settings, but app data such as the game's saves is always included.

**macOS 10.15 (Catalina) and later: Finder**

1. Connect the device with a cable and unlock it. Tap **Trust** if it asks.
2. Open a Finder window and select the device in the sidebar, under Locations.
3. In the **General** tab, under Backups, choose **Back up all of the data on your iPhone to this Mac**.
4. Untick **Encrypt local backup**.
5. Click **Back Up Now** and wait until it finishes.

On macOS 10.14 and earlier, the same options are in iTunes, under the device's **Summary** page.

**Windows 10 and 11: Apple Devices (or iTunes)**

1. Install **Apple Devices** from the Microsoft Store (or iTunes, if you already use it). Connect the device, unlock it and tap **Trust**.
2. *Apple Devices:* select the device in the sidebar, open **General**, and under Backups choose **Back up all of the data on your iPhone to this PC**. *iTunes:* click the device button near the top left, open **Summary**, and under Backups choose **This Computer**.
3. Untick **Encrypt local backup**.
4. Click **Back Up Now** and wait until it finishes.

If a current Mac or PC doesn't recognise a very old device, an older Mac or PC with iTunes works too. The game reads backups from iOS 4 onward: `Manifest.mbdb` (iOS 4 to 9) and `Manifest.db` (iOS 10 and later).

## 2. Import it in the game

1. Start MapleThief on the same computer and stay on the title screen or the main menu (not in the field: an import never replaces the character you are playing).
2. Open the settings menu (F1, or Guide on a gamepad) → **Saves** → **iPhone backup**.
3. The list shows every backup the game found, newest first, with the device's name, its iOS version and when it was made. An encrypted one says *encrypted*.
4. Choose the backup. The game asks *Import the saves of …? The files they replace are backed up first.* with **No** selected. Press Right (or tap **>**) for **Yes**, then choose again.

The status line then says *Imported 7 files from …*, or why nothing was imported. With a gamepad: the d-pad moves, A chooses, B goes back.

The game looks for backups where Finder, iTunes and Apple Devices keep them:

| System | Where backups are kept |
|---|---|
| macOS (Finder or iTunes) | `~/Library/Application Support/MobileSync/Backup/` |
| Windows, Apple Devices or iTunes from the Microsoft Store | `%USERPROFILE%\Apple\MobileSync\Backup\` |
| Windows, iTunes from apple.com | `%APPDATA%\Apple Computer\MobileSync\Backup\` |

and in the `import` folder of the game's data folder ([Saves](saves.md#where-your-saves-are)). Each backup is a folder named after the device's identifier. A backup made on another computer, or on Linux, a Steam Deck or a Switch: copy its whole folder into `import` and it shows in the list. In Finder, **Manage Backups…** in the device's General tab lists your backups: right-click one and choose **Show in Finder**.

**macOS: the list is empty or says "permission denied"?** macOS protects the backup folder. Either give MapleThief Full Disk Access (System Settings > Privacy & Security > Full Disk Access), or copy the backup's folder into the data folder's `import` in Finder (Option-drag it there) and choose it from the list.

### What the import does

The game reads only the files of MapleStory Thief Edition in the backup, and only the names the game uses (`theif0`–`2`, `quest0`–`2`, `config`, `cupon`, the purchase receipts). Other files are left out. It never changes the backup.

Then it checks every file, as every import does. A file that would break the game (cut short, an item or map the game doesn't have, a character without its quest file) means nothing is imported, and the status line gives the reason. So does a damaged backup (a manifest that can't be read, a file missing or of the wrong size).

Otherwise it copies the saves it replaces to `backups/import/<time>/` in the data folder, then writes the backup's saves (all three slots, `config`, `cupon` and the purchase receipts) over the port's. Saves the backup doesn't hold stay. The port's cash shop counters of each slot it replaces are backed up and removed, because they belonged to the old character. To undo it, choose that backup (*Replaced: …*) in the settings menu's Saves → Backups list ([Saves](saves.md#backups)).

To see whether your saves are all right afterwards: Saves → **Check saves** ([Saves](saves.md#checking-your-saves)).

## From the command line

The same reader, without a window:

```sh
# Each save file in the backup with its verdict (theif0: ok, ...); exit status 3 if one fails.
MapleThief --check-ios-backup ~/Library/Application\ Support/MobileSync/Backup/<id>

# Import them, as the menu's iPhone backup does.
MapleThief --import-ios-backup ~/Library/Application\ Support/MobileSync/Backup/<id>
```

On a Mac, `MapleThief` is `MapleThief.app/Contents/MacOS/MapleThief`; on Windows, `MapleThief.exe`. `--save-dir DIR` goes with both, as with `--import-saves`.

### With Python, from a checkout

The repository's `tools/mtport/iosbackup.py` reads backups with the same rules. It is handy for scripts and for looking at a backup before the game sees it. It needs Python 3.10 or newer and a checkout; run it from the `tools/` folder:

```sh
python3 -m mtport.iosbackup find                      # every backup in the places above
python3 -m mtport.iosbackup <backup>                  # its save files, each with its verdict
python3 -m mtport.iosbackup extract <backup> ~/Desktop/iphone-saves.zip
python3 -m mtport.iosbackup import <backup> --program /Applications/MapleThief.app
```

`extract` writes a zip when the name ends in `.zip`, and otherwise a new, empty folder; `MapleThief --import-saves` imports either. `import` runs `MapleThief --import-saves` on the backup's saves; `--program` takes the executable, or `MapleThief.app` on a Mac, and `--save-dir DIR` passes the port's `save_dir` setting on.

## Other routes

- **iMazing** and similar tools can copy an app's `Documents/` folder directly. Put that folder into the data folder's `import` and use Saves → **Saves from a folder**, or run `MapleThief --import-saves <that folder>`.
- **A jailbroken device:** copy `Documents/` from the app's folder ([save.md](../formats/save.md#pulling-a-save-from-a-device)) and import it the same way.
