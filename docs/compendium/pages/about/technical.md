# Technical overview

A short account of how MapleStory Thief Edition is built, for readers who want to know what sits behind the compendium's pages. Each file format has a full specification and a parser in the port's repository ([the format index](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/README.md)); the port's design is in [ARCHITECTURE.md](https://github.com/armaatus/maplethief-port/blob/main/docs/ARCHITECTURE.md).

## Two programs in one

The app is one binary with two parts. A small Objective-C shell, written for iOS, draws the main menu, character select, help, the alerts, and the touch key pads with UIKit. Underneath runs a C++ engine that came from a Korean feature-phone game: it draws the game world with OpenGL ES, and it still thinks in phone keypad codes, which the shell's touch buttons send ([re/program-flow.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/program-flow.md)). The binary has an armv6 and an armv7 slice; the port works from the armv7 one.

## Scripts

Every NPC, every portal, and some maps run a script when the hero talks to them, enters them, or arrives. The 307 script files under `script/` are compiled Lua 2.4 with the usual header removed, run by the engine's own C++ copy of the Lua 2.4 interpreter, which counts in whole numbers only. The scripts call 52 game functions for dialogue, quests, items, and warps. They carry no text: they name keys that the game looks up in the language's strings file. A file in a chapter's directory replaces the file of the same name in `common/` while that chapter runs ([formats/script-out.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/script-out.md)). The compendium walks every path of every script to find the quests, shops, trades, and warps; the [scripts list](about/scripts.html) shows where each file runs.

## Maps

Each of the 90 maps is three files named by its area and map number ([formats/map.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/map.md)):

- the `.map` file holds the tile layers, run-length packed;
- the `.att` file holds the collision grid, two cells per tile each way: air, one-way floors, ladders, and walls ([formats/att.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/att.md));
- the `.obj` file places the portals, the NPCs, and the monster spawns, each with the name of its script ([formats/obj.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/obj.md)).

An area's `.mapprj` file names its tile sheets ([formats/mapprj.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/mapprj.md)), and the backdrops are plain images.

## Sprites and images

The hero, the monsters, the NPCs, the portals, and the effects are sprite projects (`.sprprj`): frames cut from PNG sheets, grouped into actions such as standing, moving, and dying, each frame with its delay ([formats/sprprj.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/sprprj.md)). Item icons and interface pieces sit in packs of PNG files joined end to end ([formats/png-dat.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/png-dat.md)). The shell's own images are PNGs in Apple's altered iPhone format, CgBI ([formats/cgbi.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/cgbi.md)).

## Data tables

Monsters, NPC names, items, skills, shop stock, and the EXP of each level live in fixed-record binary tables, one set per language ([formats/data-tables.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/data-tables.md)). The game keeps the raw files in memory and reads each column by its byte offset.

## Saves

The game saves into raw binary files in its Documents folder: one file per character for the hero, one for the quests, the options, and lists of App Store receipts. They have no checksum and no version number ([formats/save.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/save.md)). The port reads and writes the same files, so a save from a device carries over ([formats/ios-backup.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/ios-backup.md)).

## The port

The MapleThief port does not emulate the iPhone. It translates the game's ARM code into C++ ahead of time (static recompilation) and runs it on a runtime that provides the parts of iOS the game uses: memory, Objective-C, UIKit drawing, OpenGL ES, sound, and files. It runs on macOS, Windows, iOS, and Linux from a player's own copy of the game, and it ships none of the game's files ([the port on GitHub](https://github.com/armaatus/maplethief-port)).
