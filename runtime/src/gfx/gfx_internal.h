// mt_gfx (#55, ADR 0039, maplethief/gfx.h): what gfx.cpp (targets, draws, clipping, frames),
// texture.cpp (texture objects and texel conversions) and raster.cpp (coverage, texturing,
// blending) share.
#ifndef MAPLETHIEF_RUNTIME_GFX_GFX_INTERNAL_H
#define MAPLETHIEF_RUNTIME_GFX_GFX_INTERNAL_H

#include <cstdint>
#include <vector>

#include "maplethief/gfx.h"

struct mt_gfx_target {
    std::uint32_t width = 0, height = 0;                 // in pixels: logical size x scale
    std::uint32_t logical_width = 0, logical_height = 0; // what GL sees
    std::uint32_t scale = 1;
    std::vector<std::uint8_t> pixels; // RGBA8, top row first, width * 4 bytes per row
    // The GPU backend's side of the target (#120, ADR 0060): its GPU texture and which copy is
    // current. nullptr while the software rasterizer draws into `pixels`, and until a backend
    // first draws into the target.
    void *backend = nullptr;
};

namespace mt_gfx {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;

struct Texture {
    u32 logical_width = 0, logical_height = 0; // of the upload (GL's size)
    u32 width = 0, height = 0;                 // of the texels: the upload's or a replacement's
    bool replaced = false;
    std::vector<u8> texels; // RGBA8, row 0 is t = 0, width * 4 bytes per row
    u32 min_filter = MT_GFX_NEAREST_MIPMAP_LINEAR;
    u32 mag_filter = MT_GFX_LINEAR;
    u32 wrap_s = MT_GFX_REPEAT;
    u32 wrap_t = MT_GFX_REPEAT;
    // Bumped at every new image (and emptying), so a backend's copy of the texels can tell that
    // it is stale.
    u64 generation = 0;
};

// The texture of `handle`, or nullptr (0, unknown).
const Texture *find_texture(u32 handle) noexcept;

// The texture part of mt_gfx_get_stats: textures alive, their bytes, the replaced ones and the
// replacement counters.
void texture_stats(mt_gfx_stats *stats) noexcept;

// Frees every texture (mt_gfx_reset).
void texture_reset() noexcept;

// The filter of a minification filter's base level: GL_NEAREST or GL_LINEAR.
u32 base_filter(u32 filter) noexcept;

// A vertex after clipping, the divide by w and the viewport: image coordinates (y down) in
// 1/16 pixel, the texture coordinates, and q = 1 / w for perspective-correct interpolation.
struct RasterVertex {
    i64 x = 0, y = 0;
    float w = 1.0f;
    double s = 0.0, t = 0.0, q = 1.0;
};

// What a fragment of the draw is made of.
struct Shader {
    const Texture *texture = nullptr; // nullptr: untextured (or an empty texture)
    u8 color[4] = {255, 255, 255, 255};
    bool blend = false;
    u32 src_factor = MT_GFX_ONE, dst_factor = MT_GFX_ZERO;
    // The pixels the draw may write (image coordinates, inclusive): its viewport. The target's
    // bounds apply as well. Triangles are clipped to a guard band, not to the viewport, so this
    // is what keeps them inside it.
    i64 x0 = 0, y0 = 0, x1 = INT64_MAX, y1 = INT64_MAX;
};

// Rasterizes one triangle into `target`; returns the pixels it wrote.
u64 raster_triangle(mt_gfx_target &target, const Shader &shader, const RasterVertex &a,
                    const RasterVertex &b, const RasterVertex &c) noexcept;

// Rasterizes the line segment a -> b (width 1, GL ES 1.1's diamond-exit rule, half-open: b's
// diamond is not drawn) into `target`; returns the pixels it wrote.
u64 raster_line(mt_gfx_target &target, const Shader &shader, const RasterVertex &a,
                const RasterVertex &b) noexcept;

// One pixel's blend, exposed for tests of the rounding: `src` over `dst` with the factors
// (MT_GFX_* values, already validated), result into `out`.
void blend_pixel(const u8 src[4], const u8 dst[4], u32 src_factor, u32 dst_factor,
                 u8 out[4]) noexcept;

// True if `factor` is one of the eleven GL ES 1.1 blend factors.
bool valid_factor(u32 factor) noexcept;

// --- backends (#120, ADR 0060) ------------------------------------------------------------

// A draw that gfx.cpp has validated, as a backend executes it.
struct Draw {
    const mt_gfx_draw *src = nullptr; // primitive (MT_GFX_LINES..MT_GFX_TRIANGLE_FAN), vertices
    u32 texture_handle = 0;           // 0 with `texture` nullptr
    const Texture *texture = nullptr; // nullptr: untextured (or an empty texture)
    u8 color[4] = {255, 255, 255, 255};
    bool blend = false; // false also for blending with GL_ONE / GL_ZERO
    u32 src_factor = MT_GFX_ONE, dst_factor = MT_GFX_ZERO;
    // The viewport in the target's pixels (the logical viewport, clamped, times the scale), in
    // window coordinates (y up from the bottom row). Whole numbers.
    float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
};

// How the CPU is about to use a target's pixels.
enum class Access {
    kRead,      // read them (mt_gfx_target_cpixels)
    kWrite,     // read and maybe write them (mt_gfx_target_pixels)
    kOverwrite, // write every pixel (mt_gfx_target_fill)
};

// Something other than the software rasterizer that executes the draws: the GPU backend
// (runtime/src/gfx/gpu/). Called on the guest's thread only, like the rest of mt_gfx. With no
// backend installed, mt_gfx rasterizes on the CPU as it always has.
class Backend {
  public:
    virtual ~Backend() = default;
    // "gpu (metal)", for logs and tests.
    virtual const char *name() const noexcept = 0;
    // Executes (or queues) the draw into `target`. Counts its triangles and lines in `count`,
    // the same way the software rasterizer does. A backend may set target.backend.
    virtual void submit(mt_gfx_target &target, const Draw &draw,
                        mt_gfx_counters &count) noexcept = 0;
    // Called before the CPU uses the pixels of a target with target.backend set: they must then
    // be current (queued draws executed and read back, unless the access overwrites them).
    virtual void access(mt_gfx_target &target, Access access) noexcept = 0;
    // The target is going away (destroyed, or the backend is being replaced, after an
    // Access::kRead): frees target.backend and sets it to nullptr.
    virtual void forget(mt_gfx_target &target) noexcept = 0;
    // A texture is about to get a new image or be destroyed (`destroyed`). Queued draws that use
    // it must run first; a destroyed texture's copy is freed.
    virtual void texture_changing(u32 handle, bool destroyed) noexcept = 0;
    // mt_gfx_end_frame: the frame's draws can go to the device.
    virtual void end_frame() noexcept = 0;
};

// The installed backend, or nullptr (the software rasterizer).
Backend *backend() noexcept;

// Installs `next` (nullptr: back to the software rasterizer). The current backend first reads
// every target it drew back into its pixels and forgets it, so targets and textures carry over.
// mt_gfx does not own the backend: the caller frees the old one after this returns.
void set_backend(Backend *next) noexcept;

// Tells the installed backend (if any) that a texture is about to change (texture.cpp).
void notify_texture_changing(u32 handle, bool destroyed) noexcept;

} // namespace mt_gfx

#endif // MAPLETHIEF_RUNTIME_GFX_GFX_INTERNAL_H
