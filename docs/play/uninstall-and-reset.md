# Uninstall and reset

How to put the settings back, start over with new characters, or remove MapleThief completely, on each system. Close the game first every time: it writes its files when it quits.

Before you delete anything, think about your saves. They live in the `data` folder ([Saves](saves.md#where-your-saves-are)), and copying that folder somewhere safe keeps them. The settings menu's Saves → **Export saves…** makes a zip you can import again later ([Saves](saves.md#moving-saves-to-another-computer)).

## What to delete for what

These paths are inside the data folder, which is `data` next to the game unless you chose the user folder ([below](#the-user-folder)).

| To | Delete | Kept |
|---|---|---|
| Reset the settings | `settings.ini` | saves, keys and buttons |
| Reset the keys and buttons | `bindings.ini` | saves, settings |
| Reset the window's size and place | `window.ini` | everything else |
| Start over with new characters | `saves` | settings, `backups` |
| Forget the earlier saves too | `saves` and `backups` | settings |
| Start completely fresh | the whole `data` folder | the imported game in `game` |
| Import the game again | the `game` folder, next to `data` | saves and settings |

The game uses the defaults for whatever you deleted. Without `settings.ini` the game asks again how you want to play (Original, Original + controller or Enhanced).

You can also reset only the settings a preset controls without deleting anything: settings menu → **Preset**, then choose one. Volumes, your controls and the border colour stay as they are ([Settings](settings.md#the-presets)).

## Windows

- **Reset or start over**: settings menu (F1) → Saves → **Open data folder** opens it in Explorer. Close the game and delete what the [table](#what-to-delete-for-what) says.
- **Uninstall**: delete the folder you unpacked, for example `Documents\MapleThief`. MapleThief has no installer and writes nothing to the registry, so that's all. If you chose the user folder, also delete `%APPDATA%\MapleThief` (paste it into Explorer's address bar).

## Mac

- **Reset or start over**: settings menu (F1) → Saves → **Open data folder** opens it in Finder. Quit the game and delete what the [table](#what-to-delete-for-what) says.
- **Uninstall**: drag the folder with `MapleThief.app` in it to the Bin. If the app ran from Applications, or the Saves tab said *User folder*, also delete `~/Library/Application Support/MapleThief` (in Finder: Go → Go to Folder…).

## Linux and Steam Deck

- **Reset or start over**: settings menu (F1, or Guide on the Deck) → Saves → **Open data folder**, or look in `data/` next to `MapleThief.sh`. Close the game and delete what the [table](#what-to-delete-for-what) says. On the Deck, do this in Desktop Mode, in Dolphin.
- **Uninstall**:
  1. If you added the menu entry, remove it first, from the game's folder: `./MapleThief.sh --uninstall-desktop`. It deletes `~/.local/share/applications/io.github.armaatus.maplethief.desktop`, the file `--install-desktop` wrote.
  2. On the Deck, remove the Steam shortcut: in the library, the shortcut's gear → Manage → *Remove non-Steam game from your library*.
  3. Delete the game's folder, for example `~/Games/MapleThief-<version>-linux-x86_64`.
  4. If you chose the user folder, delete `~/.local/share/MapleThief` too.

## iPhone

On the iPhone the game keeps its saves in its own folder, which the Files app shows under On My iPhone → MapleThief. The settings are inside the app, where the Files app can't reach them.

- **Reset the settings**: settings menu (the gear) → **Preset**, and choose one. To reset everything else as well, reinstall the app as below.
- **Start over with new characters**: in the Files app, open On My iPhone → MapleThief and delete the files whose names start with `theif` and `quest`, while the game isn't running. Swipe it away in the app switcher first.
- **Uninstall**: touch and hold the app's icon, then *Remove App* → *Delete App*. This deletes the saves and settings with it. To keep your saves, copy the MapleThief folder out of the Files app first (or with the Finder's file sharing), and copy it back after you install the app again ([Playing on your iPhone](iphone.md#saves)). On a Mac you can also delete the app from Xcode's Devices window.

## Nintendo Switch

Everything is on the SD card, in `sdmc:/switch/maplethief/` ([Playing on the Nintendo Switch](switch.md#your-saves-and-settings)). Close MapleThief and put the card in a computer.

- **Reset or start over**: delete what the [table](#what-to-delete-for-what) says inside `switch/maplethief/data/`.
- **Import the game again**: delete `switch/maplethief/game/`, put the IPA back in `switch/maplethief/` and start MapleThief.
- **Uninstall**: delete the folder `switch/maplethief/`. Copy its `data` folder to a computer first if you want to keep your saves. If you made a HOME menu forwarder yourself, remove it the way the tool that made it says.

## The user folder

If you chose Saves → *Data location* → *User folder*, or the game couldn't write next to itself, your data is in your user account's folder instead of `data` next to the game:

| System | User folder |
|---|---|
| Windows | `%APPDATA%\MapleThief` |
| Mac | `~/Library/Application Support/MapleThief` |
| Linux, Steam Deck | `~/.local/share/MapleThief` |

The same files are in it, and the [table](#what-to-delete-for-what) works the same way. The settings menu's Saves tab says which folder is in use, and `MapleThief --print-data-dir` prints it. Deleting the game's own folder leaves the user folder as it is, so delete it as well to remove everything.
