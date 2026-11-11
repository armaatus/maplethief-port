/* mt_gfx's GPU backend (#120, ADR 0060): GL ES 1.1's fixed-function pipeline on SDL3 GPU (Metal
 * on Apple, D3D12 or Vulkan on Windows, Vulkan on Linux, deko3d on the Nintendo Switch), behind
 * mt_gfx's draw interface (maplethief/gfx.h). Installing it changes nothing above that
 * interface. The GLES layer, EAGL, the compositor and the frontend's Screen keep calling
 * mt_gfx_submit and mt_gfx_target_cpixels as before.
 *
 *     SDL_GPUDevice *device = mt_gfx_gpu_create_device();       // or the renderer's device
 *     if (device != NULL && mt_gfx_gpu_install(device) == 0) {
 *         ... draws now run on the GPU; mt_gfx_backend_name() is "gpu (metal)" ...
 *     }
 *     mt_gfx_gpu_uninstall();          // back to the software rasterizer, targets read back
 *     SDL_DestroyGPUDevice(device);
 *
 * What it does with a draw (ADR 0060):
 *   - It expands strips, fans and loops into lists on the CPU and appends them to one vertex
 *     array per frame (clip-space position, texture coordinates, the constant colour). It
 *     starts a new batch only when the target, pipeline (primitive class and blend factors),
 *     texture, sampler, viewport or scissor changes.
 *   - The vertex shader maps GL's clip depth -w..w to the GPU's 0..w, so the GPU clips the
 *     same volume. The viewport is the draw's, times the target's scale; the scissor is the
 *     viewport cut to the target. The fragment shader is texel x colour (GL_MODULATE), with a
 *     1x1 white texture when untextured. Each texture's sampler follows its magnification
 *     filter and its minification filter's base filter, and its wrap modes. Blending is
 *     fixed-function with the GL ES 1.1 factors one to one, SRC_ALPHA_SATURATE included.
 *   - The queued draws go to the GPU (a "flush") at mt_gfx_end_frame, when the CPU reads a
 *     target's pixels (read back with a fence), and before a texture that a queued draw uses
 *     gets a new image or is destroyed.
 *
 * Shaders come from the HLSL files in runtime/src/gfx/gpu/shaders/, compiled by
 * scripts/build-shaders into the committed runtime/src/gfx/gpu/shaders_compiled.h (SPIR-V, DXIL,
 * MSL, and DKSH for deko3d). The frontend's present stage loads its own passes from the same
 * library with mt_gfx_gpu_create_shader.
 *
 * One thread: the guest's, which is also the thread that made the device. C17, also C++20. */
#ifndef MAPLETHIEF_GFX_GPU_H
#define MAPLETHIEF_GFX_GPU_H

#include <SDL3/SDL_gpu.h>
#include <stdint.h>

#include "maplethief/gfx.h"

MT_EXTERN_C_BEGIN

/* The shader formats the compiled shader library has: SPIR-V, DXIL, MSL, and DKSH as
 * SDL_GPU_SHADERFORMAT_PRIVATE (sdl3-nx's deko3d backend on the Nintendo Switch, #242). */
SDL_GPUShaderFormat mt_gfx_gpu_shader_formats(void) MT_NOEXCEPT;

/* A new SDL GPU device that can run the library's shaders, without a window. SDL picks the
 * driver (MAPLETHIEF_GPU_DRIVER, e.g. "vulkan", overrides it). NULL, logged, if there is no
 * usable GPU (CI, the Linux container). Initialises SDL's video subsystem if nobody has (SDL GPU
 * needs it). The caller destroys the device after mt_gfx_gpu_uninstall. */
SDL_GPUDevice *mt_gfx_gpu_create_device(void) MT_NOEXCEPT;

/* Makes the GPU backend on `device` mt_gfx's backend: from now on every mt_gfx_submit runs on
 * it. Targets and textures that already exist carry over (their pixels are uploaded when a draw
 * first needs them). Returns 0, or -1 (logged; mt_gfx stays on the software rasterizer) if the
 * shaders or pipelines cannot be made on the device. Installing again replaces the old backend. */
int mt_gfx_gpu_install(SDL_GPUDevice *device) MT_NOEXCEPT;

/* Back to the software rasterizer: the GPU's targets are read back into their pixels and every
 * GPU object is freed (the device is the caller's). No-op without the backend. */
void mt_gfx_gpu_uninstall(void) MT_NOEXCEPT;

/* 1 if the GPU backend is installed. */
int mt_gfx_gpu_installed(void) MT_NOEXCEPT;

/* Runs the queued draws now, without reading anything back. Returns 0, or -1 if the device
 * failed (logged). */
int mt_gfx_gpu_flush(void) MT_NOEXCEPT;

/* GPU timing: when on, every flush with draws waits for the GPU (a fence) and adds the time from
 * submit to the fence's signal to the stats. That is an upper bound of the GPU's work: queueing
 * and the completion notice are in it too. Off by default, since waiting stalls the CPU;
 * MAPLETHIEF_GFX_GPU_TIMING=1 in the environment turns it on at install. */
void mt_gfx_gpu_set_timing(int on) MT_NOEXCEPT;

typedef struct mt_gfx_gpu_stats {
    uint64_t flushes;         /* command buffers submitted with draws */
    uint64_t batches;         /* draw calls made (each a run of draws with the same state) */
    uint64_t vertices;        /* vertices uploaded */
    uint64_t pipelines;       /* pipelines alive (one per primitive class and blend state) */
    uint64_t texture_uploads; /* texture images uploaded (each upload, at the draw that needs it) */
    uint64_t target_uploads;  /* targets uploaded after the CPU wrote them */
    uint64_t readbacks;       /* targets read back for the CPU */
    uint64_t upload_bytes, readback_bytes;
    uint64_t timed_flushes;       /* flushes waited for with timing on */
    uint64_t gpu_ns;              /* their total GPU time, nanoseconds (upper bounds) */
    uint64_t max_gpu_ns;          /* the longest one */
    uint64_t last_frame_gpu_ns;   /* the GPU time of the last ended frame's flushes */
    uint64_t timed_frames;        /* ended frames with timed flushes (the first 2^20 kept) */
    uint64_t median_frame_gpu_ns; /* the median of those frames' GPU time */
} mt_gfx_gpu_stats;

/* The counters since install (zeros without the backend). */
mt_gfx_gpu_stats mt_gfx_gpu_get_stats(void) MT_NOEXCEPT;

/* The GPU texture holding a target's current pixels, for a host that presents on the GPU
 * (#130, ADR 0068: R8G8B8A8_UNORM, sampler and color-target usage, top row first, its pixel
 * size). It runs the queued draws first; a target the GPU has not drawn yet gets its GPU side
 * now, and pixels the CPU wrote since the GPU last drew go up first. Nothing is read back, so
 * the stats' readbacks do not move. Work the caller submits on the device after this call sees
 * those pixels (SDL GPU runs command buffers in submission order). NULL if the backend is not
 * installed or the device failed (logged). Valid until the target is destroyed or the backend
 * is uninstalled; later draws go into the same texture, so a host that shows the frame later
 * copies it first. */
SDL_GPUTexture *mt_gfx_gpu_target_texture(const mt_gfx_target *target) MT_NOEXCEPT;

/* A shader of the compiled library, by name ("gfx.vert", "gfx.frag", "present_copy.frag"), in
 * the device's format, with the resource counts the build recorded. NULL (logged) for an unknown
 * name or if the device takes none of the formats. The caller releases it
 * (SDL_ReleaseGPUShader). */
SDL_GPUShader *mt_gfx_gpu_create_shader(SDL_GPUDevice *device, const char *name) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_GFX_GPU_H */
