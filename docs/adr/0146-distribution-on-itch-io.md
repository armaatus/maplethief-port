# 146. Distribution on itch.io: every published player build pushed with butler, "$0 or donate"

- **Status:** Proposed
- **Date:** 2026-11-13
- **Issue:** [#208](https://github.com/armaatus/maplethief-port/issues/208) (epic [#106](https://github.com/armaatus/maplethief-port/issues/106))
- **Builds on:** [ADR 0100](0100-public-repository-and-player-releases.md) (what a release may carry, the name, the withdrawal plan), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (the player builds, the guard's player mode, the release flow), [ADR 0126](0126-dev-branch-release-pull-requests-and-ci-by-target.md) (release pull requests), [ADR 0003](0003-dependencies.md) (tools pinned by version and hash)

## Context

Since ADR 0125 a published GitHub Release carries a player build per platform: the runtime and the recompiled code, none of the game's assets, the player's own IPA imported on first launch. #208 adds a page on itch.io where players download them, with an optional donation. The point of the page (Joris, 2026-11-13) is to make getting **every** player build easy: the desktop builds that import the player's IPA on first launch, and the Nintendo Switch zip, all on one page that the itch app can install and keep up to date. On GitHub they sit among the kits, `.sha256` files and notes. Facts and constraints:

- **butler** is itch.io's upload tool. `butler push <file or folder> <user>/<game>:<channel> --userversion <v>` uploads a build to a channel (one per platform), sending only what changed; the itch app then updates installed copies. `butler status <user>/<game>` lists each channel's latest build and version. It authenticates with an API key, read from `BUTLER_API_KEY`. broth.itch.zone serves each version as a zip per OS (`/butler/<os>-<arch>/<version>/archive/default`). Version 15.32.0 (2026-10-06) has builds for darwin-arm64, darwin-amd64, linux-amd64 and windows-amd64; the zips' SHA-256 were stable across two downloads (2026-11-13).
- **No API for the page or for devlogs.** Both are written in itch.io's web editor. The repository can hold the page text; Joris pastes it in.
- **"$0 or donate"** pricing lets anyone download for free and shows a skippable "support the developer" box. Receiving money needs a payout method and itch.io's tax form in Joris's account.
- **Money is new.** ADR 0100 accepted the risk of downloads that carry a translation of Nexon's program; it did not consider taking money next to them.
- **Releases are cut locally** after the kits come from CI (ADR 0126): `scripts/release build TAG --players-only` on the Mac and `scripts\release.ps1 build TAG -PlayersOnly` on Windows, then `scripts/release publish TAG`. Publishing writes `SHA256SUMS` over every file.
- **#94 versus ADR 0100 and ADR 0125.** #94 (v1.0.0, open) is titled "private release: own-code kits only, never game builds". Its body predates ADR 0100 (2026-11-03): it still says game builds are never shared and that a public distribution would need its own legal decision and ADR. ADR 0100 is that decision (Joris chose the player releases on 2026-11-03), ADR 0125 (Accepted, 2026-11-06) built them, and the repository is meant to be public. A push to itch.io makes the same files available on a second public host.
- **Scope change (Joris, 2026-11-06, on #208):** no devlog per version. The page gets one launch post in Joris's own words; release history stays in the GitHub Release notes.

## Options considered

### What is pushed

1. **The published GitHub Release's player builds, downloaded again, checked against `SHA256SUMS` and the guard's player mode, then pushed as they are.** **Chosen.** itch.io carries exactly the files that went through ADR 0125's checks and are on GitHub, never a build made only for itch.io. Pushing after `publish` means `SHA256SUMS` exists and the release is public before the page changes.
2. **Push from `build/release/TAG/` right after `build`.** One download less, but a file could reach itch.io before the release is published, or one that `publish` later rejected. Rejected.
3. **Build again for itch.io** (a channel per preset). Another build that the guard and the checksums would have to cover. Rejected.

### Channels and pre-releases

1. **One channel per player build, named after the file's platform: `macos-universal`, `windows-x64`, `linux-x86_64` and `switch`. `-rc.N` is not pushed.** **Chosen.** The names match `mtport.release.player_names()`, so the tool finds a channel's file without a table that could drift. Release candidates stay on GitHub for testers; the page shows only final versions, and the itch app never updates a player to a candidate.
2. **Only the desktop builds; the Switch zip stays on GitHub** until it is checked on a console (#243, #245). The Switch players would then have to find the zip among the kits, which is what the page is for. Rejected; the page says plainly that the Switch build is not checked on a console yet, as [Playing on the Nintendo Switch](../play/switch.md) does.
3. **Separate `*-beta` channels for `-rc.N`.** More channels on the page, and a candidate that a player installs by mistake. Rejected; it can be added later without changing the stable channels.

The Switch zip is copied to an SD card, not run on the computer, so its upload gets no platform tick on itch.io (which has none for consoles) and the itch app doesn't install it; the page names it "Nintendo Switch (homebrew)".

### butler

1. **Pinned to 15.32.0, downloaded per OS from broth.itch.zone and checked by SHA-256** into `build/_downloads/butler-<version>-<os>-<arch>/`, as other tools are (ADR 0003). **Chosen.**
2. **Whatever `butler` is on `PATH`.** Not reproducible and not checked. Rejected; `--butler EXE` exists for the tests' fake butler only.

### Pricing and donations

1. **"$0 or donate" with a suggested amount, plus a Buy Me a Coffee link in the page text.** **Chosen** (#208, Joris). The download stays free and the box can be skipped.
2. **Free, no donations.** The lowest risk, and the fallback: the first step of the withdrawal plan below.
3. **A price.** Selling a port of Nexon's game. Rejected.

### Screenshots

1. **Screenshots of gameplay, captured by Joris from his own local build, uploaded to itch.io only, never committed.** **Chosen** (Joris, 2026-11-06). Players need to see what they download. They show Nexon's art, which is the reason they never enter the repository (CLAUDE.md) and are the first thing removed on an objection.
2. **No screenshots.** The cleanest, and a page that says little. Kept as the fallback.

The cover (630 x 500) and banner are our own art, like the icon (ADR 0125): no Nexon logo, no game art.

## Decision

- **What is pushed:** every player build of the published release, byte-identical: `MapleThief-<version>-macos-universal.zip`, `-windows-x64.zip`, `-linux-x86_64.tar.gz` and `-switch.zip`. Before any push, `python3 -m mtport.release itch TAG` (`tools/mtport/itch.py`):
  - refuses a pre-release tag;
  - refuses a release that is a draft or a pre-release on GitHub, or that lacks a requested player build or `SHA256SUMS`;
  - downloads them with `gh release download` into `build/release/TAG/itch/`;
  - refuses a file not listed in `SHA256SUMS` or with another SHA-256;
  - runs the guard's player mode on every file (`mtport.release guard --mode player`, with the machine's `work/` references and dispatch table as in `publish`).
  Nothing is pushed unless every file passes.
- **The pushes:** `butler push <file> armaatus/maplethief:<channel> --userversion <version>` per channel, then `butler status armaatus/maplethief`. The page is `armaatus.itch.io/maplethief`.
- **Where:** `scripts/release itch TAG` on the Mac pushes `macos-universal`, `linux-x86_64` and `switch` (the Mac builds those three); `scripts\release.ps1 itch TAG` on Windows pushes `windows-x64` (`--platforms` / the tool's option picks others). `--dry-run` (`-DryRun`) does every check and prints the butler commands instead, so it needs neither butler nor the key. `--from DIR` checks local files instead of the published release (a rehearsal).
- **The key:** butler reads `BUTLER_API_KEY` from the environment. It is never written to the repository, a file or a command line, and the tool refuses to push without it.
- **butler:** 15.32.0, pinned per OS by SHA-256 in `mtport.itch.BUTLER`, downloaded once into `build/_downloads/`.
- **The page text** is ours, in `docs/itch/page.md` (what it is, the player's own file and its SHA-256, which download, install steps per OS, controls, known issues, credits, the legal note, links), with the form's short fields in `docs/itch/metadata.md`. The one devlog post is Joris's launch post, in his own words; the repository generates none.
- **Pricing:** "$0 or donate", and a Buy Me a Coffee link (https://buymeacoffee.com/armaatus). Comments on.
- **Screenshots:** gameplay only, from Joris's local build, on itch.io only, never committed. Cover and banner: our own art.
- **The name:** as ADR 0100. "MapleStory" only descriptively ("a port of MapleStory Thief Edition"), the page title "MapleThief", no Nexon logo or branding, and the page says it is not affiliated with Nexon and includes no game content.
- **On an objection** from Nexon, in this order: donations off (the page becomes free), then the screenshots removed, then the itch.io downloads withdrawn together with the GitHub Release's player builds (ADR 0100). The source stays. Money received is not a reason to keep anything up.
- **#94:** ADR 0100 and ADR 0125 hold over #94's wording. #94 was written when releases could carry only own-code kits; ADR 0100 is the later, separate legal decision #94 itself asked for, and ADR 0125 made the player builds part of every release. So a published release, v1.0.0 included, carries the player builds, and this ADR pushes exactly those. This ADR stays Proposed until Joris confirms that reading (the open question on #208); #94's text is his to update.

## Consequences

- A release reaches players on itch.io with one command per machine after `publish`, and the itch app updates installed copies.
- itch.io never carries anything that has not passed the checks that the GitHub Release's files passed; a tampered or missing download stops the push.
- Taking donations next to the recompiled code raises the stakes of ADR 0100's accepted risk. The plan above puts the money first in line to go.
- **What players download from the page is served by itch.io from the pushed build.** butler unpacks a pushed zip and stores its contents; whether the page's download is then the same bytes as the GitHub file is what #208's needs-human check (`shasum -a 256` against `SHA256SUMS`) answers. If it is not, the fix is to push each file inside a folder of its own, so the build's one file is the archive itself; the itch app would then install the archive rather than the game, which is why the plain file is pushed first.
- The release machines need the network for the butler download once, and `BUTLER_API_KEY` set by Joris (`needs-human`).
- **How we'd know this was wrong:** a notice from Nexon or from itch.io, a page download whose SHA-256 is not in `SHA256SUMS` with no explanation, or players installing release candidates.
