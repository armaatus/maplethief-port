# 65. Linux and Steam Deck: a local game folder as a tarball, built on Ubuntu 24.04, a gamescope-aware launcher

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #84
- **Revised by:** [ADR 0100](0100-public-repository-and-player-releases.md) (releases may carry the recompiled code in a player build without the bundle, #182; this game folder, with the bundle inside, stays personal)
- **Builds on:** [ADR 0023](0023-local-ci-and-packaging.md) (the Linux container, the TGZ kit), [ADR 0024](0024-release-contents-and-flow.md) (releases carry own-code kits only, no AppImage yet), [ADR 0042](0042-frontend-game-loop-and-presentation.md) (`--bundle`, `MAPLETHIEF_BUNDLE_DIR`), [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (scaling modes, `--window`), [ADR 0048](0048-gamepad-actions-and-delivery.md) (gamepad bindings)

## Context

#84 asks for a Linux x86_64 release build, as an AppImage or a tarball, that also runs well on a Steam Deck added as a non-Steam game, plus notes on adding it and on the controller layout. The acceptance criterion is "runs on Ubuntu (CI build + local or VM run); Deck check by me if available".

Facts that limit the options:

- **The playable game is never distributed.** A release kit holds only our own code, and the Linux one is the platform shell ([ADR 0024](0024-release-contents-and-flow.md)). The game (an executable linked with `maplethief_gen` plus the bundle's files) is built locally from the user's IPA and stays on the user's machines (CLAUDE.md). So "a Linux release build that runs on the Deck" is something the user builds and copies over: a local artefact, not a release asset.
- **glibc only runs programs linked against it or an older one.** The CI image is Ubuntu 26.04 with glibc 2.43 ([ADR 0023](0023-local-ci-and-packaging.md)). Measured on the game's executable built there: it needs `GLIBC_2.43`, because glibc 2.43 gave `asinf`, `acosf`, `atan2f`, `log10f` and `sqrtf` new symbol versions. It also needs `GLIBC_2.38` (`__isoc23_strtol` and friends, `strlcpy`, `fmod`) and `GLIBC_2.36` (`arc4random`). Such a build runs on no current LTS distribution, and on no SteamOS release that predates glibc 2.43.
- **GCC 15's libstdc++ is newer than most systems'.** A dynamic libstdc++ would add `GLIBCXX_3.4.3x` requirements of the build machine.
- **SDL3 is static and loads its back ends at run time.** X11, Wayland, libdecor, ALSA, PulseAudio, PipeWire and udev go through `dlopen`, so the executable's NEEDED list is only `libc`, `libm` and the loader (measured).
- **In Game Mode the Deck runs a non-Steam shortcut natively, under gamescope.** gamescope scales whatever window it gets to the 1280 × 800 screen, by default with its own filter. Steam Input presents the controls as an Xbox-style gamepad, which SDL3 opens like any other pad.
- CPack's AppImage generator needs CMake 4.2+ (the floor is 3.28, [ADR 0002](0002-language-and-toolchain.md)). Otherwise an AppImage needs `appimagetool` and its runtime downloaded and pinned, and FUSE (or `--appimage-extract-and-run`) on the target.

## Options considered

### Format of the local game build

1. **A folder plus its deterministic `.tar.gz`**, written by `mtport.linux_game` (Python's `tarfile`: sorted members, owner root, fixed modes, `SOURCE_DATE_EPOCH` or 0). The launcher, the executable, a copy of the bundle, an icon and a README. Unpacks anywhere, and Steam's *Add a Non-Steam Game* takes the launcher or the `.desktop` entry it writes. Accepted.
2. **AppImage.** One file and a nicer double-click on desktops, but it needs a pinned `appimagetool` and type-2 runtime (or CMake 4.2's CPack), FUSE on the target, and a writable location for `--appimage-extract` when FUSE is missing. It would also pack about 20 MB of game files into a squashfs that our content guard can't look into. For a non-Steam shortcut it adds nothing. Rejected for now.
3. **Flatpak.** It needs a manifest, a runtime (GNOME/Freedesktop SDK), `flatpak-builder` and a repository or bundle. Much more machinery than a personal build needs. Rejected.
4. **Release the Linux kit with the game in it.** Forbidden (ADR 0024). Rejected.

### Where the binary is built (glibc)

1. **A second pinned image, Ubuntu 24.04 (glibc 2.39) with GCC 14**, `tools/containers/linux-game/Dockerfile`, pinned like the CI image (base by digest, apt from the same archive snapshot, the CI image's CMake by hash). No clang, uv, PowerShell or Node.js: it only builds `MapleThief` and stages the folder. Built on the oldest glibc we support, so the result runs on 2.39 and later: Ubuntu 24.04 LTS and newer, Debian 13 and current Arch. SteamOS still has to be confirmed on a Deck. Accepted.
2. **The CI image with `.symver` pins** to old symbol versions in a forced-include header. It doesn't reach the static libstdc++ and libgcc objects, which are compiled against 2.43's headers, nor SDL3's sources unless they are forced too. Fragile, and silently wrong when glibc adds the next version. Rejected.
3. **The Steam Linux Runtime 3.0 "sniper" SDK** (Debian 11, glibc 2.31). This is what Valve recommends for Steam games. Its compilers are older than ADR 0002's GCC 13 minimum without extra packages, its image is several GB, and a non-Steam shortcut runs on the host anyway, not in the runtime. Rejected. It stays the option should the port ever ship through Steam.
4. **Build on the Deck.** SteamOS has a read-only root and no toolchain. Possible in a distrobox, but that is a user setup, not a build we can pin. Documented only as `--native` on any Linux.

### The C++ runtime

1. **`-static-libstdc++ -static-libgcc` on `MapleThief`, on Linux** (`platform/linux/linux.cmake`). The executable then needs only glibc. Accepted.
2. Ship `libstdc++.so.6` next to the binary with an `$ORIGIN` rpath. One more file to keep in step, and a copy of a GPL-with-exception library to carry. Rejected.

### Steam Deck defaults

1. **A POSIX `sh` launcher** (`platform/linux/MapleThief.sh`). It sets `MAPLETHIEF_BUNDLE_DIR` to the folder's `game/MapleThief.app` (unless set), and **under gamescope** (`GAMESCOPE_WAYLAND_DISPLAY` set, or `XDG_CURRENT_DESKTOP=gamescope`) it adds `--window 1280x800` (`MAPLETHIEF_GAMESCOPE_WINDOW` overrides it, an explicit `--window` wins). The game's own scaling (ADR 0043) then fills the screen, and gamescope doesn't filter it again. `--install-desktop` writes `~/.local/share/applications/io.github.armaatus.maplethief.desktop` with absolute, quoted paths and the icon, which makes the game appear in Steam's *Add a Non-Steam Game* list. Accepted.
2. **Detect the Deck in C++** (`SteamDeck=1`, the screen size) and go fullscreen. It changes the shared frontend for one device, and Desktop Mode would also go fullscreen. Rejected; a launcher option is enough.
3. **Controller layout.** Keep ADR 0048's bindings and document the mapping to Steam's *Gamepad* layout. No Deck-specific bindings, and no Steam Input API (the game isn't a Steam app). Accepted.

## Decision

- **Portable executable.** On Linux, `MapleThief` links libstdc++ and libgcc statically (`platform/linux/linux.cmake`, included by `platform/CMakeLists.txt` only on Linux). The CTest **`platform.linux_portable`** (`platform/linux/portable_test.cmake`, `readelf`) fails when a NEEDED entry is not glibc's own (`libc`, `libm`, `libpthread`, `libdl`, `librt`, the loader), when any `GLIBCXX`, `CXXABI` or `GCC` version is needed, and, if the cache variable `MAPLETHIEF_LINUX_GLIBC_FLOOR` is set, when a `GLIBC` version newer than the floor is needed. The CI image leaves the floor empty, since its glibc is 2.43. The game build sets it to 2.39.
- **The game folder.** `scripts/linux-game build [REV] [--worktree] [--native] [--arch …]` builds it: by default in the image of `tools/containers/linux-game/` (Ubuntu 24.04, GCC 14), from a clean export of the commit with `work/` mounted read-only; `--native` on Linux with the host's toolchain. It configures `linux-release` with the generated code and `MAPLETHIEF_LINUX_GLIBC_FLOOR=2.39`, builds `MapleThief`, runs `platform.linux_portable`, `platform.version` and `platform.smoke`, and calls `mtport.linux_game stage`. The result is `build/linux-game/MapleThief-game-<version>-linux-<arch>/` and its `.tar.gz`. The stage refuses an executable without the generated code (the placeholder), copies the bundle (and refuses links in it), and converts `Icon@2x.png` to a standard PNG (`mtport.cgbi`). The release content guard rejects the folder and the tarball (a test checks it), so it can't slip into a release.
- **The check.** `scripts/linux-game check [TARBALL] [--image …]` unpacks the tarball on the host (`mtport.linux_game unpack`: files and directories only, safe names) and runs it, read-only, in a stock image with no package installed, `ubuntu:24.04` pinned by digest by default: `MapleThief.sh --version`, then the game with SDL's dummy drivers, a tap on Play at 3 s and 150 frames, which must exit 0.
- **Launcher and Deck notes** as in option 1 above. The notes (add as a non-Steam game, no compatibility tool, the scaling table for 1280 × 800, the Deck-button table, the touchscreen) are in [docs/play/linux-and-steam-deck.md](../play/linux-and-steam-deck.md).
- **The port kit is unchanged.** It stays CPack's TGZ from the CI image (ADR 0023/0024). Its shell is only a placeholder, so its glibc 2.43 floor doesn't block anyone from playing.

## Consequences

- The game folder runs on Linux with glibc 2.39 or newer and needs no package beyond what any desktop has. Measured on 2026-10-30 (#84). Built in the image, the executable's newest needed version is `GLIBC_2.38`, and NEEDED is only `libm`, `libc` and the loader. In a stock `ubuntu:24.04` with no package added (`scripts/linux-game check`), it boots, taps Play and is in Henesys at frame 150. Under Xvfb with only Mesa and the X client libraries added, and `XDG_CURRENT_DESKTOP=gamescope`, it gets SDL's `x11` driver and the `opengl` renderer and a 1280 × 800 window. There the image is integer ×2 at (160, 80).
- A second image to maintain. Its pins move like the CI image's (one per commit). Its packages come from the same snapshot and its CMake is the same archive, so a bump in one is mirrored in the other. Its snapshot outage workaround is the same `--build-arg UBUNTU_SNAPSHOT=`.
- The game's float math on Linux uses glibc's pre-2.43 symbol versions of `asinf` and friends (whatever the target system's glibc provides for them). The game build is not a golden-image reference (those run on the developer's own build), so this changes no test.
- Whether SteamOS's glibc meets 2.39, Game Mode's window and the default controller layout are a Deck check (`needs-human` on #84).
- AppImage stays possible later. The folder is already AppImage-shaped: `MapleThief.sh` would become `AppRun`, plus a `.desktop` and icon at the top.
