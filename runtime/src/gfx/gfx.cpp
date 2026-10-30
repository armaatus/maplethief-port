// mt_gfx (#55, ADR 0039, maplethief/gfx.h): targets, draws, primitive assembly, clipping, the
// divide by w, the viewport, snapping, and the frame counters. raster.cpp rasterizes the
// resulting triangles and lines, texture.cpp keeps the textures. A line segment is clipped
// against the same planes in the same order (one parametric cut per plane, from its inside
// endpoint), so the viewport bounds its pixels as it does a triangle's.
//
// Clipping is Sutherland-Hodgman in clip space against w > 2^-20 (so the divide is safe), then
// a guard band of -16w <= x <= 16w, -16w <= y <= 16w, then -w <= z <= w, in that order; the
// rasterizer keeps the pixels inside the viewport (Shader's bounds), so what is drawn is what
// clipping to -w <= x, y <= w would draw. The guard band (ADR 0041, #57) leaves a sprite that
// hangs over the screen's edge unclipped: its vertices, snapped to whole pixels, keep their
// texture coordinates, so a 1:1 sprite samples texel centres up to the edge; a vertex made by
// clipping would be snapped away from where its coordinates were computed, and GL_LINEAR would
// mix in a little of the neighbouring texels. Where an edge crosses a plane the new vertex is
// always interpolated from the inside vertex towards the outside one, so two triangles that
// share a clipped edge get the same new vertex and stay watertight. The clipped polygon is
// drawn as a fan from its first vertex.
//
// This file is compiled without floating-point contraction (runtime/CMakeLists.txt), like
// raster.cpp: every float and double operation rounds the same way on every compiler and CPU.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <unordered_set>

#include "diag/diag_internal.h"
#include "gfx/gfx_internal.h"
#include "maplethief/diag.h"

namespace mt_gfx {
namespace {

struct State {
    mt_gfx_stats stats{};
    bool logged_points = false;
    u32 invalid_logged = 0;
};

// The backend (#120, ADR 0060) and the targets alive, which set_backend hands over. Kept across
// mt_gfx_reset, like the replacement hook.
struct Backends {
    Backend *current = nullptr;
    std::unordered_set<mt_gfx_target *> targets;
};

Backends &backends() {
    static Backends b;
    return b;
}

State &state() {
    static State s;
    return s;
}

// Invalid draws are logged up to this many times per process (a broken caller would repeat).
constexpr u32 k_max_invalid_logs = 16;

void log_invalid(const char *why) {
    State &st = state();
    if (st.invalid_logged >= k_max_invalid_logs) {
        return;
    }
    ++st.invalid_logged;
    char line[160];
    std::snprintf(line, sizeof line, "gfx: invalid draw: %s%s", why,
                  st.invalid_logged == k_max_invalid_logs ? " (further invalid draws not logged)"
                                                          : "");
    mt_diag_log(line);
}

using ClipVertex = mt_gfx_vertex;

constexpr float k_min_w = 1.0f / 1048576.0f; // 2^-20
constexpr float k_guard = 16.0f;             // the guard band, in viewport sizes from its centre
constexpr int k_planes = 7;

// Signed distance of `v` to clip plane `plane` (>= 0: inside).
inline float plane_distance(const ClipVertex &v, int plane) {
    switch (plane) {
    case 0:
        return v.w - k_min_w;
    case 1:
        return k_guard * v.w + v.x;
    case 2:
        return k_guard * v.w - v.x;
    case 3:
        return k_guard * v.w + v.y;
    case 4:
        return k_guard * v.w - v.y;
    case 5:
        return v.w + v.z;
    default:
        return v.w - v.z;
    }
}

// Bit `plane` set: `v` is outside it.
inline u32 outcode(const ClipVertex &v) {
    u32 code = 0;
    for (int p = 0; p < k_planes; ++p) {
        if (!(plane_distance(v, p) >= 0.0f)) { // NaN counts as outside
            code |= 1u << p;
        }
    }
    return code;
}

// `in` + t * (`out` - `in`) where the edge crosses the plane, t = d_in / (d_in - d_out).
ClipVertex intersect(const ClipVertex &in, const ClipVertex &out, float d_in, float d_out) {
    const float t = d_in / (d_in - d_out);
    ClipVertex r;
    r.x = in.x + t * (out.x - in.x);
    r.y = in.y + t * (out.y - in.y);
    r.z = in.z + t * (out.z - in.z);
    r.w = in.w + t * (out.w - in.w);
    r.s = in.s + t * (out.s - in.s);
    r.t = in.t + t * (out.t - in.t);
    return r;
}

// A triangle plus a vertex per plane at most.
constexpr int k_max_polygon = 3 + k_planes;

// Clips the triangle against the planes in `planes` (outcode bits); returns the polygon's vertex
// count (0: nothing left).
int clip_triangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c, u32 planes,
                  ClipVertex out[k_max_polygon]) {
    ClipVertex buf[2][k_max_polygon];
    int n = 3;
    buf[0][0] = a;
    buf[0][1] = b;
    buf[0][2] = c;
    int cur = 0;
    for (int p = 0; p < k_planes && n > 0; ++p) {
        if ((planes & (1u << p)) == 0) {
            continue;
        }
        const ClipVertex *src = buf[cur];
        ClipVertex *dst = buf[cur ^ 1];
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const ClipVertex &v0 = src[i];
            const ClipVertex &v1 = src[(i + 1) % n];
            const float d0 = plane_distance(v0, p), d1 = plane_distance(v1, p);
            const bool in0 = d0 >= 0.0f, in1 = d1 >= 0.0f;
            if (in0) {
                dst[m++] = v0;
            }
            if (in0 && !in1) {
                dst[m++] = intersect(v0, v1, d0, d1);
            } else if (!in0 && in1) {
                dst[m++] = intersect(v1, v0, d1, d0);
            }
        }
        n = m;
        cur ^= 1;
    }
    std::copy(buf[cur], buf[cur] + n, out);
    return n;
}

struct Viewport {
    float x = 0, y = 0, w = 0, h = 0;
};

// Divide by w, viewport, snap to 1/16 pixel, flip to image rows.
RasterVertex to_raster(const ClipVertex &v, const Viewport &vp, u32 target_height) {
    const float xd = v.x / v.w, yd = v.y / v.w;
    const float wx = (xd + 1.0f) * 0.5f * vp.w + vp.x;
    const float wy = (yd + 1.0f) * 0.5f * vp.h + vp.y;
    RasterVertex r;
    r.x = static_cast<i64>(std::floor(wx * 16.0f + 0.5f));
    const i64 y_window = static_cast<i64>(std::floor(wy * 16.0f + 0.5f));
    r.y = static_cast<i64>(target_height) * 16 - y_window;
    r.w = v.w;
    r.s = v.s;
    r.t = v.t;
    r.q = 1.0 / static_cast<double>(v.w);
    return r;
}

inline bool finite(const ClipVertex &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w) &&
           std::isfinite(v.s) && std::isfinite(v.t);
}

// Clips, maps and rasterizes one triangle. A triangle with a vertex that is not finite (or
// becomes infinite in clipping) is dropped: GL leaves it undefined.
u64 draw_triangle(mt_gfx_target &target, const Shader &shader, const Viewport &vp,
                  const ClipVertex &a, const ClipVertex &b, const ClipVertex &c) {
    if (!finite(a) || !finite(b) || !finite(c)) {
        return 0;
    }
    const u32 ca = outcode(a), cb = outcode(b), cc = outcode(c);
    if ((ca & cb & cc) != 0) {
        return 0; // entirely outside one plane
    }
    ClipVertex poly[k_max_polygon];
    int n = 3;
    if ((ca | cb | cc) == 0) {
        poly[0] = a;
        poly[1] = b;
        poly[2] = c;
    } else {
        n = clip_triangle(a, b, c, ca | cb | cc, poly);
    }
    if (n < 3) {
        return 0;
    }
    RasterVertex rv[k_max_polygon];
    for (int i = 0; i < n; ++i) {
        if (!finite(poly[i])) {
            return 0;
        }
        rv[i] = to_raster(poly[i], vp, target.height);
    }
    u64 pixels = 0;
    for (int i = 1; i + 1 < n; ++i) {
        pixels += raster_triangle(target, shader, rv[0], rv[i], rv[i + 1]);
    }
    return pixels;
}

// Clips, maps and rasterizes one line segment: the same planes in the same order as a
// triangle, each new endpoint interpolated from the inside one towards the outside one.
u64 draw_line(mt_gfx_target &target, const Shader &shader, const Viewport &vp, const ClipVertex &a,
              const ClipVertex &b) {
    if (!finite(a) || !finite(b)) {
        return 0;
    }
    const u32 ca = outcode(a), cb = outcode(b);
    if ((ca & cb) != 0) {
        return 0;
    }
    ClipVertex p = a, q = b;
    const u32 planes = ca | cb;
    for (int plane = 0; plane < k_planes && planes != 0; ++plane) {
        if ((planes & (1u << plane)) == 0) {
            continue;
        }
        const float dp = plane_distance(p, plane), dq = plane_distance(q, plane);
        const bool in_p = dp >= 0.0f, in_q = dq >= 0.0f;
        if (!in_p && !in_q) {
            return 0;
        }
        if (in_p && !in_q) {
            q = intersect(p, q, dp, dq);
        } else if (!in_p && in_q) {
            p = intersect(q, p, dq, dp);
        }
    }
    if (!finite(p) || !finite(q)) {
        return 0;
    }
    return raster_line(target, shader, to_raster(p, vp, target.height),
                       to_raster(q, vp, target.height));
}

void add(mt_gfx_counters &to, const mt_gfx_counters &d) {
    to.draws += d.draws;
    to.triangles += d.triangles;
    to.lines += d.lines;
    to.pixels += d.pixels;
    to.unsupported += d.unsupported;
    to.invalid += d.invalid;
    to.ns += d.ns;
}

int submit(mt_gfx_target *target, const mt_gfx_draw *draw, mt_gfx_counters &count) {
    if (target == nullptr || draw == nullptr) {
        log_invalid("no target or no draw");
        ++count.invalid;
        return MT_GFX_INVALID;
    }
    const u32 prim = draw->primitive;
    if (prim == MT_GFX_POINTS) {
        State &st = state();
        if (!st.logged_points) {
            st.logged_points = true;
            mt_diag_log("gfx: GL_POINTS draws are not supported: counted, not drawn (logged once)");
        }
        ++count.unsupported;
        return MT_GFX_UNSUPPORTED;
    }
    if (prim > MT_GFX_TRIANGLE_FAN) {
        log_invalid("unknown primitive");
        ++count.invalid;
        return MT_GFX_INVALID;
    }
    if (draw->vertices == nullptr && draw->count != 0) {
        log_invalid("no vertices");
        ++count.invalid;
        return MT_GFX_INVALID;
    }
    if (!valid_factor(draw->src_factor) || !valid_factor(draw->dst_factor)) {
        log_invalid("unknown blend factor");
        ++count.invalid;
        return MT_GFX_INVALID;
    }
    Shader shader;
    if (draw->texture != 0) {
        const Texture *tex = find_texture(draw->texture);
        if (tex == nullptr) {
            log_invalid("unknown texture");
            ++count.invalid;
            return MT_GFX_INVALID;
        }
        if (!tex->texels.empty()) {
            shader.texture = tex; // an empty texture draws untextured, like GL's incomplete one
        }
    }
    std::memcpy(shader.color, draw->color, 4);
    shader.blend = draw->blend != 0;
    shader.src_factor = draw->src_factor;
    shader.dst_factor = draw->dst_factor;
    // Without blending GL_ONE / GL_ZERO is what happens; blending with them is the same.
    if (shader.blend && shader.src_factor == MT_GFX_ONE && shader.dst_factor == MT_GFX_ZERO) {
        shader.blend = false;
    }

    // The viewport in logical units, times the target's scale: pixels (exact integers in float).
    const int32_t scale = static_cast<int32_t>(target->scale);
    Viewport vp;
    vp.x = static_cast<float>(
        std::clamp(draw->viewport[0], -MT_GFX_MAX_VIEWPORT, MT_GFX_MAX_VIEWPORT) * scale);
    vp.y = static_cast<float>(
        std::clamp(draw->viewport[1], -MT_GFX_MAX_VIEWPORT, MT_GFX_MAX_VIEWPORT) * scale);
    vp.w = static_cast<float>(std::clamp(draw->viewport[2], 0, MT_GFX_MAX_VIEWPORT) * scale);
    vp.h = static_cast<float>(std::clamp(draw->viewport[3], 0, MT_GFX_MAX_VIEWPORT) * scale);
    // The viewport's pixels in image rows (y down): what clipping to it would leave.
    shader.x0 = static_cast<i64>(vp.x);
    shader.x1 = static_cast<i64>(vp.x) + static_cast<i64>(vp.w) - 1;
    shader.y0 = static_cast<i64>(target->height) - static_cast<i64>(vp.y) - static_cast<i64>(vp.h);
    shader.y1 = static_cast<i64>(target->height) - static_cast<i64>(vp.y) - 1;

    if (Backend *b = backends().current) {
        Draw d;
        d.src = draw;
        d.texture = shader.texture;
        d.texture_handle = shader.texture != nullptr ? draw->texture : 0;
        std::memcpy(d.color, shader.color, 4);
        d.blend = shader.blend;
        d.src_factor = shader.src_factor;
        d.dst_factor = shader.dst_factor;
        d.vp_x = vp.x;
        d.vp_y = vp.y;
        d.vp_w = vp.w;
        d.vp_h = vp.h;
        b->submit(*target, d, count);
        ++count.draws;
        return MT_GFX_DRAWN;
    }

    const mt_gfx_vertex *v = draw->vertices;
    const u32 n = draw->count;
    u64 triangles = 0, lines = 0, pixels = 0;
    const bool visible = vp.w > 0 && vp.h > 0;
    auto tri = [&](u32 i0, u32 i1, u32 i2) {
        ++triangles;
        if (visible) {
            pixels += draw_triangle(*target, shader, vp, v[i0], v[i1], v[i2]);
        }
    };
    auto seg = [&](u32 i0, u32 i1) {
        ++lines;
        if (visible) {
            pixels += draw_line(*target, shader, vp, v[i0], v[i1]);
        }
    };
    if (prim == MT_GFX_LINES) {
        for (u32 i = 0; i + 1 < n; i += 2) {
            seg(i, i + 1);
        }
    } else if (prim == MT_GFX_LINE_STRIP || prim == MT_GFX_LINE_LOOP) {
        for (u32 i = 0; i + 1 < n; ++i) {
            seg(i, i + 1);
        }
        if (prim == MT_GFX_LINE_LOOP && n >= 2) {
            seg(n - 1, 0);
        }
    } else if (prim == MT_GFX_TRIANGLES) {
        for (u32 i = 0; i + 2 < n; i += 3) {
            tri(i, i + 1, i + 2);
        }
    } else if (prim == MT_GFX_TRIANGLE_STRIP) {
        for (u32 i = 0; i + 2 < n; ++i) {
            if ((i & 1u) == 0) {
                tri(i, i + 1, i + 2);
            } else {
                tri(i + 1, i, i + 2);
            }
        }
    } else {
        for (u32 i = 1; i + 1 < n; ++i) {
            tri(0, i, i + 1);
        }
    }
    ++count.draws;
    count.triangles += triangles;
    count.lines += lines;
    count.pixels += pixels;
    return MT_GFX_DRAWN;
}

// Brings a backend's target up to date for the CPU (a no-op for the software rasterizer).
void cpu_access(const mt_gfx_target *target, Access access) {
    if (target != nullptr && target->backend != nullptr && backends().current != nullptr) {
        // The pixels are the target's state, not its identity: syncing them is fine on a
        // const target.
        backends().current->access(*const_cast<mt_gfx_target *>(target), access);
    }
}

} // namespace

Backend *backend() noexcept { return backends().current; }

void set_backend(Backend *next) noexcept {
    Backends &b = backends();
    if (b.current == next) {
        return;
    }
    if (b.current != nullptr) {
        for (mt_gfx_target *t : b.targets) {
            if (t->backend != nullptr) {
                b.current->access(*t, Access::kRead);
                b.current->forget(*t);
            }
        }
    }
    b.current = next;
}

void notify_texture_changing(u32 handle, bool destroyed) noexcept {
    if (Backend *b = backends().current) {
        b->texture_changing(handle, destroyed);
    }
}

} // namespace mt_gfx

using namespace mt_gfx;

extern "C" mt_gfx_target *mt_gfx_target_create(uint32_t width, uint32_t height,
                                               uint32_t scale) noexcept {
    if (width == 0 || height == 0 || scale == 0 || scale > MT_GFX_MAX_SCALE ||
        width > MT_GFX_MAX_SIZE / scale || height > MT_GFX_MAX_SIZE / scale) {
        return nullptr;
    }
    auto *target = new (std::nothrow) mt_gfx_target;
    if (target == nullptr) {
        return nullptr;
    }
    target->logical_width = width;
    target->logical_height = height;
    target->scale = scale;
    target->width = width * scale;
    target->height = height * scale;
    try {
        target->pixels.assign(static_cast<std::size_t>(target->width) * target->height * 4u, 0);
        backends().targets.insert(target);
    } catch (const std::bad_alloc &) {
        delete target;
        return nullptr;
    }
    return target;
}

extern "C" void mt_gfx_target_destroy(mt_gfx_target *target) noexcept {
    if (target == nullptr) {
        return;
    }
    if (target->backend != nullptr && backends().current != nullptr) {
        backends().current->forget(*target);
    }
    backends().targets.erase(target);
    delete target;
}

extern "C" uint32_t mt_gfx_target_width(const mt_gfx_target *target) noexcept {
    return target != nullptr ? target->logical_width : 0;
}

extern "C" uint32_t mt_gfx_target_height(const mt_gfx_target *target) noexcept {
    return target != nullptr ? target->logical_height : 0;
}

extern "C" uint32_t mt_gfx_target_scale(const mt_gfx_target *target) noexcept {
    return target != nullptr ? target->scale : 0;
}

extern "C" uint32_t mt_gfx_target_pixel_width(const mt_gfx_target *target) noexcept {
    return target != nullptr ? target->width : 0;
}

extern "C" uint32_t mt_gfx_target_pixel_height(const mt_gfx_target *target) noexcept {
    return target != nullptr ? target->height : 0;
}

extern "C" uint8_t *mt_gfx_target_pixels(mt_gfx_target *target) noexcept {
    cpu_access(target, Access::kWrite);
    return target != nullptr ? target->pixels.data() : nullptr;
}

extern "C" const uint8_t *mt_gfx_target_cpixels(const mt_gfx_target *target) noexcept {
    cpu_access(target, Access::kRead);
    return target != nullptr ? target->pixels.data() : nullptr;
}

extern "C" void mt_gfx_target_fill(mt_gfx_target *target, const uint8_t rgba[4]) noexcept {
    if (target == nullptr || rgba == nullptr) {
        return;
    }
    cpu_access(target, Access::kOverwrite);
    for (std::size_t i = 0; i < target->pixels.size(); i += 4) {
        std::memcpy(target->pixels.data() + i, rgba, 4);
    }
}

extern "C" mt_gfx_draw mt_gfx_draw_default(void) noexcept {
    mt_gfx_draw d{};
    d.primitive = MT_GFX_TRIANGLES;
    d.color[0] = d.color[1] = d.color[2] = d.color[3] = 255;
    d.src_factor = MT_GFX_ONE;
    d.dst_factor = MT_GFX_ZERO;
    return d;
}

extern "C" int mt_gfx_submit(mt_gfx_target *target, const mt_gfx_draw *draw) noexcept {
    const auto start = std::chrono::steady_clock::now();
    mt_gfx_counters count{};
    const int result = submit(target, draw, count);
    count.ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count());
    mt_gfx_stats &stats = state().stats;
    add(stats.frame, count);
    add(stats.total, count);
    return result;
}

extern "C" mt_gfx_stats mt_gfx_get_stats(void) noexcept {
    mt_gfx_stats stats = state().stats;
    texture_stats(&stats);
    return stats;
}

extern "C" const char *mt_gfx_backend_name(void) noexcept {
    return backends().current != nullptr ? backends().current->name() : "software";
}

extern "C" void mt_gfx_end_frame(void) noexcept {
    if (Backend *b = backends().current) {
        b->end_frame();
    }
    mt_gfx_stats &stats = state().stats;
    ++stats.frames;
    if (mt_trace_enabled("gfx")) {
        const mt_gfx_counters &f = stats.frame;
        mt_tracef(
            "gfx",
            "frame %llu: %llu draws, %llu triangles, %llu lines, %llu pixels, %llu "
            "unsupported, %.3f ms",
            static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(f.draws),
            static_cast<unsigned long long>(f.triangles), static_cast<unsigned long long>(f.lines),
            static_cast<unsigned long long>(f.pixels),
            static_cast<unsigned long long>(f.unsupported), static_cast<double>(f.ns) / 1e6);
    }
    stats.frame = mt_gfx_counters{};
}

extern "C" void mt_gfx_reset(void) noexcept {
    texture_reset();
    state() = State{};
}
