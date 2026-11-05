# Bringing your iPhone or iPod save into the port

The original game keeps its saves in its own `Documents/` folder on the device, and it doesn't share that folder with the Files app or iTunes file sharing. To get your saves out without a jailbreak, make an **unencrypted** backup of the device on a Mac or PC. Then let `python3 -m mtport.iosbackup` take the game's files out of the backup and hand them to the port's import, which checks every file before anything replaces a save ([ADR 0078](../adr/0078-save-export-import-backups-and-checks.md)). The backup format is in [formats/ios-backup.md](../formats/ios-backup.md).

Before you start:

- **Keep the game installed on the device.** A backup holds the data of installed apps only. The game is no longer in the App Store, so if you delete it, its saves are gone from the next backup.
- You need **Python 3.10 or newer** and a checkout of this repository. The tool runs from its `tools/` folder. On a Mac, `python3` comes with the Xcode command line tools (`xcode-select --install`). On Windows, install Python from python.org or with `winget install Python.Python.3.12`, then use `py -3` where this page says `python3`.

## 1. Make an unencrypted backup

An encrypted backup's files can't be read without its password, so the tool refuses one and says so. Turning encryption off asks for the backup password. If you've forgotten it, iOS 11 and later can remove it: Settings > General > Transfer or Reset iPhone > Reset > Reset All Settings. This resets settings such as wallpaper and Wi-Fi, and leaves your data alone. An unencrypted backup doesn't include saved passwords, Health or Wi-Fi settings, but app data such as the game's saves is always included.

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

If a current Mac or PC doesn't recognise a very old device, an older Mac or PC with iTunes works too. The tool reads backups from iOS 4 onward: `Manifest.mbdb` (iOS 4 to 9) and `Manifest.db` (iOS 10 and later).

## 2. Find the backup

Each backup is a folder named after the device's identifier:

| System | Where backups are kept |
|---|---|
| macOS (Finder or iTunes) | `~/Library/Application Support/MobileSync/Backup/` |
| Windows, Apple Devices or iTunes from the Microsoft Store | `%USERPROFILE%\Apple\MobileSync\Backup\` |
| Windows, iTunes from apple.com | `%APPDATA%\Apple Computer\MobileSync\Backup\` |

Run, from the repository's `tools/` folder:

```sh
python3 -m mtport.iosbackup find
```

It lists every backup in those places, each with the device's name, its iOS version, the date, and whether it is encrypted and holds the game. `python3 -m mtport.iosbackup find DIR` looks in another folder instead. In Finder, **Manage Backups…** in the device's General tab shows the same list: right-click a backup and choose **Show in Finder**.

**macOS says "permission denied"?** macOS protects the backup folder. Either give your terminal app Full Disk Access (System Settings > Privacy & Security > Full Disk Access), or copy the backup's folder somewhere else in Finder (Option-drag it to your Desktop) and use the copy.

## 3. Check what the backup holds

```sh
python3 -m mtport.iosbackup ~/Library/Application\ Support/MobileSync/Backup/<id>
```

This prints the device line and each save file with its verdict (`theif0: ok`, `quest0: ok`, …), and lists the other files of the game's `Documents/` as `ignored`. A file that would break the game (cut short, an item or map the game doesn't have) gets the reason, and the tool exits with status 3. The port would refuse such a set, so nothing more is written. The tool only reads the backup and never changes it.

## 4. Import

Quit the game first, or stay on the title screen ([ADR 0078](../adr/0078-save-export-import-backups-and-checks.md#consequences)). Then use one of these:

```sh
# Straight into the port: runs MapleThief --import-saves on the backup's saves.
python3 -m mtport.iosbackup import <backup> --program /Applications/MapleThief.app

# Or write them out first, then import that folder or zip yourself.
python3 -m mtport.iosbackup extract <backup> ~/Desktop/iphone-saves.zip
MapleThief --import-saves ~/Desktop/iphone-saves.zip
```

`--program` takes the executable, or `MapleThief.app` on a Mac (on Windows, the path to `MapleThief.exe`). `--save-dir DIR` passes the port's `save_dir` setting on to it. `extract` writes a zip when the name ends in `.zip`, and otherwise a new, empty folder.

The import checks every file again. It copies the saves it replaces to `backups/import/<time>/` in the data directory, then writes the backup's saves (all three slots, `config`, `cupon` and the purchase receipts) over the port's. Saves the backup doesn't hold stay. The port's cash shop counters of each slot it replaces are backed up and removed, because they belonged to the old character. To undo it, choose that backup (*Replaced: …*) in the settings menu's Saves → Backups list, or import its `backups/import/<time>/` folder ([Saves](saves.md)).

## Other routes

- **iMazing** and similar tools can copy an app's `Documents/` folder directly. `MapleThief --import-saves <that folder>` imports it as it is.
- **A jailbroken device:** copy `Documents/` from the app's folder ([save.md](../formats/save.md#pulling-a-save-from-a-device)) and import it the same way.
