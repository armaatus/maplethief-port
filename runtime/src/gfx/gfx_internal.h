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
};

// Rasterizes one triangle into `target`; returns the pixels it wrote.
u64 raster_triangle(mt_gfx_target &target, const Shader &shader, const RasterVertex &a,
                    const RasterVertex &b, const RasterVertex &c) noexcept;

// One pixel's blend, exposed for tests of the rounding: `src` over `dst` with the factors
// (MT_GFX_* values, already validated), result into `out`.
void blend_pixel(const u8 src[4], const u8 dst[4], u32 src_factor, u32 dst_factor,
                 u8 out[4]) noexcept;

// True if `factor` is one of the eleven GL ES 1.1 blend factors.
bool valid_factor(u32 factor) noexcept;

} // namespace mt_gfx

#endif // MAPLETHIEF_RUNTIME_GFX_GFX_INTERNAL_H
