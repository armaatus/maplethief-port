# 23. Local CI and packaging

- **Status:** Accepted
- **Date:** 2026-10-27
- **Issue:** #113

## Context

The GitHub Actions minutes ran out on 2026-10-26, in the middle of epic #98. Until then CI ([ADR 0006](0006-continuous-integration.md)) was the only place where Linux x86_64 and MSVC were checked. The stand-in used for #98 was a Docker image built by hand (Ubuntu 24.04, aarch64, unpinned), `UV_PYTHON=3.10 scripts/test --python-only` and `scripts/lint`, with clang-cl syntax checks for Windows. Real MSVC caught bugs that clang didn't (C4113 in #30, C4459 in #35), so clang-cl is no substitute.

Issue #113 asks that every check CI runs, packaging included, can run locally through CMake presets: macOS and Windows natively on Joris's machines, Linux in Docker on the Mac. The same presets must produce the release files of #114. Constraints:

- macOS can't run in Docker, and Windows containers can't run on a Mac host, so those two run natively.
- CMake's minimum is 3.28 ([ADR 0002](0002-language-and-toolchain.md)); CPack's AppImage generator needs 4.2.
- Nothing derived from the IPA may end up in a package (CLAUDE.md guardrail, #114).
- A plain `git archive` applies Git LFS's smudge filter (the `.gitattributes` filter), so an export made with it contains the **real** 17 MB IPA. The #98 stand-in had that problem without noticing.
- On Apple silicon, x86_64 containers run under Rosetta. Ubuntu 26.04's GNU tar 1.35 fails there ("Cannot mkdir: Function not implemented"); CMake's own `cmake -E tar` and Python's `tarfile` work.

## Options considered

### Packaging

1. **CPack archive generators, an External script on macOS.** ZIP on Windows, TGZ on Linux. On macOS an External generator script zips the staged kit with `ditto -c -k --keepParent`, as Finder does, so the `.app`'s symlinks, extended attributes and code signature survive. Package presets for the three release presets, and a `package` step in their workflow presets. Accepted.
2. **CPack's ZIP generator on macOS too.** One generator fewer, but libarchive's zip doesn't promise to keep a bundle's symlinks and extended attributes, which a signature depends on. Rejected.
3. **A packaging script per OS outside CMake.** Duplicates what `install()` already knows, and `cmake --workflow` can't run it. Rejected.

### Linux in a container

1. **A committed Dockerfile, pinned, run on a clean export of a commit.** Base image by multi-arch index digest (`ubuntu:26.04`, x86_64 by default, arm64 as the fast option), apt packages from an Ubuntu archive snapshot, CMake 4.4.4 (what `ubuntu-latest` had on 2026-10-24) and PowerShell from their release archives and uv from its image, each hash-checked. It also installs what `ubuntu-latest` has and the tests use when present: clang (the recompiler tests prefer it as host compiler), pwsh (`tests/test_bootstrap.py` runs the `.ps1` scripts), Node.js. `CI=true`, and a non-root `runner` user, as on GitHub. Accepted.
2. **A self-hosted Linux runner only.** It needs Actions to schedule it and a token from Joris, and it doesn't help a worker that wants to check a commit before pushing. Kept as an option (ADR 0006 addendum), not the default.

### Local entry point

1. **`scripts/local-ci [REV]` on the Mac**, running CI's jobs on a clean checkout: lint, Python tests on 3.10, `scripts/test` on macOS on the latest Python, and the Linux container in parallel, with a per-job summary (status, duration, CTest and pytest counts) and a non-zero exit on any failure. Windows runs `scripts\test.ps1` on the PC. Accepted.
2. **Run the checks in the developer's own tree.** Uncommitted files, `work/` (real-asset tests that CI skips) and cached CMake options (`MAPLETHIEF_WARNINGS_AS_ERRORS` is read from `CI` only on the first configure) all make the result differ from CI's. Rejected.

## Decision

- **Install rules and CPack** live in `cmake/Packaging.cmake`, included last by the top-level `CMakeLists.txt`. The kit's top-level directory is `MapleThief-kit-<version>-<os>-<arch>` (`macos-universal`, `windows-x64`, `linux-x86_64`), and it holds:
  - the platform shell: `MapleThief.app` on macOS, ad-hoc signed again as a whole bundle at install time (the linker signs only the executable); `bin/MapleThief[.exe]` elsewhere;
  - `lib/` (the runtime and the static SDL3 library) and `include/` (the runtime headers and SDL3's);
  - `recomp/`, `tools/mtport/`, `tools/ghidra/`, `pyproject.toml`, `uv.lock` and the user scripts (`bootstrap`, `verify-original.sh`, `pipeline`, `difftest`, with their `.ps1` twins) in the repository's layout, so the scripts run unchanged. The developer scripts (`test`, `lint`, `local-ci`, `test-linux`, `re-analyze`) stay out;
  - `README.md`, `THIRD_PARTY.md`, `docs/` and `licenses/` (SDL3's licence; the shipped binaries link nothing else yet).

  `maplethief_gen` has no install rule, and nothing is installed from `work/` or `assets-out/`. Components (`platform`, `sdk`, `tools`, `docs`) are declared, but CPack packages them as one kit (`CPACK_MONOLITHIC_INSTALL`). Generators: External + `cmake/DittoZip.cmake` on macOS, ZIP on Windows, TGZ on Linux. The package is written to the build directory, `build/<preset>/`. #114 settles what a release kit finally contains ([ADR 0024](0024-release-contents-and-flow.md)); this ADR only fixes the mechanism.
- **Presets.** `packagePresets` `mac-release-universal`, `win-release` and `linux-release`; their workflow presets gain a `package` step, so `cmake --workflow --preset <os>-release` configures, builds, tests and packages. The Debug workflows don't package.
- **Run targets.** `run` starts `MapleThief`, `smoke` runs `MapleThief --smoke-frames 3` with `SDL_VIDEO_DRIVER=dummy`, outside CTest. CTest keeps the C/C++ tests and `platform.smoke`; pytest keeps the Python tests.
- **`scripts/test`** gains `--release` (the release workflow, a check that a kit was written, and on macOS `lipo -archs` showing both `arm64` and `x86_64`) and `--python LIST` (`latest` is the newest stable CPython uv finds installed, `default` is uv's own choice, anything else is passed to uv). Its default on a Mac outside CI, without `UV_PYTHON`, is `latest,3.10`, each in its own environment `build/python-env/<os>-<cpu>-py<python>`; elsewhere one run as before. **`scripts/test.ps1`** gains `-Release`.
- **`scripts/test-linux [REV] [--worktree] [--release] [--arch amd64|arm64] [--real-assets] [--build-arg …]`** builds `tools/containers/linux/Dockerfile` (tagged by a hash of the Dockerfile and the build arguments, so an unchanged image is reused), streams `git archive` of the commit with the LFS filter off into the container, unpacks it with Python's `tarfile`, runs the same IPA guard as CI, and runs `scripts/test` (or `--release`). `build/_downloads` is shared with the host; uv's cache is a Docker volume per architecture. CTest logs and `build/test-output/` come back to `build/test-linux/<arch>/logs/`, the kit to `build/linux-release/`. `--real-assets` mounts `work/` read-only. `--worktree` exports `git stash create` (tracked changes included, the stash list untouched).
- **`scripts/local-ci [REV] [--only lint,py-min,mac,linux] [--release] [--arch …] [--keep] [--build-arg …]`** clones the repository with `--shared` into `build/local-ci/<rev>/src`, checks out the commit with the LFS smudge filter off, runs the IPA guard, links the shared download cache, and runs each job with `CI=true`, the Linux one in parallel. Logs and the summary's inputs stay in `build/local-ci/<rev>/logs/`; a release run keeps the kits next to them; the clone is deleted unless `--keep`.
- **Agents** run `scripts/local-ci` before they report, and name what they couldn't run (Windows, or a job that couldn't start). CI confirms (CLAUDE.md, PR template).

## Consequences

- A worker can show its commit green on macOS, Linux x86_64 with GCC 15, both Pythons and the linters without spending Actions minutes. The remaining gap is MSVC, which needs Joris's PC (`needs-human`) or CI.
- The image follows `ubuntu-latest` only as far as the pins say. When the runner image moves (a new GCC, CMake or Python), the Dockerfile's pins are bumped by hand, one per commit; a local-vs-CI disagreement after such a move points there first.
- `snapshot.ubuntu.com` returned 503 for every snapshot on 2026-10-27, the day this was written. `--build-arg UBUNTU_SNAPSHOT=` builds the image from the live archive instead; the image records which source it used, with every package version, in `/etc/maplethief-ci-image`.
- x86_64 under Rosetta is several times slower than native arm64. `--arch arm64` is there for quick iterations, but only amd64 counts as "the Linux job".
- A clean checkout costs a full build of SDL3 per run (about a minute on the Mac); the download cache avoids the network.
- The macOS kit's bundle is ad-hoc signed; signing with an identity, notarisation and the Gatekeeper guidance stay with #114 and #81 ([ADR 0024](0024-release-contents-and-flow.md) keeps releases ad-hoc signed). The Windows kit uses the dynamic MSVC runtime until #82 decides between a static CRT and bundled redistributables.
- AppImage needs CMake 4.2+ or an External script; it is left to #114 ([ADR 0024](0024-release-contents-and-flow.md) defers it: TGZ only).
