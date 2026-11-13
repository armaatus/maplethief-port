# Playing on the Nintendo Switch

MapleThief runs on the Nintendo Switch (the original model, OLED and Lite) as a homebrew app, `MapleThief.nro`, started from the Homebrew Menu. You copy it to the SD card with your own copy of the game, and the first start imports the game from it ([ADR 0138](../adr/0138-nintendo-switch-homebrew-target.md)).

> **Not checked on a console yet.** The Switch build is new: the import and the first map on real hardware are still to be confirmed ([#243](https://github.com/armaatus/maplethief-port/issues/243), [#245](https://github.com/armaatus/maplethief-port/issues/245), [the checklist](../testing/switch-checklist.md)). If you try it, please tell us how it goes.

## What you need

- **A Switch that runs homebrew apps from the Homebrew Menu.** That means custom firmware. This project doesn't explain how to set it up and doesn't link to the tools for it. The Switch 2 is not supported.
- **Your own copy of the game:** the file `MapleThief-1.5.ipa`, 17,872,861 bytes, with the SHA-256 `a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387`, the one in [`original/SHA256SUMS`](../../original/SHA256SUMS). We can't give it to you. [Your game file](your-ipa.md) explains which file it has to be and how to check it on a computer.
- **A computer** to copy files to the SD card, and about 65 MB free on the card: the program and the imported game take about 45 MB, and the IPA 18 MB until you delete it.

## Download and copy

1. Download `MapleThief-<version>-switch.zip` from the [Releases page](https://github.com/armaatus/maplethief-port/releases), under *Assets*, or from the [itch.io page](https://armaatus.itch.io/maplethief). It holds the program and no file of the game. You can check it against the release's `SHA256SUMS`.
2. Unpack it. Copy its `switch` folder to the root of the SD card, so that the program is at `sdmc:/switch/maplethief/MapleThief.nro` (on the card in a computer: `switch/maplethief/MapleThief.nro`).
3. Copy your `MapleThief-1.5.ipa` into the same folder: `switch/maplethief/MapleThief-1.5.ipa`. Another name works too, as long as it ends in `.ipa`.

The folder then holds:

```text
switch/maplethief/
  MapleThief.nro
  MapleThief-1.5.ipa
```

## Start

Put the card back, open the Homebrew Menu and start **MapleThief**.

**Open the Homebrew Menu through a game, not the Album.** Started from the Album, homebrew apps run in a mode with much less memory. Holding R while you start any game opens the Homebrew Menu with the game's memory instead ("title takeover"). MapleThief measures its memory as it runs; when started from the Album it warns you on screen if it is running short ([ADR 0141](../adr/0141-switch-lifecycle-applet-mode-and-audio.md)). Use a game you don't mind being closed while MapleThief runs.

### The first start: importing your game file

The first time, MapleThief shows its import screen and starts on its own:

1. It looks in `sdmc:/switch/maplethief/` for `MapleThief-1.5.ipa` first, then for any other `.ipa` file, and tries them in that order.
2. It checks the file and copies the game's files into `sdmc:/switch/maplethief/game/`. A bar shows the progress. This takes about a minute; leave MapleThief open.
3. It asks whether to delete the `.ipa` from the SD card to free 17 MB. The game no longer needs it. **Keep** is selected at first, so A keeps it; press Right for **Delete**, then A, to delete it (a tap on either works too). Keep it if you want to import again later.

Then the game starts. Every later start goes straight to the game.

If the import screen says **No IPA found**, the file isn't in that folder: copy it there, then press A to look again, or + to return to the Homebrew Menu. If it says **That file cannot be used**, it tells you why, for example:

- not an IPA, or not MapleThief's;
- another version than 1.5;
- an encrypted copy: the port can't run an encrypted game executable;
- version 1.5, but damaged or changed: copy the file again, and check its SHA-256 on a computer ([Your game file](your-ipa.md)).

Nothing is kept from a file that is refused. To import again later (another copy, or after deleting `game/`), delete `sdmc:/switch/maplethief/game/`, put the IPA back and start MapleThief. Your saves and settings are kept.

## Controls

Joy-Con (attached or detached, a pair or one on its side), a Pro Controller, and the touch screen in handheld mode. **A confirms and B goes back** in every menu. **+** opens the settings menu. The full table: [Nintendo Switch controls](../controls/switch.md).

## Your saves and settings

Everything is on the SD card, in `sdmc:/switch/maplethief/` ([ADR 0127](../adr/0127-data-folder-next-to-the-game.md)):

| Folder | What it holds |
|---|---|
| `data/saves/` | your characters and the game's own files |
| `data/settings.ini`, `data/bindings.ini` | the settings, the buttons |
| `data/backups/` | the earlier saves of each character, and what an import replaced |
| `data/exports/`, `data/import/` | the settings menu's export and import of saves |
| `data/logs/`, `data/crashes/` | logs of each run, crash reports |
| `game/` | the game imported from your IPA: import it again any time |
| `texture-packs/`, `audio-packs/`, `text-packs/`, `borders/` | packs, if you add any ([Settings](settings.md)) |

- **Back up** by copying `data/` to a computer. To restore, put it back while MapleThief is closed.
- **Move saves between the Switch and a computer:** the settings menu's Saves tab exports a zip into `data/exports/`; copy it to the computer and import it there. The other way, put a zip of saves in the Switch's `data/import/` and choose **Import saves…** in the Saves tab, from the title screen. [Saves](saves.md#moving-saves-to-another-computer) has the details.
- **Updating to a new version:** copy the new `MapleThief.nro` over the old one. `data/` and `game/` stay.

## Troubleshooting

- **"Running short of memory" on screen, or the game closes on a map:** you started it from the Album. Start the Homebrew Menu through a game (hold R while starting one) and start MapleThief from there.
- **The import screen comes back on every start:** the import didn't finish (the console went to sleep or MapleThief was closed during it), or `game/` was deleted. Let it run to the end; nothing half-imported is kept.
- **The import says there is not enough memory:** the same as above: start through a game.
- **Something else:** the log of each run is in `sdmc:/switch/maplethief/data/logs/MapleThief-<time>.log`, and a crash report in `sdmc:/switch/maplethief/crash.txt` and `data/crashes/`. Attach them to an [issue](https://github.com/armaatus/maplethief-port/issues). More in [Troubleshooting](troubleshooting.md).

## A HOME menu icon

The project ships only the NRO for the Homebrew Menu. It doesn't build, ship or support forwarders or NSPs; players can make a forwarder for the HOME menu themselves with existing tools.
