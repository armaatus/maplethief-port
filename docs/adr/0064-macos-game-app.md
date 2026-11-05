# 64. The macOS game app: a `mac-app` preset, the bundle and icon inside, ad-hoc signed

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #81
- **Revised by:** [ADR 0100](0100-public-repository-and-player-releases.md) (releases may carry the recompiled code in a player build without the bundle or its icon, #182; this app, with the bundle inside, stays personal)
- **Builds on:** [ADR 0021](0021-generated-code-partition-and-build.md) (`maplethief_gen`, `MAPLETHIEF_GEN_DIR`), [ADR 0023](0023-local-ci-and-packaging.md) (presets, the kit, `cmake/Packaging.cmake`), [ADR 0024](0024-release-contents-and-flow.md) (what a release may carry, ad-hoc signing), [ADR 0042](0042-frontend-game-loop-and-presentation.md) (the frontend boots the game from an extracted bundle)

## Context

Issue #81 asks for a double-clickable, universal `MapleThief.app` built locally from the user's IPA: an icon generated from the IPA's icon at build time, an ad-hoc signature with the hardened runtime off, and a doc on getting past Gatekeeper. The acceptance criterion: `lipo -info` shows arm64 and x86_64, and the app launches on Apple silicon and under Rosetta or on an Intel Mac. Facts and constraints:

- **The game build is local only** (ADR 0024, CLAUDE.md). A release kit never holds recompiled code or anything from the IPA. The kit preset `mac-release-universal` builds the placeholder shell on a clean checkout, and `scripts/release` guards the result.
- **A build with `work/gen` already runs the game** (ADR 0042), but it finds the bundle through `MT_DEV_BUNDLE_DIR`, an absolute path to the checkout's `work/ipa/Payload/MapleThief.app` compiled in. That build-tree app breaks once it's moved, or once `work/` is gone.
- **The IPA's artwork:** `iTunesArtwork` at the IPA's root is the 1024 × 1024 store image, a JPEG in version 1.5. The bundle's `Info.plist` names six CgBI PNG icons (`CFBundleIconFiles`), the largest `Icon@2x.png` at 114 × 114. `sips` (ImageIO) reads both formats; `mtport.cgbi` reads only the PNGs, and nothing in the repository decodes JPEG.
- **SDL3's `SDL_GetBasePath()`** returns `<app>/Contents/Resources/` for a program inside an `.app` bundle.
- **Signing:** the linker signs only the executable (ad hoc). `codesign --sign -` on the bundle seals `Info.plist` and every resource too. No Developer ID exists, so there is no notarisation (ADR 0024). The hardened runtime matters to notarisation; without it, it changes nothing for Gatekeeper.
- **Gatekeeper** checks only quarantined files. A browser, AirDrop, Mail or Messages sets `com.apple.quarantine`; a local build doesn't. `spctl --assess` rejects every ad-hoc app, quarantined or not. Since macOS 15 Sequoia, Control-click › Open no longer bypasses the check: the user allows the app once with System Settings › Privacy & Security › Open Anyway. Ventura and Sonoma still have Control-click › Open.

## Options considered

### Where the game app is built

1. **A preset of its own, `mac-app`,** with the cache option `MAPLETHIEF_MAC_APP` (Release, `arm64;x86_64`, macOS 13). It needs `maplethief_gen` and stops at configure time, with the steps to fix it, when that is missing. Its workflow configures and builds into `build/mac-app/`. `mac-release-universal` stays the kit's preset, unchanged. Accepted.
2. **`mac-release-universal` makes the app whenever `work/gen` exists.** One preset fewer, but the kit preset would then build the game in a developer's tree, and its `package` step would have to fail there. Rejected.
3. **A script that post-processes the build tree's app** (copy, icon, sign). It repeats what CMake knows and isn't incremental. Rejected.

### Where the app finds the game's files

1. **A copy inside the app:** `work/ipa/Payload/MapleThief.app` copied into `Contents/Resources/Game/` (`cmake -E copy_directory_if_different`, 27 MB). `MT_EMBEDDED_BUNDLE` makes `generated_program()` use `SDL_GetBasePath()` + `Game` as the default bundle instead of `MT_DEV_BUNDLE_DIR`. `--bundle` and `MAPLETHIEF_BUNDLE_DIR` still come first. Accepted: the app works wherever it is moved.
2. **The extracted bundle in place** (the dev builds' absolute path). Rejected for the app: it breaks on a move. The dev builds keep it.
3. **The copy named `MapleThief.app`.** Rejected: a directory with a bundle extension inside `Resources` reads as nested code to `codesign` and LaunchServices, and the IPA's bundle is an iPhone app. `Game` is a plain directory, sealed as resources.

### The icon

1. **Built from the IPA at build time:** `python3 -m mtport.macicon` takes `iTunesArtwork`, or else the largest PNG that `Info.plist` names. `sips` scales it to the macOS icon grid's 824 × 824 content. Python masks it to the grid's rounded rectangle (radius 185.4, anti-aliased by 4 × 4 supersampling at the edge), centres it on a transparent 1024 × 1024 canvas, halves it down to 16 × 16 (premultiplied 2 × 2 box) and writes the `icns` container itself (PNG entries `icp4`, `icp5`, `ic07`–`ic14`). The result goes to `build/mac-app/platform/MapleThief.icns` and into `Contents/Resources` (`CFBundleIconFile`). Accepted. The geometry and the container are tested on synthetic images on every OS; `sips` and `iconutil` are tested on macOS.
2. **`sips` for every size, then `iconutil -c icns`.** Fewer lines, but square art with no mask, and nothing testable off the Mac. Rejected.
3. **A committed icon.** Rejected: it is Nexon's art (CLAUDE.md).

### Signing

1. **Ad hoc over the whole bundle after every build:** `codesign --force --sign - --timestamp=none`, with no `--options runtime`, in the always-run target `MapleThief_app`, after the bundle copy. Accepted. The hardened runtime is off, as the issue asks: without notarisation it gains nothing.
2. **Signing in a `POST_BUILD` step.** Rejected: it runs only on relink, so it misses a changed icon or bundle copy.

### Packaging

`MAPLETHIEF_MAC_APP` builds are never installed or packaged. `cmake/Packaging.cmake` adds one install rule that fails with the reason, and no CPack. A kit comes from `mac-release-universal`.

## Decision

- **`cmake/MacApp.cmake`** (included by `platform/CMakeLists.txt` on Apple) defines `MAPLETHIEF_MAC_APP` (default off) and `maplethief_mac_app(MapleThief)`: requirements, icon command, `MT_EMBEDDED_BUNDLE="Game"`, and the `MapleThief_app` target that copies the bundle and signs. The `mac-app` configure, build, test and workflow presets turn it on.
- **`platform/src/main.cpp`**: with `MT_EMBEDDED_BUNDLE`, the default bundle is `SDL_GetBasePath()` + `Game`. Otherwise it is `MT_DEV_BUNDLE_DIR` as before.
- **`tools/mtport/macicon.py`** writes the `.icns` (above); `tests/test_macicon.py` (synthetic) and `tests/test_macicon_real.py` (`real_assets`) test it.
- **`platform/macos/Info.plist.in`** gains `CFBundleIconFile`, which is empty in every other build.
- **Gatekeeper guidance** is in the README ("The macOS app"). It covers the kit's download as well as a game app copied to another Mac.

## Consequences

- `cmake --workflow --preset mac-app` gives a 49 MB universal app (`lipo -info`: x86_64 arm64). Checked on Apple silicon: the app opened with `open` (as a double-click does) and with `open --arch x86_64` (LaunchServices reported `LSArchitecture` arm64 and x86_64) from a copy outside the build tree. A scripted headless run through the title, the main menu, the intro and the first field produced bit-identical shots on both architectures.
- An Intel Mac and the Gatekeeper flow on a second Mac (a quarantined copy) are not checked by the agent: a `needs-human` step.
- Each build re-signs the bundle (about a second) and copies only changed files.
- The game app and its icon exist only in `build/`. Nothing derived from the IPA is committed or packaged.
- macOS 26 may draw an icon that doesn't fill its grid inside a grey frame. The masked icon fills the standard grid, but an Icon Composer `.icon` would be a later change if needed.
