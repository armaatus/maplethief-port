# 249. The AppImage runtime ships with its notices, and its LGPL part is a separate program's

- **Status:** Accepted (delegated by Joris, 2026-11-28)
- **Date:** 2026-11-28
- **Issue:** #439 (epic #291)
- **Revises:** [ADR 0003](0003-dependencies.md) (its rule on LGPL code in what ships, for a third-party program we pass on unchanged)
- **Builds on:** [ADR 0193](0193-linux-glibc-2-35-appimage-and-flatpak-bundle.md) (the AppImage is the pinned type-2 runtime followed by a SquashFS image of the player's files)

## Context

ADR 0193 ships `MapleThief-<version>-linux-x86_64.AppImage`. The first 944,632 bytes of it are AppImage/type2-runtime's release `20251108`, a static executable we download and check by SHA-256 but don't build. THIRD_PARTY.md had no entry for it, and GitHub reports its licence as NOASSERTION.

Read from its source at commit `dd6cebedcbddde9c82f89b011e8e1d40b6e43868`: `LICENSE` and `src/runtime/runtime.c` are MIT. `LICENSE` also lists what the binary links statically: musl (MIT), libfuse (LGPL-2.1), squashfuse (BSD-2-Clause), zstd (BSD-3-Clause or GPL-2.0, its choice of the user's), zlib (Zlib). `src/runtime/Makefile` links mimalloc (MIT) too, which the list leaves out. The versions are libfuse 3.15.0 and squashfuse 0.5.2 (the build scripts, each archive checked by SHA-256), zstd 1.5.6 and zlib 1.3.1 (version strings in the binary), musl 1.2.5 and mimalloc 2.1.7 (Alpine 3.21, which the build uses).

ADR 0003 says: no GPL in anything we compile, link or ship, and LGPL only if it can be linked dynamically on every target. The runtime is LGPL code we ship, linked statically. But we don't link it: the runtime is its own program. It mounts the SquashFS image with FUSE and runs `AppRun`, which starts our executable as a new process. Our program never calls into the runtime, and the runtime never loads our code. ADR 0003 already lets GPL developer tools run as separate programs; this is the shipped case of the same thing.

## Decision

- **The runtime may ship.** It is a separate program, redistributed unchanged from its upstream release and checked by SHA-256. ADR 0003's static-LGPL rule is about what links with our code. It still holds there, unchanged. Any other third-party program that is to ship inside a download needs its own decision.
- **Its notices go with every Linux player download**, in `licenses/AppImage-runtime/`: the runtime's own `LICENSE` and the licence text of each library it links. Each is pinned by URL and SHA-256 (`MAPLETHIEF_APPIMAGE_LICENSE_PINS` in `cmake/Dependencies.cmake`) and downloaded at configure time, like the Switch's notices. They go into the player tarball because `mtport.linux_package` packs the tarball's files behind the runtime, so the AppImage carries them. The tarball and the Flatpak bundle carry them too, which costs a few KB and keeps one layout.
- **The LGPL's source.** The runtime's complete corresponding source is public: its repository at the pinned commit (with its patch to libfuse) and the libfuse and squashfuse archives its build script names, each by SHA-256. THIRD_PARTY.md names all three. Whether our release page should also carry copies of those archives, so that the source sits where the binary is downloaded, is a question for Joris's legal sign-off. It is not decided here.
- **Pins.** The runtime's release and SHA-256s stay in `tools/containers/linux-game/Dockerfile`. `scripts/deps check` lists the release against upstream's tags (`APPIMAGE_RUNTIME_RELEASE`) and the licence texts as following it. `tests/test_deps.py` fails when a pin of a download that ships in a player download (`DOCKER_ARG_PACKAGED` in `tools/mtport/deps.py`) is missing from THIRD_PARTY.md, and when the guard's `APPIMAGE_RUNTIMES` differs from the Dockerfile's hashes.

## Alternatives

1. **Build the runtime ourselves, without libfuse.** The runtime's FUSE support is its point. Without it, users would have to run `--appimage-extract-and-run` every time. Rejected.
2. **Drop the AppImage.** The tarball and the Flatpak would remain. This undoes ADR 0193 for a licence that only needs notices and source. Rejected.
3. **Notices only in THIRD_PARTY.md.** The MIT and BSD licences ask for their text to go with the binary, and the LGPL asks for a copy of itself. A link isn't a copy. Rejected.

## Consequences

- Every Linux player download has seven more text files, about 40 KB.
- A runtime bump also means reading its `LICENSE`, `Makefile` and build scripts again and moving the licence pins ([Bumping a dependency](../build/how-to/bump-a-dependency.md)).
- Open: Joris's sign-off on how the LGPL's source is offered (a link to upstream, or copies on the release page).
