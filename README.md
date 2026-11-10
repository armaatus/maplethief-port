# MapleThief: a native port of MapleStory Thief Edition

[![CI](https://github.com/armaatus/maplethief-port/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/armaatus/maplethief-port/actions/workflows/ci.yml)

**MapleStory Thief Edition** was a single-player MapleStory game for the iPhone and iPod touch (Nexon Mobile, 2010). It has been gone from the App Store for years. This project makes it run natively on **Windows, macOS, Linux and the Steam Deck**, with keyboard and controller support, fullscreen and sharper graphics. The game itself is unchanged.

You play it with **your own copy** of the game. This repository contains none of the game's code, art, music or text. Our downloads contain the program, with the game's code recompiled, but none of its art, music, maps or text: the game imports those from your copy on first launch. See [What's included, and what isn't](docs/legal.md).

## Play

**→ [Playing MapleThief](docs/play/README.md)**: what you need, how to get it running, controls, settings and saves. Written for everyone, no programming knowledge needed.

## Compendium

**→ [MapleThief Compendium](https://armaatus.github.io/maplethief-port/)**: an unofficial fan reference with everything known about the game. It has every monster, item, NPC, shop, map, quest and skill, the full dialogue in English and Korean, a walkthrough of every chapter, an interactive world map, and the game's own rules for damage, drops and EXP. It is built from our own copy of the game and published on GitHub Pages; none of it is in this repository ([how it is built](docs/compendium/README.md), [ADR 0121](docs/adr/0121-public-compendium-site.md)).

## Reporting a bug

The game writes a log to `data/logs/` next to it (settings menu → Saves → **Open logs folder**) and a report to `data/crashes/` after a crash; a dialog names the file. Attach the newest log and the crash report to a [bug report](https://github.com/armaatus/maplethief-port/issues/new?template=bug.yml). Paths in the log have your folders replaced by `<data>`, `<game folder>` and `~`. After a crash the next start offers default settings without changing your settings file.

## Help build it

**→ [Build guide](docs/BUILDING.md)** · **[Contributing](CONTRIBUTING.md)**

You need your own copy of the game and a few free tools. Then one command per system, with `just` or without it:

| On | With [just](https://just.systems) | Without | Result |
|---|---|---|---|
| macOS | `just build mac` | `scripts/build mac` | `dist/mac/MapleThief.app` |
| Windows | `just build win` | `scripts\build.ps1 win` | `dist\win\MapleThief-game-…-windows-x64\` |
| Linux, Steam Deck | `just build linux` | `scripts/build linux` | `dist/linux/MapleThief-game-…-linux-x86_64/` |

`just run` starts it, and `just doctor` says what's missing. A build holds the game, so keep it to your own machines.

## How it works

The original game is a 32-bit ARM program for iOS 3. The port **recompiles** that program ahead of time into C, function by function (2,655 of them), and links it against a **runtime of our own** that stands in for everything the game expected from iOS: UIKit, OpenGL ES, audio, files and the rest, 333 functions in all. SDL3 provides the window, input and sound on every system. No emulator runs at play time, so the game runs at full speed with its original logic intact.

The details are in the [docs](docs/README.md).

## Status

The game boots, plays and saves on Windows, macOS and Linux, with mouse, touch, keyboard and gamepad input. The full playthrough is being verified, and checks on a clean Windows machine and a real Steam Deck are still to come ([roadmap](docs/ROADMAP.md), [known issues](docs/KNOWN_ISSUES.md)). Ready-to-play downloads are on the [Releases page](https://github.com/armaatus/maplethief-port/releases): the program for your system, which imports your own copy of the game on first launch ([Download and play](docs/play/README.md#download-and-play)).

| System | |
|---|---|
| Windows 10/11 | x64 |
| macOS 13+ | Universal (Apple silicon and Intel) |
| Linux | x86_64, glibc 2.39+ |
| Steam Deck | As a non-Steam game ([guide](docs/play/linux-and-steam-deck.md)); not yet checked on a real Deck |
| iOS 16+ | Planned, sideloaded through Xcode ([#83](https://github.com/armaatus/maplethief-port/issues/83)) |

## Privacy

The game runs offline. The port's only network use is an optional check for new versions, off by default: once a day it asks github.com which version is the latest, and downloads nothing. See [Settings](docs/play/settings.md#checking-for-new-versions) and [ADR 0137](docs/adr/0137-network-use-and-the-update-notice.md).

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Nexon. MapleStory is a trademark of Nexon. The code in this repository is our own, under the [MIT licence](LICENSE); third-party libraries are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
