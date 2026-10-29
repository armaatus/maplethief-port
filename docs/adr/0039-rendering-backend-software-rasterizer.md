# 39. Rendering backend: a deterministic software rasterizer behind a draw interface, presented through SDL_Renderer

- **Status:** Proposed
- **Date:** 2026-10-29
- **Issue:** #55
- **Builds on:** [ADR 0003](0003-dependencies.md) (SDL3, static), [ADR 0005](0005-test-harness.md) (golden images), [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (the CPU compositor and its GL slot), [ADR 0038](0038-gl-recording-eagl-silent-audio-boot-to-first-frame.md) (decode, state tracker, command and present hooks)

## Context

The game draws with **OpenGL ES 1.1 fixed-function only** ([ANALYSIS.md → Rendering](../ANALYSIS.md#rendering)): 24 core entry points and 7 `GL_OES_framebuffer_object` ones. It uses `glDrawArrays` with client arrays (vertex and texture coordinates; no colour array), one constant colour (`glColor4x`), 2D textures from `glTexImage2D`, `glBlendFunc`, the three matrix stacks and `glViewport`. There is no `glClear`, no depth or stencil buffer, no lighting, no fog, no `glDrawElements`, no `glReadPixels`. The drawable is one 480 × 320 RGBA8 renderbuffer ([ADR 0038](0038-gl-recording-eagl-silent-audio-boot-to-first-frame.md)). Frame 1 of the real boot has 33 draws; a busy map frame is expected to have a few hundred textured quads.

UIKit's views are already drawn **on the CPU** by the compositor ([ADR 0037](0037-uikit-host-classes-nib-and-compositor.md)), which leaves a GL slot that it fills with a given GL frame (full mode) or leaves transparent (overlay mode).

The targets are macOS 13+ (Metal; OpenGL deprecated), Windows 10/11 (D3D11/D3D12/Vulkan/GL), iOS 16+ (Metal; OpenGL ES deprecated) and Linux/Steam Deck (Vulkan/GL). CI and `scripts/local-ci` run headless: the Linux job is an x86_64 container under Rosetta with no GPU, and macOS CI runners have no usable GPU for tests either. M5's exit criterion is pixel comparison against device screenshots within tolerance, and #56 asks for golden tests that pass **in CI**.

## Options considered

1. **SDL3 GPU** (one API onto Metal, D3D12 and Vulkan).
   - Pro: one modern API on every target, SDL is already a dependency.
   - Con: shaders must ship as MSL, SPIR-V and DXIL. That needs a shader toolchain (SDL_shadercross or glslang + spirv-cross + dxc) in the build, or committed bytecode with a script to regenerate it.
   - Con: no GPU in CI or the Linux container, so the golden tests would either not run there or run on a software Vulkan (lavapipe) that differs from Metal and D3D in rounding and filtering. Goldens per driver, or wide tolerances.
   - Con: fixed-function rules (exact `GL_FIXED` conversions, the device's filtering precision) become shader code tuned per backend.
2. **OpenGL 3.3 core / GLES 3, plus a Metal path** for Apple.
   - Pro: GLES1 maps almost one-to-one onto a small GL 3.3 shader.
   - Con: two backends to write and keep in step, since GL is deprecated on macOS and iOS. Same CI problem as option 1 (Mesa llvmpipe is possible on Linux only).
3. **A software rasterizer on the CPU** that executes the fixed-function pipeline into a 480 × 320 RGBA8 target, with the finished frame presented through **SDL_Renderer** (SDL picks Metal, D3D11/D3D12, Vulkan or GL per platform) as one streaming texture, scaled there.
   - Pro: **deterministic**. The same frame gives the same bytes on every OS and CPU, in CI and in the Linux container, so the golden tests are exact (or nearly so) everywhere and a device screenshot is compared against one picture.
   - Pro: one implementation for every platform, no shader toolchain, no new dependency. It matches the compositor, which is already CPU-side and wants the GL frame as pixels.
   - Pro: the cost is small. 480 × 320 is 153,600 pixels; even with 5× overdraw that is under a million blended texels per frame, a few milliseconds of one core at 60 Hz.
   - Con: higher internal resolutions (M10's HD rendering, #88) cost CPU in proportion to pixels. At 2× (960 × 640) it is still cheap; beyond that a GPU backend may be needed.
4. **A GPU backend now plus the software rasterizer as a test reference.** Two implementations before either is needed; the GPU one would not be testable in CI.

## Decision

- **Option 3.** GLES 1.1 runs on our own software rasterizer, `mt_gfx`, in `runtime/src/gfx/` behind `runtime/include/maplethief/gfx.h`. The platform layer presents the finished frame with SDL_Renderer.
- **The seam is a draw interface, not GL.** `mt_gfx` knows nothing about GL's state machine. #56's GLES layer (`runtime/src/gles/`) resolves the GL state at each `glDrawArrays` into one self-contained **draw**: the primitive, the vertices already transformed to clip space (position x, y, z, w and texture coordinates s, t, after the texture matrix), the constant colour, the bound texture's `mt_gfx` handle or none, the blend enable and factors, and the viewport. A later GPU backend implements the same interface; nothing above it changes. The draws are the command list: `mt_gfx` may execute each at once (the software backend does) or batch them until the target is read.
- **Objects.**
  - A **target** is an RGBA8 image owned by `mt_gfx`, stored top row first (an image, as `tests/golden/golden.h` and the compositor use). GL window coordinates have y up, so `mt_gfx` maps window row y to image row `height − 1 − y`; nothing above it flips anything. A target is never cleared by `mt_gfx`: its contents survive presents (the game has no `glClear` and overdraws everything).
  - A **texture** is an `mt_gfx` handle with level-0 RGBA8 texels converted from the uploaded format, its size, its min/mag filter (`GL_NEAREST`, `GL_LINEAR`; mipmapped min filters fall back to their base level filter) and its wrap modes (`GL_REPEAT`, `GL_CLAMP_TO_EDGE`). #57 owns the format conversions and premultiplied alpha; #55 provides RGBA/UNSIGNED_BYTE.
- **Rasterization rules**, to be as close to the device as the tests can show:
  - Clip-space triangles are clipped against the view volume, divided by w and mapped by the viewport. Window coordinates are snapped to **1/16 pixel** (28.4 fixed point), and coverage is decided with integer edge functions at pixel centres (x + ½, y + ½) with the **top-left fill rule**, so shared edges are drawn exactly once.
  - Primitives: `GL_TRIANGLES`, `GL_TRIANGLE_STRIP`, `GL_TRIANGLE_FAN`. Points and lines are logged as unsupported until the game is seen to draw them.
  - Texture coordinates are interpolated perspective-correctly (they are affine whenever w = 1, which every ortho draw has).
  - Fragment: texel (nearest, or bilinear with 8-bit weights) **modulated** by the constant colour (GL ES's default `GL_MODULATE`; the game never calls `glTexEnv`), per channel `(a × b + 127) / 255`. Without a texture the fragment is the colour.
  - Blending: the GL ES 1.1 factors (`ZERO`, `ONE`, `SRC_COLOR`, `ONE_MINUS_SRC_COLOR`, `DST_COLOR`, `ONE_MINUS_DST_COLOR`, `SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA`, `DST_ALPHA`, `ONE_MINUS_DST_ALPHA`, `SRC_ALPHA_SATURATE`) in 8-bit integer arithmetic with the same rounding, saturated.
- **Determinism is a requirement**, checked by tests. The rasterizer's sources compile with floating-point contraction off (`-ffp-contract=off`; `/fp:precise` on MSVC, `/clang:-ffp-contract=off` on clang-cl), as the generated code already does, and do no float work after snapping except texture-coordinate interpolation in a fixed order. A golden image of the same synthetic scene matches **exactly** on macOS arm64, macOS x86_64 and Linux x86_64.
- **Presentation.** The present hook (#58) takes the renderbuffer's target, hands it to `mt_uikit_composite` in full mode as the GL slot's frame, and the platform uploads the composited 480 × 320 image into one SDL streaming texture (`SDL_PIXELFORMAT_ABGR8888`, which is R, G, B, A in memory) and draws it with `SDL_RenderTexture` into the rectangle #59's scaling chooses, with SDL's nearest, linear or pixel-art scale mode. SDL_Renderer's own backend choice (Metal on Apple, D3D on Windows, Vulkan or GL on Linux) is the only GPU code we have.
- **Prototype (#55):** `mt_gfx` with RGBA textures, filtering, wrapping, colour modulation and blending; `tests/cpp/gfx_test.cpp` with golden images of a textured, alpha-blended quad over a background; and `MapleThief --gfx-prototype` showing that scene in an SDL window on macOS (the smoke test reads it back headless).

## Consequences

- **Testable everywhere.** #56's synthetic goldens (ortho quad, rotated sprite, blend modes), #57's fringe test and #60's game-screen goldens run in CI and in `scripts/local-ci` on every OS with exact or near-exact tolerances. A difference between platforms is a bug, not driver noise.
- **#56** builds the fixed-function layer (matrix stacks with 16.16 conversion, client arrays captured from guest memory at draw time, enables, colour, blend state, texture binding) on top of the state tracker of ADR 0038 and emits `mt_gfx` draws from the command hook. **#57** adds the texture formats the game uploads. **#58** connects the EAGL renderbuffer to an `mt_gfx` target and presents it in the window. **#59** chooses the destination rectangle and scale mode. **#60** compares targets with device screenshots.
- **Speed is watched, not assumed.** `mt_gfx` counts pixels shaded per frame; the frame time is logged with the `gfx` trace channel. If a real map frame takes more than 4 ms on the Mac, the inner loops get specialised (nearest/no-blend/opaque spans) before anything else changes.
- **How we'd know this was wrong:**
  - **The CPU cost is too high** on a target we care about (the Steam Deck or an older iPhone) at 480 × 320, or for the HD modes of M10. Then a GPU backend (SDL3 GPU is the likely choice) goes behind the same draw interface, and the software rasterizer stays as the reference that the GPU output is compared with.
  - **Device screenshots show filtering or rounding we cannot match** with integer rules (#60). Then the rules change here, once, for every platform.
