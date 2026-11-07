# Release checklist

How to cut a release. A tag becomes a GitHub Release that carries, per platform, a **player build** and a **port kit**, plus `SHA256SUMS` and notes:

- The **player builds** are what people download to play: the game with its code recompiled, and none of the game's files. The player imports their own IPA on first launch. They are named `MapleThief-<version>-macos-universal.zip`, `MapleThief-<version>-windows-x64.zip` and `MapleThief-<version>-linux-x86_64.tar.gz` ([ADR 0125](adr/0125-player-builds-icon-guard-modes-and-release-flow.md)).
- The **kits** hold only our own code: the runtime, the platform shell without the game, SDL3, the recompiler, the tools, the scripts, the docs and the licences. They are named `MapleThief-kit-<version>-<os>-<arch>` ([ADR 0024](adr/0024-release-contents-and-flow.md)).

The content guard checks every file before it is uploaded and again before publishing: kits in kit mode (no recompiled code at all), player builds in player mode (the recompiled code, but no image, sound, bundle file or anything identical to a file of the IPA). Everything runs on Joris's Mac and Windows PC, without GitHub Actions. The decisions are in [ADR 0024](adr/0024-release-contents-and-flow.md), [ADR 0100](adr/0100-public-repository-and-player-releases.md) and ADR 0125.

Tags are annotated SemVer: `vX.Y.Z`, or `vX.Y.Z-rc.N` for a pre-release. Below, `TAG` is the tag, for example `v0.0.1-rc.1`.

## Before

- [ ] Everything for the release is merged into `main`, and `scripts/local-ci` was green on it (the PR lists it).
- [ ] The epic PRs since the last release carry their epic's labels (`gh pr edit N --add-label type:epic,area:...`), so the generated notes group them ([`.github/release.yml`](../.github/release.yml)). Unlabelled PRs land in "Other changes".
- [ ] [`.github/release-notes-header.md`](../.github/release-notes-header.md), the text above the generated notes, still says the right thing.
- [ ] On both machines: `gh auth status` is logged in with access to `armaatus/maplethief-port`. The checkout has `work/` (`scripts/bootstrap`, then `python3 -m recomp all` for `work/gen/`), up to date with `main`'s recompiler. The player builds link `work/gen/`, and the guard checks the IPA's hashes and the dispatch table. Docker (OrbStack) runs on the Mac.
- [ ] Optional rehearsal, with nothing sent to GitHub: `git tag -a v0.0.0-rc.99 -m rehearsal`, then `scripts/release build v0.0.0-rc.99 --dry-run` and `scripts/release publish v0.0.0-rc.99 --dry-run --platforms mac,linux` on the Mac, or `scripts\release.ps1 build v0.0.0-rc.99 -DryRun` on Windows. Then `git tag -d v0.0.0-rc.99`.

## 1. Tag (Mac)

```sh
git switch main && git pull --ff-only
git tag -a TAG -m "MapleThief TAG"
git push origin TAG
```

## 2. Draft (Mac)

```sh
scripts/release start TAG
```

It checks for a clean tree on `main`, and that `TAG` is annotated, on `origin/main` and pushed. It runs `scripts/local-ci TAG` (lint, Python 3.10, macOS and the Linux container, Debug) and creates the draft release, as a pre-release for `-rc.N`. The draft's notes start with the header (what each download holds, and that players need their own IPA), followed by GitHub's generated notes. Read them on the draft and edit them on GitHub if needed.

## 3. Builds

On the Mac (macOS universal and Linux x86_64):

```sh
scripts/release build TAG
```

On the Windows PC, from the x64 Visual Studio developer prompt:

```powershell
git fetch origin --tags
powershell -ExecutionPolicy Bypass -File scripts\release.ps1 build TAG
```

Each script builds from a clean checkout of the tag, with `MAPLETHIEF_VERSION` taken from the tag.

The kits:

- Mac: `scripts/local-ci TAG --release`.
- Windows: `scripts\test.ps1 -Release`.

The player builds use this checkout's `work/gen/`, linked in read-only:

- Mac: the `mac-player` workflow.
- Linux: `scripts/linux-game player TAG`, in the glibc 2.39 container.
- Windows: the `win-player` workflow.

Then each script unpacks every file and checks it:

- `--version` must print `MapleThief <version>`.
- On the Mac, `lipo -archs` must show arm64 and x86_64, and `codesign --verify --deep --strict` must pass.
- The Linux files run in the container.
- On Windows, `dumpbin /dependents` must list no MSVC runtime DLL in the player build.

Last, it runs the content guard (kits in kit mode, player builds in player mode), writes a `.sha256` for each file and uploads everything to the draft. A second upload of the same file fails; `--replace` (`-Replace`) replaces it. Everything also stays in `build/release/TAG/`.

## 4. Publish (Mac)

```sh
scripts/release publish TAG
```

It checks that the draft has all six files and their `.sha256`: the macOS, Linux and Windows kits and player builds. It downloads them to `build/release/TAG/download/` and checks each against its `.sha256`. It runs the guard again (each file in its mode), writes `SHA256SUMS` over all six and checks it with `shasum -a 256 -c`. Then it uploads `SHA256SUMS` and publishes the release.

## After

- [ ] On the release page: three player builds, three kits, six `.sha256`, `SHA256SUMS`, the notes with the header on top, and "Pre-release" for `-rc.N`.
- [ ] Download the assets into an empty directory and run `shasum -a 256 -c SHA256SUMS` (`sha256sum -c SHA256SUMS` on Linux).
- [ ] Unpack each file and run `MapleThief --version`: `MapleThief.app/Contents/MacOS/MapleThief --version` on macOS, `MapleThief.exe --version | Out-String` (player build) or `bin\MapleThief.exe --version | Out-String` (kit) on Windows, and `./MapleThief.sh --version` or `bin/MapleThief --version` on Linux. Each prints the tag's version.
- [ ] Each player build, unpacked into a fresh folder with an empty data directory, shows the import screen, imports your `MapleThief-1.5.ipa` and starts the game. On the Mac, also check the Gatekeeper prompt and the app on Apple silicon and under Rosetta (`arch -x86_64`). This is a `needs-human` check.

## If something goes wrong

- A failed `build` uploads nothing. Fix the cause on `main`. If the fix needs a new commit, delete the draft and the tag (`gh release delete TAG --cleanup-tag`) and start again with the next `-rc.N`, rather than moving a pushed tag.
- A wrong file on the draft: rebuild it and upload with `--replace`.
- The guard rejected a file: never upload it by hand. Find which install rule put the file there (`cmake/Packaging.cmake`, the player branch for player builds) and fix that. A player build that is rejected as "not a player build" was built without `work/gen/`, or on a machine without `work/gen/dispatch.c` for the guard.
