# Documentation

Start with the section that fits you.

## Playing

For everyone. No programming needed.

| | |
|---|---|
| [Playing MapleThief](play/README.md) | Start here: what you need and how to get the game running |
| [Your game file](play/your-ipa.md) | Which copy of the game you need, and how to check it |
| [Controls](play/controls.md) | Keyboard, mouse, touch, gamepad |
| [Settings](play/settings.md) | The settings menu, presets, display, sound and gameplay options |
| [Saves](play/saves.md) | Where saves are, backups, moving them, portable mode |
| [Saves from your iPhone](play/iphone-saves.md) | Bringing your original save over |
| [Linux and Steam Deck](play/linux-and-steam-deck.md) | Installing on Linux and adding it to Steam |
| [Troubleshooting](play/troubleshooting.md) | When something doesn't work, and common questions |
| [What's included, and what isn't](legal.md) | The legal side, in plain words |

## Building and contributing

For people who want to build the game themselves or help develop it.

| | |
|---|---|
| [Contributing](../CONTRIBUTING.md) | How to help, and the rules of the repository |
| [Build from source](build/README.md) | Tools per system, `just build`, and every step by hand |
| [Running and options](build/running.md) | Every command-line option and setting |
| [Testing and CI](build/testing.md) | The test suites, local CI, GitHub CI |
| [Releases](build/releases.md) | How a release is cut ([checklist](release-checklist.md)) |
| [Roadmap](ROADMAP.md) | The milestones, from foundation to v1.0 |

## How it works

The technical reference: how the original game is built, and how the port is designed.

| | |
|---|---|
| [Architecture](ARCHITECTURE.md) | The design of the port: recompiler, runtime, frontends, build |
| [Analysis](ANALYSIS.md) | Facts about the original program and its data |
| [Decisions (ADRs)](adr/) | Every significant design decision, with the options considered and why |
| [File formats](formats/README.md) | Specs of the game's file formats, each with a tested parser |
| [Reverse-engineering notes](re/) | How parts of the original game work: audio, saves, scripts, program flow, … |
| [Control details](controls/) | The on-screen key pad, the keyboard layout's reasoning, remapping |
| [Playtest scripts](testing/) | Step-by-step runs used to check the game by hand |
