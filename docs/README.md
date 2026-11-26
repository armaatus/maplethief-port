# Documentation

Start with the section that fits you.

## Playing

For everyone. No programming needed.

| | |
|---|---|
| [Playing MapleThief](play/README.md) | Start here: what you need and how to get the game running |
| [Your game file](play/your-ipa.md) | Which copy of the game you need, and how to check it |
| [Controls](play/controls.md) | Keyboard, mouse, touch, gamepad |
| [Nintendo Switch controls](controls/switch.md) | The Joy-Con, Pro Controller and touch screen, in the game and in the menus |
| [Settings](play/settings.md) | The settings menu, presets, display, sound and gameplay options |
| [Saves](play/saves.md) | Where saves are, backups, moving them, the data folder next to the game or in your user folder |
| [Saves from your iPhone](play/iphone-saves.md) | Bringing your original save over |
| [Linux and Steam Deck](play/linux-and-steam-deck.md) | Installing on Linux and adding it to Steam |
| [Playing on your iPhone](play/iphone.md) | Building the app on a Mac with Xcode and installing it, with a free or paid Apple account |
| [Playing on the Nintendo Switch](play/switch.md) | Copying it to the SD card, the import on the console, saves |
| [Troubleshooting](play/troubleshooting.md) | When something doesn't work, and common questions |
| [Known issues](KNOWN_ISSUES.md) | What is open today, with workarounds |
| [What's included, and what isn't](legal.md) | The legal side, in plain words |

## Building and contributing

For people who want to build the game themselves or help develop it.

| | |
|---|---|
| [Contributing](../CONTRIBUTING.md) | How to help, and the rules of the repository |
| [Build guide](BUILDING.md) | From a fresh clone to a running game, per system |
| [Build from source](build/README.md) | The deep reference: tools per system, `just build`, and every step by hand |
| [Running and options](build/running.md) | Every command-line option and setting |
| [Testing and CI](build/testing.md) | The test suites, local CI, GitHub CI |
| [Releases](build/releases.md) | How a release is cut ([checklist](release-checklist.md)) |
| [Roadmap](ROADMAP.md) | The milestones, from foundation to v1.0 and after |
| [Compendium: build and deploy](compendium/README.md) | How the [compendium site](https://armaatus.github.io/maplethief-port/) is built from your own copy, checked and deployed to GitHub Pages |
| [Compendium style guide](compendium/style.md) | Voice, page templates and terms of the compendium site, and its prose lint |

## How it works

The technical reference: how the original game is built, and how the port is designed.

| | |
|---|---|
| [Architecture](ARCHITECTURE.md) | The design of the port: recompiler, runtime, frontends, build |
| [Analysis](ANALYSIS.md) | Facts about the original program and its data |
| [Decisions (ADRs)](adr/) | Every significant design decision, with the options considered and why |
| [File formats](formats/README.md) | Specs of the game's file formats, each with a tested parser |
| [Reverse-engineering notes](re/) | How parts of the original game work: audio, saves, scripts, program flow, … |
| [Control details](controls/README.md) | The default keys and buttons, the on-screen key pad, remapping |
| [Playtest scripts](testing/) | Step-by-step runs used to check the game by hand |

### Controls

| | |
|---|---|
| [Controls reference](controls/README.md) | The index of the pages below |
| [Keyboard controls](controls/keyboard.md) | The default keys |
| [The on-screen keypad](controls/keypad.md) | How the iOS build turns touches into actions, and every action |
| [Remapping controls](controls/remapping.md) | How a player changes keys and gamepad buttons |
| [The controls card](controls/controls-card.md) | The one-page card for the device in use |
| [Nintendo Switch controls](controls/switch.md) | The defaults on the Switch |

### Playtests and hand-offs

| | |
|---|---|
| [The scripted first-map run](testing/first-map-run.md) | `play.first_map`: the real game played to the first map |
| [Chapter 1's quests](testing/chapter-1-quests.md) | The checks of chapter 1's NPCs and quests |
| [The regression replay](testing/regression-replay.md) | `scripts/regress`: input replay against recorded checkpoints |
| [Playthrough checklist](testing/playthrough-checklist.md) | The chapter by chapter checklist, generated from the knowledge base |
| [Switch sign-off checklist](testing/switch-checklist.md) | The hardware sign-off of the Switch build |
| [Switch performance](testing/switch-performance.md) | The logged performance session on the console |
| [Hand-off checklists](testing/handoffs/README.md) | Everything that waits on a device or a person, one page each: [Mac](testing/handoffs/mac.md), [Windows PC](testing/handoffs/windows.md), [Linux and Steam Deck](testing/handoffs/linux.md), [iPhone](testing/handoffs/iphone.md), [Nintendo Switch](testing/handoffs/switch.md), [playthrough, review and release](testing/handoffs/playthrough.md) |
