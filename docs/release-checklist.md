# Release checklist

How to cut a release: a tag becomes a GitHub Release with one port kit per platform, `SHA256SUMS` and generated notes. The kits hold only our own code (runtime, platform shell, SDL3, recompiler, tools, scripts, docs, licences), never the game build. The design is [ADR 0024](adr/0024-release-contents-and-flow.md). Everything runs on Joris's Mac and Windows PC, without GitHub Actions. [ADR 0100](adr/0100-public-repository-and-player-releases.md) adds player builds (the recompiled code, none of the game's assets) next to the kits; until [#182](https://github.com/armaatus/maplethief-port/issues/182) adds them to this flow, a release is kits only.

Tags are annotated SemVer: `vX.Y.Z`, or `vX.Y.Z-rc.N` for a pre-release. Below, `TAG` is the tag, for example `v0.0.1-rc.1`.

## Before

- [ ] Everything for the release is merged into `main`, and `scripts/local-ci` was green on it (the PR lists it).
- [ ] The epic PRs since the last release carry their epic's labels (`gh pr edit N --add-label type:epic,area:...`), so the generated notes group them ([`.github/release.yml`](../.github/release.yml)). Unlabelled PRs land in "Other changes".
- [ ] On both machines: `gh auth status` is logged in with access to `armaatus/maplethief-port`; the checkout has `work/` (`scripts/bootstrap`, and `python3 -m recomp all` for `work/gen/`), so the content guard also checks the IPA's hashes and the dispatch table; Docker (OrbStack) runs on the Mac.
- [ ] Optional rehearsal on the Mac, with nothing sent to GitHub: `git tag -a v0.0.0-rc.99 -m rehearsal`, `scripts/release build v0.0.0-rc.99 --dry-run`, `scripts/release publish v0.0.0-rc.99 --dry-run --platforms mac,linux`, then `git tag -d v0.0.0-rc.99`.

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

It checks a clean tree on `main`, that `TAG` is annotated, on `origin/main` and pushed, runs `scripts/local-ci TAG` (lint, Python 3.10, macOS and the Linux container, Debug), and creates the draft release with generated notes, as a pre-release for `-rc.N`. Read the notes on the draft and edit them on GitHub if needed.

## 3. Kits

On the Mac (macOS universal and Linux x86_64):

```sh
scripts/release build TAG
```

On the Windows PC, from the x64 Visual Studio developer prompt:

```powershell
git fetch origin --tags
powershell -ExecutionPolicy Bypass -File scripts\release.ps1 build TAG
```

Each builds the kits from a clean checkout of the tag with `MAPLETHIEF_VERSION` from it (Mac: `scripts/local-ci TAG --release`; Windows: `scripts\test.ps1 -Release`), unpacks each kit and checks that `MapleThief --version` prints `MapleThief <version>` (on the Mac also `lipo -archs` and `codesign --verify`; the Linux kit runs in the container), runs the content guard, writes `<kit>.sha256` and uploads both to the draft. A second upload of the same file fails; `--replace` (`-Replace`) replaces it. Everything also stays in `build/release/TAG/`.

## 4. Publish (Mac)

```sh
scripts/release publish TAG
```

It checks that the macOS, Linux and Windows kits and their `.sha256` are on the draft, downloads them to `build/release/TAG/download/`, checks each against its `.sha256`, runs the guard again, writes `SHA256SUMS`, checks it with `shasum -a 256 -c`, uploads it and publishes the release.

## After

- [ ] On the release page: three kits, three `.sha256`, `SHA256SUMS`, the notes, and "Pre-release" for `-rc.N`.
- [ ] Download the assets into an empty directory and run `shasum -a 256 -c SHA256SUMS` (`sha256sum -c SHA256SUMS` on Linux).
- [ ] Unpack each kit and run `MapleThief --version` (on macOS `MapleThief.app/Contents/MacOS/MapleThief --version`; on Windows `bin\MapleThief.exe --version | Out-String`): it prints the tag's version.

## If something goes wrong

- A failed `build` uploads nothing. Fix the cause on `main`; if the fix needs a new commit, delete the draft and the tag (`gh release delete TAG --cleanup-tag`) and start again with the next `-rc.N` rather than moving a pushed tag.
- A wrong kit on the draft: rebuild and upload with `--replace`.
- The guard rejected a kit: never upload it by hand. Find which install rule put the file there (`cmake/Packaging.cmake`) and fix that.
