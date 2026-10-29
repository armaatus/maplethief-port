/* mt_gfx: the rendering backend (#55, ADR 0039). A deterministic software rasterizer that runs
 * GL ES 1.1's fixed-function pipeline, as the game uses it, into RGBA8 targets. It knows nothing
 * about GL's state machine: the GLES layer (#56, runtime/src/gles/) resolves the GL state at each
 * glDrawArrays into one self-contained draw and submits it here.
 *
 *     mt_gfx_target *target = mt_gfx_target_create(480, 320, 1);
 *     uint32_t tex = mt_gfx_texture_create();
 *     mt_gfx_texture_image(tex, 64, 64, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texels);
 *     mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MIN_FILTER, MT_GFX_LINEAR);
 *
 *     mt_gfx_vertex quad[4] = {{-1, -1, 0, 1, 0, 0}, {1, -1, 0, 1, 1, 0},
 *                              {-1, 1, 0, 1, 0, 1}, {1, 1, 0, 1, 1, 1}};
 *     mt_gfx_draw draw = mt_gfx_draw_default();
 *     draw.primitive = MT_GFX_TRIANGLE_STRIP;
 *     draw.vertices = quad;
 *     draw.count = 4;
 *     draw.texture = tex;
 *     draw.blend = 1;
 *     draw.src_factor = MT_GFX_SRC_ALPHA;
 *     draw.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
 *     draw.viewport[2] = 480;
 *     draw.viewport[3] = 320;
 *     mt_gfx_submit(target, &draw);
 *     mt_gfx_end_frame();
 *     ... mt_gfx_target_pixels(target): 480 x 320 RGBA8 (x scale), top row first ...
 *
 * Enum values are GL's (GL_TRIANGLES, GL_SRC_ALPHA, GL_LINEAR, ...), so the GLES layer passes the
 * game's arguments through unchanged; mt_gfx rejects values it does not know.
 *
 * The rules, the same on every platform (ADR 0039):
 *   - Targets: RGBA8, top row first (like an image), never cleared by mt_gfx. GL window
 *     coordinates have y up: window pixel (x, y) is image pixel (x, height - 1 - y).
 *   - Internal resolution: a target has a logical size (what GL and EAGL see, 480 x 320) and an
 *     integer scale N >= 1; its pixels are N times the logical size. Draws are in logical
 *     units (the viewport is logical); mt_gfx multiplies the viewport by N, so snapping, the fill
 *     rule and sampling happen at the target's pixel resolution. N = 1 is GL ES exactly.
 *   - Vertices are in clip space. Triangles are clipped against the view volume
 *     (-w <= x, y, z <= w, and w > 0), divided by w and mapped by the (scaled) viewport. Window
 *     coordinates are snapped to 1/16 pixel (28.4 fixed point); a pixel is covered when its
 *     centre is inside the triangle by integer edge functions, with the top-left fill rule on the
 *     image (y down), so pixels on an edge shared by two triangles are drawn exactly once.
 *     No face culling, no depth or stencil buffer, no scissor; pixels outside the target are
 *     dropped.
 *   - Texture coordinates are interpolated perspective-correctly (affinely when a triangle's
 *     three w are equal, as in every ortho draw). Texel lookup: GL_NEAREST, or GL_LINEAR with
 *     8-bit weights; the magnification filter applies when a triangle maps at most one texel to
 *     a pixel, the minification filter otherwise (per triangle; mipmapped minification filters
 *     use their base level's filter). Wrap: GL_REPEAT or GL_CLAMP_TO_EDGE.
 *   - Texture replacement: an optional host hook sees every level-0 upload (its logical size,
 *     format, type and a content hash) and may supply an RGBA8 image k times its size. The
 *     texture keeps its logical size; texture coordinates are normalised, so the replacement maps
 *     onto the same coordinates and is filtered at its own resolution.
 *   - Fragment: the texel modulated by the constant colour (GL_MODULATE), per channel
 *     (a * b + 127) / 255; without a texture, or with an empty one, the colour itself.
 *   - Blending: result = min(255, (src * f_src + dst * f_dst + 127) / 255) per channel, with
 *     every GL ES 1.1 factor as an 8-bit value; without blending the fragment replaces the pixel.
 *   - Floating point (clipping, the viewport, interpolation) is IEEE float and double in a fixed
 *     order, compiled without contraction (no FMA), so a frame has the same bytes on every OS and
 *     CPU: golden images match exactly.
 *
 * Trace: MAPLETHIEF_TRACE=gfx logs each frame at mt_gfx_end_frame, e.g.
 * "gfx: frame 12: 212 draws, 424 triangles, 301344 pixels, 0 unsupported, 1.250 ms". Points and
 * lines are not drawn: they are counted and logged once per primitive type (always).
 *
 * The textures and counters are per process, one thread at a time (the guest's main thread).
 * C17 that also compiles as C++20; the implementation is runtime/src/gfx/. */
#ifndef MAPLETHIEF_GFX_H
#define MAPLETHIEF_GFX_H

#include <stdint.h>

#include "maplethief/recomp/compiler.h"

MT_EXTERN_C_BEGIN

/* --- enums (GL ES 1.1 values) --------------------------------------------------------------- */

/* Primitives. Only the triangle ones are drawn. */
#define MT_GFX_POINTS 0x0000u
#define MT_GFX_LINES 0x0001u
#define MT_GFX_LINE_LOOP 0x0002u
#define MT_GFX_LINE_STRIP 0x0003u
#define MT_GFX_TRIANGLES 0x0004u
#define MT_GFX_TRIANGLE_STRIP 0x0005u
#define MT_GFX_TRIANGLE_FAN 0x0006u

/* Blend factors. GL ES 1.1 allows SRC_COLOR and ONE_MINUS_SRC_COLOR only as the destination
 * factor, DST_COLOR, ONE_MINUS_DST_COLOR and SRC_ALPHA_SATURATE only as the source factor;
 * mt_gfx takes every factor on either side (the GLES layer rejects what GL rejects). */
#define MT_GFX_ZERO 0x0000u
#define MT_GFX_ONE 0x0001u
#define MT_GFX_SRC_COLOR 0x0300u
#define MT_GFX_ONE_MINUS_SRC_COLOR 0x0301u
#define MT_GFX_SRC_ALPHA 0x0302u
#define MT_GFX_ONE_MINUS_SRC_ALPHA 0x0303u
#define MT_GFX_DST_ALPHA 0x0304u
#define MT_GFX_ONE_MINUS_DST_ALPHA 0x0305u
#define MT_GFX_DST_COLOR 0x0306u
#define MT_GFX_ONE_MINUS_DST_COLOR 0x0307u
#define MT_GFX_SRC_ALPHA_SATURATE 0x0308u

/* Texel formats and types of mt_gfx_texture_image. */
#define MT_GFX_UNSIGNED_BYTE 0x1401u
#define MT_GFX_ALPHA 0x1906u
#define MT_GFX_RGB 0x1907u
#define MT_GFX_RGBA 0x1908u
#define MT_GFX_LUMINANCE 0x1909u
#define MT_GFX_LUMINANCE_ALPHA 0x190Au
#define MT_GFX_UNSIGNED_SHORT_4_4_4_4 0x8033u
#define MT_GFX_UNSIGNED_SHORT_5_5_5_1 0x8034u
#define MT_GFX_UNSIGNED_SHORT_5_6_5 0x8363u

/* Texture parameters and their values. */
#define MT_GFX_TEXTURE_MAG_FILTER 0x2800u
#define MT_GFX_TEXTURE_MIN_FILTER 0x2801u
#define MT_GFX_TEXTURE_WRAP_S 0x2802u
#define MT_GFX_TEXTURE_WRAP_T 0x2803u
#define MT_GFX_NEAREST 0x2600u
#define MT_GFX_LINEAR 0x2601u
#define MT_GFX_NEAREST_MIPMAP_NEAREST 0x2700u
#define MT_GFX_LINEAR_MIPMAP_NEAREST 0x2701u
#define MT_GFX_NEAREST_MIPMAP_LINEAR 0x2702u
#define MT_GFX_LINEAR_MIPMAP_LINEAR 0x2703u
#define MT_GFX_REPEAT 0x2901u
#define MT_GFX_CLAMP_TO_EDGE 0x812Fu

/* Limits: a target (in pixels, after its scale) or a texture (its logical size, and a
 * replacement's size) is at most MT_GFX_MAX_SIZE pixels wide and high; a target's scale and a
 * replacement's factor are at most MT_GFX_MAX_SCALE. Viewport widths and heights are clamped to
 * MT_GFX_MAX_VIEWPORT, its x and y to +-MT_GFX_MAX_VIEWPORT (logical units). */
#define MT_GFX_MAX_SIZE 4096u
#define MT_GFX_MAX_SCALE 8u
#define MT_GFX_MAX_VIEWPORT 8192

/* --- targets -------------------------------------------------------------------------------- */

/* An RGBA8 image that draws render into (an EAGL renderbuffer, #58). */
typedef struct mt_gfx_target mt_gfx_target;

/* A target of logical size width x height at internal scale `scale` (1 = GL ES's resolution):
 * width * scale x height * scale pixels, transparent black. NULL if a size or the scale is 0,
 * the scale is above MT_GFX_MAX_SCALE, a pixel size is above MT_GFX_MAX_SIZE, or out of memory. */
mt_gfx_target *mt_gfx_target_create(uint32_t width, uint32_t height, uint32_t scale) MT_NOEXCEPT;

void mt_gfx_target_destroy(mt_gfx_target *target) MT_NOEXCEPT;

/* The logical size (what GL sees: the renderbuffer's size, the viewport's units). */
uint32_t mt_gfx_target_width(const mt_gfx_target *target) MT_NOEXCEPT;
uint32_t mt_gfx_target_height(const mt_gfx_target *target) MT_NOEXCEPT;
uint32_t mt_gfx_target_scale(const mt_gfx_target *target) MT_NOEXCEPT;
/* The size in pixels: the logical size times the scale. */
uint32_t mt_gfx_target_pixel_width(const mt_gfx_target *target) MT_NOEXCEPT;
uint32_t mt_gfx_target_pixel_height(const mt_gfx_target *target) MT_NOEXCEPT;

/* The pixels: RGBA8, top row first, pixel width * 4 bytes per row without padding. The caller
 * may read and write them between submits (presentation, tests, a background). */
uint8_t *mt_gfx_target_pixels(mt_gfx_target *target) MT_NOEXCEPT;
const uint8_t *mt_gfx_target_cpixels(const mt_gfx_target *target) MT_NOEXCEPT;

/* Sets every pixel to `rgba` (mt_gfx itself never clears a target; GL ES has no glClear in the
 * game, but a host may want a known first frame). */
void mt_gfx_target_fill(mt_gfx_target *target, const uint8_t rgba[4]) MT_NOEXCEPT;

/* --- textures ------------------------------------------------------------------------------- */

/* A new texture handle (never 0): no image yet, GL's default parameters (minification
 * GL_NEAREST_MIPMAP_LINEAR, magnification GL_LINEAR, wrap GL_REPEAT both ways). */
uint32_t mt_gfx_texture_create(void) MT_NOEXCEPT;

/* Frees the texture; 0 and unknown handles are ignored. */
void mt_gfx_texture_destroy(uint32_t texture) MT_NOEXCEPT;

/* Replaces the level-0 image (glTexImage2D): `pixels` holds width x height texels of
 * `format`/`type` in GL's order (the first row is texture coordinate t = 0, the first texel of a
 * row s = 0), each row padded to 4 bytes (GL's default GL_UNPACK_ALIGNMENT; the game never changes
 * it). NULL pixels: transparent black. mt_gfx stores RGBA8. A 0 x 0 image empties the texture.
 * Returns 0, or -1 (nothing changes) for an unknown handle, a size above MT_GFX_MAX_SIZE or a
 * format/type pair it cannot convert. #55 converts MT_GFX_RGBA/MT_GFX_UNSIGNED_BYTE; #57 adds
 * the others (one conversion function per pair in runtime/src/gfx/texture.cpp). */
int mt_gfx_texture_image(uint32_t texture, uint32_t width, uint32_t height, uint32_t format,
                         uint32_t type, const void *pixels) MT_NOEXCEPT;

/* glTexParameteri for MT_GFX_TEXTURE_{MIN,MAG}_FILTER and MT_GFX_TEXTURE_WRAP_{S,T}. Returns 0,
 * or -1 (nothing changes) for an unknown handle, parameter or value. */
int mt_gfx_texture_parameter(uint32_t texture, uint32_t pname, uint32_t value) MT_NOEXCEPT;

/* The texture's logical size: that of the last image uploaded (0 x 0 without one). Returns 0, or
 * -1 for an unknown handle. Either pointer may be NULL. */
int mt_gfx_texture_size(uint32_t texture, uint32_t *width, uint32_t *height) MT_NOEXCEPT;

/* The texels mt_gfx samples, RGBA8 (row 0 is t = 0), and their size: the uploaded image, or its
 * replacement (k times the logical size). NULL (sizes 0) if the handle is unknown or the texture
 * has no image. Valid until the next image upload or destroy. */
const uint8_t *mt_gfx_texture_texels(uint32_t texture, uint32_t *width,
                                     uint32_t *height) MT_NOEXCEPT;

/* --- texture replacement (the seam for HD textures and mods, M10) --------------------------- */

/* The content hash of an upload: 64-bit FNV-1a (offset basis 0xCBF29CE484222325, prime
 * 0x00000100000001B3) over the texel bytes GL reads, row by row, without the rows' alignment
 * padding (width * bytes per texel of each of the height rows). Stable across platforms and
 * versions: a replacement pack names textures by it. 0 for a format/type pair mt_gfx does not
 * convert or NULL pixels. */
uint64_t mt_gfx_upload_hash(uint32_t width, uint32_t height, uint32_t format, uint32_t type,
                            const void *pixels) MT_NOEXCEPT;

/* What the hook sees of an upload (with pixels; uploads without pixels are not offered). */
typedef struct mt_gfx_upload {
    uint32_t texture;       /* the handle */
    uint32_t width, height; /* the logical size */
    uint32_t format, type;
    uint64_t hash;      /* mt_gfx_upload_hash of the upload */
    const void *pixels; /* as uploaded, valid during the call */
} mt_gfx_upload;

/* What the hook may supply instead: RGBA8, row 0 is t = 0, width * 4 bytes per row, with
 * width = k * upload width and height = k * upload height for one integer k from 1 to
 * MT_GFX_MAX_SCALE (and at most MT_GFX_MAX_SIZE). mt_gfx copies the pixels right after the
 * hook returns, so they need to stay valid only until then. */
typedef struct mt_gfx_replacement {
    const uint8_t *pixels;
    uint32_t width, height;
} mt_gfx_replacement;

/* Returns 1 after filling *out to replace the upload, 0 to keep it. A replacement of the wrong
 * size is rejected (logged, counted) and the upload kept. Runs inside mt_gfx_texture_image: it
 * must not call mt_gfx itself. */
typedef int (*mt_gfx_replace_fn)(void *user, const mt_gfx_upload *upload, mt_gfx_replacement *out);

/* Installs the hook (NULL: none, the default). Kept across mt_gfx_reset. */
void mt_gfx_set_replace_hook(mt_gfx_replace_fn hook, void *user) MT_NOEXCEPT;

/* --- draws ---------------------------------------------------------------------------------- */

/* A vertex in clip space (after the modelview and projection matrices) with its texture
 * coordinates (after the texture matrix, divided by its q). */
typedef struct mt_gfx_vertex {
    float x, y, z, w;
    float s, t;
} mt_gfx_vertex;

/* One glDrawArrays, self-contained: nothing of it is kept after mt_gfx_submit returns. */
typedef struct mt_gfx_draw {
    uint32_t primitive; /* MT_GFX_TRIANGLES, MT_GFX_TRIANGLE_STRIP, MT_GFX_TRIANGLE_FAN */
    const mt_gfx_vertex *vertices;
    uint32_t count;
    uint8_t color[4];    /* the constant colour, RGBA8 */
    uint32_t texture;    /* 0: untextured */
    uint32_t blend;      /* 1: blending on (GL_BLEND) */
    uint32_t src_factor; /* MT_GFX_ZERO ... MT_GFX_SRC_ALPHA_SATURATE */
    uint32_t dst_factor;
    int32_t viewport[4]; /* x, y, width, height in window coordinates (y up) */
} mt_gfx_draw;

/* GL ES's initial state: triangles, no vertices, opaque white, untextured, blending off with
 * GL_ONE / GL_ZERO, a 0 x 0 viewport. */
mt_gfx_draw mt_gfx_draw_default(void) MT_NOEXCEPT;

#define MT_GFX_DRAWN 0
#define MT_GFX_UNSUPPORTED 1 /* points or lines: counted, logged once, not drawn */
#define MT_GFX_INVALID (-1)  /* NULL arguments, an unknown primitive, factor or texture */

/* Executes the draw into `target` at once. A count that does not make a whole triangle draws
 * the whole triangles it has (as GL does). Returns MT_GFX_DRAWN (even if nothing was visible),
 * MT_GFX_UNSUPPORTED or MT_GFX_INVALID (logged, nothing drawn). */
int mt_gfx_submit(mt_gfx_target *target, const mt_gfx_draw *draw) MT_NOEXCEPT;

/* --- frames and statistics ------------------------------------------------------------------ */

typedef struct mt_gfx_counters {
    uint64_t draws;       /* submits that returned MT_GFX_DRAWN */
    uint64_t triangles;   /* triangles assembled (before clipping) */
    uint64_t pixels;      /* pixels shaded (written) */
    uint64_t unsupported; /* MT_GFX_UNSUPPORTED submits */
    uint64_t invalid;     /* MT_GFX_INVALID submits */
    uint64_t ns;          /* host time spent in mt_gfx_submit, nanoseconds */
} mt_gfx_counters;

typedef struct mt_gfx_stats {
    uint64_t frames;                /* mt_gfx_end_frame calls */
    mt_gfx_counters total;          /* since the start (or mt_gfx_reset) */
    mt_gfx_counters frame;          /* since the last mt_gfx_end_frame */
    uint32_t textures;              /* textures alive */
    uint64_t texture_bytes;         /* their RGBA8 bytes (replacements' sizes for replaced ones) */
    uint32_t replaced_textures;     /* textures alive whose image is a replacement */
    uint64_t replacements;          /* uploads replaced by the hook */
    uint64_t rejected_replacements; /* replacements of the wrong size, ignored */
} mt_gfx_stats;

mt_gfx_stats mt_gfx_get_stats(void) MT_NOEXCEPT;

/* Ends a frame (the present hook calls it): logs the frame's counters on the trace channel
 * "gfx" and starts the next frame's. */
void mt_gfx_end_frame(void) MT_NOEXCEPT;

/* Frees every texture and zeroes the counters. Targets belong to their owners and stay. For
 * tests and a new process. */
void mt_gfx_reset(void) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_GFX_H */
