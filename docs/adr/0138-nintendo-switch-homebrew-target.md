# 138. The Nintendo Switch target: a homebrew NRO, devkitA64 in a pinned container, SDL3 from nx-mod/sdl3-nx

- **Status:** Accepted (2026-11-10, [Joris on #234](https://github.com/armaatus/maplethief-port/issues/234#issuecomment-6779964411))
- **Date:** 2026-11-10
- **Issue:** [#234](https://github.com/armaatus/maplethief-port/issues/234) (epic [#233](https://github.com/armaatus/maplethief-port/issues/233))
- **Builds on:** [ADR 0002](0002-language-and-toolchain.md) (languages), [ADR 0003](0003-dependencies.md) (dependencies; this ADR adds an addendum), [ADR 0004](0004-build-presets-and-generators.md) (presets), [ADR 0023](0023-local-ci-and-packaging.md) (containers), [ADR 0025](0025-guest-memory-and-address-space.md) (guest memory), [ADR 0039](0039-rendering-backend-software-rasterizer.md) and [ADR 0060](0060-gpu-rendering-backend.md) (rendering), [ADR 0100](0100-public-repository-and-player-releases.md) (player releases), [ADR 0127](0127-data-folder-next-to-the-game.md) (data folder)

## Context

The epic's research (2026-11-06) sets out why the Switch fits: no JIT is needed, the CPU is AArch64 little endian, guest pointers already go through `cpu->mem + addr`, and the 20 Hz tick divides the 60 Hz display. This ADR settles the choices every child of the epic builds on. The checks below ran on 2026-11-10 on this Mac (Apple silicon, Docker 29.8.2). Full commands and log tails are in [the evidence comment on #234](https://github.com/armaatus/maplethief-port/issues/234#issuecomment-6774634381). **Nothing here ran on a console.** Everything about behaviour comes from reading the source or from what the build enabled, and the hardware runs belong to the later children.

**The toolchain image.** `docker buildx imagetools inspect devkitpro/devkita64` shows a multi-architecture index, digest `sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282`, with `linux/amd64` and `linux/arm64` images. In the arm64 image (`uname -m` says `aarch64`, native on this Mac) and in the amd64 one (run under emulation), the packages are the same:

| Package | Version |
|---|---|
| `devkita64-gcc` | 15.2.0-7 |
| `libnx` | 4.12.0-1 |
| `devkita64-newlib` | 4.6.0.20260123-4 |
| `switch-cmake` | 1.5.2-1 |
| `switch-tools` (`elf2nro`, `nacptool`) | 1.13.1-1 |
| `switch-mesa` | 20.1.0-5 |
| `deko3d` | 0.5.0-1 |
| `uam` | 1.1.0-1 |

The image holds CMake 3.31.6 (our minimum is 3.28), Ninja 1.11.1 and Python 3.11.2 on Debian 12. `Switch.cmake` is `/opt/devkitpro/cmake/Switch.cmake`, and `nx_generate_nacp` and `nx_create_nro` are in `/opt/devkitpro/cmake/Platform/NintendoSwitch.cmake`. The image has `switch-sdl2` but no SDL3 package. It has no `libvulkan_nouveau` and no Vulkan headers.

**The two SDL3 sources**, each fetched as a GitHub archive at a commit:

| | devkitPro `switch-sdl-3.4` | [`nx-mod/sdl3-nx`](https://github.com/nx-mod/sdl3-nx) |
|---|---|---|
| Commit | `c329016c0e338ab4397ad930f83330c9fe058348` | `07ea26a3ca7b60bac414823f0f45e8738c61c217` (branch `switch-sdl3-deko3d`) |
| Archive SHA-256 | `6047e4787f105ab4a0c6293ee3090ce5d1ebad112fcbe2a3e5a8874b0cc6f709` | `d6511751694fb868c123d0da142ad0417420bb1fe650bca25fb2ad6c8be81801` |
| SDL version | 3.4.0 | 3.4.18 (a merge of the same tag as our desktop pin, commit `829a65d7`) |
| Last commit | 2026-02-01 (Dave Murphy) | 2026-10-17 (nx-mod) |
| Upstream activity | devkitPro's own fork; this branch last changed on 2026-02-01; its open pull requests #128 to #130 (platform name, keyboard, locale) carry fixes the other fork took; 26 open issues, mostly about other consoles | a single person's fork of devkitPro's, created 2026-10-04, 0 stars, issues disabled, last pushed 2026-10-17 |
| License | Zlib | Zlib |

Both built with the options of `maplethief_use_sdl3()` (`BUILD_SHARED_LIBS=OFF SDL_SHARED=OFF SDL_STATIC=ON SDL_TEST_LIBRARY=OFF SDL_TESTS=OFF SDL_EXAMPLES=OFF SDL_INSTALL=OFF`) through `$DEVKITPRO/cmake/Switch.cmake`, Release. Each also linked a small test program calling `SDL_Init`, `SDL_CreateRenderer`, `SDL_CreateGPUDevice`, `SDL_OpenAudioDeviceStream`, `SDL_GetGamepads`, `SDL_GetPrefPath` and `SDL_GetBasePath`, and `nx_create_nro` wrapped that program in an NRO. Both NROs have the `NRO0` magic and the `ASET` asset block holding the NACP.

| | devkitPro `switch-sdl-3.4` | `nx-mod/sdl3-nx` |
|---|---|---|
| `libSDL3.a` | 16.9 MB | 17.8 MB |
| Link line | `-lnx -lEGL -lstdc++ -lglapi -ldrm_nouveau` | the same plus `-ldeko3d` |
| Renderers (`SDL_CreateRenderer` tries in this order) | `opengl`, `opengles2`, `software` | `opengl`, `opengles2`, `gpu`, `software` |
| What presents | Mesa (`switch-mesa`) through EGL; the backend has no window framebuffer, so `software` also presents through a texture on a GL renderer | the same |
| The `gpu` renderer | not built | built, but `SDL_GPU_PRIVATE` is not defined, so it carries no DKSH shaders for deko3d: it can't start on the Switch, and `SDL_CreateRenderer` falls through it |
| SDL_GPU | none (`SDL_GPU_DEKO3D` absent; no driver) | `SDL_GPU_DEKO3D` on (`src/gpu/deko3d`, 2,593 lines), `SDL_GPU_SHADERFORMAT_PRIVATE` DKSH shaders from GLSL through UAM. Its README calls it "early": a clear test runs on hardware |
| Audio | `SDL_AUDIO_DRIVER_SWITCH`: audren at 48 kHz, `S16` only (SDL converts), mono and stereo, voices resampled by audren | the same file |
| Gamepads | `SDL_JOYSTICK_SWITCH`: up to 8, the name `Switch Controller` for all, a mapping that puts A on `b1` and B on `b0` so SDL's South is the bottom face button; nothing in SDL's type tables knows that name, so the gamepad type is Standard and SDL's labels are Xbox ones | the same, plus rumble with two motors per controller (the branch hands libnx one handle) and motion sensors |
| Joy-Con | attached pair, detached pair and a single Joy-Con on its side (`pad_mapping_left_joy`, `_right_joy`) | the same |
| Touch | `SDL_TOUCH_DEVICE_DIRECT`, fingers normalised against 1280 x 720 | the same |
| `SDL_GetBasePath` | `romfs:/` | `romfs:/` |
| `SDL_GetPrefPath` | the working directory; ignores `org` and `app` | the same |
| Locale | none (`SDL_GetPreferredLocales` gives nothing) | `src/locale/switch` |
| Lifecycle | **none.** libnx's default focus mode suspends the process inside `appletMainLoop`, and SDL hears only `SDL_EVENT_QUIT` | focus mode `SuspendHomeSleepNotify` for applications (applets get focus changes anyway). `Background` focus sends `SDL_EVENT_WILL_ENTER_BACKGROUND`, `DID_ENTER_BACKGROUND`, the foreground pair on return, and closing from the HOME menu sends `SDL_EVENT_TERMINATING` before `SDL_EVENT_QUIT` |
| Extra libraries over devkitPro's branch | none | deko3d 0.5.0 (Zlib). The optional NVK path (`-DSDL_VULKAN=ON`, off by default on Switch) was not built. The link line and the ELF's symbols contain no Vulkan driver: the only name matching `nvk` is deko3d's own `NvKind` type |

**The memory model today.** `runtime/src/mem/vm.cpp` has a POSIX branch (`mmap`) and a Windows branch. newlib in the image has no `sys/mman.h` and `libc.a` has no `mmap`, so Horizon needs a third branch. `MAPLETHIEF_MEM_MODEL` takes `auto`, `checked` and `reserved`; `maplethief_mem_checked()` forces `checked` for iOS by system name only, and the `ios-base` preset sets it to `checked`. The checked model reserves only the 512 MiB arena (`MT_MEM_RESERVE_ARENA`, ADR 0025), with the 480 MiB guest heap committed on demand.

**The data folder.** ADR 0127 already names the Switch: location `switch`, reason `platform`, `sdmc:/switch/maplethief/data/` and the bundle in `sdmc:/switch/maplethief/game/`.

## Options considered

### Format

1. **NRO only, started from the Homebrew Menu.** **Chosen.** It is the native homebrew format, and needs nothing beyond devkitPro's tools.
2. **An NSP or forwarder.** Rejected: it would need the console's keys or signature patches. Neither belongs in this project.

### SDL3 source

1. **devkitPro `switch-sdl-3.4`.** Rejected, despite being devkitPro's own and the less risky repository to depend on, because it has no lifecycle events, no SDL_GPU and a different SDL version from every other target. Without SDL_GPU, [#242](https://github.com/armaatus/maplethief-port/issues/242) would be dropped; without lifecycle events, ADR 0063's background handling and ADR 0089's autosave on background would have to go through libnx directly. It stays the fallback (see *How we'd know*).
2. **`nx-mod/sdl3-nx`.** **Chosen**, at commit `07ea26a3`. It is SDL 3.4.18, the same version as our pin on every other target, so the frontend meets the same SDL. It has the lifecycle events and rumble #239 and #240 want and the SDL_GPU backend #242 needs. Its risks are a single maintainer, a repository five weeks old, and an SDL_GPU backend described as early. The pin by commit and SHA-256 takes care of "it changes under us"; a rewrite of the repository would make the configure step fail, loudly.

### Vulkan through Mesa NVK

Rejected. Linking the driver statically makes the binary GPL, and ADR 0003 forbids GPL in anything linked or shipped. `SDL_VULKAN` is off by default on the Switch in the chosen fork; #235 sets it `OFF` explicitly.

## Decision

**Format.** The Nintendo Switch target is the NRO `MapleThief.nro`, with an NACP and a 256 x 256 JPEG icon made from our own files, started from the Homebrew Menu on a console running custom firmware. We build, ship and document only that. No NSP or forwarder is built, shipped or documented. `docs/play/switch.md` may say in one sentence that players can make a forwarder with existing tools, and gives no steps.

**Toolchain.** devkitA64 with libnx, from the Docker image `devkitpro/devkita64` pinned by the index digest `sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282` (GCC 15.2.0, libnx 4.12.0, newlib 4.6.0.20260123), wrapped in `tools/containers/switch/` as ADR 0023's other containers are ([#235](https://github.com/armaatus/maplethief-port/issues/235)). The build uses devkitPro's `Switch.cmake` toolchain file and `nx_generate_nacp` and `nx_create_nro`. This is the one target that compiles the generated code with GCC rather than clang, as devkitPro ships no clang for it (ADR 0002's "clang for shipping builds" has this exception); ADR 0002's minimum GCC is 13. The image runs natively on Apple silicon (arm64 manifest, no Rosetta) and through Docker on Windows and CI (amd64 manifest). The index digest covers both, and both were checked to hold the same package versions.

**SDL3 source.** `nx-mod/sdl3-nx`, commit `07ea26a3ca7b60bac414823f0f45e8738c61c217` (SDL 3.4.18), fetched as `https://github.com/nx-mod/sdl3-nx/archive/07ea26a3ca7b60bac414823f0f45e8738c61c217.tar.gz` with SHA-256 `d6511751694fb868c123d0da142ad0417420bb1fe650bca25fb2ad6c8be81801`, built static with `SDL_VULKAN=OFF`. GitHub does not promise that generated archives stay byte for byte the same; if the hash breaks, configure fails on it, and the commit is the real pin to re-derive it from. The other targets keep the upstream SDL 3.4.18 release archive. #235 wires this in with a Switch branch in `maplethief_use_sdl3()`.

**Rendering.** The software rasterizer (ADR 0039) is the default on the Switch. It presents through `SDL_CreateRenderer`. The frontend (#238) sets `SDL_HINT_RENDER_DRIVER` to `opengles2`, so the picture goes through switch-mesa's GLES 2 path; `opengl` would otherwise be tried first, and the driver's own default profile is ES 2.0. The GPU backend (ADR 0060) on deko3d through SDL_GPU is optional and comes later, in [#242](https://github.com/armaatus/maplethief-port/issues/242), with a fourth shader format (DKSH from UAM). Mesa NVK is ruled out.

**Memory.** The `switch-*` presets set `MAPLETHIEF_MEM_MODEL=checked`, as `ios-base` does, and the runtime reserves only the 512 MiB arena. The arena is reserved in address space and committed on demand, so the game fits applet mode. The mechanism belongs to [#236](https://github.com/armaatus/maplethief-port/issues/236). #235 adds the Switch to `maplethief_mem_checked()`'s check, so that `reserved` on the Switch is a configure error, as on iOS.

**Files on the SD card.** ADR 0127's layout holds, with `<base>` fixed at `sdmc:/switch/maplethief/`:

```text
sdmc:/switch/maplethief/
  MapleThief.nro
  <the player's IPA>          the player's own copy, read-only input to the import (ADR 0100)
  game/                       the imported bundle, regenerable from the IPA
  data/                       settings.ini, bindings.ini, window.ini, saves/, backups/, states/,
                              screenshots/, exports/, import/, logs/, crashes/ (ADR 0127's layout)
```

The folder is fixed by the platform, not derived from SDL: `SDL_GetBasePath()` answers `romfs:/` and `SDL_GetPrefPath()` the working directory, so the resolver of ADR 0127 reports the location `switch` with the reason `platform` and calls neither. There is no per-user option, no marker and no fallback, as ADR 0127 says. Anything that cannot be written is an error on screen. How the import finds the IPA in this folder is #243's.

**What ships.** The player build of the NRO under ADR 0100: the runtime and the recompiled code, with no file from the IPA's bundle and nothing converted from one. It carries the license notices of what it links: libnx (ISC), newlib (its BSD-style notices), libstdc++ and libgcc (GPL with the GCC Runtime Library Exception, which allows this), SDL (Zlib), Mesa `libEGL`, `libglapi` and `libdrm_nouveau` (MIT, linked on the default path), and deko3d (Zlib). The notice set lives in `THIRD_PARTY.md` and goes into the player kit like every other target (#243).

**Not a target:** Switch 2 homebrew, NSPs. Overclocking is not done by default.

## Consequences

- The Switch gets the same SDL version as every other target, and the frontend's event handling works unchanged: `SDL_EVENT_WILL_ENTER_BACKGROUND` and its partners reach ADR 0063's `Lifecycle` and ADR 0089's autosave.
- `SDL_GetGamepads` reports the Switch's controllers with Standard type and Xbox labels, while the physical layout is Nintendo's. #239 sets the Nintendo layout and glyphs itself rather than trusting SDL's type or labels. The mapping already puts South on the bottom face button.
- No SDL_GPU backend means no `gfx_backend = gpu` until #242. The GPU renderer listed by SDL cannot start on deko3d.
- SD card paths are hard-coded for this one target: the resolver of ADR 0127 gets a Switch branch.
- Mesa's EGL is in every Switch build, so its MIT notices ship.
- Follow-up work: every child of epic #233, with the SDL3 pin, the image digest and the Switch branch of `maplethief_use_sdl3()` in #235, and the Horizon memory backend in #236.

**How we'd know this was wrong:**

- SDL3 for the Switch stalls upstream: the fork disappears or stops following SDL releases, or the deko3d backend does not work on real hardware in #242. Then the fallback is devkitPro's `switch-sdl-3.4` for what it offers (software rendering, audio, input), #242 is dropped, and the lifecycle events of ADR 0063 go through libnx's applet hooks in `platform/`. A mirror of the pinned archive under this project's account keeps the build alive if the repository alone goes away.
- The game doesn't fit applet mode, measured by #240's check: then title takeover becomes the documented way to run it.
- The image digest stops pulling or its toolchain turns out to be unable to build the generated code: #235 finds this first.

## Addendum: the game's start on the console (#238, 2026-11-11)

What #238 built, read from the source of the pinned SDL fork and checked by building; the console runs are `needs-human` on #238.

- **The renderer.** `platform/switch/switch_app.cpp`'s `prepare_sdl` sets `SDL_HINT_RENDER_DRIVER = opengles2` before `SDL_Init`, as decided above. The window is created with `SDL_WINDOW_OPENGL`, so SDL does not recreate it for the renderer.
- **The window is the screen.** It is created at the size of the console's operation mode (`appletGetOperationMode`: 1280 x 720 handheld, 1920 x 1080 docked; `platform/src/switch_screen.h`) and resizable, never fullscreen. `nwindow`'s buffer has the window's size and the system scales it to the screen, so a window of any other size (the desktop's 960 x 640) would be stretched. No `window.ini`, no fullscreen display, no minimum size, as on iOS.
- **Dock and undock.** The fork's `SWITCH_PumpEvents` calls `SDL_SetWindowSize` with the new screen's size when the operation mode changes, but only for a resizable window, and its `SWITCH_SetWindowSize` rebuilt the EGL surface at `window->w` x `window->h`, the size *before* the change (SDL 3 passes the new size in `window->pending`), and sent no `SDL_EVENT_WINDOW_RESIZED`. The picture would have kept its old size and the renderer its old output size. **We fix that function at configure time**: `cmake/patches/sdl3-nx-window-size.cmake`, FetchContent's `PATCH_COMMAND` for the Switch's SDL3, replaces it with one that reads `window->pending`, rebuilds the surface at that size and sends `SDL_EVENT_WINDOW_RESIZED` (which updates `window->w` and `h`, and gives the renderer and the presenter `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`). It changes the file only if both texts it replaces are there exactly, and fails the configure step otherwise; a second run on the same source does nothing. The pin by commit and hash is unchanged; the fix is a candidate for the fork's upstream. The presenter places the picture at either size with ADR 0043's modes and ADR 0085's borders, unchanged; `scaling: the Nintendo Switch's screens, handheld and docked (#238)` pins the placements (integer 2x at (160, 40) handheld, 3x at (240, 60) docked; fit, smooth and sharp 1080 x 720 and 1620 x 1080, pillarboxed). The Switch's default scale mode is `sharp` (ADR 0070's platform rows).
- **No bundle.** The import screen needs a file dialog or a drop, so on the Switch a game that finds no bundle shows `platform/src/missing_bundle.{h,cpp}` instead: the folder it looked in (`sdmc:/switch/maplethief/game/MapleThief.app`), that the player's own extracted `MapleThief.app` goes there, and that the import on the console comes with #243. Any confirm or back control, a tap or HOME's close ends it, and the run ends with the "no bundle" status. Until #243, copying the bundle there by hand is the player's own setup under ADR 0100; nothing of it is in the NRO or its RomFS.
- **Logs.** When nxlink sent the NRO (`__nxlink_host` set), `NxApp` opens the sockets and points stdout and stderr at it, so `scripts/switch-run` shows every `SDL_Log` line; from the Homebrew Menu the sockets are not opened (applet mode is short of memory). Every line also goes to the player log, `sdmc:/switch/maplethief/data/logs/MapleThief-<time>.log`, the layout above (ADR 0127's `logs/`, rotated to the newest 5, with the header and the crash report's tail), rather than a separate `sdmc:/switch/maplethief/log.txt` as #238's text proposed: one place for logs on every platform. The crash report stays where #236 put it (`sdmc:/switch/maplethief/crash.txt`, plus `data/crashes/`).

## Addendum: the GPU backend on deko3d (#242, 2026-11-11)

[ADR 0143](0143-gpu-backend-on-deko3d.md) settles `gfx_backend = gpu` on the Switch. The table above says SDL's `gpu` renderer can't start on deko3d because the fork carries no DKSH shaders for it. A second configure-time patch (`cmake/patches/sdl3-nx-gpu-renderer.cmake`, after #238's) gives it DKSH builds of SDL's own render shaders (`cmake/patches/sdl3-nx-render-shaders.h`, from `scripts/build-shaders`) and makes the deko3d swapchain follow a dock or undock. With it, SDL's GPU renderer presents through deko3d's swapchain on mt_gfx's device. The window leaves out `SDL_WINDOW_OPENGL` when the GPU is chosen at start, so the EGL surface doesn't take the screen first. The software rasterizer on `opengles2` stays the default.

## Addendum: the import from the SD card and the player zip (#243, 2026-11-11)

**How the import finds the IPA**, left to #243 above: the import screen's SD-card route (`platform/src/import_screen.{h,cpp}`, chosen by ADR 0142's new capability `kCapIpaOnSdCard`, the Switch's only). With no file dialog and no drop, it looks in `<base>` at once (`ipa_import.h`'s `find_ipas`): `MapleThief-1.5.ipa` first, then every other regular file whose name ends in `.ipa` (any case) by name, leaving out hidden files and the `._` files a Mac writes beside each file it copies to a FAT card. It imports them in that order until one is the game, with #182's checks and safe extraction unchanged (the exact 1.5 file, ADR 0135; a temporary folder renamed into `game/`), and shows the progress, the file's place in the list, and each refusal with its reason (not a zip, another app or version, encrypted, damaged). With no IPA it says what to copy where; A looks again, + returns to the Homebrew Menu. After an import it asks whether to delete the imported IPA (No focused, as the settings menu's questions; Right to Yes, A answers, B is No, a tap works). `main.cpp`'s importer holds `mt::switch_nx::CpuBoost` while it runs (ADR 0141). The missing-bundle screen of #238 (`platform/src/missing_bundle.{h,cpp}`) is gone: the import screen replaces it. Host tests run the search order and the whole flow over a fake SD folder (`tests/cpp/ipa_import_test.cpp`), and `ipa_import_real_test` imports the real IPA through the route (local).

**What ships** is now built: the `switch-player` preset and `MapleThief-<version>-switch.zip` (ADR 0125's addendum), with the notices listed above in `licenses/`, each text downloaded from the source of the image's version, since the image ships none of them. Players read [Playing on the Nintendo Switch](../play/switch.md). The hardware runs (import, first map, a refusal, the second start) are `needs-human` on #243.
