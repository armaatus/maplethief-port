# 202. The GPU tests run in CI on a software Vulkan device

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#364](https://github.com/armaatus/maplethief-port/issues/364) (epic [#293](https://github.com/armaatus/maplethief-port/issues/293))
- **Builds on:** [ADR 0060](0060-gpu-rendering-backend.md) (the GPU backend and its tolerance against the software rasterizer), [ADR 0068](0068-gpu-presentation-without-read-back.md) (GPU presentation), [ADR 0126](0126-dev-branch-release-pull-requests-and-ci-by-target.md) (CI by target)

## Context

`gfx_gpu_test` checks the GPU backend against the software rasterizer: the synthetic golden scenes within ADR 0060's tolerance, the sync points, batching, the present filters, the border, the colour-blind filter, the grade, widescreen's wide picture and GPU presentation. Its 34 cases carry the `gpu` label. Until now every case skipped on GitHub Actions and in the Linux container, because there was no GPU device: a change that broke the GPU renderer passed CI and was caught, if at all, on the Mac.

Mesa's lavapipe is a Vulkan 1.4 driver that rasterizes on the CPU (llvmpipe). SDL GPU's Vulkan backend accepts a CPU device unless the app asks for hardware acceleration, and `mt_gfx_gpu_create_device` does not ask. SDL needs a video subsystem for the device; CI's runner has no display, but SDL's offscreen video driver gives Vulkan a headless surface, and the tests never open a window (their renderers are offscreen).

Five cases measure GPU time against limits written for the Mac's GPU (2 ms for the busy frame, 1 ms for each present pass). On a CPU rasterizer those limits say nothing.

Measured on lavapipe (Mesa 26.0.8, LLVM 21, Ubuntu 26.04 arm64 container on an M4, Debug): 29 cases pass unchanged within ADR 0060's tolerance, the lines scene does not. 139 of its 6144 pixels (2.3%) differ: Vulkan's default line rasterization is not GL ES's diamond-exit rule, which the backend's line shift aims at, so a line takes some of its steps one pixel away from the reference's. 121 of those pixels are a line's pixel moved by one; the other 18 are where the star's 32 lines cross at its centre, so the moved pixels change which line ends on top.

## Decision

**CI's Linux debug build job runs the gpu tests on lavapipe.** It installs `mesa-vulkan-drivers` and `libvulkan1` and runs `scripts/test` with `MAPLETHIEF_GPU_TESTS=1` (a missing device fails the tests instead of skipping them) and `VK_DRIVER_FILES` set to lavapipe's ICD, so the loader never picks another driver. The Linux container (`tools/containers/linux/Dockerfile`) installs and sets the same, so `scripts/test-linux` and `scripts/local-ci` run them too. Other jobs (the release builds, ASan, the older compilers, macOS) keep skipping on GitHub Actions. The `gfx_gpu.*` timeout is 300 s: the slowest picture cases (the colour-blind filter and the wide picture under every pass) take 35 to 46 s on lavapipe.

**No display: the offscreen video driver.** When `SDL_VIDEO_DRIVER` is unset and SDL's video subsystem cannot start, `gfx_gpu_test` sets the hint to `offscreen` before it creates the device.

**A software device skips the time limits, never a picture.** The test reads SDL's device name (`SDL_PROP_GPU_DEVICE_NAME_STRING`): llvmpipe, SwiftShader or Direct3D's WARP adapter (the Microsoft Basic Render Driver) is a software device. There the four cases that only measure time skip (`REQUIRE_HARDWARE_GPU`), and the batching case keeps its counts and prints its time without the 2 ms check. Every comparison with a golden or between the backends runs.

**Lines on a software device: a pixel may move by one.** Only for the lines scene, and only when it fails ADR 0060's tolerance on a software device: a differing pixel passes when its colour is within the tolerance somewhere in the reference's 3 x 3 neighbourhood and the reference's colour is in the GPU image's. The pixels that are not such a move must stay within ADR 0060's 0.5%. A missing or extra line, a wrong colour or a wrong blend still fails. The rule is lavapipe's, not the backend's: hardware Vulkan drivers on Linux and Windows are not checked by it.

**Windows: a try on D3D12's WARP adapter.** GitHub's Windows runners have no GPU; the only DXGI adapter is WARP, Direct3D's CPU rasterizer. SDL 3.4.18's D3D12 backend takes the first adapter by GPU preference without excluding software ones, so the device opens on WARP if the runner's WARP takes our DXIL shaders. The Windows debug jobs (x64 and ARM64, on the runs that build every OS) run the `gfx_gpu.*` cases after `scripts/test.ps1` in a step of their own with `MAPLETHIEF_GPU_TESTS=1` and `continue-on-error`, so the outcome is in the log (the device's name, each case) and never fails the job. Lavapipe's line rule is Linux's; WARP draws lines by Direct3D's rules, which the backend's line shift was written for. When the try is green on both runners, a later change moves the variable into the `scripts/test.ps1` step and drops the try step. The outcome of the first run is recorded on #364.

## Consequences

- A pull request into `dev` that breaks the GPU backend's pictures, its sync points, its batching counts or GPU presentation fails the Linux debug job.
- The time limits stay a local check on the Mac (and `scripts/timing-stress`); CI cannot catch a GPU-time regression.
- The Linux debug job takes about two minutes more (the gpu cases share one `RESOURCE_LOCK` and run one at a time).
- A lavapipe update can move pixels; a new failure is read from the job's uploaded `build/test-output/` diff.

## Addendum: the first all-OS run (2026-11-27)

Accepted (delegated by Joris, 2026-11-27). CI run 45959090979 on the sweep branch (`be5f3f25`):

- **Linux x86_64** (`ubuntu-latest`, Ubuntu 24.04's Mesa): all 34 `gfx_gpu.*` cases pass on lavapipe. This is the gate.
- **Linux aarch64** (`ubuntu-24.04-arm`, ADR 0194): 32 pass; two golden scenes fail on its lavapipe, `gfx_blend_factors` (1.33 % of pixels, at most 4 per channel) and `gfx_perspective` (0.67 %, up to 200: the floor's far texels sample differently). The gate therefore runs on `ubuntu-latest` only; the arm job skips the gpu tests as the macOS and Windows jobs do. A per-driver golden is not worth keeping for a second software device.
- **Windows WARP try**, x64 and ARM64 debug: 23 of 34 pass on each, 11 fail (among them the grade, the widescreen passes and the shader filter's pass); the step never fails the job, so it stays a try. Making it a gate needs a look at those 11 cases on WARP, which is not this ADR's work.
