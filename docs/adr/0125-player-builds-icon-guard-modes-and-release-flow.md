# 125. Player builds: the first-launch import, contents, names, our own icon, guard modes and the release flow

- **Status:** Accepted
- **Date:** 2026-11-06
- **Issue:** #182
- **Revises:** [ADR 0024](0024-release-contents-and-flow.md) (a release also carries a player build per platform; the guard gets a player mode and an icon allowlist), [ADR 0100](0100-public-repository-and-player-releases.md) (the player releases it allowed now exist)
- **Builds on:** [ADR 0021](0021-generated-code-partition-and-build.md) (`maplethief_gen`), [ADR 0023](0023-local-ci-and-packaging.md) (presets, CPack), [ADR 0064](0064-macos-game-app.md) (ad-hoc signing, the `.icns` writer), [ADR 0065](0065-linux-and-steam-deck-build.md) (the `linux-game` container, glibc 2.39, the launcher)

## Context

[ADR 0100](0100-public-repository-and-player-releases.md) allowed releases that carry the recompiled code and none of the game's assets. The player downloads the game, and on first launch imports their own IPA. #182 builds those downloads. It has two parts: the import in the game (part A: `platform/src/ipa_import.*`, the import screen, `--import-ipa`, and the bundle lookup that ends in the data directory's `game/` folder) and the builds and release flow. This ADR covers both: the import's decisions are under *The import* in the Decision. Facts and constraints:

- The game build links `maplethief_gen` and, for a developer, compiles in the bootstrap's bundle path `MT_DEV_BUNDLE_DIR` (`<repo>/work/ipa/Payload/MapleThief.app`). The `mac-app` preset and `scripts/linux-game` copy the bundle into the app or folder. A download must have neither: no path into the builder's `work/`, and no file from the bundle.
- ADR 0024's guard rejects any binary with the generated-code symbols (`f_XXXXXXXX`, `mt_import_thunk_N`, `mt_fn_first`) or the dispatch table's first keys. That is exactly what a player build is, so the guard needs a second mode.
- The guard's media rule rejects every image and audio file. ADR 0024 said an app icon of our own would need an allowlist. The `mac-app` icon is Nexon's art (`mtport.macicon` draws it from `iTunesArtwork`) and can't ship.
- Measured on this PC (Windows 10, VS 2022, real `work/gen`, 63 C files): the `win-player` workflow takes about 2.5 minutes from a clean build directory. The executable is 14.6 MB. It keeps no symbol names, but the guard finds the dispatch table at a 16-byte stride. The Linux player executable built in the container is 17.7 MB, keeps its 2,878 `f_` symbols, and needs at most `GLIBC_2.39`.
- MSVC links the dynamic CRT by default (`VCRUNTIME140.dll`). ADR 0023 left that open until #82. A player can't be expected to install the Visual C++ redistributable.
- `gh release create --notes-file F --generate-notes` puts the file's text above the generated notes (`gh release create --help`: "Additional release notes can be prepended to automatically generated notes").
- Compressed PNG bytes depend on the zlib build: the deflate stream can differ between zlib, zlib-ng and Apple's zlib at the same level. The pixels don't.

## Options considered

### What a player build contains

1. **Only the game and what the player needs**: the executable (or `MapleThief.app`), a short player README, `LICENSE`, `THIRD_PARTY.md` and `licenses/`, plus `MapleThief.sh` and `icon.png` on Linux. **Chosen.** The download is small and obvious, and everything in it is ours or a licence.
2. **The kit plus the game**: one download for players and contributors. The kit's `lib/`, `include/`, recompiler and tools mean nothing to a player, and a contributor already has the kit. Rejected.

### The names

1. **`MapleThief-<version>-<os>-<arch>`**: `MapleThief-<version>-macos-universal.zip`, `MapleThief-<version>-windows-x64.zip`, `MapleThief-<version>-linux-x86_64.tar.gz`. These are the kits' names without `-kit`, from the same CPack platform function. The shortest name is the one players should pick. **Chosen.**
2. **`MapleThief-player-…`**: symmetric with `-kit`, but "player" means nothing to the person downloading. Rejected.

### The build switch

1. **One CMake option `MAPLETHIEF_PLAYER_BUILD`** (OFF by default) and an `<os>-player` preset per desktop OS. **Chosen.** It needs `maplethief_gen` and fails at configure time without it, or together with `MAPLETHIEF_MAC_APP`. It defines `MT_PLAYER_BUILD=1` on `MapleThief`, but not `MT_DEV_BUNDLE_DIR` or `MT_EMBEDDED_BUNDLE`. It copies no bundle, and it switches `cmake/Packaging.cmake` to the player layout and name. The build presets build `MapleThief` only, and `CMAKE_SKIP_INSTALL_ALL_DEPENDENCY` keeps CPack from building the test executables too. The test presets run `platform.version`, `platform.player_contents` (the executable must not contain this build's `work/` path) and `platform.linux_portable`.
2. **A separate CMake project** for the downloads. It would duplicate the platform target. Rejected.

Windows: `win-player` sets `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`. Every library is static (SDL3 and the runtime), so the whole program links the static CRT, and `dumpbin /dependents` lists only Windows system DLLs. This settles #82's question for the player build. The kit keeps the dynamic CRT. macOS: `mac-player` is universal and is sealed ad hoc as a whole bundle at install, like the kit (ADR 0024: no Developer ID, no notarisation, no hardened runtime). Linux: `linux-player` is built by `scripts/linux-game player`, a new mode of ADR 0065's container. It uses the same pinned Ubuntu 24.04 image and the 2.39 floor, mounts only `work/gen/` (read only), and CPack writes the tarball. The launcher now sets `MAPLETHIEF_BUNDLE_DIR` only when its folder has a `game/MapleThief.app`, so in the player tarball the game finds its imported copy itself. iOS is out of scope (sideloaded, #83).

### The icon

1. **Our own geometric mark, drawn by a script, with the derived files committed.** **Chosen.** `tools/mtport/appicon.py` is the source: a gold four-pointed star with a hole, on a blue rounded square on the macOS icon grid. It has no letters, no maple leaf and nothing from Nexon's art or branding. It draws with signed distances and only `+ - * /` and `sqrt`, so the pixels are the same on every platform. It writes `platform/icon/MapleThief.svg`, `.icns` (16 to 512 px, through `mtport.macicon.icns`), `.ico` (16 to 256 px PNG entries) and `icon.png` (256 px). These are committed (52 KB in all). The guard allowlists files by SHA-256, and a build that drew them again could compress them into other bytes on another zlib. `tests/test_appicon.py` checks that every committed image decodes to exactly the script's pixels, and that the allowlist's hashes are the files'.
2. **Drawing them at build time.** That needs Python in every build and gives no stable hash for the allowlist. Rejected.
3. **A hand-drawn SVG plus a rasterizer** (Inkscape, rsvg, cairosvg). That is another tool to pin, for a simple shape. Rejected.

The icon goes into every build except `mac-app` (whose icon stays the IPA's, ADR 0064). On Windows it is the executable's first icon resource (`platform/windows/MapleThief.rc.in`). On macOS it is `Contents/Resources/MapleThief.icns` (`CFBundleIconFile`). On Linux it is `icon.png` next to `MapleThief.sh`, named by `--install-desktop`.

### Guard modes

1. **An explicit flag, `guard --mode kit|player`, defaulting to `kit`.** **Chosen.** A misnamed file can't loosen the guard.
2. **The mode inferred from the file name** (`MapleThief-kit-…` → kit). This is shorter to call, but a renamed kit would be checked in the looser mode. Rejected.

Player mode keeps every rule of ADR 0024 except the one on generated code in binaries. A binary may hold the `f_XXXXXXXX` symbols and the dispatch table, and at least one binary must, so a placeholder shell is never released as the game. Both modes gain three rules. Anything inside an IPA's `Payload/<name>.app/` folder is rejected by path, the bundle's executable `MapleThief` included. A 32-bit ARM Mach-O (thin or fat, cputype 12) is rejected, because only the iOS game's executable is one. The port's own icon files are allowed only by path below the package's top-level directory and SHA-256 (`ICON_ALLOWLIST`: `MapleThief.app/Contents/Resources/MapleThief.icns`, `icon.png`). Everything else stays rejected: images and audio, files with the SHA-256 of a file under `work/ipa/`, `work/gen/` or `assets-out/`, members under `work/`, `assets-out/` or `build/`, `*.ipa` and nested archives, the recompiler's file names and banners (`fn_NNN.c`, `mt_gen.h`, `dispatch.c`, …) and the guest symbol table. `mtport.release players VERSION` lists the player build names, next to `kits`.

### Release notes

1. **A committed header, `.github/release-notes-header.md`, passed with `--notes-file` together with `--generate-notes`.** gh puts it above the generated notes. It says which file to download, that it holds no game files, that the player needs their own IPA, and what the kits are. **Chosen.**
2. **Editing the notes by hand on the draft.** This is easy to forget. Rejected.

## Decision

- **The import** (`platform/src/ipa_import.{h,cpp}`, `import_screen.{h,cpp}`):
  - The check and the extraction take a path or bytes and a data directory and never touch SDL video, so other front ends (the Switch importer, #243, reads a fixed SD-card path) call `check_ipa` / `import_ipa` with `ipa_expected()` and show `IpaResult::why`.
  - The IPA must be `MapleThief-1.5.ipa`'s exact size and SHA-256, the same values as `original/SHA256SUMS` and bootstrap.py's `IPA_SIZE` (`tests/test_ipa_import.py` keeps them equal). A refusal names its reason: not a zip, no `Payload/*.app`, another app, another version (`CFBundleVersion` from the binary or XML `Info.plist`), encrypted (zip encryption, or `cryptid != 0` in the armv7 slice), version 1.5 with other bytes, an unsafe entry, the wrong executable. No message says how to decrypt anything.
  - Only `Payload/MapleThief.app/` is extracted, under bootstrap.py's safe-extraction rules (no absolute paths, `..`, backslashes, drive letters, reserved or colliding names, links), into `<data dir>/game.tmp-*`. The executable's SHA-256 is checked, then the folder is renamed to `<data dir>/game/` (an old `game/` is set aside and restored if the rename fails). An interrupted import leaves no `game/`; stale temp folders go at the next import. The data directory is `MAPLETHIEF_DATA_DIR` or `SDL_GetPrefPath("", "MapleThief")`.
  - The bundle is looked up in this order: `--bundle`, `MAPLETHIEF_BUNDLE_DIR` (both must exist), the build's own default (`MT_DEV_BUNDLE_DIR` / `MT_EMBEDDED_BUNDLE`, absent in a player build), `<executable dir>/game/MapleThief.app`, `<data dir>/game/`, else the import screen. The C++ never reads `MT_PLAYER_BUILD`: a player build just lacks the defaults.
  - The import screen (Canvas's 5x7 font, no game assets) takes the IPA from the system's file dialog (`file_dialog.h`) or a drop on the window, imports on a worker thread with a progress bar, shows a refusal and lets the player retry, and starts the game in the same run. Where no dialog can show it imports `<data dir>/MapleThief-1.5.ipa`. A scripted run with no bundle exits 4. `--import-ipa FILE` imports again headless (exit 0, or 3 with the reason).
- A release carries, per platform, the kit (ADR 0024) and the **player build**: `MapleThief-<version>-macos-universal.zip` (universal, ad-hoc-signed `MapleThief.app`), `MapleThief-<version>-windows-x64.zip` (`MapleThief.exe`, static CRT) and `MapleThief-<version>-linux-x86_64.tar.gz` (`bin/MapleThief`, `MapleThief.sh`, `icon.png`; glibc 2.39+). Each also holds `README.md` (`platform/player/README.md`), `LICENSE`, `THIRD_PARTY.md` and `licenses/`, and none of the game's assets. `SHA256SUMS` covers them all.
- `MAPLETHIEF_PLAYER_BUILD` and the `mac-player`, `win-player` and `linux-player` presets and workflows build them (`cmake/PlayerBuild.cmake`, `cmake/Packaging.cmake`).
- The port's own icon (`tools/mtport/appicon.py`, `platform/icon/`) replaces the empty icon in every build except `mac-app`.
- `mtport.release guard --mode player` guards the player builds; `--mode kit` (the default) the kits.
- **Flow** ([release-checklist.md](../release-checklist.md)):
  - `scripts/release start` passes the notes header.
  - `scripts/release build` (Mac) builds the kits as before, then `mac-player` on a clean clone of the tag with this checkout's `work/gen` linked in, and the Linux tarball with `scripts/linux-game player TAG`. It checks each file as it checks the kits (`lipo -archs`, `codesign --verify --deep --strict`, `--version`), guards the kits in kit mode and the player builds in player mode, checksums them all and uploads them.
  - `scripts\release.ps1 build` (Windows) runs `win-player` in its clone the same way (a junction to `work/gen`), checks `--version` and that `dumpbin /dependents` shows no MSVC runtime DLL, guards in player mode, checksums and uploads.
  - `scripts/release publish` expects six files plus their `.sha256`, guards each in its mode, and writes `SHA256SUMS` over all of them.
  - `--dry-run` works as before.

## Consequences

- Anyone with the IPA can play without a compiler. The release machines need `work/gen/` (`python3 -m recomp all`), and the guard's hash and dispatch-table checks run there.
- The guard's limit from ADR 0024 also applies to player builds: on a machine without `work/gen/`, a stripped Windows player executable shows no sign of the recompiled code, so player mode fails it as "not a player build". That is safe, and the release machines have `work/gen/`.
- The kits now carry the port's icon too: inside the Windows executable, and in the mac kit's `MapleThief.app`, which the allowlist covers.
- Changing the icon means running `python3 -m mtport.appicon write` and updating `ICON_ALLOWLIST`; `tests/test_appicon.py` fails until both are done.
- The macOS player build can't be built or checked on the Windows PC: `lipo`, `codesign` and the Apple silicon and Rosetta runs are a `needs-human` check on the Mac.
- **How we'd know this was wrong:** a player build that the guard passes but that holds a file from the bundle, or a player who can't start the download without installing something.

## Addendum: player builds next to CI's kits (ADR 0126, 2026-11-07)

The kits now come from `.github/workflows/release.yml` after a release pull request merges. The player builds still need `work/gen`, so they stay local: `scripts/release build TAG --players-only` (Mac: macOS and Linux) and `scripts\release.ps1 build TAG -PlayersOnly` (Windows) build, check, guard and upload only the player builds to CI's draft; `scripts/release publish TAG` is unchanged ([ADR 0126](0126-dev-branch-release-pull-requests-and-ci-by-target.md)).
