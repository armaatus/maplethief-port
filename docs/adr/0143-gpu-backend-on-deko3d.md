# 143. The GPU backend on the Nintendo Switch: SDL_GPU on deko3d, DKSH from the same HLSL, SDL's GPU renderer made to start

- **Status:** Accepted
- **Date:** 2026-11-11
- **Issue:** [#242](https://github.com/armaatus/maplethief-port/issues/242) (epic [#233](https://github.com/armaatus/maplethief-port/issues/233))
- **Builds on:** [ADR 0060](0060-gpu-rendering-backend.md) (the GPU backend, the shader build path, the tolerance), [ADR 0066](0066-shader-filters-and-text-at-the-render-scale.md) (the present filters), [ADR 0068](0068-gpu-presentation-without-read-back.md) (GPU presentation), [ADR 0138](0138-nintendo-switch-homebrew-target.md) (the Switch target, sdl3-nx at `07ea26a3`, the `cmake/patches/` mechanism of its #238 addendum)

## Context

ADR 0138 chose `nx-mod/sdl3-nx` at commit `07ea26a3` partly for its SDL_GPU backend on deko3d, and left `gfx_backend = gpu` on the Switch to this issue. What the pinned source says, read file by file (nothing here ran on a console):

- **The backend** (`src/gpu/deko3d/SDL_gpu_deko3d.c`, 2,593 lines) takes shaders only as `SDL_GPU_SHADERFORMAT_PRIVATE`: DKSH modules, deko3d's format, which UAM compiles from GLSL. In that GLSL each resource kind is bound per stage from slot 0, by `layout(binding = slot)`: samplers 0 to 15, uniform blocks 0 to 3, storage images and buffers 0 to 7. It has everything ADR 0060's backend and the present stage use: render passes with load and store, the eleven blend factors including `SRC_ALPHA_SATURATE`, line lists, nearest and linear samplers with clamp and repeat, `UBYTE4_NORM` and float vertex attributes, buffer and texture uploads, texture downloads, texture copies, linear blits, fences, and a swapchain on the screen's `NWindow` with vsync or immediate presentation.
- **Clip space.** The backend makes its device with deko3d's defaults, `DepthZeroToOne | OriginUpperLeft`. In deko3d 0.5.0 (`source/maxwell/gpu_3d_base.cpp`) an upper-left origin makes the viewport's Y scale negative, so clip-space +Y points up, the window's row 0 is at the top and depth runs 0 to 1: SDL's (and D3D's) convention, the one our HLSL is written for. No `-fvk-invert-y`.
- **SDL's GPU renderer** (`src/render/gpu/`) carries its own shaders as SPIR-V, DXIL and MSL, and as private shaders only when `SDL_GPU_PRIVATE` is defined, from a header `SDL_render_private_shaders.h` that the fork doesn't have. `SDL_GPU_PRIVATE` names a console SDK's own driver: defining it would also register a `PrivateGPUDriver` that doesn't exist. So `SDL_CreateGPURenderer` fails on deko3d ("Unsupported GPU backend") and `SDL_CreateRenderer` falls through it (ADR 0138's table). Everything the frontend presents goes through `SDL_Renderer`: the Presenter, GpuScreen (ADR 0068) and every filter's `SDL_GPURenderState` (ADR 0066) need SDL's GPU renderer on mt_gfx's device.
- **The screen.** Since #238 the window is made with `SDL_WINDOW_OPENGL`, so the Switch's video backend gives it an EGL surface on `nwindowGetDefault()`. deko3d's swapchain wants the same `NWindow`, which can have one producer: `nwindowSetDimensions` refuses once a producer has configured its buffers. The backend makes its swapchain once, at the window's size when the window is claimed, and never again, while the window changes size at a dock or undock (#238).
- **The tools.** The devkitA64 image of ADR 0138 has UAM 1.1.0 (`/opt/devkitpro/tools/bin/uam`, needing only libc and libstdc++). devkitPro's package server answers scripted downloads with 403.

## Options considered

### Where UAM comes from

1. **Copied out of the devkitA64 image ADR 0138 pins** (its `linux/amd64` image, by the same index digest), into the shaders container, checked by its SHA-256 and version line. **Chosen**: the same UAM the Switch build's image carries, no new download, one pin. Its cost is pulling that image's amd64 variant once (about 1.4 GB) to build the shaders container.
2. Running UAM in a second container (the devkitA64 image) between two runs of the shaders container. Rejected: two containers per build, and UAM would run as arm64 on a Mac and as amd64 elsewhere, a second source of difference in the blobs.
3. Building UAM 1.1.0 from source. Rejected: its build needs `distutils` (gone from Python 3.12) and an old Mesa that a current GCC may not take.
4. devkitPro's `uam` package by URL. Rejected: the server refuses scripted downloads.

### Presentation on the Switch

1. **Make SDL's GPU renderer start on deko3d.** Give it DKSH builds of SDL's own render shaders (compiled from the pinned archive's HLSL by our chain), turned on for `SDL_GPU_DEKO3D` by a configure-time patch. **Chosen**: then `GfxBackendHost`, GpuScreen, the Presenter and every filter work as on the desktop, unchanged. The renderer presents by blitting its backbuffer into deko3d's swapchain.
2. Present through the GPU device's swapchain directly, with our own pass. Rejected: it means a second Presenter and GpuScreen without `SDL_Renderer` (the menus, the import and missing-bundle screens, the overlays all draw with it), for nothing the first option lacks.
3. mt_gfx on deko3d, read back every frame and presented by switch-mesa's GLES 2 renderer. Rejected: two GPU stacks (Mesa and deko3d) in one process, a read-back per frame, and no filters (they need SDL's GPU renderer).

## Decision

**Shader chain.** `scripts/build-shaders` (`tools/mtport/shaderbuild.py`) adds a fourth format to each shader of `runtime/src/gfx/gpu/shaders/`: the SPIR-V it already makes goes through SPIRV-Cross to GLSL 4.60 without Vulkan semantics (`--version 460 --no-es --combined-samplers-inherit-bindings`), then through UAM (`--stage vert|frag`) to DKSH. `shaders_compiled.h` gains `k_<shader>_dksh` and `ShaderBlob::dksh`; the SPIR-V, DXIL and MSL blobs are unchanged byte for byte. The build checks every DKSH as the backend loads it: the magic, the sizes, one program of the shader's stage, at most the backend's 64 KiB of scratch per warp. The shaders container (`tools/containers/shaders/`) copies UAM 1.1.0 (ISC) from `devkitpro/devkita64@sha256:1fc388c3…`, checks its SHA-256 (`5c3cdb84…`) and lists it among the tools in the header.

**Binding layout.** ADR 0060's layout puts a stage's samplers and storage in one descriptor set and its uniform blocks in another (vertex 0 and 1, fragment 2 and 3), each numbered from 0 within its set. GLSL without Vulkan semantics drops the set and keeps the number, and in GLSL each resource kind has its own binding space. That is exactly what the deko3d backend binds: SDL's slot n of a kind is `binding = n` of that kind. `shaderbuild` checks it for every shader: each kind's GLSL bindings are the slots 0 to n − 1, n being the count SDL is given (`check_glsl_bindings`). Two samplers may share a slot: SPIRV-Cross turns one texture sampled with two samplers (SDL's `texture_advanced`, YUV) into two `sampler2D` on the texture's binding, which is how SDL binds a texture and its sampler together.

**SDL's renderer shaders.** `scripts/build-shaders` also compiles SDL's own render shaders (`src/render/gpu/shaders/*.hlsl` of the pinned sdl3-nx archive, zlib; read from the archive by name, never extracted with their own paths) into `cmake/patches/sdl3-nx-render-shaders.h`, as `<shader>_<stage>_private` arrays. The list and the counts come from the archive's `SDL_shaders_gpu.c`, and the bindings are checked against those counts. The header is stamped with the archive's SHA-256, and CTest `shaders.stamps` fails when `cmake/Dependencies.cmake` pins another: a new SDL pin needs the shaders rebuilt. The archive is the configure step's download (`MAPLETHIEF_DOWNLOAD_DIR`), fetched and checked by the script if missing.

**The patch.** `cmake/patches/sdl3-nx-gpu-renderer.cmake` runs after #238's in SDL3's `PATCH_COMMAND`. It copies that header into SDL's source as `src/render/gpu/SDL_render_private_shaders.h` and makes `SDL_shaders_gpu.c` use private shaders when `SDL_GPU_DEKO3D` is on. It also makes the deko3d swapchain follow the window: acquiring a swapchain texture when the window's pixel size differs from the swapchain's releases the window and claims it again at the new size, with the swap interval last set. SDL's renderer then remakes its backbuffer at that size. As with #238's patch, each text must be there exactly or configure fails, and a second run changes nothing. The pin by commit and hash is unchanged.

**The frontend on the Switch.** `mt_gfx_gpu_shader_formats()` adds `SDL_GPU_SHADERFORMAT_PRIVATE`, so `mt_gfx_gpu_create_device()` gets the deko3d device; `create_shader` takes the DKSH when the device offers no other format. `GfxBackendHost` is unchanged in what it builds: mt_gfx on the device, SDL's GPU renderer on it for the window, GpuScreen when both run. Two rules are the Switch's (`kGpuOnlyWithGpuRenderer`):

- mt_gfx draws on deko3d only together with SDL's GPU renderer. Without that renderer the backend is uninstalled and the device destroyed, and SDL's default renderer (GLES 2 on Mesa) presents the software path: we never run Mesa and deko3d side by side.
- The window is made for it at start. `main.cpp` leaves out `SDL_WINDOW_OPENGL` when `wants_switch_gpu_window` (`gfx_backend = gpu`, or SDL's GPU renderer for a filter or the colour step), since an EGL surface would already hold the screen. A window made with OpenGL keeps the software path and the log says that a restart is needed. `gfx_backend` is read once at start anyway; only the first-launch preset picker could change it, and presets don't touch it.

**Default.** Software stays the default on the Switch, as everywhere (no `kPlatformDefaults` row); `gfx_backend = gpu` is the player's choice until #244 measures whether the console needs it.

**Timing.** `MAPLETHIEF_GFX_GPU_TIMING=1` works as on the desktop, and `--gfx-gpu-timing` sets it from the command line, since nxlink passes arguments but no environment: fences are deko3d fences, and the log at exit gives the median frame's GPU time (an upper bound, ADR 0060).

## Consequences

- Evidence on the build machine: `scripts/build-shaders` twice gives byte-identical headers; the SPIR-V, DXIL and MSL arrays equal the previous header's; `scripts/build-shaders --check`, CTest `shaders.stamps`, pytest `tests/test_shaderbuild.py` (GLSL bindings, DKSH headers, SDL's table and archive, both patches on the real archive, applied twice) and doctest `platform.gfx dksh: …` (every library DKSH as the backend reads it, the formats, the window rule, the default) pass; the Switch NRO builds with the patched SDL. **Nothing ran on a console**: the comparison against the software rasterizer within ADR 0060's tolerance, the filters and the GPU time are the hardware steps on #242 (`needs-human`).
- The shaders container needs the devkitA64 image's amd64 variant when it is built. It is built only when shaders change.
- A second generated header, of SDL's shaders, is committed (about 850 lines, 13 KB of DKSH). It is compiled from SDL's zlib-licensed sources, as the library's blobs are from ours; nothing in it comes from the IPA.
- The DKSH blobs are in every build's library (about 10 KB); only the Switch uses them.
- A Switch player who sets `gfx_backend = gpu` or a filter restarts the game for it to take effect, as the desktop already does for `gfx_backend`.

**How we'd know this was wrong:**

- On hardware the GPU frames are off by more than ADR 0060's tolerance, or the present filters draw wrong (a binding, the Y direction, a sampler): then the mapping or the clip-space reading above is wrong, and the DKSH path gets a fix in `shaderbuild` (an `-fvk-invert-y` build, a binding remap), checked by the same tests.
- SDL's GPU renderer on deko3d fails or draws badly on hardware (its blit to the swapchain, its pixel-art shader): then the present path goes to option 2, our own pass on the device's swapchain.
- The swapchain doesn't follow a dock on hardware: then the window keeps its first size and the system scales it, which looks soft but works, until the patch is fixed.
- deko3d's GPU time doesn't beat the software path on the console (#244): then `gpu` stays an option nobody needs, and this ADR costs only build time.

## Addendum: the default on the Switch is gpu (#244, 2026-11-11)

[ADR 0145](0145-switch-performance-session-and-defaults.md) supersedes the "Default" paragraph above. The Switch's `kPlatformDefaults` row makes `gfx_backend = gpu` the default and every preset's value. On the Mac the software rasterizer costs about 13 ms of CPU a tick on a map, against about 2 ms on the GPU path, and a 1020 MHz Cortex-A57 is roughly 23 times slower by Geekbench 6. Software stays the fallback when no deko3d device starts. If #242's hardware checks find the GPU picture wrong, removing that row brings software back as the default.

## Addendum: the GPU's window really has no OpenGL (#281, 2026-11-11)

The rule "the window is made for it at start" never held. `main.cpp` left `SDL_WINDOW_OPENGL` out of the window's flags, but sdl3-nx's `SDL_DefaultGraphicsBackends` (`src/video/SDL_video.c` at the pinned commit) adds `SDL_WINDOW_OPENGL` to every Switch window created with no graphics flag, because a window without EGL could not be shown before deko3d. So `GfxBackendHost::open` always found an OpenGL window, and with `gfx_backend = gpu`, the default since ADR 0145, mt_gfx stayed on the software rasterizer on every start. The log line said a restart would help. It didn't. An emulator run of the NRO found it: the header said `gpu: gpu (mt_gfx on the CPU), renderer opengles2`.

SDL skips its default backends for a window created with `SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN`, and on the Switch that property does nothing else; the video backend then makes no EGL surface (`SDL_switchvideo.c`). `switch_window_properties` (`gfx_backend.h`) builds the Switch window's creation properties: for the GPU's window the external context and no `SDL_WINDOW_OPENGL`, for the software window `SDL_WINDOW_OPENGL`. `main.cpp` creates the Switch's window from them. No SDL patch is needed. If deko3d can't start, SDL's GLES 2 renderer recreates the window with OpenGL itself, as for any window without the flag, so the software fallback still presents. The fallback's log line now says the window was made with OpenGL and no longer mentions a restart.

- **Evidence:** doctest `gfx dksh: the Switch's GPU window has no OpenGL, even by SDL's default (#281)`. A run of the `--gen` NRO in a Ryujinx-based emulator, docked, with the defaults: `gpu: gpu (mt_gfx on the GPU), renderer gpu`, `gfx: mt_gfx draws with gpu (deko3d); the window's renderer is gpu`, `video driver Switch, renderer gpu, window 1920x1080`, and the main menu reached. The previous build's run of the same NRO on the same emulator said `mt_gfx on the CPU`.
- **Not shown:** that deko3d draws right on a console. An emulator implements the GPU its own way. The hardware rows stay on #242 and the checklist (`needs-human`).

