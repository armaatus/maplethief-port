# 193. Linux reach: glibc 2.35, an AppImage and a Flatpak bundle on the release page

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** #349 (epic #291)
- **Revises:** [ADR 0065](0065-linux-and-steam-deck-build.md) (the build image moves to Ubuntu 22.04 and glibc 2.35; the AppImage and Flatpak it rejected now ship), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (the Linux player build gains two packages, the guard reads both), [ADR 0127](0127-data-folder-next-to-the-game.md) (the data folder rule for each package)
- **Builds on:** [ADR 0100](0100-public-repository-and-player-releases.md) (player releases carry none of the game's files), [ADR 0126](0126-dev-branch-release-pull-requests-and-ci-by-target.md) (the release flow)

## Context

The Linux player tarball (ADR 0125) is built in ADR 0065's Ubuntu 24.04 image, so it needs glibc 2.39. Ubuntu 22.04 LTS has 2.35 and Debian 12 has 2.36, so it doesn't start on either. Steam's own floor is 2.31 (Ubuntu 20.04, Debian 11).

On SteamOS, an update may wipe what is installed outside Flatpak and the home folder. A tarball unpacked into the home folder survives, but players expect a one-file download (an AppImage) or a Flatpak. Flathub requires redistributable content built from source on its own builders. A player build carries the recompiled code, and no recompilation port is listed there. Such ports ship Flatpak bundles on their own release pages instead.

ADR 0065 rejected an AppImage for three reasons, and each needs an answer: a tool to pin, FUSE on the target, and a release guard that can't look into a SquashFS image. A Flatpak bundle has the same guard problem (an OSTree static delta), and its sandbox meets ADR 0127's data folder: next to the game is `/app`, which is read-only.

Facts measured for this ADR (2026-11-27, on the Mac under Rosetta, the player build of `0.0.1`):

- In the Ubuntu 22.04 image the player executable passes `platform.linux_portable` with the floor 2.35. NEEDED is still only glibc's libraries. libstdc++ and libgcc are static (ADR 0065), and SDL3 loads the rest with `dlopen`.
- The tarball (7,189,935 bytes) starts in stock `ubuntu:22.04` (glibc 2.35) and `debian:12` (glibc 2.36) with no package added. It prints `--version`, then a headless run ends with exit 4, no bundle yet: the import screen's case.
- The AppImage (8,215,032 bytes) and the bundle (5,165,224 bytes) both reach the import screen under Xvfb with SDL's `x11` driver and the `opengl` renderer.
- `mtport.release guard --mode player` passes on all three. Each holds 1 binary with the recompiled code. In kit mode the guard finds the 2,878 `f_` symbols and the dispatch table inside the AppImage.
- Under Rosetta, an x86_64 Flatpak can't load its seccomp filter (`prctl(PR_SET_SECCOMP): Invalid argument`), because the kernel is arm64.
- The type-2 runtime release 20251108 reads zlib, xz and zstd SquashFS. Flatpak 1.12's `build-export` checks exported icons with `flatpak-validate-icon` in a bwrap sandbox. An unprivileged container can't create that sandbox. `FLATPAK_VALIDATE_ICON` names the validator.

## Options considered

### The glibc baseline

1. **2.31** (Ubuntu 20.04, Debian 11, the Steam Linux Runtime's "sniper" and Steam's floor). Ubuntu 20.04's standard support ended in May 2025, and Debian 11's LTS in August 2026. A current GCC on either needs a toolchain we can't pin through the snapshot archives (ADR 0065 rejected the sniper SDK for the same reason). It adds reach only for distributions that no longer get updates. Rejected.
2. **2.35** (Ubuntu 22.04 LTS, supported until 2027-06, with ESM after that). Ubuntu's snapshot service pins the base packages. Launchpad's PPA snapshot service (`snapshot.ppa.launchpadcontent.net`) pins GCC 14 from the Ubuntu Toolchain PPA at the same timestamp, signed by the PPA's key, which is committed. It covers Ubuntu 22.04 and later, Debian 12 and later, current Fedora, Arch and SteamOS 3.x. **Chosen.**
3. Keep 2.39. Ubuntu 22.04 and Debian 12 users stay out. Rejected; that is what #349 fixes.

### The AppImage: the tool

1. **appimagetool**, pinned. It downloads the runtime itself unless given one, and validates the `.desktop` file with tools it doesn't ship. It's one more binary in the image whose job is two steps. Rejected.
2. **CPack's AppImage generator** (CMake 4.2+). The image's CMake is 4.4, but CPack still needs the runtime file and a `mksquashfs`, and it would package from the install tree, not from the guarded tarball. Rejected.
3. **No tool: the runtime and `mksquashfs`.** A type-2 AppImage is the runtime's ELF followed by a SquashFS image. The runtime finds the image at the end of its section header table. The build image downloads the runtime of `AppImage/type2-runtime` release 20251108 (static, FUSE 3 built in) and checks its SHA-256. `squashfs-tools` comes from the snapshot archive. `mtport.linux_package appimage` unpacks the player tarball into an AppDir and adds `AppRun` (a link to `MapleThief.sh`), the `.desktop` entry, the icon under the entry's name and `.DirIcon`. It runs `mksquashfs -comp gzip -all-root -no-xattrs`, with every time set to `SOURCE_DATE_EPOCH` or 0, and writes the runtime and the image back to back. **Chosen.** No update information and no signature are embedded. The release's `SHA256SUMS` covers the file.

### The AppImage: FUSE

The runtime mounts the image with FUSE 3 (`fusermount3`). SteamOS, Ubuntu 22.04+, Debian 12, Fedora and Arch desktops have it. Where it's missing (containers, minimal installs), `APPIMAGE_EXTRACT_AND_RUN=1` or `--appimage-extract-and-run` unpacks the image into a temporary folder and runs it from there. Our checks use that. The play notes say so. **Chosen**; the tarball stays for systems where neither works.

### The Flatpak bundle

1. **flatpak-builder with a manifest.** It builds from sources in a sandbox, which needs the SDK (several GB) and our recompiled code as a source: a second build of the same game. Rejected.
2. **Flathub.** It needs redistributable content built on Flathub's builders. Rejected (see Context).
3. **A build directory written by `mtport.linux_package flatpak`, committed by `flatpak build-export` and written by `flatpak build-bundle`.** The build directory holds `metadata` against `org.freedesktop.Platform//25.08` (supported until 2027-08) and the player tarball's files under `files/`, with `/app/MapleThief.sh` as the command. It also holds the menu entry and icon under `files/share` and `export/share`. Flatpak 1.12 comes from the snapshot archive. `--runtime-repo` names Flathub, so `flatpak install MapleThief-….flatpak` installs the runtime from there. Permissions: X11, Wayland, the GPU and every device (for gamepads), PulseAudio, `ipc`, and Downloads read-only (the IPA when the file dialog portal is missing). No network. The bundle is unsigned; its checksum is in `SHA256SUMS`. **Chosen.** `build-export`'s icon check runs through a wrapper (`FLATPAK_VALIDATE_ICON`) that calls `flatpak-validate-icon` without its bwrap sandbox. The icon is our own committed file.

### How the release guard reads them

1. **Guard the tarball only and trust the repacking.** The guard would never see the bytes players download. Rejected.
2. **Unpack with system tools** (`unsquashfs`, `ostree`) at release time. They aren't on the Mac, and the guard runs there (`scripts/release publish`). Rejected.
3. **Read both in Python, as the NRO is read (ADR 0138).** **Chosen.**
   - `mtport.squashfs` reads SquashFS 4.0 with zlib (anything else is refused): superblock, metadata blocks, inodes, directories, data blocks, fragments and sparse blocks. `appimage_parts` splits off the runtime at the section header table's end. Bytes after the image other than zero padding are refused.
   - `mtport.gvariant` decodes GVariant's normal form. `mtport.flatpak_bundle` reads the static delta's superblock, inflates each inline part (xz) and runs its operations (`S`, `o`, `w`, `c`). It refuses operations that need another commit (`r`, `R`, `B`) and fallback objects. It checks **every object against its OSTree checksum** (metadata: SHA-256 of the bytes; content: of OSTree's file header and the bytes) and the commit against the superblock's. It walks the tree from the commit, so the guard sees exactly the files the bundle installs.
   - The guard expands `*.AppImage` into the runtime under the file's own name, plus each file below `<name>.AppImage/`. The runtime must have one of the pinned SHA-256s (`APPIMAGE_RUNTIMES`). It expands `*.flatpak` into its tree below `<name>.flatpak/`, plus other bytes the bundle's header carries, below `<name>.flatpak/.bundle/`. Every rule of ADR 0024 and ADR 0125 then applies. The icon allowlist gains the paths where the packages name `icon.png`, with the same hash.

### The data folder in each package (ADR 0127)

1. **The AppImage: next to the `.AppImage` file.** The runtime sets `APPIMAGE` to the image's absolute path. On Linux the resolver takes that file's folder as `<base>`, instead of the read-only mount `SDL_GetBasePath` names. So `data/`, `game/` and `user-data.txt` sit beside the `.AppImage`, as they sit beside `MapleThief.sh` for the tarball. On a Deck, `~/Games/MapleThief.AppImage` keeps its data in `~/Games/data/`, which survives SteamOS updates. **Chosen.** Taking the per-user folder instead was rejected: it hides the data from the player, which ADR 0127 replaced.
2. **The Flatpak: the sandbox's per-user folder, `~/.var/app/io.github.armaatus.maplethief/data/MapleThief`.** `/app` can't be written, and the sandbox sees no folder that the player would call "next to the game". The resolver gets a rule after the macOS Applications one: on Linux with `FLATPAK_ID` set, location `user`, reason **`flatpak`**. It is a normal state, not a fallback. The Saves tab says where the data is and that the AppImage or tarball keeps it next to the game, and switching to *Game folder* is refused with that reason. `--data-location game` still tries, and falls back with `fallback-not-writable`. Letting the probe fail silently into `fallback-not-writable` was rejected: it would show a warning for the Flatpak's normal case. **Chosen.**

### The checks

`scripts/linux-game check-player` unpacks the player tarball and starts it read-only in stock `ubuntu:22.04` and `debian:12`, both pinned by digest. It runs `--version` and `--print-data-dir`, then a headless run that must end with exit 4. `scripts/linux-game check-packages` runs the AppImage (`APPIMAGE_EXTRACT_AND_RUN=1`) and the bundle under Xvfb in the build image. Each passes when it logs that the import screen is up, and when `--print-data-dir` shows its rule. The Flatpak needs a privileged container (bwrap creates namespaces) and Flathub's runtime, installed into the Docker volume `maplethief-flatpak-check`, about 2 GB, which is removed afterwards. Under x86_64 emulation on an arm64 kernel, `FLATPAK_BWRAP` points at `tools/containers/linux-game/bwrap-no-seccomp`. It drops only the seccomp filter, which Rosetta can't load. These checks need the IPA's generated code and Docker, so they are local. `scripts/release build` runs both.

## Decision

- The Linux build image is Ubuntu 22.04 by digest with apt from the 2026-11-25 snapshot, GCC 14 from the Toolchain PPA's snapshot (its key committed), `squashfs-tools`, Flatpak 1.12, Xvfb and the pinned AppImage runtime. `scripts/linux-game` builds with `MAPLETHIEF_LINUX_GLIBC_FLOOR=2.35`, and `platform.linux_portable` checks the highest `GLIBC_` version against it.
- `scripts/linux-game player` writes `MapleThief-<version>-linux-x86_64.tar.gz`, `.AppImage` and `.flatpak` (`mtport.linux_package`). The data folder rules are as above. `MapleThief.sh --install-desktop` names the `.AppImage` file and copies the icon out of the mount. In the Flatpak it only says that Flatpak manages the entry.
- **One image for both CPUs.** The same Dockerfile builds the amd64 and the arm64 image of ADR 0194: Ubuntu's snapshot and the Toolchain PPA's both carry jammy's arm64 packages (glibc 2.35, GCC 14.3), and Kitware's CMake and the type-2 runtime have aarch64 builds. So the aarch64 player tarball has the same 2.35 floor. The AppImage and the Flatpak bundle are **x86_64 only**: `scripts/linux-game` packs them only in the amd64 image, and `linux-packages` names only x86_64 files. No aarch64 package has been run to the import screen, the Steam Deck is x86_64, and ARM Linux users have the tarball. `mtport.linux_package` itself takes either CPU's tarball, so adding them later is a release-list and check change.
- The guard reads AppImages and Flatpak bundles (`mtport.squashfs`, `mtport.gvariant`, `mtport.flatpak_bundle`; [squashfs.md](../formats/squashfs.md), [flatpak-bundle.md](../formats/flatpak-bundle.md)).
- `scripts/release build` builds, checks (`check-player`, `check-packages`), guards and uploads the two packages with the tarball. `publish` expects them (`mtport.release linux-packages VERSION`) and guards them again. itch.io keeps the tarball as the `linux-x86_64` channel.

## Consequences

- The Linux downloads run on glibc 2.35 and later. The game folder of ADR 0065 is built in the same image, so it runs there too.
- Two more release assets, about 8 MB and 5 MB, plus three Python readers to keep. Their inputs are only what our own packer writes. Other compressors (SquashFS) and non-scratch deltas (Flatpak) are refused, not guessed.
- The Flatpak check costs about 2 GB of download per run unless `--keep-flatpak-runtime` keeps the volume.
- The bundle and the AppImage are unsigned. A player checks them with `SHA256SUMS`, as every other asset.
- Not checked on hardware: the AppImage and the Flatpak on a Steam Deck in Game Mode (`needs-human`, as ADR 0065's Deck check).
