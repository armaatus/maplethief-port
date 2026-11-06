# 100. A public repository without the game, and player releases without the game's assets

- **Status:** Accepted
- **Date:** 2026-11-03
- **Issue:** #181 (follow-up: #182)
- **Revises:** [ADR 0024](0024-release-contents-and-flow.md) (what a release may contain)
- **Revised by:** [ADR 0121](0121-public-compendium-site.md) (the public compendium site, #211), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (the player builds, their names and contents, and the release flow that makes them, #182)

## Context

The repository was private and held one game file: `original/MapleThief-1.5.ipa`, through Git LFS. Everything else was already our own work or generated locally and git-ignored. Joris wants two groups to use the project:

- **Contributors** with their own IPA, who change the code and build it themselves.
- **Players**, who download a release and play.

ADR 0024 allowed only own-code kits and said a public release needs its own legal decision. This ADR records that decision. It is not legal advice, and it records a risk the project accepts.

What belongs to whom:

| Part | Whose | Copyright position |
|---|---|---|
| Recompiler, HLE runtime, platform shells, tools, tests, docs | Ours | Our own work, like an emulator |
| Addresses, names, file-format specs, reverse-engineering notes | Facts about the game | Not protected as such. Reverse engineering for interoperability is allowed in the EU (Directive 2009/24/EC art. 6, Auteurswet art. 45m) and was fair use in the US cases (Sega v. Accolade, Sony v. Connectix) |
| Generated C (`work/gen/`) and any binary linked with it | A mechanical translation of Nexon's program | A derivative of Nexon's code. Reverse engineering without their source doesn't change this |
| Art, audio, maps, tables, text (the IPA's bundle) | Nexon's | Never ours to distribute |

An audit of all 1,151 tracked files (#181) found no game content apart from the IPA and two short doc excerpts (a reconstructed function in `docs/re/audio.md`, a reconstructed quest script in `docs/re/script-vm.md`). Both were replaced.

Other recompilation and decompilation ports, such as Zelda64Recomp (N64Recomp) and Ship of Harkinian, ship the game's code in their release downloads and load the assets from the player's own copy on first launch. Their rights holders have tolerated them. Others have not been tolerated (the GTA re3/reVC takedown and lawsuit). Nexon has not ruled on this game.

## Options considered

1. **Own-code kits only** (ADR 0024 as it was). Legally the cleanest. Every player needs a compiler, Python and the command line, so in practice only developers can play.
2. **Releases with the recompiled code and no assets; the player supplies the IPA on first launch.** The Zelda64Recomp model. Anyone can play. The download carries a translation of Nexon's program: a risk accepted knowingly. **Chosen** (Joris, 2026-11-03).
3. **Releases with everything** (code and assets). Plain redistribution of the game. Rejected.

## Decision

- **The repository contains no game file.** The IPA is untracked and `*.ipa` is git-ignored. Each user puts their own copy at `original/MapleThief-1.5.ipa`; `original/SHA256SUMS` lets them check it. The guards in CI, `scripts/local-ci`, `scripts/test-linux` and `scripts/release.ps1` fail when any `*.ipa` (or a Git LFS object) is in a checkout. Before, they required an LFS pointer.
- **The history is rewritten** to drop the IPA from every commit before the repository goes public. GitHub Support is asked to purge the LFS object and the pull-request refs that still point at the old commits. This is a `needs-human` step, done by Joris.
- **Our code is MIT-licensed** (`LICENSE`). The README and [docs/legal.md](../legal.md) say what the project is and isn't: not affiliated with Nexon, "MapleStory" is Nexon's trademark, no game content included, bring your own copy.
- **Player releases (#182)** may contain the runtime and the recompiled code, never any of the game's assets: no file from the IPA's bundle (art, audio, maps, tables, text, the executable itself) and nothing converted from one. The recompiled code is the one thing derived from the IPA that a release carries: it is built from the executable, and the loader manifest compiled into it comes from the executable too. On first launch the game asks for the player's IPA, checks its SHA-256 and extracts the bundle into its data directory. Until #182 lands, releases stay as ADR 0024 describes: own-code kits.
- **Builds that bundle the assets** (`mac-app`, `scripts/linux-game`, texture dumps and packs) stay personal. They are never uploaded anywhere.
- **The docs never explain how to decrypt an IPA.** They say which file is needed and how to check it.
- **The name.** "MapleStory" is used only descriptively ("a port of MapleStory Thief Edition"). No Nexon logo or branding outside the player's own copy.

## Consequences

- Contributors clone, add their IPA and build. Nothing about the build changes except that `git lfs pull` is gone.
- A takedown notice stays possible. The repository's clean history and the separation (our code in git, the game only on the user's machine) is the position the project would defend. If Nexon objects to the player releases, they are withdrawn first and the source stays.
- ADR 0024's guard needs a second mode for the player builds (#182): generated code allowed, every asset still rejected.
- **How we'd know this was wrong:** a notice from Nexon, or a change in how comparable projects are treated.
