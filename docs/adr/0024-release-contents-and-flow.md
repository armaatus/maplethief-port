# 24. Release contents and flow

- **Status:** Accepted
- **Date:** 2026-10-27
- **Issue:** #114
- **Revised by:** [ADR 0100](0100-public-repository-and-player-releases.md) (a public repository; player releases with the recompiled code and no assets, #182), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (the player builds next to the kits, the guard's player mode and icon allowlist, the notes header, #182)

## Context

[ADR 0023](0023-local-ci-and-packaging.md) gave every platform a CPack kit, built locally through the `<os>-release` workflow presets, and left open what a *release* ships and how it is cut. Issue #114 asks for a repeatable flow from a tag to a GitHub Release in the private repository, with one file per platform, checksums and release notes, built on Joris's machines and not on hosted CI. Facts and constraints:

- **What a release may contain** (Joris, 2026-10-26): only our own work, built from the repository: the runtime and platform binaries and libraries, the recompiler and tools, scripts, docs and licences. Never the game build: recompiled code (`work/gen/`), converted assets (`assets-out/`), anything from `work/`, or a playable `.app`/exe with game code in it. Each user builds the game from their own IPA (#85). This is the CLAUDE.md guardrail, and the reason #94 and ROADMAP M11 said "no binaries".
- The GitHub Actions minutes ran out on 2026-10-26 (ADR 0006 addendum), so the release must not need them.
- `gh` covers the whole flow from a local machine: `gh release create --draft --verify-tag --generate-notes`, `gh release upload` per machine, `gh release edit --draft=false`. `upload --clobber` deletes the old asset first. Limits: 1000 assets per release, each under 2 GiB.
- Artifact attestations need Actions OIDC, and Enterprise Cloud for private repositories, so they are not available.
- macOS: no Developer ID. The bundle is ad-hoc signed (ADR 0023); since Sequoia a user allows such an app once in System Settings › Privacy & Security (#81 writes that guidance).
- The recompiler names every generated function `f_XXXXXXXX` (eight upper-case hex digits of the function pointer, `recomp/lift/core.py`), and only generated code defines `mt_import_thunk_N` (`recomp/imports.py`) and `mt_fn_first` (`recomp/dispatch.py`). The runtime *references* `mt_fn_table`, `mt_import_table` and the other table names, so their names alone don't prove anything. Checked on the real build: `libmaplethief_gen.a` (95 MB, Debug) holds 2,878 such symbols and the dispatch table's keys at a 16-byte stride; `libSDL3.a`, `libruntime.a` and the `MapleThief` executable hold none.
- PRs in this repository carry no labels so far; issues do (`type:*`, `area:*`).

## Options considered

### What a release ships

1. **An own-code "port kit" per platform**, the ADR 0023 kit: the platform shell, `lib/` (runtime and static SDL3), `include/`, the recompiler, the Python tools, the user scripts, docs and licences. A user with the kit and their own IPA has everything but the game build, without building the dependencies. Accepted.
2. **Tags and source archives only** (the old #94 wording). Safe, but every user rebuilds SDL3 and the toolchain setup. Rejected now that the kits exist and are guarded.
3. **Game builds** (recompiled code, assets, a playable app). Rejected: Nexon's code and art, and the guardrail. A public distribution would need its own legal decision and ADR.

### Where the kits are built

1. **On Joris's machines** with the release workflow presets on a clean checkout of the tag: macOS and the Linux container on the Mac, Windows on the PC; `gh` uploads to a draft. No CI minutes. Accepted.
2. **A CI release workflow** (tag push → matrix → upload), as Zelda64Recomp and UnleashedRecomp do. Needs minutes, and CI would have to be trusted with release credentials. Rejected for now; nothing here prevents adding it later as a confirmation.

### Release notes

1. **GitHub's generated notes** (`--generate-notes`) grouped by PR labels through `.github/release.yml`. No tool to pin; `gh` does it. Grouping needs labelled PRs, so the epic PR gets its epic's labels. Accepted.
2. **git-cliff** locally from commit messages. One more pinned tool, and our commit subjects are not Conventional Commits, so its grouping would need a custom parser. Rejected.

### Where the version comes from

1. **The tag, through a CMake cache variable.** `MAPLETHIEF_VERSION` (`X.Y.Z` or `X.Y.Z-rc.N`) defaults to the environment variable of the same name on the first configure, else `0.0.1`. Its numeric part is `project(VERSION)` (and the bundle's `CFBundleVersion`, which must be numeric); the whole string is `MT_VERSION`, `MapleThief --version` and the kit's file name. `scripts/release` sets the variable from the tag; `scripts/test-linux` passes it into the container. Works with the workflow presets, which take no `-D`. Accepted.
2. **`git describe` at configure time.** The container's export has no `.git` (ADR 0023 streams a `git archive`), and a dirty or untagged tree would get a made-up version. Rejected.
3. **A committed `VERSION` file.** Needs a bump commit before every tag, and the tag and the file can disagree. Rejected.

### How "own code only" is enforced

1. **By construction only**: `maplethief_gen` has no install rule, nothing is installed from `work/` or `assets-out/`. Necessary, but a future install rule or a linked game library would slip through silently. Not enough alone.
2. **A content guard over the finished kit**, in Python so that the Mac and the Windows script share it, and with synthetic tests. Accepted, on top of 1.

### Checksums

1. **`<kit>.sha256` per kit, merged into `SHA256SUMS`** at publish, in the `sha256sum` format (`<hex>  <name>`), so `sha256sum -c` and `shasum -a 256 -c` read it. Accepted. Signing `SHA256SUMS` (minisign or GPG) is left for when releases leave the private repository.
2. Attestations: not available (see Context).

## Decision

**Contents.** A release carries, per platform, the port kit of ADR 0023: `MapleThief-kit-<version>-macos-universal.zip` (universal `MapleThief.app`, ad-hoc signed as a whole bundle, zipped with `ditto -c -k --norsrc --keepParent`: no AppleDouble `._` entries for extended attributes, which ADR 0023's plain `--keepParent` added next to every file; the signature still verifies after `ditto -x` or `unzip`), `MapleThief-kit-<version>-linux-x86_64.tar.gz` (from the Linux container) and `MapleThief-kit-<version>-windows-x64.zip` (MSVC runtime as #82 decides; dynamic until then). Each holds the platform shell (no game code), `lib/` with the runtime and static SDL3, `include/`, `recomp/`, `tools/mtport/`, `tools/ghidra/`, `pyproject.toml`, `uv.lock`, the user scripts, `README.md`, `THIRD_PARTY.md`, `docs/` (Markdown only, so a reference crop of game art in `docs/` never ships) and `licenses/`. Plus `SHA256SUMS` and each kit's `.sha256`. GitHub attaches the source archives. No iOS asset (sideloaded through Xcode, #83); no AppImage yet (CMake 4.2+ or an External script; later). The game build (`maplethief_gen`, `work/`, `assets-out/`, an app or exe linked with generated code) is never uploaded anywhere, private or not.

**Content guard** (`tools/mtport/release.py`, `python3 -m mtport.release guard KIT...`). It reads a `.zip`, `.tar.gz` or directory as untrusted data (members read into memory, never extracted or executed, links not followed) and fails on:

- a member under `work/`, `assets-out/` or `build/`, or a CMake build-tree output (`CMakeFiles/`, `_deps/`, `Testing/`, `CMakeCache.txt`, `build.ninja`, ...); an `*.ipa` or nested archive; an unsafe name (absolute, `..`) or a link out of the kit;
- the recompiler's file names (`mt_gen.h`, `fn_NNN.c`, `dispatch.c`, `imports.c`, `gen.cmake`), and C/C++ sources that include `mt_gen.h` or carry its `Generated by recomp` banner;
- a non-empty file whose SHA-256 equals a file below `work/ipa/`, `work/gen/` or `assets-out/` (whichever exist; `--reference DIR` adds more), and any image or audio file, since the kits carry none of our own;
- a binary (Mach-O, fat, ELF, PE/COFF, `ar`, or anything with a NUL byte) containing `f_XXXXXXXX`, `mt_import_thunk_N` or `mt_fn_first`, or, when `work/gen/dispatch.c` exists, the first eight keys of `mt_fn_table` as little-endian words 8 or 16 bytes apart, which a stripped executable keeps.

Text files may mention these names (docs and headers do). The guard runs in `scripts/release build`, again in `publish` on the downloaded kits, and in `scripts/release.ps1 build`. `tests/test_release.py` plants each kind of violation, a generated `fn_000.c` among them, and expects the guard to fail; `tests/test_release_real.py` (local, `real_assets`) checks the real IPA's hashes and dispatch table.

**Versions and tags.** Annotated SemVer tags `vX.Y.Z`; pre-releases `vX.Y.Z-rc.N`, published as GitHub pre-releases. The version is `MAPLETHIEF_VERSION` as above; `MapleThief --version` prints `MapleThief <version>` to stdout and exits 0 (CTest `platform.version`).

**Release notes.** `gh release create --generate-notes`, grouped by `.github/release.yml` into epics (`type:epic`), recompiler, runtime and HLE, platforms, RE/formats/assets, build/CI/releases, enhancements, fixes, docs and decisions, and other changes. The epic PR gets its epic's labels before merging so it is grouped.

**Flow** ([docs/release-checklist.md](../release-checklist.md)):

1. Joris creates and pushes the annotated tag on `main`.
2. `scripts/release start TAG` (Mac): clean tree on `main`, tag annotated, on `origin/main` and pushed, `scripts/local-ci TAG` green; then the draft release with generated notes.
3. `scripts/release build TAG` (Mac): `MAPLETHIEF_VERSION=<version> scripts/local-ci TAG --release` (lint, Python 3.10, the macOS and Linux release workflows on a clean checkout, the IPA left a pointer); each kit unpacked and checked (`lipo -archs`, `codesign --verify --deep --strict`, `--version` equals the tag; the Linux one inside the container); the guard; `<kit>.sha256`; `gh release upload` (`--clobber` only with `--replace`).
4. `scripts\release.ps1 build TAG` (Windows PC): the same for the Windows kit, on a shared clone with the LFS smudge filter off, through `scripts\test.ps1 -Release`.
5. `scripts/release publish TAG` (Mac): every platform's kit and `.sha256` is on the draft; download, check each against its `.sha256`, guard again, merge `SHA256SUMS`, `shasum -a 256 -c SHA256SUMS`, upload it, `gh release edit --draft=false` (`--prerelease` for `-rc.N`).

Every command takes `--dry-run` (`-DryRun`): all local steps, the GitHub calls printed instead of run, the pushed/`main` checks only warned about, and `publish` reading the local kits. That is how the flow is checked on a local tag.

## Consequences

- A release costs no Actions minutes, and each kit comes from a commit that `scripts/local-ci` showed green, rebuilt from a clean checkout.
- `--version` and the kit names carry the tag, so a downloaded kit says which release it is.
- The guard makes "own code only" checkable instead of a promise. Its limits: a game file re-encoded into a non-media format, or a stripped binary with recompiled code on a machine without `work/gen/`, would only be caught by the build rules; the release machines have `work/`, so the hash and table checks run there. If a kit ever needs our own image (an app icon), the media rule needs an allowlist in the same PR.
- Publishing needs both machines: the Windows kit is a `needs-human` step until a Windows runner (ADR 0006 addendum) or another way exists.
- macOS users get an ad-hoc signed app (the Gatekeeper step is #81's guidance); Developer ID signing and notarisation would be a new decision.
- #94 and ROADMAP M11 now allow own-code kits on v1.0 and still forbid game builds. A public release needs its own legal decision and ADR.
- If generated notes stay all in "Other changes", PRs are not being labelled: label them, or reconsider git-cliff.
