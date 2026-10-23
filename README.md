# maplethief-port

A native port of **MapleStory Thief Edition** (Nexon Mobile, iOS, 2010; version 1.5) built from my own legally owned, decrypted IPA. It's a personal preservation project.

**Status: pre-work done, implementation not started.** The repo, docs, issues and project board are set up. The next step is milestone M0 (foundation).

## Targets

| Platform | Notes |
|---|---|
| macOS 13+ | Universal binary (Apple Silicon + Intel), ad-hoc signed `.app` |
| Windows 10/11 x64 | Portable zip |
| iOS 16+ arm64 | Sideloaded via Xcode |
| Linux / Steam Deck | Bonus, where it's cheap |

Planned features: full **controller support** (direct action mapping, remapping, glyphs), keyboard/mouse play, scaling modes, and opt-in quality-of-life and display enhancements. The original game logic is kept intact.

## How it works

**Hybrid static recompilation.** The armv7 code from the original binary (2,655 functions, mostly Thumb-2) is translated ahead of time into C. A high-level runtime then implements the 333 iOS imports on the host: libc/C++, CoreFoundation/Foundation, the Objective-C runtime, GCD, CoreGraphics, OpenGL ES 1.1 → a modern renderer, OpenAL/AVAudioPlayer, UIKit input, and offline stubs for ads/IAP/Game Center. SDL3 provides windowing, input, gamepads and audio on every platform. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build from your own copy

The repo contains exactly one game input: [`original/MapleThief-1.5.ipa`](original/README.md) (Git LFS), plus checksums. **Everything else is generated from it locally by scripts and is never committed**: extracted files, converted assets, recompiled code and binaries. The game's code and assets are Nexon's property. This repo stays private, and builds are never shared publicly.

```sh
git clone git@github.com:armaatus/maplethief-port.git
cd maplethief-port
git lfs install && git lfs pull
scripts/verify-original.sh      # checks SHA-256s, extracts to work/ipa/
```

Build instructions will appear here as M0 lands ([#2](https://github.com/armaatus/maplethief-port/issues/2), [#4](https://github.com/armaatus/maplethief-port/issues/4)).

## Repository layout

| Path | Purpose |
|---|---|
| `original/` | The IPA (LFS) + `SHA256SUMS`, the single source of truth |
| `docs/` | [Analysis](docs/ANALYSIS.md), [architecture](docs/ARCHITECTURE.md), [roadmap](docs/ROADMAP.md) |
| `docs/formats/` | [File format specs](docs/formats/README.md) |
| `docs/adr/` | Architecture decision records |
| `docs/re/` | Reverse-engineering notes |
| `tools/` | Extractor, format parsers, converters, viewers, Ghidra scripts |
| `recomp/` | Static recompiler |
| `runtime/` | HLE runtime |
| `platform/` | SDL3 frontends per OS |
| `tests/` | Unit, recompiler-snippet and golden-image tests |
| `scripts/` | Bootstrap / verification / build helpers |
| `work/`, `assets-out/`, `build/` | Generated, git-ignored |

## Planning

- **Roadmap:** [docs/ROADMAP.md](docs/ROADMAP.md), milestones M0–M11 from foundation to v1.0
- **Project board:** [MapleThief Port](https://github.com/users/armaatus/projects/2) (Backlog → Ready → In progress → In review → Done)
- **Issues:** one epic per milestone, plus 94 task issues with goals, tasks, acceptance criteria and blockers

## How the AI agent works through this repo

The port is built by an AI agent working **one issue at a time** in milestone order. It picks the lowest-numbered unblocked issue, posts a plan, works on an `N-slug` branch, and opens one PR per issue whose body includes `Closes #N` and the evidence for each acceptance criterion. Design decisions become ADRs, file formats get specs plus tested parsers, and anything that needs me (device testing, signing, playthroughs) is labelled `needs-human`. The full rules are in [CLAUDE.md](CLAUDE.md) (also available as `AGENTS.md`).
