# 126. A dev branch, release pull requests, and CI chosen by the target branch

- **Status:** Accepted
- **Date:** 2026-11-07
- **Issue:** none (asked by Joris in session, 2026-11-07)
- **Revises:** [ADR 0006](0006-continuous-integration.md) (triggers and matrix), [ADR 0023](0023-local-ci-and-packaging.md) (`scripts/local-ci` is no longer required before a push), [ADR 0024](0024-release-contents-and-flow.md) and [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (the tag, the draft and the kits come from CI)
- **Builds on:** [ADR 0100](0100-public-repository-and-player-releases.md) (CI never sees the IPA)

## Context

Since #113 (ADR 0006 addendum) every check had to run locally first: `scripts/local-ci` on the Mac (lint, both Pythons, macOS, the Linux container) before every push, with CI only confirming. Two things went wrong with that:

- **The wait.** A full `scripts/local-ci` run takes a long time on the Mac (the Linux job is an x86_64 container under Rosetta), and every child, wave and epic paid it.
- **CI was red anyway.** Of the CI runs from 2026-10-26 to 2026-11-06 most failed, nearly all on Windows: MSVC's `/W4 /WX` rejected code that clang and GCC accepted (C4244, int to float, in `tests/cpp/ui_scaling_test.cpp`; C4459, a declaration hiding a global, in `runtime/src/audio/audio.cpp`). Windows runs only on Joris's PC or in CI, so nothing local caught it, and it showed up in the epic's pull request after the local run had passed. The rest were Python 3.10 failures of a cleanup race, fixed by #247.

Meanwhile the reason for running locally went away: the repository is public since the release preparation (ADR 0100), and GitHub-hosted standard runners are free for public repositories, macOS and Windows included. Their minute multipliers apply only to private repositories' quota.

Joris asked for a flow where the **target branch** decides what runs: cheap checks while work flows in, the full set of platforms when a release is cut, and the release itself made by CI from a tag named in the release pull request.

## Options considered

1. **CI on every push to every branch, full matrix.** Free, and Windows results arrive about 12 minutes after a push. But every push builds three OSes for work that may not be ready, and there is no release step; it only moves the wait from the Mac to GitHub.
2. **Keep local-first (ADR 0023), add a Windows machine to the loop.** Needs Joris's PC online for every child; doesn't scale to parallel workers.
3. **A `dev` integration branch; CI by target; releases by pull request into `main`** (chosen). Feature and epic pull requests go into `dev` and get a quick check. A pull request from `dev` into `main` titled `Release vX.Y.Z` runs every platform and, once merged, tags and drafts the release and builds the kits.

## Decision

**Branches.**

- `dev` is the integration branch and the repository's default branch: epic branches start from it, and every epic, child or fix pull request targets it. `Closes #N` closes issues when the pull request merges into `dev`, the default branch.
- `main` only moves through **release pull requests**: from `dev`, titled `Release vX.Y.Z` (or `Release vX.Y.Z-rc.N`, a pre-release). `main` is always the last release.

**CI by target** (`.github/workflows/ci.yml`). Pushes trigger nothing; the pull request's base decides:

| Pull request into | Runs |
|---|---|
| `dev` | lint + Python 3.10 and latest (Linux), and the Linux Debug build with the C/C++ tests |
| `main` | lint + Python, Debug **and** Release builds with the C/C++ tests and the packaged kits on macOS, Windows and Linux |
| manual run | the full set, Release unless its `release` input is off; optionally on self-hosted runners |

A pull request that only touches documentation or compendium work still skips the build jobs (ADR 0006 addendum, `scripts/ci-scope`).

**MSVC's warnings on the Mac.** `maplethief_target_defaults` (top-level `CMakeLists.txt`) adds `-Wshadow`, `-Wimplicit-int-float-conversion` and `-Wfloat-conversion` for clang, the nearest matches of the MSVC warnings that broke Windows before (C4459, C4244). Clang's int-to-float warning is slightly stricter than MSVC's (it also flags comparisons); the 22 sites it found were fixed. Most Windows breaks now fail the Mac build, before a release pull request.

**Releases** (`.github/workflows/release.yml`).

- While a pull request into `main` is open (and on every edit), the `release title` check fails unless it comes from `dev` of this repository, its title is `Release vX.Y.Z[-rc.N]`, and that tag doesn't exist yet.
- When it is merged, `tag and draft` creates the annotated tag (`MapleThief <version>`) on the merge commit through the API and the draft release: the committed notes header above the generated notes, a pre-release for `-rc.N`.
- `kit <os>` then builds each kit of ADR 0024 on a hosted runner from the tag, with `MAPLETHIEF_VERSION` from the tag: `mac-release-universal` and `win-release` workflows, and the Linux kit in the pinned container (`scripts/test-linux HEAD --release`, ADR 0023/0065). Each kit is checked as `scripts/release build` checks it (macOS: `lipo`, `codesign --verify --deep --strict`; every OS: `--version` prints the tag's version, Linux inside the container), passes the content guard in kit mode, gets its `.sha256`, and is uploaded to the draft.
- **The player builds stay local.** They link the code recompiled from the IPA (`work/gen`), which CI never has (ADR 0100). After the merge, `scripts/release build TAG --players-only` on the Mac and `scripts\release.ps1 build TAG -PlayersOnly` on the Windows PC build, check, guard and upload them to the same draft; `scripts/release publish TAG` then checks all six files, writes `SHA256SUMS` and publishes, as before.
- `scripts/release start` and a `build` without `--players-only` still work, for a release cut entirely by hand.

**Local checks.** Before pushing, the author runs what is quick on the Mac: `scripts/lint`, and the affected build and tests (`cmake --workflow --preset mac-debug`, or `scripts/test`), plus the real-asset tests the change touches (CI never has the IPA). `scripts/local-ci` stays as an optional tool (the Linux container, a clean checkout); it is no longer required before a push or a pull request. CI is the gate: a pull request is merged only when its checks are green.

## Consequences

- A feature pull request waits for one Linux build and the Python jobs (about 8 minutes), not for a local Rosetta container run.
- macOS and Windows are first tested on the release pull request. A break there blocks the release, not the work in `dev`; the clang warnings above make it rarer. A manual run of `ci.yml` on `dev` tests all three OSes at any time.
- `main` gets no Debug run after the merge: the release pull request ran on the same tree.
- The tag and the draft are made by `github-actions[bot]` with the workflow's token, which can't trigger other workflows; nothing relies on that.
- The Linux kit's container image is built on the runner on each release (snapshot packages; `UBUNTU_SNAPSHOT` as in ADR 0023 if the snapshot service is down).
- The player builds could also move to CI later, on self-hosted runners that hold `work/gen` (Joris's Mac and PC, ADR 0006 addendum), triggered only by the merged release pull request, never by a pull request's code.
- Hotfixes go through `dev` too: fix in `dev`, then a `Release vX.Y.Z+1` pull request.
- The compendium site still deploys on a push to `main` (`compendium.yml`, ADR 0121), so it now follows the releases; a manual run of `compendium.yml` deploys it in between.
