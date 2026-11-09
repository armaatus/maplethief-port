# Releases

How a release is cut. The step-by-step version, with what each step checks and what to do when one fails, is the [release checklist](../release-checklist.md). This page is the overview.

## What a release holds

A release is a GitHub Release with, for each desktop platform, a **player build** and a **port kit**, plus a `.sha256` for each, one `SHA256SUMS` over all of them, and generated release notes ([ADR 0024](../adr/0024-release-contents-and-flow.md), [ADR 0125](../adr/0125-player-builds-icon-guard-modes-and-release-flow.md)).

- A **player build** is what people download to play: the game with its code recompiled, and none of the game's art, music, maps or text. On first launch the player imports their own `MapleThief-1.5.ipa`, and the game copies the game's files into `game/` next to itself. Saves and settings go in `data/` beside it ([ADR 0127](../adr/0127-data-folder-next-to-the-game.md), [Playing MapleThief](../play/README.md)).
- A **kit** holds only our own code, built from the tag: the platform shell without the game, the runtime and SDL3 libraries and headers, the recompiler, the Python tools and scripts, the docs and the licences. It never holds a game build. Each user builds that from their own IPA.

| File | Built on | By |
|---|---|---|
| `MapleThief-<version>-macos-universal.zip` | the Mac | `scripts/release build TAG --players-only` |
| `MapleThief-<version>-linux-x86_64.tar.gz` | the Mac, in the glibc 2.39 container | `scripts/release build TAG --players-only` |
| `MapleThief-<version>-windows-x64.zip` | the Windows PC | `scripts\release.ps1 build TAG -PlayersOnly` |
| `MapleThief-kit-<version>-macos-universal.zip` | hosted CI | `.github/workflows/release.yml` |
| `MapleThief-kit-<version>-linux-x86_64.tar.gz` | hosted CI, in the Linux container | `.github/workflows/release.yml` |
| `MapleThief-kit-<version>-windows-x64.zip` | hosted CI | `.github/workflows/release.yml` |

The player builds stay on local machines because they link the code recompiled from the IPA (`work/gen`), which CI never has ([ADR 0100](../adr/0100-public-repository-and-player-releases.md)).

A content guard (`python3 -m mtport.release guard`) checks every file before it's uploaded and again before publishing. Kits are checked in kit mode: no generated code, nothing from `work/`, `assets-out/` or `build/`, nothing identical to a file of the IPA, and no images or audio apart from the port's own icon. Player builds are checked in player mode: the recompiled code, but none of the game's files. The guard's rules are in [ADR 0024](../adr/0024-release-contents-and-flow.md) and [ADR 0125](../adr/0125-player-builds-icon-guard-modes-and-release-flow.md).

## The flow

Tags are annotated SemVer: `vX.Y.Z`, or `vX.Y.Z-rc.N` for a pre-release. A release starts as a pull request from `dev` into `main` ([ADR 0126](../adr/0126-dev-branch-release-pull-requests-and-ci-by-target.md)):

```sh
gh pr create --base main --head dev --title "Release TAG" --body "Release TAG"
```

CI runs every OS, Debug and Release, and packages the kits. A `release title` check fails unless the pull request comes from `dev`, its title is `Release vX.Y.Z[-rc.N]`, and that tag doesn't exist yet. Merge it with a merge commit when everything is green.

The merge starts `release.yml`, which creates the annotated tag on the merge commit and the draft release, with the notes header ([`.github/release-notes-header.md`](../../.github/release-notes-header.md)) above GitHub's generated notes. It then builds the three kits from the tag on hosted runners, checks each, runs the guard, and uploads them to the draft.

Then build the player builds on the two machines that have `work/gen` (`scripts/bootstrap`, then `python3 -m recomp all`), and publish:

```sh
scripts/release build TAG --players-only     # Mac: macOS universal and Linux x86_64
scripts\release.ps1 build TAG -PlayersOnly   # Windows PC, from the x64 developer environment
scripts/release publish TAG                  # Mac: every file present, SHA256SUMS, publish
```

Each `build` works from a clean checkout of the tag, with `MAPLETHIEF_VERSION` taken from the tag, and unpacks every file to check it (`--version` prints the tag's version; `lipo` and `codesign` on the Mac; on Windows no MSVC runtime DLL in the player build). `publish` checks that all six files and their `.sha256` are on the draft, downloads and verifies them, guards each again, writes `SHA256SUMS` and publishes.

Options: `--dry-run` runs every local step and prints the GitHub commands instead, so the flow can be rehearsed on a local tag. `--replace` (`-Replace`) replaces a file already on the draft. `--platforms` narrows what `build` makes and what `publish` requires. Everything also stays in `build/release/TAG/`.

`scripts/release start TAG` and a `build` without `--players-only` still work, for a release cut entirely by hand from a tag on `main`. The checklist says how.

## Checking a download

```sh
shasum -a 256 -c SHA256SUMS      # macOS
sha256sum -c SHA256SUMS          # Linux
```

On a Mac, a downloaded app is quarantined until you allow it: [Mac](../play/README.md#mac) in the player guide.
