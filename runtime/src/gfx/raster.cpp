// mt_gfx's rasterizer (#55, ADR 0039, maplethief/gfx.h): coverage, texturing, colour
// modulation and blending of one triangle or line whose vertices are already snapped.
//
// Coverage. Vertices are in image space (y down) in 1/16 pixel. The edge function of the edge
// p -> q at point (X, Y) is E = (q.x - p.x) * (Y - p.y) - (q.y - p.y) * (X - p.x), exact in 64-bit
// integers. With the triangle oriented so that its doubled area is positive, a pixel centre
// (16 x + 8, 16 y + 8) is inside when all three edge functions are >= 0; a centre exactly on an
// edge counts only for a top edge (horizontal, interior below) or a left edge (going up, interior
// to its right): the top-left rule. Two triangles that share an edge see it in opposite
// directions, so one of them owns the centres on it.
//
// Interpolation. The three edge functions at a centre, divided by the area, are its barycentric
// weights (the weight of vertex i is the function of the edge opposite it). They are exact
// integers, converted to double; s and t are sums of weight * value in a fixed order, divided by
// the sum of weight * q when the triangle is in perspective (its w differ).
//
// Lines (#121): width 1, GL ES 1.1 section 3.4.1's diamond-exit rule, exactly. A segment from a
// to b produces the pixels whose open diamond |X - cx| + |Y - cy| < 1/2 pixel it crosses, except
// the one that holds b, with both endpoints moved by an infinitesimal (-e, -e^2) in window
// coordinates. On the snapped 28.4 endpoints this is integer arithmetic: an x-major segment has
// exactly one pixel in each column whose centre lies in its x-range (an exact floor division;
// the e terms break the ties), and only a's own diamond in the columns beyond; y-major ones the
// same by rows. The perturbation makes the diamonds' boundaries belong to exactly one pixel, and
// half-open segments draw the joints of a strip or loop once. Texture coordinates come from the
// centre's projection onto the segment. Fragments are written by the triangles' code (put).
#include <algorithm>
#include <cmath>
#include <utility>

#include "gfx/gfx_internal.h"

namespace mt_gfx {
namespace {

// (a * b + 127) / 255: an 8-bit product, rounded (GL_MODULATE).
inline u32 mul255(u32 a, u32 b) { return (a * b + 127u) / 255u; }

// floor(a / b) and ceil(a / b) for b > 0.
inline i64 floor_div(i64 a, i64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline i64 ceil_div(i64 a, i64 b) { return -floor_div(-a, b); }

// A wrap mode for a texture size: maps a texel coordinate (may be outside the texture) into it.
// GL_REPEAT on a power-of-two size is a mask (the same result as the positive remainder).
struct Wrap {
    i64 size = 1;
    i64 mask = -1; // >= 0: REPEAT on a power of two
    bool clamp = false;

    Wrap() = default;
    Wrap(u32 n, u32 mode) : size(n), clamp(mode == MT_GFX_CLAMP_TO_EDGE) {
        if (!clamp && (n & (n - 1)) == 0) {
            mask = static_cast<i64>(n) - 1;
        }
    }
    i64 operator()(i64 i) const {
        if (clamp) {
            return std::clamp<i64>(i, 0, size - 1);
        }
        if (mask >= 0) {
            return i & mask;
        }
        const i64 m = i % size;
        return m < 0 ? m + size : m;
    }
};

// Texture coordinate times size, kept in a range where the integer conversions below are exact
// and cannot overflow (2^30 texels; REPEAT makes larger values equivalent anyway, up to the
// texture-size period, which this clamp does not preserve beyond 2^30).
inline double texel_space(double coord, u32 size) {
    return std::clamp(coord * static_cast<double>(size), -1073741824.0, 1073741824.0);
}

struct Sampler {
    const Texture *tex = nullptr;
    u32 filter = MT_GFX_NEAREST; // GL_NEAREST or GL_LINEAR for this triangle
    Wrap wrap_s, wrap_t;

    void fetch(double s, double t, u8 out[4]) const {
        const u32 w = tex->width, h = tex->height;
        const u8 *texels = tex->texels.data();
        const double u = texel_space(s, w), v = texel_space(t, h);
        if (filter == MT_GFX_NEAREST) {
            const i64 x = wrap_s(static_cast<i64>(std::floor(u)));
            const i64 y = wrap_t(static_cast<i64>(std::floor(v)));
            const u8 *p = texels + (static_cast<std::size_t>(y) * w + x) * 4u;
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            out[3] = p[3];
            return;
        }
        // GL_LINEAR: the four texels around (u - 1/2, v - 1/2), 8-bit weights.
        const i64 uf = static_cast<i64>(std::floor(u * 256.0 - 128.0));
        const i64 vf = static_cast<i64>(std::floor(v * 256.0 - 128.0));
        const u32 fu = static_cast<u32>(uf & 255), fv = static_cast<u32>(vf & 255);
        const i64 x0 = wrap_s(uf >> 8), x1 = wrap_s((uf >> 8) + 1);
        const i64 y0 = wrap_t(vf >> 8), y1 = wrap_t((vf >> 8) + 1);
        const u8 *p00 = texels + (static_cast<std::size_t>(y0) * w + x0) * 4u;
        const u8 *p10 = texels + (static_cast<std::size_t>(y0) * w + x1) * 4u;
        const u8 *p01 = texels + (static_cast<std::size_t>(y1) * w + x0) * 4u;
        const u8 *p11 = texels + (static_cast<std::size_t>(y1) * w + x1) * 4u;
        const u32 w00 = (256u - fu) * (256u - fv), w10 = fu * (256u - fv);
        const u32 w01 = (256u - fu) * fv, w11 = fu * fv;
        for (int c = 0; c < 4; ++c) {
            out[c] = static_cast<u8>(
                (p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11 + 32768u) >> 16);
        }
    }
};

// The 8-bit factor of channel `c` (0..3 = R, G, B, A).
inline u32 factor_value(u32 factor, const u8 src[4], const u8 dst[4], int c) {
    switch (factor) {
    case MT_GFX_ZERO:
        return 0;
    case MT_GFX_ONE:
        return 255;
    case MT_GFX_SRC_COLOR:
        return src[c];
    case MT_GFX_ONE_MINUS_SRC_COLOR:
        return 255u - src[c];
    case MT_GFX_SRC_ALPHA:
        return src[3];
    case MT_GFX_ONE_MINUS_SRC_ALPHA:
        return 255u - src[3];
    case MT_GFX_DST_ALPHA:
        return dst[3];
    case MT_GFX_ONE_MINUS_DST_ALPHA:
        return 255u - dst[3];
    case MT_GFX_DST_COLOR:
        return dst[c];
    case MT_GFX_ONE_MINUS_DST_COLOR:
        return 255u - dst[c];
    case MT_GFX_SRC_ALPHA_SATURATE:
        return c == 3 ? 255u : std::min<u32>(src[3], 255u - dst[3]);
    default:
        return 0;
    }
}

// An edge function in 1/16-pixel image coordinates.
struct Edge {
    i64 a = 0, b = 0; // E = a * X + b * Y + c
    i64 c = 0;
    i64 bias = 0; // 0 for a top or left edge, -1 otherwise: covered when E + bias >= 0
};

Edge make_edge(const RasterVertex &p, const RasterVertex &q) {
    Edge e;
    e.a = -(q.y - p.y);
    e.b = q.x - p.x;
    e.c = -(e.a * p.x + e.b * p.y);
    const bool top = q.y == p.y && q.x > p.x;
    const bool left = q.y < p.y;
    e.bias = top || left ? 0 : -1;
    return e;
}

// How a fragment is written: replacing the pixel, or blended. The two blends the game is
// expected to use most get their own code (the same arithmetic as blend_pixel, without the
// factor switches); a fully transparent fragment under alpha blending leaves the pixel as it is,
// which is exactly what the equation gives.
enum class Write { replace, alpha, add, generic };

// Writes one fragment into the pixel at `dst`: replacing it, or blended. Triangles and lines
// share it, so a fragment is written the same way whatever primitive made it.
template <Write kWrite> inline void put(u8 *dst, const u8 frag[4], const Shader &shader) {
    if constexpr (kWrite == Write::alpha) {
        const u32 a = frag[3], ia = 255u - a;
        if (a != 0) {
            for (int c = 0; c < 4; ++c) {
                dst[c] =
                    static_cast<u8>(std::min<u32>(255u, (frag[c] * a + dst[c] * ia + 127u) / 255u));
            }
        }
    } else if constexpr (kWrite == Write::add) {
        for (int c = 0; c < 4; ++c) {
            dst[c] = static_cast<u8>(std::min<u32>(255u, frag[c] + dst[c]));
        }
    } else if constexpr (kWrite == Write::generic) {
        blend_pixel(frag, dst, shader.src_factor, shader.dst_factor, dst);
    } else {
        dst[0] = frag[0];
        dst[1] = frag[1];
        dst[2] = frag[2];
        dst[3] = frag[3];
    }
}

// The write of the shader's blend state.
Write choose_write(const Shader &shader) {
    if (!shader.blend) {
        return Write::replace;
    }
    if (shader.src_factor == MT_GFX_SRC_ALPHA && shader.dst_factor == MT_GFX_ONE_MINUS_SRC_ALPHA) {
        return Write::alpha;
    }
    if (shader.src_factor == MT_GFX_ONE && shader.dst_factor == MT_GFX_ONE) {
        return Write::add;
    }
    return Write::generic;
}

// The pixels [first, last] of a row whose centres pass all three edges: each edge function is
// linear in x, E(x) = 16 a x + k with k = 8 a + b Y + c + bias, so the bounds are exact integer
// divisions. Returns false if the row has none.
bool row_span(const Edge edges[3], i64 cy, i64 px0, i64 px1, i64 *first, i64 *last) {
    i64 lo = px0, hi = px1;
    for (int i = 0; i < 3; ++i) {
        const Edge &e = edges[i];
        const i64 k = 8 * e.a + e.b * cy + e.c + e.bias;
        if (e.a > 0) {
            lo = std::max(lo, ceil_div(-k, 16 * e.a)); // 16 a x + k >= 0
        } else if (e.a < 0) {
            hi = std::min(hi, floor_div(k, -16 * e.a));
        } else if (k < 0) {
            return false;
        }
    }
    *first = lo;
    *last = hi;
    return lo <= hi;
}

// Fills the covered pixels of the box [px0, px1] x [py0, py1] with the shader's choices fixed
// at compile time.
template <bool kTextured, bool kPerspective, Write kWrite>
u64 fill(mt_gfx_target &target, const Shader &shader, const Sampler &sampler,
         const RasterVertex *v[3], const Edge edges[3], i64 area, i64 px0, i64 px1, i64 py0,
         i64 py1) {
    const double inv_area = 1.0 / static_cast<double>(area);
    // Per-vertex attributes; edge i's function is the weight of vertex i.
    double s[3], t[3], q[3];
    for (int i = 0; i < 3; ++i) {
        q[i] = v[i]->q;
        s[i] = kPerspective ? v[i]->s * v[i]->q : v[i]->s;
        t[i] = kPerspective ? v[i]->t * v[i]->q : v[i]->t;
    }
    u64 pixels = 0;
    const u8 *color = shader.color;
    // Modulating by opaque white changes nothing ((a * 255 + 127) / 255 = a).
    const bool white = color[0] == 255 && color[1] == 255 && color[2] == 255 && color[3] == 255;
    for (i64 py = py0; py <= py1; ++py) {
        const i64 cy = py * 16 + 8;
        i64 first = 0, last = 0;
        if (!row_span(edges, cy, px0, px1, &first, &last)) {
            continue;
        }
        const i64 cx = first * 16 + 8;
        i64 e[3];
        for (int i = 0; i < 3; ++i) {
            e[i] = edges[i].a * cx + edges[i].b * cy + edges[i].c;
        }
        u8 *dst = target.pixels.data() + (static_cast<std::size_t>(py) * target.width + first) * 4u;
        for (i64 px = first; px <= last; ++px, dst += 4) {
            u8 frag[4];
            if constexpr (kTextured) {
                const double w0 = static_cast<double>(e[0]), w1 = static_cast<double>(e[1]),
                             w2 = static_cast<double>(e[2]);
                double ss = w0 * s[0] + w1 * s[1] + w2 * s[2];
                double tt = w0 * t[0] + w1 * t[1] + w2 * t[2];
                if constexpr (kPerspective) {
                    const double qq = w0 * q[0] + w1 * q[1] + w2 * q[2];
                    ss /= qq;
                    tt /= qq;
                } else {
                    ss *= inv_area;
                    tt *= inv_area;
                }
                sampler.fetch(ss, tt, frag);
                if (!white) {
                    for (int c = 0; c < 4; ++c) {
                        frag[c] = static_cast<u8>(mul255(frag[c], color[c]));
                    }
                }
            } else {
                frag[0] = color[0];
                frag[1] = color[1];
                frag[2] = color[2];
                frag[3] = color[3];
            }
            put<kWrite>(dst, frag, shader);
            e[0] += edges[0].a * 16;
            e[1] += edges[1].a * 16;
            e[2] += edges[2].a * 16;
        }
        pixels += static_cast<u64>(last - first + 1);
    }
    return pixels;
}

template <bool kTextured, bool kPerspective>
u64 dispatch_write(mt_gfx_target &target, const Shader &shader, const Sampler &sampler,
                   const RasterVertex *v[3], const Edge edges[3], i64 area, i64 px0, i64 px1,
                   i64 py0, i64 py1, Write write) {
    switch (write) {
    case Write::alpha:
        return fill<kTextured, kPerspective, Write::alpha>(target, shader, sampler, v, edges, area,
                                                           px0, px1, py0, py1);
    case Write::add:
        return fill<kTextured, kPerspective, Write::add>(target, shader, sampler, v, edges, area,
                                                         px0, px1, py0, py1);
    case Write::generic:
        return fill<kTextured, kPerspective, Write::generic>(target, shader, sampler, v, edges,
                                                             area, px0, px1, py0, py1);
    default:
        return fill<kTextured, kPerspective, Write::replace>(target, shader, sampler, v, edges,
                                                             area, px0, px1, py0, py1);
    }
}

u64 dispatch(mt_gfx_target &target, const Shader &shader, const Sampler &sampler,
             const RasterVertex *v[3], const Edge edges[3], i64 area, i64 px0, i64 px1, i64 py0,
             i64 py1, bool textured, bool perspective, Write write) {
    if (!textured) {
        return dispatch_write<false, false>(target, shader, sampler, v, edges, area, px0, px1, py0,
                                            py1, write);
    }
    if (perspective) {
        return dispatch_write<true, true>(target, shader, sampler, v, edges, area, px0, px1, py0,
                                          py1, write);
    }
    return dispatch_write<true, false>(target, shader, sampler, v, edges, area, px0, px1, py0, py1,
                                       write);
}

// GL_NEAREST or GL_LINEAR for a primitive that maps sqrt(rho2) texels to a pixel: the
// magnification filter up to one (within 1/1024), the minification filter's base level above.
// GL: with a GL_LINEAR magnification filter and a *_MIPMAP_NEAREST minification filter, the
// switch-over is at lambda = 0.5 (rho^2 = 2) instead of 0.
u32 filter_for(const Texture &tex, double rho2) {
    const u32 mag = tex.mag_filter, min = base_filter(tex.min_filter);
    if (mag == min) {
        return mag;
    }
    const bool half = mag == MT_GFX_LINEAR && (tex.min_filter == MT_GFX_NEAREST_MIPMAP_NEAREST ||
                                               tex.min_filter == MT_GFX_LINEAR_MIPMAP_NEAREST);
    const double limit = (half ? 2.0 : 1.0) * (1.0 + 1.0 / 1024.0);
    return rho2 <= limit ? mag : min;
}

// GL_NEAREST or GL_LINEAR for the triangle: the magnification filter when it maps at most one
// texel (within 1/1024) to a pixel along x and along y, the minification filter's base level
// otherwise. The scale comes from the affine texture-coordinate gradients of the snapped
// triangle (for a triangle in perspective, an approximation of GL's per-pixel choice).
u32 choose_filter(const Texture &tex, const RasterVertex *v[3], i64 area) {
    const u32 mag = tex.mag_filter, min = base_filter(tex.min_filter);
    if (mag == min) {
        return mag;
    }
    const double dx1 = static_cast<double>(v[1]->x - v[0]->x);
    const double dy1 = static_cast<double>(v[1]->y - v[0]->y);
    const double dx2 = static_cast<double>(v[2]->x - v[0]->x);
    const double dy2 = static_cast<double>(v[2]->y - v[0]->y);
    const double du1 = (v[1]->s - v[0]->s) * tex.width, du2 = (v[2]->s - v[0]->s) * tex.width;
    const double dv1 = (v[1]->t - v[0]->t) * tex.height, dv2 = (v[2]->t - v[0]->t) * tex.height;
    // Gradients per 1/16 pixel, times 16: per pixel.
    const double k = 16.0 / static_cast<double>(area);
    const double dudx = (du1 * dy2 - du2 * dy1) * k, dvdx = (dv1 * dy2 - dv2 * dy1) * k;
    const double dudy = (du2 * dx1 - du1 * dx2) * k, dvdy = (dv2 * dx1 - dv1 * dx2) * k;
    const double rho2 = std::max(dudx * dudx + dvdx * dvdx, dudy * dudy + dvdy * dvdy);
    return filter_for(tex, rho2);
}

} // namespace

bool valid_factor(u32 factor) noexcept {
    switch (factor) {
    case MT_GFX_ZERO:
    case MT_GFX_ONE:
    case MT_GFX_SRC_COLOR:
    case MT_GFX_ONE_MINUS_SRC_COLOR:
    case MT_GFX_SRC_ALPHA:
    case MT_GFX_ONE_MINUS_SRC_ALPHA:
    case MT_GFX_DST_ALPHA:
    case MT_GFX_ONE_MINUS_DST_ALPHA:
    case MT_GFX_DST_COLOR:
    case MT_GFX_ONE_MINUS_DST_COLOR:
    case MT_GFX_SRC_ALPHA_SATURATE:
        return true;
    default:
        return false;
    }
}

void blend_pixel(const u8 src[4], const u8 dst[4], u32 src_factor, u32 dst_factor,
                 u8 out[4]) noexcept {
    u8 result[4];
    for (int c = 0; c < 4; ++c) {
        const u32 sum = src[c] * factor_value(src_factor, src, dst, c) +
                        dst[c] * factor_value(dst_factor, src, dst, c);
        result[c] = static_cast<u8>(std::min<u32>(255u, (sum + 127u) / 255u));
    }
    out[0] = result[0];
    out[1] = result[1];
    out[2] = result[2];
    out[3] = result[3];
}

u64 raster_triangle(mt_gfx_target &target, const Shader &shader, const RasterVertex &a,
                    const RasterVertex &b, const RasterVertex &c) noexcept {
    const RasterVertex *v[3] = {&a, &b, &c};
    i64 area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0) {
        return 0;
    }
    if (area < 0) {
        std::swap(v[1], v[2]);
        area = -area;
    }
    // Edge i is opposite vertex i.
    const Edge edges[3] = {make_edge(*v[1], *v[2]), make_edge(*v[2], *v[0]),
                           make_edge(*v[0], *v[1])};

    // Pixels whose centres lie in the bounding box, inside the target and the viewport.
    const i64 min_x = std::min({v[0]->x, v[1]->x, v[2]->x});
    const i64 max_x = std::max({v[0]->x, v[1]->x, v[2]->x});
    const i64 min_y = std::min({v[0]->y, v[1]->y, v[2]->y});
    const i64 max_y = std::max({v[0]->y, v[1]->y, v[2]->y});
    const i64 px0 = std::max<i64>({ceil_div(min_x - 8, 16), 0, shader.x0});
    const i64 px1 =
        std::min<i64>({floor_div(max_x - 8, 16), static_cast<i64>(target.width) - 1, shader.x1});
    const i64 py0 = std::max<i64>({ceil_div(min_y - 8, 16), 0, shader.y0});
    const i64 py1 =
        std::min<i64>({floor_div(max_y - 8, 16), static_cast<i64>(target.height) - 1, shader.y1});
    if (px0 > px1 || py0 > py1) {
        return 0;
    }

    Sampler sampler;
    const bool textured = shader.texture != nullptr;
    if (textured) {
        sampler.tex = shader.texture;
        sampler.wrap_s = Wrap(shader.texture->width, shader.texture->wrap_s);
        sampler.wrap_t = Wrap(shader.texture->height, shader.texture->wrap_t);
        sampler.filter = choose_filter(*shader.texture, v, area);
    }
    const bool perspective = !(a.w == b.w && a.w == c.w);
    const Write write = choose_write(shader);
    return dispatch(target, shader, sampler, v, edges, area, px0, px1, py0, py1, textured,
                    perspective, write);
}

namespace {

// The pixel whose open diamond |X - cx| + |Y - cy| < 8 holds the perturbed point p' (GL ES 1.1
// section 3.4.1: p moved by (-e, -e^2) in window coordinates, (-e, +e^2) on the image, y down).
// p' lies strictly inside the pixel square (floor((x - e) / 16), floor((y + e^2) / 16)), and a
// diamond lies inside its own square, so that pixel is the only candidate. On the diamond's
// boundary (L1 distance exactly 8) the e term decides: p' is inside when p is right of the
// centre (moving left takes it in).
struct DiamondHit {
    bool hit = false;
    i64 x = 0, y = 0;
};

DiamondHit diamond_of(const RasterVertex &p) {
    DiamondHit h;
    h.x = floor_div(p.x - 1, 16);
    h.y = floor_div(p.y, 16);
    const i64 dx = p.x - (16 * h.x + 8), dy = p.y - (16 * h.y + 8);
    const i64 l1 = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    h.hit = l1 < 8 || (l1 == 8 && dx > 0);
    return h;
}

// Shades and writes a line's fragments.
struct LineWriter {
    mt_gfx_target &target;
    const Shader &shader;
    const RasterVertex &a, &b;
    Sampler sampler;
    bool textured = false, perspective = false;
    Write write = Write::replace;
    i64 px0 = 0, px1 = 0, py0 = 0, py1 = 0; // the pixels the draw may write (inclusive)
    double len2 = 0.0;                      // |b - a|^2 in (1/16 pixel)^2, exact
    u64 pixels = 0;

    LineWriter(mt_gfx_target &tg, const Shader &sh, const RasterVertex &pa, const RasterVertex &pb)
        : target(tg), shader(sh), a(pa), b(pb) {}

    void operator()(i64 x, i64 y) {
        if (x < px0 || x > px1 || y < py0 || y > py1) {
            return;
        }
        u8 frag[4] = {shader.color[0], shader.color[1], shader.color[2], shader.color[3]};
        if (textured) {
            // The fragment centre projected onto the segment: k in [0, 1] from a to b (an exact
            // integer dot product, then one division), the attributes interpolated as GL ES 1.1
            // section 3.4.1 does, perspective-correctly when the w differ.
            const i64 cx = x * 16 + 8, cy = y * 16 + 8;
            const i64 dot = (cx - a.x) * (b.x - a.x) + (cy - a.y) * (b.y - a.y);
            const double k = std::clamp(static_cast<double>(dot) / len2, 0.0, 1.0);
            const double ik = 1.0 - k;
            double ss = 0.0, tt = 0.0;
            if (perspective) {
                const double qq = ik * a.q + k * b.q;
                ss = (ik * (a.s * a.q) + k * (b.s * b.q)) / qq;
                tt = (ik * (a.t * a.q) + k * (b.t * b.q)) / qq;
            } else {
                ss = ik * a.s + k * b.s;
                tt = ik * a.t + k * b.t;
            }
            u8 texel[4];
            sampler.fetch(ss, tt, texel);
            for (int c = 0; c < 4; ++c) {
                frag[c] = static_cast<u8>(mul255(texel[c], shader.color[c]));
            }
        }
        u8 *dst = target.pixels.data() + (static_cast<std::size_t>(y) * target.width + x) * 4u;
        switch (write) {
        case Write::alpha:
            put<Write::alpha>(dst, frag, shader);
            break;
        case Write::add:
            put<Write::add>(dst, frag, shader);
            break;
        case Write::generic:
            put<Write::generic>(dst, frag, shader);
            break;
        default:
            put<Write::replace>(dst, frag, shader);
            break;
        }
        ++pixels;
    }
};

} // namespace

u64 raster_line(mt_gfx_target &target, const Shader &shader, const RasterVertex &a,
                const RasterVertex &b) noexcept {
    const i64 dx = b.x - a.x, dy = b.y - a.y;
    if (dx == 0 && dy == 0) {
        return 0; // a' = b': whatever diamond holds a' holds b'
    }
    LineWriter out(target, shader, a, b);
    out.px0 = std::max<i64>(0, shader.x0);
    out.px1 = std::min<i64>(static_cast<i64>(target.width) - 1, shader.x1);
    out.py0 = std::max<i64>(0, shader.y0);
    out.py1 = std::min<i64>(static_cast<i64>(target.height) - 1, shader.y1);
    if (out.px0 > out.px1 || out.py0 > out.py1) {
        return 0;
    }
    out.len2 = static_cast<double>(dx * dx + dy * dy);
    out.write = choose_write(shader);
    if (shader.texture != nullptr) {
        const Texture &tex = *shader.texture;
        out.textured = true;
        out.perspective = a.w != b.w;
        out.sampler.tex = &tex;
        out.sampler.wrap_s = Wrap(tex.width, tex.wrap_s);
        out.sampler.wrap_t = Wrap(tex.height, tex.wrap_t);
        // Texels per pixel along the line (len2 is in 1/16 pixel).
        const double du = (b.s - a.s) * tex.width, dv = (b.t - a.t) * tex.height;
        out.sampler.filter = filter_for(tex, (du * du + dv * dv) * 256.0 / out.len2);
    }

    const DiamondHit hit_a = diamond_of(a), hit_b = diamond_of(b);
    auto emit = [&](i64 x, i64 y) {
        if (hit_b.hit && x == hit_b.x && y == hit_b.y) {
            return; // the diamond that holds b': lines are half-open
        }
        out(x, y);
    };

    const i64 adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx >= ady) {
        // x-major (|slope| <= 1). With l the left and r the right endpoint, the point of the
        // segment nearest (in L1) to a column's perturbed centre is the one at that centre's x
        // when it lies in [l.x, r.x], else an endpoint. So:
        //  - an interior column, l.x <= 16 i + 8 < r.x, has exactly one fragment: the row j
        //    where N = (l.y - 16 j - 8) DX + (16 i + 8 - l.x) DY, the segment's offset from the
        //    centre times DX, satisfies -8 DX < N + e DY + e^2 DX < 8 DX;
        //  - another column has a fragment only where an endpoint's diamond is, and only a's
        //    counts (b' is excluded).
        const RasterVertex &l = dx > 0 ? a : b, &r = dx > 0 ? b : a;
        const i64 DX = r.x - l.x, DY = r.y - l.y;
        const i64 first = std::max(ceil_div(l.x - 8, 16), out.px0);
        const i64 last = std::min(ceil_div(r.x - 8, 16) - 1, out.px1);
        for (i64 i = first; i <= last; ++i) {
            const i64 m = (l.y - 8) * DX + (16 * i + 8 - l.x) * DY;
            i64 j = floor_div(m + 8 * DX, 16 * DX); // N = m - 16 j DX in [-8 DX, 8 DX)
            if (m - 16 * j * DX == -8 * DX && DY < 0) {
                --j; // N = -8 DX is outside when DY < 0; N = 8 DX a row up is inside
            }
            emit(i, j);
        }
        const i64 centre = 16 * hit_a.x + 8;
        if (hit_a.hit && !(l.x <= centre && centre < r.x)) {
            emit(hit_a.x, hit_a.y);
        }
    } else {
        // y-major, the same by rows. With top and bottom endpoints, an interior row,
        // top.y < 16 j + 8 <= bottom.y, has the column i where
        // N = (top.x - 16 i - 8) DY + (16 j + 8 - top.y) DX satisfies
        // -8 DY < N - e DY - e^2 DX < 8 DY, that is -8 DY < N <= 8 DY.
        const RasterVertex &top = dy > 0 ? a : b, &bottom = dy > 0 ? b : a;
        const i64 DX = bottom.x - top.x, DY = bottom.y - top.y;
        const i64 first = std::max(floor_div(top.y - 8, 16) + 1, out.py0);
        const i64 last = std::min(floor_div(bottom.y - 8, 16), out.py1);
        for (i64 j = first; j <= last; ++j) {
            const i64 m = (top.x - 8) * DY + (16 * j + 8 - top.y) * DX;
            emit(ceil_div(m - 8 * DY, 16 * DY), j);
        }
        const i64 centre = 16 * hit_a.y + 8;
        if (hit_a.hit && !(top.y < centre && centre <= bottom.y)) {
            emit(hit_a.x, hit_a.y);
        }
    }
    return out.pixels;
}

} // namespace mt_gfx
