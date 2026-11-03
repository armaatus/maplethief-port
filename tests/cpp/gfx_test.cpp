// mt_gfx, the rendering backend (#55, ADR 0039, maplethief/gfx.h): targets and their row order,
// coverage (pixel centres, the top-left rule, shared edges drawn exactly once), clipping,
// texture uploads and parameters, nearest and bilinear filtering, wrap modes, colour
// modulation, every blend factor, perspective-correct texture coordinates, the counters and
// the "gfx" trace line, determinism, and golden images of synthetic scenes that must match
// exactly (tolerance 0) on every platform: tests/golden/data/gfx_*.png (MAPLETHIEF_UPDATE_GOLDEN=1
// rewrites them). The prototype scene of `MapleThief --gfx-prototype` (platform/src/
// gfx_prototype.cpp) is one of them.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "gfx/gfx_internal.h"
#include "gfx_prototype.h"
#include "golden.h"
#include "maplethief/diag.h"
#include "maplethief/gfx.h"

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
namespace golden = maplethief::golden;
namespace proto = maplethief::gfx_prototype;

struct TargetDeleter {
    void operator()(mt_gfx_target *t) const { mt_gfx_target_destroy(t); }
};
using Target = std::unique_ptr<mt_gfx_target, TargetDeleter>;

Target make_target(u32 w, u32 h, golden::Rgba fill = {0, 0, 0, 0}, u32 scale = 1) {
    Target t(mt_gfx_target_create(w, h, scale));
    REQUIRE(t != nullptr);
    const u8 rgba[4] = {fill.r, fill.g, fill.b, fill.a};
    mt_gfx_target_fill(t.get(), rgba);
    return t;
}

golden::Image to_image(const mt_gfx_target *t) {
    golden::Image img(static_cast<int>(mt_gfx_target_pixel_width(t)),
                      static_cast<int>(mt_gfx_target_pixel_height(t)));
    std::memcpy(img.bytes().data(), mt_gfx_target_cpixels(t), img.bytes().size());
    return img;
}

// Image pixel (x, y), top row first.
golden::Rgba px(const mt_gfx_target *t, u32 x, u32 y) {
    const u8 *p = mt_gfx_target_cpixels(t) +
                  (static_cast<std::size_t>(y) * mt_gfx_target_pixel_width(t) + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

// GL window position (x, y up) of a w x h viewport at the origin, as a clip-space vertex.
mt_gfx_vertex win(float x, float y, u32 w, u32 h, float s = 0, float t = 0) {
    return {x / static_cast<float>(w) * 2.0f - 1.0f,
            y / static_cast<float>(h) * 2.0f - 1.0f,
            0.0f,
            1.0f,
            s,
            t};
}

mt_gfx_draw draw_for(const mt_gfx_target *t) {
    mt_gfx_draw d = mt_gfx_draw_default();
    d.viewport[2] = static_cast<int32_t>(mt_gfx_target_width(t));
    d.viewport[3] = static_cast<int32_t>(mt_gfx_target_height(t));
    return d;
}

// An axis-aligned rectangle [x0, x1] x [y0, y1] in window coordinates as two triangles, with
// texture coordinates [s0, s1] x [t0, t1].
std::array<mt_gfx_vertex, 6> rect(float x0, float y0, float x1, float y1, u32 w, u32 h,
                                  float s0 = 0, float t0 = 0, float s1 = 1, float t1 = 1) {
    return {win(x0, y0, w, h, s0, t0), win(x1, y0, w, h, s1, t0), win(x1, y1, w, h, s1, t1),
            win(x0, y0, w, h, s0, t0), win(x1, y1, w, h, s1, t1), win(x0, y1, w, h, s0, t1)};
}

int submit_rect(mt_gfx_target *t, float x0, float y0, float x1, float y1, mt_gfx_draw d) {
    const auto v = rect(x0, y0, x1, y1, mt_gfx_target_width(t), mt_gfx_target_height(t));
    d.vertices = v.data();
    d.count = 6;
    return mt_gfx_submit(t, &d);
}

// A draw that adds (1, 1, 1, 1) to every pixel it covers: a pixel drawn twice shows 2.
mt_gfx_draw additive(const mt_gfx_target *t) {
    mt_gfx_draw d = draw_for(t);
    d.color[0] = d.color[1] = d.color[2] = d.color[3] = 1;
    d.blend = 1;
    d.src_factor = MT_GFX_ONE;
    d.dst_factor = MT_GFX_ONE;
    return d;
}

// The number of pixels with each red value, 0..255.
std::array<u32, 256> red_histogram(const mt_gfx_target *t) {
    std::array<u32, 256> h{};
    const u8 *p = mt_gfx_target_cpixels(t);
    const std::size_t n =
        static_cast<std::size_t>(mt_gfx_target_pixel_width(t)) * mt_gfx_target_pixel_height(t);
    for (std::size_t i = 0; i < n; ++i) {
        ++h[p[i * 4]];
    }
    return h;
}

u32 make_texture(u32 w, u32 h, const std::vector<u8> &texels, u32 filter = MT_GFX_NEAREST,
                 u32 wrap = MT_GFX_REPEAT) {
    const u32 tex = mt_gfx_texture_create();
    REQUIRE(tex != 0);
    REQUIRE(mt_gfx_texture_image(tex, w, h, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texels.data()) == 0);
    REQUIRE(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MIN_FILTER, filter) == 0);
    REQUIRE(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MAG_FILTER, filter) == 0);
    REQUIRE(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_WRAP_S, wrap) == 0);
    REQUIRE(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_WRAP_T, wrap) == 0);
    return tex;
}

// A deterministic pseudo-random sequence (no <random>: its distributions differ by library).
struct Lcg {
    u32 state = 12345;
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    float unit() { return static_cast<float>(next() & 0xFFFF) / 65536.0f; } // [0, 1)
};

void check_golden(const golden::Image &img, const char *name) {
    const std::filesystem::path path =
        std::filesystem::path(MT_GOLDEN_DATA_DIR) / (std::string(name) + ".png");
    const auto r = golden::check_golden(img, path, std::string("gfx/") + name); // tolerance 0
    CHECK_MESSAGE(r.passed, r.message);
}

// Every test starts with no textures and zero counters.
struct Fresh {
    Fresh() { mt_gfx_reset(); }
    ~Fresh() { mt_gfx_reset(); }
};

const u32 k_factors[] = {MT_GFX_ZERO,
                         MT_GFX_ONE,
                         MT_GFX_SRC_COLOR,
                         MT_GFX_ONE_MINUS_SRC_COLOR,
                         MT_GFX_SRC_ALPHA,
                         MT_GFX_ONE_MINUS_SRC_ALPHA,
                         MT_GFX_DST_ALPHA,
                         MT_GFX_ONE_MINUS_DST_ALPHA,
                         MT_GFX_DST_COLOR,
                         MT_GFX_ONE_MINUS_DST_COLOR,
                         MT_GFX_SRC_ALPHA_SATURATE};

// The GL ES 1.1 blend equation for one channel, written from the spec's factor table.
u32 reference_factor(u32 f, const u8 s[4], const u8 d[4], int c) {
    switch (f) {
    case MT_GFX_ZERO:
        return 0;
    case MT_GFX_ONE:
        return 255;
    case MT_GFX_SRC_COLOR:
        return s[c];
    case MT_GFX_ONE_MINUS_SRC_COLOR:
        return 255 - s[c];
    case MT_GFX_SRC_ALPHA:
        return s[3];
    case MT_GFX_ONE_MINUS_SRC_ALPHA:
        return 255 - s[3];
    case MT_GFX_DST_ALPHA:
        return d[3];
    case MT_GFX_ONE_MINUS_DST_ALPHA:
        return 255 - d[3];
    case MT_GFX_DST_COLOR:
        return d[c];
    case MT_GFX_ONE_MINUS_DST_COLOR:
        return 255 - d[c];
    default:
        return c == 3 ? 255 : std::min<u32>(s[3], 255 - d[3]); // SRC_ALPHA_SATURATE
    }
}

golden::Rgba reference_blend(const u8 s[4], const u8 d[4], u32 sf, u32 df) {
    u8 out[4];
    for (int c = 0; c < 4; ++c) {
        const u32 v =
            (s[c] * reference_factor(sf, s, d, c) + d[c] * reference_factor(df, s, d, c) + 127) /
            255;
        out[c] = static_cast<u8>(std::min<u32>(v, 255));
    }
    return {out[0], out[1], out[2], out[3]};
}

} // namespace

TEST_CASE("targets start transparent black, are top row first and never cleared") {
    Fresh fresh;
    CHECK(mt_gfx_target_create(0, 4, 1) == nullptr);
    CHECK(mt_gfx_target_create(4, MT_GFX_MAX_SIZE + 1, 1) == nullptr);
    CHECK(mt_gfx_target_create(4, 4, 0) == nullptr);
    CHECK(mt_gfx_target_create(4, 4, MT_GFX_MAX_SCALE + 1) == nullptr);
    CHECK(mt_gfx_target_create(MT_GFX_MAX_SIZE / 2 + 1, 4, 2) == nullptr);
    Target t(mt_gfx_target_create(4, 3, 1));
    REQUIRE(t != nullptr);
    CHECK(mt_gfx_target_width(t.get()) == 4);
    CHECK(mt_gfx_target_height(t.get()) == 3);
    CHECK(mt_gfx_target_scale(t.get()) == 1);
    CHECK(mt_gfx_target_pixel_width(t.get()) == 4);
    CHECK(mt_gfx_target_pixel_height(t.get()) == 3);
    for (u32 y = 0; y < 3; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            CHECK(px(t.get(), x, y) == golden::Rgba{0, 0, 0, 0});
        }
    }
    // Window pixel (0, 0), bottom left in GL, is image pixel (0, height - 1).
    mt_gfx_draw d = draw_for(t.get());
    d.color[0] = 255;
    d.color[1] = d.color[2] = 0;
    CHECK(submit_rect(t.get(), 0, 0, 1, 1, d) == MT_GFX_DRAWN);
    CHECK(px(t.get(), 0, 2) == golden::Rgba{255, 0, 0, 255});
    CHECK(px(t.get(), 0, 0) == golden::Rgba{0, 0, 0, 0});
    // A draw without vertices changes nothing; nothing is cleared between draws or frames.
    mt_gfx_draw empty = draw_for(t.get());
    CHECK(mt_gfx_submit(t.get(), &empty) == MT_GFX_DRAWN);
    mt_gfx_end_frame();
    CHECK(px(t.get(), 0, 2) == golden::Rgba{255, 0, 0, 255});
}

TEST_CASE("coverage: a pixel-aligned rectangle covers exactly its pixels") {
    Fresh fresh;
    auto t = make_target(8, 8);
    CHECK(submit_rect(t.get(), 2, 1, 6, 4, additive(t.get())) == MT_GFX_DRAWN);
    // Window x 2..5, y 1..3 = image rows 4..6.
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const bool inside = x >= 2 && x <= 5 && y >= 4 && y <= 6;
            CHECK_MESSAGE(px(t.get(), x, y).r == (inside ? 1 : 0), "pixel " << x << "," << y);
        }
    }
    CHECK(mt_gfx_get_stats().total.pixels == 12);
}

TEST_CASE("coverage: centres on an edge belong to top and left edges only") {
    Fresh fresh;
    auto t = make_target(6, 6);
    // Edges through pixel centres: window x 1.5 .. 3.5, image y 1.5 .. 3.5 (window y 2.5 .. 4.5).
    CHECK(submit_rect(t.get(), 1.5f, 2.5f, 3.5f, 4.5f, additive(t.get())) == MT_GFX_DRAWN);
    for (u32 y = 0; y < 6; ++y) {
        for (u32 x = 0; x < 6; ++x) {
            // Left edge x = 1.5 owns column 1, the right edge x = 3.5 not column 3; the top
            // edge (image y = 1.5) owns row 1, the bottom edge not row 3.
            const bool inside = (x == 1 || x == 2) && (y == 1 || y == 2);
            CHECK_MESSAGE(px(t.get(), x, y).r == (inside ? 1 : 0), "pixel " << x << "," << y);
        }
    }
    // The same with the triangle's vertices in the other order (orientation does not matter).
    auto t2 = make_target(6, 6);
    const auto v = rect(1.5f, 2.5f, 3.5f, 4.5f, 6, 6);
    const mt_gfx_vertex reversed[6] = {v[2], v[1], v[0], v[5], v[4], v[3]};
    mt_gfx_draw d = additive(t2.get());
    d.vertices = reversed;
    d.count = 6;
    CHECK(mt_gfx_submit(t2.get(), &d) == MT_GFX_DRAWN);
    CHECK(to_image(t2.get()) == to_image(t.get()));
}

TEST_CASE("coverage: shared edges are drawn exactly once") {
    Fresh fresh;
    constexpr u32 w = 64, h = 48;
    Lcg rng;

    SUBCASE("a fan around an off-centre point") {
        auto t = make_target(w, h);
        std::vector<mt_gfx_vertex> fan;
        fan.push_back(win(31.37f, 22.81f, w, h));
        // A closed ring of 40 points on the border of the target, jittered by sub-pixel amounts.
        for (int i = 0; i <= 40; ++i) {
            const int k = i % 40;
            float x, y;
            if (k < 10) {
                x = static_cast<float>(k) * 6.4f, y = 0;
            } else if (k < 20) {
                x = 64, y = static_cast<float>(k - 10) * 4.8f;
            } else if (k < 30) {
                x = 64 - static_cast<float>(k - 20) * 6.4f, y = 48;
            } else {
                x = 0, y = 48 - static_cast<float>(k - 30) * 4.8f;
            }
            fan.push_back(win(x, y, w, h));
        }
        mt_gfx_draw d = additive(t.get());
        d.primitive = MT_GFX_TRIANGLE_FAN;
        d.vertices = fan.data();
        d.count = static_cast<u32>(fan.size());
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        const auto hist = red_histogram(t.get());
        CHECK(hist[1] == w * h); // every pixel once: the ring is the target's border
        CHECK(mt_gfx_get_stats().total.triangles == 40);
    }

    SUBCASE("a jittered mesh of strips") {
        auto t = make_target(w, h);
        // A 9 x 7 grid of points; interior points jittered by up to +-2 pixels.
        constexpr int gx = 9, gy = 7;
        std::vector<mt_gfx_vertex> grid;
        for (int j = 0; j < gy; ++j) {
            for (int i = 0; i < gx; ++i) {
                float x = static_cast<float>(i) * 8.0f, y = static_cast<float>(j) * 8.0f;
                if (i != 0 && i != gx - 1) {
                    x += rng.unit() * 4.0f - 2.0f;
                }
                if (j != 0 && j != gy - 1) {
                    y += rng.unit() * 4.0f - 2.0f;
                }
                grid.push_back(win(x, y, w, h));
            }
        }
        u32 pixels = 0;
        for (int j = 0; j + 1 < gy; ++j) {
            std::vector<mt_gfx_vertex> strip;
            for (int i = 0; i < gx; ++i) {
                strip.push_back(grid[(j + 1) * gx + i]);
                strip.push_back(grid[j * gx + i]);
            }
            mt_gfx_draw d = additive(t.get());
            d.primitive = MT_GFX_TRIANGLE_STRIP;
            d.vertices = strip.data();
            d.count = static_cast<u32>(strip.size());
            CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        }
        const auto hist = red_histogram(t.get());
        pixels = hist[1];
        CHECK(hist[0] == 0); // no cracks inside the mesh (it covers the whole 64 x 48 target)
        CHECK(pixels == w * h);
        CHECK(mt_gfx_get_stats().total.pixels == w * h); // shaded once each
    }

    SUBCASE("a strip, a list and a fan of the same quad cover the same pixels once") {
        const mt_gfx_vertex q[4] = {win(3.2f, 4.1f, w, h), win(60.7f, 2.3f, w, h),
                                    win(5.5f, 44.4f, w, h), win(58.1f, 46.9f, w, h)};
        auto strip = make_target(w, h), list = make_target(w, h), fan = make_target(w, h);
        mt_gfx_draw d = additive(strip.get());
        d.primitive = MT_GFX_TRIANGLE_STRIP;
        d.vertices = q;
        d.count = 4;
        mt_gfx_submit(strip.get(), &d);
        const mt_gfx_vertex l[6] = {q[0], q[1], q[2], q[2], q[1], q[3]};
        d.primitive = MT_GFX_TRIANGLES;
        d.vertices = l;
        d.count = 7; // the 7th vertex does not make a triangle: ignored
        const mt_gfx_vertex l7[7] = {l[0], l[1], l[2], l[3], l[4], l[5], q[0]};
        d.vertices = l7;
        mt_gfx_submit(list.get(), &d);
        const mt_gfx_vertex f[4] = {q[0], q[1], q[3], q[2]};
        d.primitive = MT_GFX_TRIANGLE_FAN;
        d.vertices = f;
        d.count = 4;
        mt_gfx_submit(fan.get(), &d);
        CHECK(to_image(strip.get()) == to_image(list.get()));
        CHECK(to_image(strip.get()) == to_image(fan.get()));
        CHECK(red_histogram(strip.get())[2] == 0);
    }
}

TEST_CASE("clipping against the view volume") {
    Fresh fresh;
    constexpr u32 w = 40, h = 30;

    SUBCASE("a huge triangle fills the viewport exactly, nothing outside it") {
        auto t = make_target(w, h);
        mt_gfx_draw d = additive(t.get());
        d.viewport[0] = 5;
        d.viewport[1] = 4;
        d.viewport[2] = 20;
        d.viewport[3] = 16;
        const mt_gfx_vertex v[3] = {
            {-100, -100, 0, 1, 0, 0}, {300, -100, 0, 1, 0, 0}, {-100, 300, 0, 1, 0, 0}};
        d.vertices = v;
        d.count = 3;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                // Window x 5..24, y 4..19 = image rows 10..25.
                const bool inside = x >= 5 && x < 25 && y >= 10 && y < 26;
                CHECK_MESSAGE(px(t.get(), x, y).r == (inside ? 1 : 0), "pixel " << x << "," << y);
            }
        }
    }

    SUBCASE("inside the guard band a triangle is not clipped, the viewport still bounds it") {
        // Vertices at +-10 (inside the 16 w guard band, ADR 0041): drawn unclipped, and still
        // exactly the sub-viewport's pixels, as clipping to -w <= x, y <= w would leave.
        auto t = make_target(w, h);
        mt_gfx_draw d = additive(t.get());
        d.viewport[0] = 5;
        d.viewport[1] = 4;
        d.viewport[2] = 20;
        d.viewport[3] = 16;
        const mt_gfx_vertex v[3] = {
            {-10, -10, 0, 1, 0, 0}, {10, -10, 0, 1, 0, 0}, {0, 10, 0, 1, 0, 0}};
        d.vertices = v;
        d.count = 3;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        CHECK(mt_gfx_get_stats().total.triangles == 1);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const bool inside = x >= 5 && x < 25 && y >= 10 && y < 26;
                CHECK_MESSAGE(px(t.get(), x, y).r == (inside ? 1 : 0), "pixel " << x << "," << y);
            }
        }
    }

    SUBCASE("a 1:1 GL_LINEAR sprite over the edges samples texel centres up to them (#57)") {
        // A 32 x 32 sprite of distinct texels, half off the target on every side in turn, drawn
        // 1:1 with GL_LINEAR: every visible pixel is its texel exactly. Clipped at the edge, the
        // new vertices were snapped away from where their coordinates were computed, and the
        // sampler mixed in a little of the neighbours (1 or 2 levels on the game's sprites).
        std::vector<u8> texels(32 * 32 * 4);
        for (u32 i = 0; i < 32 * 32; ++i) {
            texels[i * 4 + 0] = static_cast<u8>(i * 37);
            texels[i * 4 + 1] = static_cast<u8>(i * 11 + 5);
            texels[i * 4 + 2] = static_cast<u8>(i ^ 0x5A);
            texels[i * 4 + 3] = 255;
        }
        const u32 tex = make_texture(32, 32, texels, MT_GFX_LINEAR);
        const int offsets[4][2] = {{-13, 3}, {21, -2}, {4, -17}, {-5, 19}};
        for (const auto &o : offsets) {
            auto t = make_target(w, h);
            mt_gfx_draw d = draw_for(t.get());
            d.texture = tex;
            const auto v = rect(static_cast<float>(o[0]), static_cast<float>(o[1]),
                                static_cast<float>(o[0] + 32), static_cast<float>(o[1] + 32), w, h);
            d.vertices = v.data();
            d.count = 6;
            REQUIRE(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
            u32 wrong = 0, seen = 0;
            for (u32 y = 0; y < h; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    const int wy = static_cast<int>(h - 1 - y); // window row
                    const int tx = static_cast<int>(x) - o[0], ty = wy - o[1];
                    if (tx < 0 || tx >= 32 || ty < 0 || ty >= 32) {
                        continue;
                    }
                    const u8 *e = &texels[(static_cast<std::size_t>(ty) * 32 + tx) * 4];
                    seen++;
                    wrong += px(t.get(), x, y) != golden::Rgba{e[0], e[1], e[2], e[3]} ? 1u : 0u;
                }
            }
            INFO("offset " << o[0] << ", " << o[1]);
            CHECK(seen > 100);
            CHECK(wrong == 0);
        }
    }

    SUBCASE("a quad crossing the boundary stays watertight") {
        auto t = make_target(w, h);
        mt_gfx_draw d = additive(t.get());
        const mt_gfx_vertex v[4] = {{-1.7f, -1.3f, 0, 1, 0, 0},
                                    {1.4f, -2.2f, 0, 1, 0, 0},
                                    {-1.1f, 1.9f, 0, 1, 0, 0},
                                    {2.6f, 1.2f, 0, 1, 0, 0}};
        d.primitive = MT_GFX_TRIANGLE_STRIP;
        d.vertices = v;
        d.count = 4;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        const auto hist = red_histogram(t.get());
        CHECK(hist[1] == w * h);
    }

    SUBCASE("z outside [-w, w] and w <= 0 are clipped away") {
        auto t = make_target(w, h);
        mt_gfx_draw d = additive(t.get());
        const mt_gfx_vertex far[3] = {
            {-1, -1, 2, 1, 0, 0}, {1, -1, 2, 1, 0, 0}, {0, 1, 2, 1, 0, 0}};
        d.vertices = far;
        d.count = 3;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        const mt_gfx_vertex behind[3] = {
            {-1, -1, 0, -1, 0, 0}, {1, -1, 0, -1, 0, 0}, {0, 1, 0, -1, 0, 0}};
        d.vertices = behind;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        CHECK(red_histogram(t.get())[0] == w * h);
        CHECK(mt_gfx_get_stats().total.pixels == 0);
        CHECK(mt_gfx_get_stats().total.triangles == 2);
    }

    SUBCASE("a triangle crossing the eye plane draws its visible part once") {
        auto t = make_target(w, h);
        mt_gfx_draw d = additive(t.get());
        // One vertex behind the eye (w < 0): only the part with w > 0 inside the volume shows.
        const mt_gfx_vertex v[3] = {
            {-0.5f, -0.5f, 0, 1, 0, 0}, {0.5f, -0.5f, 0, 1, 0, 0}, {0.0f, 0.8f, 0, -0.5f, 0, 0}};
        d.vertices = v;
        d.count = 3;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        const auto hist = red_histogram(t.get());
        CHECK(hist[1] > 0);
        CHECK(hist[0] + hist[1] == w * h);
    }

    SUBCASE("vertices that are not finite are dropped") {
        auto t = make_target(w, h);
        mt_gfx_draw d = additive(t.get());
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const mt_gfx_vertex v[3] = {
            {-1, -1, 0, 1, 0, 0}, {1, -1, 0, 1, nan, 0}, {0, 1, 0, 1, 0, 0}};
        d.vertices = v;
        d.count = 3;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        CHECK(red_histogram(t.get())[0] == w * h);
    }
}

TEST_CASE("textures: handles, uploads and parameters") {
    Fresh fresh;
    const u32 tex = mt_gfx_texture_create();
    REQUIRE(tex != 0);
    CHECK(mt_gfx_texture_create() != tex);
    u32 tw = 99, th = 99;
    CHECK(mt_gfx_texture_texels(tex, &tw, &th) == nullptr); // no image yet
    CHECK(tw == 0);
    CHECK(th == 0);

    const std::vector<u8> texels = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    CHECK(mt_gfx_texture_image(tex, 3, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texels.data()) == 0);
    const u8 *stored = mt_gfx_texture_texels(tex, &tw, &th);
    REQUIRE(stored != nullptr);
    CHECK(tw == 3);
    CHECK(th == 2);
    CHECK(std::equal(texels.begin(), texels.end(), stored));
    CHECK(mt_gfx_get_stats().textures == 2);
    CHECK(mt_gfx_get_stats().texture_bytes == 24);

    // Pairs GL ES 1.1 does not have, unknown handles and oversized images are refused and
    // change nothing.
    CHECK(mt_gfx_texture_image(tex, 2, 2, MT_GFX_ALPHA, MT_GFX_UNSIGNED_SHORT_4_4_4_4,
                               texels.data()) == -1);
    CHECK(mt_gfx_texture_image(tex, 2, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_SHORT_5_5_5_1,
                               texels.data()) == -1);
    CHECK(mt_gfx_texture_image(tex, 2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_SHORT_5_6_5,
                               texels.data()) == -1);
    CHECK(mt_gfx_texture_image(tex, 2, 2, 0x1234, MT_GFX_UNSIGNED_BYTE, texels.data()) == -1);
    CHECK(mt_gfx_upload_hash(2, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_SHORT_4_4_4_4, texels.data()) == 0);
    CHECK(mt_gfx_texture_image(12345, 2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texels.data()) ==
          -1);
    CHECK(mt_gfx_texture_image(tex, MT_GFX_MAX_SIZE + 1, 1, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE,
                               nullptr) == -1);
    CHECK(mt_gfx_texture_texels(tex, &tw, &th) != nullptr);
    CHECK(tw == 3);

    // NULL pixels: transparent black.
    CHECK(mt_gfx_texture_image(tex, 2, 1, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, nullptr) == 0);
    stored = mt_gfx_texture_texels(tex, &tw, &th);
    REQUIRE(stored != nullptr);
    CHECK(std::all_of(stored, stored + 8, [](u8 b) { return b == 0; }));

    // Parameters: filters (mipmapped ones only for minification) and the two wrap modes.
    CHECK(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MIN_FILTER, MT_GFX_LINEAR_MIPMAP_LINEAR) ==
          0);
    CHECK(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MAG_FILTER, MT_GFX_LINEAR_MIPMAP_LINEAR) ==
          -1);
    CHECK(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MAG_FILTER, MT_GFX_NEAREST) == 0);
    CHECK(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_WRAP_S, MT_GFX_CLAMP_TO_EDGE) == 0);
    CHECK(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_WRAP_T, MT_GFX_NEAREST) == -1);
    CHECK(mt_gfx_texture_parameter(tex, 0x8191 /* GL_GENERATE_MIPMAP */, 1) == -1);
    CHECK(mt_gfx_texture_parameter(9999, MT_GFX_TEXTURE_WRAP_S, MT_GFX_REPEAT) == -1);
    CHECK(mt_gfx::base_filter(MT_GFX_LINEAR_MIPMAP_NEAREST) == MT_GFX_LINEAR);
    CHECK(mt_gfx::base_filter(MT_GFX_NEAREST_MIPMAP_LINEAR) == MT_GFX_NEAREST);

    // Defaults are GL's.
    const u32 fresh_tex = mt_gfx_texture_create();
    const mt_gfx::Texture *def = mt_gfx::find_texture(fresh_tex);
    REQUIRE(def != nullptr);
    CHECK(def->min_filter == MT_GFX_NEAREST_MIPMAP_LINEAR);
    CHECK(def->mag_filter == MT_GFX_LINEAR);
    CHECK(def->wrap_s == MT_GFX_REPEAT);
    CHECK(def->wrap_t == MT_GFX_REPEAT);

    // A 0 x 0 image empties the texture; destroy frees it.
    CHECK(mt_gfx_texture_image(tex, 0, 0, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, nullptr) == 0);
    CHECK(mt_gfx_texture_texels(tex, nullptr, nullptr) == nullptr);
    mt_gfx_texture_destroy(tex);
    mt_gfx_texture_destroy(0);
    CHECK(mt_gfx::find_texture(tex) == nullptr);
    CHECK(mt_gfx_get_stats().textures == 2);
}

// The RGBA8 texels of an upload of `format`/`type` (w x h, rows padded to 4 bytes).
std::vector<u8> converted(u32 w, u32 h, u32 format, u32 type, const std::vector<u8> &src) {
    const u32 tex = mt_gfx_texture_create();
    REQUIRE(mt_gfx_texture_image(tex, w, h, format, type, src.data()) == 0);
    u32 tw = 0, th = 0;
    const u8 *p = mt_gfx_texture_texels(tex, &tw, &th);
    REQUIRE(p != nullptr);
    REQUIRE(tw == w);
    REQUIRE(th == h);
    std::vector<u8> out(p, p + static_cast<std::size_t>(w) * h * 4);
    mt_gfx_texture_destroy(tex);
    return out;
}

// round(c * 255 / max), from the spec in double.
u8 widened(u32 c, u32 max) {
    return static_cast<u8>(std::floor(static_cast<double>(c) * 255.0 / max + 0.5));
}

TEST_CASE("formats: every GL ES 1.1 format/type pair, as GL_MODULATE reads it (#57)") {
    Fresh fresh;
    // RGB, 3 x 2: rows of 9 bytes padded to 12 (GL_UNPACK_ALIGNMENT 4); the pad bytes (0xEE)
    // are skipped. Alpha 255.
    const std::vector<u8> rgb = {1,  2,  3,  4,  5,  6,  7,  8,  9,  0xEE, 0xEE, 0xEE,
                                 10, 11, 12, 13, 14, 15, 16, 17, 18, 0xEE, 0xEE, 0xEE};
    CHECK(converted(3, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_BYTE, rgb) ==
          std::vector<u8>{1,  2,  3,  255, 4,  5,  6,  255, 7,  8,  9,  255,
                          10, 11, 12, 255, 13, 14, 15, 255, 16, 17, 18, 255});
    // ALPHA (1, 1, 1, A): modulated, the fragment's colour is the vertex colour (C = Cf).
    CHECK(converted(3, 2, MT_GFX_ALPHA, MT_GFX_UNSIGNED_BYTE, {0, 128, 255, 0xEE, 7, 8, 9, 0xEE}) ==
          std::vector<u8>{255, 255, 255, 0, 255, 255, 255, 128, 255, 255, 255, 255,
                          255, 255, 255, 7, 255, 255, 255, 8,   255, 255, 255, 9});
    // LUMINANCE (L, L, L, 1), LUMINANCE_ALPHA (L, L, L, A): rows of 5 and 2 x 3 = 6 bytes.
    CHECK(converted(5, 1, MT_GFX_LUMINANCE, MT_GFX_UNSIGNED_BYTE, {0, 1, 127, 254, 255}) ==
          std::vector<u8>{0,   0,   0,   255, 1,   1,   1,   255, 127, 127,
                          127, 255, 254, 254, 254, 255, 255, 255, 255, 255});
    CHECK(converted(3, 2, MT_GFX_LUMINANCE_ALPHA, MT_GFX_UNSIGNED_BYTE,
                    {10, 20, 30, 40, 50, 60, 0xEE, 0xEE, 1, 2, 3, 4, 5, 6, 0xEE, 0xEE}) ==
          std::vector<u8>{10, 10, 10, 20, 30, 30, 30, 40, 50, 50, 50, 60,
                          1,  1,  1,  2,  3,  3,  3,  4,  5,  5,  5,  6});

    // The packed types: 16-bit words, least significant byte first (the guest's order), rows of
    // 3 x 2 = 6 bytes padded to 8. Hand-computed: 0xF800 red, 0x07E0 green, 0x001F blue,
    // 0x8410 = (16, 32, 16) -> (132, 130, 132) (16 * 255 / 31 = 131.6, 32 * 255 / 63 = 129.5).
    CHECK(converted(3, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_SHORT_5_6_5,
                    {0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0xEE, 0xEE, 0x10, 0x84, 0xFF, 0xFF, 0x00,
                     0x00, 0xEE, 0xEE}) == std::vector<u8>{255, 0,   0,   255, 0,   255, 0,   255,
                                                           0,   0,   255, 255, 132, 130, 132, 255,
                                                           255, 255, 255, 255, 0,   0,   0,   255});
    // 0x1234 in 4444 is (1, 2, 3, 4) * 17; 5551's last bit is the alpha.
    CHECK(converted(2, 1, MT_GFX_RGBA, MT_GFX_UNSIGNED_SHORT_4_4_4_4, {0x34, 0x12, 0x0F, 0xF0}) ==
          std::vector<u8>{17, 34, 51, 68, 255, 0, 0, 255});
    CHECK(converted(2, 1, MT_GFX_RGBA, MT_GFX_UNSIGNED_SHORT_5_5_5_1, {0x01, 0x00, 0xFE, 0xFF}) ==
          std::vector<u8>{0, 0, 0, 255, 255, 255, 255, 0});

    // Every 16-bit word of each packed type against round(c * 255 / (2^n - 1)).
    std::vector<u8> words(65536 * 2);
    for (u32 v = 0; v < 65536; v++) {
        words[v * 2] = static_cast<u8>(v);
        words[v * 2 + 1] = static_cast<u8>(v >> 8);
    }
    const std::vector<u8> c565 =
        converted(256, 256, MT_GFX_RGB, MT_GFX_UNSIGNED_SHORT_5_6_5, words);
    const std::vector<u8> c4444 =
        converted(256, 256, MT_GFX_RGBA, MT_GFX_UNSIGNED_SHORT_4_4_4_4, words);
    const std::vector<u8> c5551 =
        converted(256, 256, MT_GFX_RGBA, MT_GFX_UNSIGNED_SHORT_5_5_5_1, words);
    u32 wrong = 0;
    for (u32 v = 0; v < 65536; v++) {
        const u8 *a = &c565[v * 4], *b = &c4444[v * 4], *c = &c5551[v * 4];
        wrong += a[0] != widened(v >> 11, 31) || a[1] != widened((v >> 5) & 63, 63) ||
                 a[2] != widened(v & 31, 31) || a[3] != 255;
        wrong += b[0] != widened(v >> 12, 15) || b[1] != widened((v >> 8) & 15, 15) ||
                 b[2] != widened((v >> 4) & 15, 15) || b[3] != widened(v & 15, 15);
        wrong += c[0] != widened(v >> 11, 31) || c[1] != widened((v >> 6) & 31, 31) ||
                 c[2] != widened((v >> 1) & 31, 31) || c[3] != ((v & 1) != 0 ? 255 : 0);
    }
    CHECK(wrong == 0);

    // RGBA / UNSIGNED_BYTE stays byte for byte, premultiplied or not (ADR 0041).
    const std::vector<u8> premultiplied = {64, 32, 0, 128, 255, 255, 255, 255};
    CHECK(converted(2, 1, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, premultiplied) == premultiplied);

    // The content hash covers the bytes GL reads, without the rows' padding.
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (u32 y = 0; y < 2; y++) {
        for (u32 i = 0; i < 9; i++) {
            h = (h ^ rgb[y * 12 + i]) * 0x00000100000001B3ull;
        }
    }
    CHECK(mt_gfx_upload_hash(3, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_BYTE, rgb.data()) == h);

    // Drawn with GL_MODULATE: ALPHA gives the colour with the texel's alpha, LUMINANCE scales it.
    auto t = make_target(2, 1);
    const u32 alpha = mt_gfx_texture_create();
    const std::vector<u8> a8 = {128, 64, 0, 0};
    REQUIRE(mt_gfx_texture_image(alpha, 2, 1, MT_GFX_ALPHA, MT_GFX_UNSIGNED_BYTE, a8.data()) == 0);
    REQUIRE(mt_gfx_texture_parameter(alpha, MT_GFX_TEXTURE_MIN_FILTER, MT_GFX_NEAREST) == 0);
    REQUIRE(mt_gfx_texture_parameter(alpha, MT_GFX_TEXTURE_MAG_FILTER, MT_GFX_NEAREST) == 0);
    mt_gfx_draw d = draw_for(t.get());
    d.texture = alpha;
    d.color[0] = 10;
    d.color[1] = 20;
    d.color[2] = 30;
    d.color[3] = 200;
    CHECK(submit_rect(t.get(), 0, 0, 2, 1, d) == MT_GFX_DRAWN);
    CHECK(px(t.get(), 0, 0) == golden::Rgba{10, 20, 30, 100}); // 200 * 128 / 255 = 100.4
    CHECK(px(t.get(), 1, 0) == golden::Rgba{10, 20, 30, 50});  // 200 * 64 / 255 = 50.2
    const u32 lum = mt_gfx_texture_create();
    REQUIRE(mt_gfx_texture_image(lum, 2, 1, MT_GFX_LUMINANCE, MT_GFX_UNSIGNED_BYTE, a8.data()) ==
            0);
    REQUIRE(mt_gfx_texture_parameter(lum, MT_GFX_TEXTURE_MIN_FILTER, MT_GFX_NEAREST) == 0);
    d.texture = lum;
    CHECK(submit_rect(t.get(), 0, 0, 2, 1, d) == MT_GFX_DRAWN);
    CHECK(px(t.get(), 0, 0) == golden::Rgba{5, 10, 15, 200}); // 10 * 128 / 255 = 5.02, ...
    CHECK(px(t.get(), 1, 0) == golden::Rgba{3, 5, 8, 200});   // 10 * 64 / 255 = 2.5, 7.5
}

TEST_CASE("texturing: an empty texture draws the colour, an unknown one is invalid") {
    Fresh fresh;
    auto t = make_target(4, 4);
    const u32 tex = mt_gfx_texture_create();
    mt_gfx_draw d = draw_for(t.get());
    d.texture = tex;
    d.color[0] = 10;
    d.color[1] = 20;
    d.color[2] = 30;
    d.color[3] = 40;
    CHECK(submit_rect(t.get(), 0, 0, 4, 4, d) == MT_GFX_DRAWN);
    CHECK(px(t.get(), 1, 1) == golden::Rgba{10, 20, 30, 40});
    d.texture = tex + 100;
    CHECK(submit_rect(t.get(), 0, 0, 4, 4, d) == MT_GFX_INVALID);
}

TEST_CASE("filtering: nearest and bilinear with 8-bit weights") {
    Fresh fresh;
    // A 2 x 1 texture, black then white, stretched over 8 x 1 pixels: each texel spans 4.
    const std::vector<u8> texels = {0, 0, 0, 255, 255, 255, 255, 255};
    auto red_row = [](const mt_gfx_target *t) {
        std::vector<int> r;
        for (u32 x = 0; x < 8; ++x) {
            r.push_back(px(t, x, 0).r);
        }
        return r;
    };
    auto draw = [&](u32 filter, u32 wrap, float s0, float s1) {
        auto t = make_target(8, 1);
        const u32 tex = make_texture(2, 1, texels, filter, wrap);
        mt_gfx_draw d = draw_for(t.get());
        d.texture = tex;
        const auto v = rect(0, 0, 8, 1, 8, 1, s0, 0, s1, 1);
        d.vertices = v.data();
        d.count = 6;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        return red_row(t.get());
    };
    CHECK(draw(MT_GFX_NEAREST, MT_GFX_CLAMP_TO_EDGE, 0, 1) ==
          std::vector<int>{0, 0, 0, 0, 255, 255, 255, 255});
    // Linear: pixel x samples u = (x + 1/2) / 4 - 1/2 texels; the weight of the right texel is
    // floor(256 u) & 255, the result (255 * weight * 256 + 32768) >> 16.
    CHECK(draw(MT_GFX_LINEAR, MT_GFX_CLAMP_TO_EDGE, 0, 1) ==
          std::vector<int>{0, 0, 32, 96, 159, 223, 255, 255});
    // REPEAT: the left neighbour of texel 0 is texel 1.
    CHECK(draw(MT_GFX_LINEAR, MT_GFX_REPEAT, 0, 1) ==
          std::vector<int>{96, 32, 32, 96, 159, 223, 223, 159});

    SUBCASE("magnification and minification filters are chosen by the triangle's scale") {
        // 4 x 1 texels with mag NEAREST, min LINEAR: drawn over 8 pixels (2 pixels a texel) it
        // magnifies, over 2 pixels it minifies.
        const std::vector<u8> ramp = {0, 0, 0, 255, 100, 0, 0, 255, 200, 0, 0, 255, 250, 0, 0, 255};
        const u32 tex = make_texture(4, 1, ramp, MT_GFX_NEAREST, MT_GFX_CLAMP_TO_EDGE);
        REQUIRE(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MIN_FILTER,
                                         MT_GFX_LINEAR_MIPMAP_LINEAR) == 0);
        auto big = make_target(8, 1);
        mt_gfx_draw d = draw_for(big.get());
        d.texture = tex;
        auto v = rect(0, 0, 8, 1, 8, 1);
        d.vertices = v.data();
        d.count = 6;
        mt_gfx_submit(big.get(), &d);
        CHECK(red_row(big.get()) ==
              std::vector<int>{0, 0, 100, 100, 200, 200, 250, 250}); // nearest
        auto small = make_target(2, 1);
        d = draw_for(small.get());
        d.texture = tex;
        v = rect(0, 0, 2, 1, 2, 1);
        d.vertices = v.data();
        d.count = 6;
        mt_gfx_submit(small.get(), &d);
        // Linear at u = 1 and u = 3: the means of texels 0/1 and 2/3, rounded.
        CHECK(px(small.get(), 0, 0).r == 50);
        CHECK(px(small.get(), 1, 0).r == 225);
    }
}

TEST_CASE("wrap: repeat and clamp to edge") {
    Fresh fresh;
    const std::vector<u8> texels = {0, 0, 0, 255, 255, 255, 255, 255};
    for (const u32 wrap : {MT_GFX_REPEAT, MT_GFX_CLAMP_TO_EDGE}) {
        auto t = make_target(8, 1);
        const u32 tex = make_texture(2, 1, texels, MT_GFX_NEAREST, wrap);
        mt_gfx_draw d = draw_for(t.get());
        d.texture = tex;
        const auto v = rect(0, 0, 8, 1, 8, 1, -1, 0, 3, 1); // s from -1 to 3: two texels a pixel
        d.vertices = v.data();
        d.count = 6;
        mt_gfx_submit(t.get(), &d);
        std::vector<int> row;
        for (u32 x = 0; x < 8; ++x) {
            row.push_back(px(t.get(), x, 0).r);
        }
        // Pixel x samples s = (x + 1/2) / 2 - 1, u = 2 s texels: -1.5, -0.5, 0.5, 1.5, ...
        if (wrap == MT_GFX_REPEAT) {
            CHECK(row == std::vector<int>{0, 255, 0, 255, 0, 255, 0, 255});
        } else {
            CHECK(row == std::vector<int>{0, 0, 0, 255, 255, 255, 255, 255});
        }
    }
}

TEST_CASE("modulate: texel times colour, (a * b + 127) / 255") {
    Fresh fresh;
    auto t = make_target(2, 2);
    const u32 tex = make_texture(1, 1, {200, 100, 50, 255});
    mt_gfx_draw d = draw_for(t.get());
    d.texture = tex;
    d.color[0] = 128;
    d.color[1] = 255;
    d.color[2] = 0;
    d.color[3] = 64;
    CHECK(submit_rect(t.get(), 0, 0, 2, 2, d) == MT_GFX_DRAWN);
    // 200 * 128 = 25600 -> (25600 + 127) / 255 = 100; 100 * 255 -> 100; 0; 255 * 64 -> 64.
    CHECK(px(t.get(), 0, 0) == golden::Rgba{100, 100, 0, 64});
    // Exhaustive against the formula for one channel pair.
    for (u32 a = 0; a < 256; a += 5) {
        for (u32 b = 0; b < 256; b += 3) {
            const u32 one = mt_gfx_texture_create();
            const u8 texel[4] = {static_cast<u8>(a), 0, 0, 255};
            mt_gfx_texture_image(one, 1, 1, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texel);
            d.texture = one;
            d.color[0] = static_cast<u8>(b);
            submit_rect(t.get(), 0, 0, 2, 2, d);
            mt_gfx_texture_destroy(one);
            if (px(t.get(), 1, 1).r != (a * b + 127) / 255) {
                FAIL("modulate " << a << " x " << b << " = " << int(px(t.get(), 1, 1).r));
            }
        }
    }
}

TEST_CASE("blending: every factor, saturated") {
    Fresh fresh;
    // A few source/destination pairs, including saturation and the extremes.
    const u8 pairs[][2][4] = {
        {{200, 100, 0, 128}, {0, 0, 255, 255}},
        {{255, 255, 255, 255}, {255, 255, 255, 255}},
        {{17, 99, 230, 3}, {250, 1, 128, 200}},
        {{0, 0, 0, 0}, {77, 154, 231, 0}},
    };
    auto t = make_target(1, 1);
    u32 checked = 0;
    for (const auto &pair : pairs) {
        for (const u32 sf : k_factors) {
            for (const u32 df : k_factors) {
                const u8 *s = pair[0], *dst = pair[1];
                mt_gfx_target_fill(t.get(), dst);
                mt_gfx_draw d = draw_for(t.get());
                std::memcpy(d.color, s, 4);
                d.blend = 1;
                d.src_factor = sf;
                d.dst_factor = df;
                REQUIRE(submit_rect(t.get(), 0, 0, 1, 1, d) == MT_GFX_DRAWN);
                const golden::Rgba want = reference_blend(s, dst, sf, df);
                CHECK_MESSAGE(px(t.get(), 0, 0) == want,
                              "factors 0x" << std::hex << sf << " 0x" << df);
                ++checked;
            }
        }
    }
    CHECK(checked == 4 * 11 * 11);

    // Spot checks by hand: SRC_ALPHA / ONE_MINUS_SRC_ALPHA of (200, 100, 0, 128) over opaque
    // blue: R (200 * 128 + 127) / 255 = 100, B (255 * 127 + 127) / 255 = 127,
    // A (128 * 128 + 255 * 127 + 127) / 255 = 191.
    const u8 s[4] = {200, 100, 0, 128}, dst[4] = {0, 0, 255, 255};
    u8 out[4];
    mt_gfx::blend_pixel(s, dst, MT_GFX_SRC_ALPHA, MT_GFX_ONE_MINUS_SRC_ALPHA, out);
    CHECK(golden::Rgba{out[0], out[1], out[2], out[3]} == golden::Rgba{100, 50, 127, 191});
    // ONE / ONE saturates.
    mt_gfx::blend_pixel(s, dst, MT_GFX_ONE, MT_GFX_ONE, out);
    CHECK(golden::Rgba{out[0], out[1], out[2], out[3]} == golden::Rgba{200, 100, 255, 255});
    // SRC_ALPHA_SATURATE: min(As, 1 - Ad) for colour, 1 for alpha.
    const u8 d2[4] = {0, 0, 0, 200};
    mt_gfx::blend_pixel(s, d2, MT_GFX_SRC_ALPHA_SATURATE, MT_GFX_ZERO, out);
    CHECK(golden::Rgba{out[0], out[1], out[2], out[3]} == golden::Rgba{43, 22, 0, 128});

    // Unknown factors are invalid draws.
    mt_gfx_draw d = draw_for(t.get());
    d.blend = 1;
    d.src_factor = 0x0309;
    CHECK(submit_rect(t.get(), 0, 0, 1, 1, d) == MT_GFX_INVALID);
    CHECK(mt_gfx_get_stats().total.invalid == 1);
}

TEST_CASE("texture coordinates are perspective-correct") {
    Fresh fresh;
    // Four texels in four colours over 8 x 1 pixels; the right edge has w = 3. At pixel 4
    // (screen weight 0.5625 towards the right) perspective gives s = (0.5625 / 3) /
    // (0.4375 + 0.5625 / 3) = 0.3, texel 1; affine interpolation would give texel 2.
    const std::vector<u8> texels = {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255};
    const u32 tex = make_texture(4, 1, texels, MT_GFX_NEAREST, MT_GFX_CLAMP_TO_EDGE);
    auto t = make_target(8, 1);
    const mt_gfx_vertex v[4] = {
        {-1, -1, 0, 1, 0, 0}, {3, -3, 0, 3, 1, 0}, {-1, 1, 0, 1, 0, 1}, {3, 3, 0, 3, 1, 1}};
    mt_gfx_draw d = draw_for(t.get());
    d.primitive = MT_GFX_TRIANGLE_STRIP;
    d.vertices = v;
    d.count = 4;
    d.texture = tex;
    CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
    CHECK(px(t.get(), 4, 0).r == 20);
    std::vector<int> row;
    for (u32 x = 0; x < 8; ++x) {
        row.push_back(px(t.get(), x, 0).r);
    }
    // s at pixel x: ((x + 0.5) / 8 / 3) / ((1 - (x + 0.5) / 8) + (x + 0.5) / 8 / 3).
    CHECK(row == std::vector<int>{10, 10, 10, 10, 20, 20, 30, 40});
}

TEST_CASE("points are counted and logged, not drawn, and invalid draws rejected") {
    Fresh fresh;
    std::vector<std::string> lines;
    mt_diag_config cfg = mt_diag_config_default();
    cfg.log = [](void *user, const char *line) {
        static_cast<std::vector<std::string> *>(user)->emplace_back(line);
    };
    cfg.log_user = &lines;
    mt_diag_configure(&cfg);

    auto t = make_target(4, 4);
    const mt_gfx_vertex v[3] = {{0, 0, 0, 1, 0, 0}, {1, 0, 0, 1, 0, 0}, {0, 1, 0, 1, 0, 0}};
    mt_gfx_draw d = draw_for(t.get());
    d.vertices = v;
    d.count = 3;
    for (int i = 0; i < 3; ++i) {
        d.primitive = MT_GFX_POINTS;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_UNSUPPORTED);
    }
    CHECK(mt_gfx_get_stats().total.unsupported == 3);
    CHECK(red_histogram(t.get())[0] == 16);
    REQUIRE(lines.size() == 1); // once
    CHECK(lines[0] == "gfx: GL_POINTS draws are not supported: counted, not drawn (logged once)");
    // lines are drawn (#121)
    for (const u32 prim : {MT_GFX_LINES, MT_GFX_LINE_LOOP, MT_GFX_LINE_STRIP}) {
        d.primitive = prim;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
    }
    CHECK(mt_gfx_get_stats().total.unsupported == 3);
    CHECK(mt_gfx_get_stats().total.lines == 1 + 3 + 2);

    d.primitive = 7; // GL_QUADS does not exist in GL ES
    CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_INVALID);
    d.primitive = MT_GFX_TRIANGLES;
    d.vertices = nullptr;
    CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_INVALID);
    CHECK(mt_gfx_submit(nullptr, &d) == MT_GFX_INVALID);
    CHECK(mt_gfx_submit(t.get(), nullptr) == MT_GFX_INVALID);
    CHECK(mt_gfx_get_stats().total.invalid == 4);
    CHECK(lines.size() == 5);
    CHECK(lines[1] == "gfx: invalid draw: unknown primitive");
    mt_diag_reset();
}

TEST_CASE("counters and the gfx trace line per frame") {
    Fresh fresh;
    std::vector<std::string> lines;
    mt_diag_config cfg = mt_diag_config_default();
    cfg.trace = "gfx";
    cfg.log = [](void *user, const char *line) {
        static_cast<std::vector<std::string> *>(user)->emplace_back(line);
    };
    cfg.log_user = &lines;
    mt_diag_configure(&cfg);

    auto t = make_target(8, 8);
    CHECK(submit_rect(t.get(), 0, 0, 4, 4, draw_for(t.get())) == MT_GFX_DRAWN);
    mt_gfx_stats st = mt_gfx_get_stats();
    CHECK(st.frame.draws == 1);
    CHECK(st.frame.triangles == 2);
    CHECK(st.frame.pixels == 16);
    mt_gfx_end_frame();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].rfind("gfx: frame 1: 1 draws, 2 triangles, 0 lines, 16 pixels, 0 unsupported, ",
                         0) == 0);
    CHECK(lines[0].find(" ms") != std::string::npos);
    st = mt_gfx_get_stats();
    CHECK(st.frames == 1);
    CHECK(st.frame.draws == 0);
    CHECK(st.total.draws == 1);
    CHECK(st.total.pixels == 16);

    // Without the channel, no line.
    mt_diag_reset();
    mt_gfx_end_frame();
    CHECK(lines.size() == 1);
    CHECK(mt_gfx_get_stats().frames == 2);
}

// --- lines (#121): GL ES 1.1 section 3.4.1, width 1 -----------------------------------------

namespace {

// A window pixel (x, y up).
struct Px {
    int x = 0, y = 0;
    bool operator==(const Px &o) const { return x == o.x && y == o.y; }
    bool operator<(const Px &o) const { return x != o.x ? x < o.x : y < o.y; }
};
using Pixels = std::vector<Px>; // sorted

doctest::String toString(const Pixels &p) {
    std::string s = "{";
    for (const Px &q : p) {
        s += "(" + std::to_string(q.x) + "," + std::to_string(q.y) + ")";
    }
    return (s + "}").c_str();
}

// The window pixels a target's red channel has drawn (additive draws: red = times drawn), and
// how many of them were drawn more than once.
Pixels drawn_pixels(const mt_gfx_target *t, u32 *twice = nullptr) {
    Pixels out;
    u32 more = 0;
    const u32 w = mt_gfx_target_pixel_width(t), h = mt_gfx_target_pixel_height(t);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u8 r = px(t, x, y).r;
            if (r != 0) {
                out.push_back({static_cast<int>(x), static_cast<int>(h - 1 - y)});
                more += r > 1 ? 1u : 0u;
            }
        }
    }
    std::sort(out.begin(), out.end());
    if (twice != nullptr) {
        *twice = more;
    }
    return out;
}

// Draws `xy` (window coordinates in pixels, y up; multiples of 1/16 stay exact) as `prim`
// with additive blending onto a fresh w x h target; returns the pixels drawn.
Pixels line_pixels(u32 prim, const std::vector<float> &xy, u32 *twice = nullptr, u32 w = 16,
                   u32 h = 16) {
    auto t = make_target(w, h);
    std::vector<mt_gfx_vertex> v;
    for (std::size_t i = 0; i + 1 < xy.size(); i += 2) {
        v.push_back(win(xy[i], xy[i + 1], w, h));
    }
    mt_gfx_draw d = additive(t.get());
    d.primitive = prim;
    d.vertices = v.data();
    d.count = static_cast<u32>(v.size());
    REQUIRE(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
    return drawn_pixels(t.get(), twice);
}

Pixels segment(float ax, float ay, float bx, float by) {
    return line_pixels(MT_GFX_LINES, {ax, ay, bx, by});
}

// The rule as section 3.4.1 states it, written out independently of mt_gfx: the endpoints are
// moved by (-e, -e^2), e = 2^-20 (both shifts exact in double for the coordinates used here);
// a pixel is drawn when the moved segment comes closer than 1/2 (L1) to its centre, unless the
// moved end point is that close. The L1 distance along the segment is convex and piecewise
// linear, so its minimum is at an end or where x or y passes the centre. Ties in these tests are
// at least 2^-12 pixel apart, far above e and the rounding of double.
Pixels reference_line(double ax, double ay, double bx, double by, int w, int h) {
    const double e = 1.0 / 1048576.0;
    ax -= e;
    bx -= e;
    ay -= e * e;
    by -= e * e;
    const double dx = bx - ax, dy = by - ay;
    Pixels out;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const double cx = i + 0.5, cy = j + 0.5;
            if (std::fabs(bx - cx) + std::fabs(by - cy) < 0.5) {
                continue;
            }
            double ts[4] = {0.0, 1.0, 0.0, 0.0};
            int n = 2;
            if (dx != 0.0) {
                ts[n++] = std::clamp((cx - ax) / dx, 0.0, 1.0);
            }
            if (dy != 0.0) {
                ts[n++] = std::clamp((cy - ay) / dy, 0.0, 1.0);
            }
            double best = 1e30;
            for (int k = 0; k < n; ++k) {
                const double x = ax + ts[k] * dx, y = ay + ts[k] * dy;
                best = std::min(best, std::fabs(x - cx) + std::fabs(y - cy));
            }
            if (best < 0.5) {
                out.push_back({i, j});
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

Pixels row(int x0, int x1, int y) {
    Pixels p;
    for (int x = x0; x <= x1; ++x) {
        p.push_back({x, y});
    }
    return p;
}

Pixels column(int x, int y0, int y1) {
    Pixels p;
    for (int y = y0; y <= y1; ++y) {
        p.push_back({x, y});
    }
    return p;
}

Pixels merged(std::initializer_list<Pixels> parts) {
    Pixels p;
    for (const Pixels &q : parts) {
        p.insert(p.end(), q.begin(), q.end());
    }
    std::sort(p.begin(), p.end());
    return p;
}

} // namespace

TEST_CASE("lines: the diamond-exit rule by hand") {
    Fresh fresh;
    struct Case {
        const char *what;
        float ax, ay, bx, by;
        Pixels want;
    };
    const Case cases[] = {
        // pixel centre to pixel centre: Bresenham, without the last pixel
        {"horizontal, right", 0.5f, 0.5f, 4.5f, 0.5f, row(0, 3, 0)},
        {"horizontal, left", 4.5f, 0.5f, 0.5f, 0.5f, row(1, 4, 0)},
        {"vertical, up", 0.5f, 0.5f, 0.5f, 4.5f, column(0, 0, 3)},
        {"vertical, down", 0.5f, 4.5f, 0.5f, 0.5f, column(0, 1, 4)},
        {"diagonal", 0.5f, 0.5f, 4.5f, 4.5f, {{0, 0}, {1, 1}, {2, 2}, {3, 3}}},
        {"anti-diagonal", 0.5f, 4.5f, 4.5f, 0.5f, {{0, 4}, {1, 3}, {2, 2}, {3, 1}}},
        {"x-major 1/3", 0.5f, 0.5f, 6.5f, 2.5f, {{0, 0}, {1, 0}, {2, 1}, {3, 1}, {4, 1}, {5, 2}}},
        // halfway between two rows at x = 1.5 and 3.5: moving the line by -e in x lifts it
        // (slope 1/2), so the upper row; the same pixels whichever way the line runs
        {"x-major ties, right", 0.5f, 0.5f, 4.5f, 2.5f, {{0, 0}, {1, 1}, {2, 1}, {3, 2}}},
        {"x-major ties, left", 4.5f, 2.5f, 0.5f, 0.5f, {{1, 1}, {2, 1}, {3, 2}, {4, 2}}},
        // halfway between two columns at y = 1.5 and 3.5: -e in x takes the left one
        {"y-major ties", 0.5f, 0.5f, 2.5f, 4.5f, {{0, 0}, {0, 1}, {1, 2}, {1, 3}}},
        // along a row boundary (y = 1): -e^2 in y puts it in the row below
        {"on a row boundary", 0.5f, 1.0f, 4.5f, 1.0f, row(0, 3, 0)},
        // along a column boundary (x = 1): -e in x puts it in the column left of it
        {"on a column boundary, up", 1.0f, 0.5f, 1.0f, 4.5f, column(0, 0, 3)},
        {"on a column boundary, down", 1.0f, 4.5f, 1.0f, 0.5f, column(0, 1, 4)},
        // pixel corners to pixel corners (the game's integer GL_SHORT vertices)
        {"corner to corner, vertical", 2.0f, 0.0f, 2.0f, 3.0f, column(1, 0, 2)},
        {"corner to corner, diagonal", 0.0f, 0.0f, 4.0f, 4.0f, {{0, 0}, {1, 1}, {2, 2}, {3, 3}}},
        {"corner to corner, horizontal", 2.0f, 3.0f, 6.0f, 3.0f, row(2, 5, 2)},
        // endpoints exactly on diamonds' corners: a point on the left and right corners of
        // two diamonds belongs, moved by -e, to the left one
        {"on left/right diamond corners, right", 1.0f, 0.5f, 4.0f, 0.5f, row(0, 2, 0)},
        {"on left/right diamond corners, left", 4.0f, 0.5f, 1.0f, 0.5f, row(1, 3, 0)},
        // on bottom/top corners: moved by -e, in the gap between the two diamonds
        {"on bottom/top diamond corners, up", 0.5f, 1.0f, 0.5f, 4.0f, column(0, 1, 3)},
        {"on bottom/top diamond corners, down", 0.5f, 4.0f, 0.5f, 1.0f, column(0, 1, 3)},
        // on a diamond's slanted edge, (0.75, 0.75) is on pixel (0, 0)'s: moved by -e, inside
        {"start on a diamond edge", 0.75f, 0.75f, 4.75f, 0.75f, row(0, 3, 0)},
        {"end on a diamond edge", 4.75f, 0.75f, 0.75f, 0.75f, row(1, 4, 0)},
        // both ends in one diamond: nothing (half-open); one end in the next: one pixel
        {"inside one diamond", 0.25f, 0.5f, 0.75f, 0.5f, {}},
        {"into the next diamond", 0.625f, 0.5f, 1.375f, 0.5f, {{0, 0}}},
        {"zero length", 3.5f, 3.5f, 3.5f, 3.5f, {}},
    };
    for (const Case &c : cases) {
        CAPTURE(c.what);
        CHECK(segment(c.ax, c.ay, c.bx, c.by) == c.want);
        CHECK(reference_line(c.ax, c.ay, c.bx, c.by, 16, 16) == c.want);
    }
}

TEST_CASE("lines: random segments on the 1/16 grid match the rule as the spec states it") {
    Fresh fresh;
    Lcg rng;
    // Endpoints on the 1/16 grid, or (to hit every tie) on the half-pixel grid, some outside
    // the 32 x 32 target (the guard band keeps them, the viewport bounds the pixels).
    auto coord = [&](bool coarse) {
        const int steps = coarse ? 2 : 16;
        const int v = static_cast<int>(rng.next() % static_cast<u32>(40 * steps)) - 4 * steps;
        return static_cast<float>(v) / static_cast<float>(steps);
    };
    u32 mismatches = 0, pixels = 0;
    for (int n = 0; n < 3000; ++n) {
        const bool coarse = n % 2 == 0;
        float ax = coord(coarse), ay = coord(coarse), bx = coord(coarse), by = coord(coarse);
        if (n % 3 == 0) { // short lines, all directions
            bx = ax + static_cast<float>(static_cast<int>(rng.next() % 97u) - 48) / 16.0f;
            by = ay + static_cast<float>(static_cast<int>(rng.next() % 97u) - 48) / 16.0f;
        }
        const Pixels got = line_pixels(MT_GFX_LINES, {ax, ay, bx, by}, nullptr, 32, 32);
        const Pixels want = reference_line(ax, ay, bx, by, 32, 32);
        pixels += static_cast<u32>(got.size());
        if (got != want) {
            ++mismatches;
            if (mismatches <= 5) {
                MESSAGE("(" << ax << ", " << ay << ") -> (" << bx << ", " << by << ")");
                CHECK(got == want);
            }
        }
    }
    CHECK(mismatches == 0);
    CHECK(pixels > 30000);
}

TEST_CASE("lines: strips and loops draw each joint pixel once") {
    Fresh fresh;
    u32 twice = 99;
    // the outline of a 6 x 6 square through pixel centres
    const std::vector<float> square = {0.5f, 0.5f, 5.5f, 0.5f, 5.5f, 5.5f, 0.5f, 5.5f};
    CHECK(line_pixels(MT_GFX_LINE_STRIP, square, &twice) ==
          merged({row(0, 4, 0), column(5, 0, 4), row(1, 5, 5)}));
    CHECK(twice == 0);
    // the loop closes it: its 20 pixels, each once
    CHECK(line_pixels(MT_GFX_LINE_LOOP, square, &twice) ==
          merged({row(0, 4, 0), column(5, 0, 4), row(1, 5, 5), column(0, 1, 5)}));
    CHECK(twice == 0);
    // as separate lines: the first and the third side
    CHECK(line_pixels(MT_GFX_LINES, square, &twice) == merged({row(0, 4, 0), row(1, 5, 5)}));
    // On pixel corners (the game's integer vertices) the joints lie in the gaps between the
    // diamonds: no side ends in a diamond, so the half-open rule has nothing to leave out. Each
    // side is drawn as it would be alone; moved by (-e, -e^2) the right side and the top both
    // reach (7, 7), and no side reaches (1, 1). That is the rule, as GL states it.
    const std::vector<float> corners = {2.0f, 2.0f, 8.0f, 2.0f, 8.0f, 8.0f, 2.0f, 8.0f};
    CHECK(line_pixels(MT_GFX_LINE_LOOP, corners, &twice) ==
          merged({row(2, 7, 1), column(7, 2, 7), row(2, 6, 7), column(1, 2, 7)}));
    CHECK(twice == 1);
    // a zigzag through its joints: (3, 3), (6, 0), (9, 3) drawn once, the end (9, 8) not
    const std::vector<float> zigzag = {0.5f, 0.5f, 3.5f, 3.5f, 6.5f, 0.5f, 9.5f, 3.5f, 9.5f, 8.5f};
    const Pixels z = line_pixels(MT_GFX_LINE_STRIP, zigzag, &twice);
    CHECK(twice == 0);
    CHECK(z.size() == 3 + 3 + 3 + 5);
    CHECK(std::count(z.begin(), z.end(), Px{9, 8}) == 0);
    // in general a strip is its segments, each half-open: random strips, joints anywhere
    Lcg rng;
    for (int n = 0; n < 200; ++n) {
        std::vector<float> xy;
        for (int k = 0; k < 12; ++k) {
            xy.push_back(static_cast<float>(rng.next() % 512u) / 16.0f);
        }
        auto t = make_target(32, 32);
        std::vector<mt_gfx_vertex> v;
        for (std::size_t i = 0; i < xy.size(); i += 2) {
            v.push_back(win(xy[i], xy[i + 1], 32, 32));
        }
        mt_gfx_draw d = additive(t.get());
        d.primitive = n % 2 == 0 ? MT_GFX_LINE_STRIP : MT_GFX_LINE_LOOP;
        d.vertices = v.data();
        d.count = static_cast<u32>(v.size());
        REQUIRE(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        std::vector<u32> want(32 * 32, 0);
        const std::size_t segments = v.size() - (n % 2 == 0 ? 1 : 0);
        for (std::size_t s = 0; s < segments; ++s) {
            const std::size_t a = s, b = (s + 1) % v.size();
            for (const Px &p :
                 reference_line(xy[a * 2], xy[a * 2 + 1], xy[b * 2], xy[b * 2 + 1], 32, 32)) {
                ++want[static_cast<std::size_t>(p.y) * 32 + static_cast<std::size_t>(p.x)];
            }
        }
        u32 wrong = 0;
        for (u32 y = 0; y < 32; ++y) {
            for (u32 x = 0; x < 32; ++x) {
                wrong += px(t.get(), x, 31 - y).r != want[y * 32 + x] ? 1u : 0u;
            }
        }
        CHECK(wrong == 0);
    }
    // too few vertices: nothing; GL_LINES ignores an odd last vertex
    CHECK(line_pixels(MT_GFX_LINE_STRIP, {0.5f, 0.5f}).empty());
    CHECK(line_pixels(MT_GFX_LINE_LOOP, {0.5f, 0.5f}).empty());
    CHECK(line_pixels(MT_GFX_LINES, {0.5f, 0.5f, 4.5f, 0.5f, 9.5f, 9.5f}) == row(0, 3, 0));
}

TEST_CASE("lines: clipping, the viewport, colour, blending and texturing") {
    Fresh fresh;
    const golden::Rgba black{0, 0, 0, 0};
    auto t = make_target(16, 16);
    // A sub-viewport bounds the pixels; the endpoints far outside it (inside the guard band)
    // keep their positions, so the pixels are those of the unclipped line.
    {
        mt_gfx_draw d = additive(t.get());
        d.viewport[0] = 4;
        d.viewport[1] = 4;
        d.viewport[2] = 8;
        d.viewport[3] = 8;
        // window x = 4 + (x + 1) * 4: -3 -> -4, 3 -> 20; y: -0.875 -> 4.5
        const mt_gfx_vertex v[2] = {{-3, -0.875f, 0, 1, 0, 0}, {3, -0.875f, 0, 1, 0, 0}};
        d.primitive = MT_GFX_LINES;
        d.vertices = v;
        d.count = 2;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        CHECK(drawn_pixels(t.get()) == row(4, 11, 4));
    }
    // z beyond the far plane: clipped at z = w, x = 0 (window 8, a diamond corner): columns
    // 0..6, as for a line ending there
    {
        auto u = make_target(16, 16);
        mt_gfx_draw d = additive(u.get());
        const mt_gfx_vertex v[2] = {{-1, -0.6875f, 0, 1, 0, 0}, {1, -0.6875f, 2, 1, 0, 0}};
        d.primitive = MT_GFX_LINES;
        d.vertices = v;
        d.count = 2;
        CHECK(mt_gfx_submit(u.get(), &d) == MT_GFX_DRAWN);
        CHECK(drawn_pixels(u.get()) == row(0, 6, 2));
        CHECK(mt_gfx_get_stats().frame.pixels == 8 + 7);
        // wholly outside, behind the eye, or not finite: nothing
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const mt_gfx_vertex out[6] = {{-1, 0, 2, 1, 0, 0},  {1, 0, 3, 1, 0, 0},
                                      {-1, 0, 0, -1, 0, 0}, {1, 0, 0, -2, 0, 0},
                                      {nan, 0, 0, 1, 0, 0}, {1, 0, 0, 1, 0, 0}};
        d.vertices = out;
        d.count = 6;
        auto w = make_target(16, 16);
        CHECK(mt_gfx_submit(w.get(), &d) == MT_GFX_DRAWN);
        CHECK(drawn_pixels(w.get()).empty());
        // crossing the eye plane: only the part in front is drawn; its projection runs from
        // window (0, 2.5) to the right towards infinity (y / w stays -0.6875), cut by the guard
        // band
        const mt_gfx_vertex eye[2] = {{-1, -0.6875f, 0, 1, 0, 0}, {3, 0.6875f, 0, -1, 0, 0}};
        d.vertices = eye;
        d.count = 2;
        CHECK(mt_gfx_submit(w.get(), &d) == MT_GFX_DRAWN);
        u32 twice = 0;
        const Pixels p = drawn_pixels(w.get(), &twice);
        CHECK(twice == 0);
        CHECK(p.size() >= 15);
        for (const Px &q : p) {
            CHECK(q.y == 2);
        }
    }
    // The game's line: blended with SRC_ALPHA / ONE_MINUS_SRC_ALPHA over a background, the
    // colour as glColor4x gives it.
    {
        const golden::Rgba bg{20, 40, 60, 255};
        auto u = make_target(16, 16, bg);
        mt_gfx_draw d = draw_for(u.get());
        d.color[0] = 217;
        d.color[1] = 145;
        d.color[2] = 85;
        d.color[3] = 254;
        d.blend = 1;
        d.src_factor = MT_GFX_SRC_ALPHA;
        d.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
        const mt_gfx_vertex v[2] = {win(2, 3, 16, 16), win(10, 3, 16, 16)};
        d.primitive = MT_GFX_LINES;
        d.vertices = v;
        d.count = 2;
        CHECK(mt_gfx_submit(u.get(), &d) == MT_GFX_DRAWN);
        // (217 * 254 + 20 * 1 + 127) / 255 = 216.7, (145 * 254 + 40 + 127) / 255 = 145.1, ...
        const golden::Rgba want{216, 145, 85, 254}; // alpha (254 * 254 + 255 + 127) / 255
        for (u32 x = 0; x < 16; ++x) {
            CAPTURE(x);
            CHECK(px(u.get(), x, 16 - 1 - 2) == (x >= 2 && x <= 9 ? want : bg));
            CHECK(px(u.get(), x, 16 - 1 - 3) == bg);
        }
    }
    // Texturing: s from 0 to 1 over 8 pixels of a 4 x 1 texture, nearest: two pixels a texel.
    {
        const std::vector<u8> texels = {255, 0, 0,   255, 0,   255, 0,   255,
                                        0,   0, 255, 255, 255, 255, 255, 255};
        const u32 tex = make_texture(4, 1, texels);
        auto u = make_target(16, 16);
        mt_gfx_draw d = draw_for(u.get());
        d.texture = tex;
        d.color[1] = 128; // modulated: green halved
        const mt_gfx_vertex v[2] = {win(0.5f, 0.5f, 16, 16, 0, 0.5f),
                                    win(8.5f, 0.5f, 16, 16, 1, 0.5f)};
        d.primitive = MT_GFX_LINES;
        d.vertices = v;
        d.count = 2;
        CHECK(mt_gfx_submit(u.get(), &d) == MT_GFX_DRAWN);
        const golden::Rgba want[9] = {{255, 0, 0, 255},     {255, 0, 0, 255},     {0, 128, 0, 255},
                                      {0, 128, 0, 255},     {0, 0, 255, 255},     {0, 0, 255, 255},
                                      {255, 128, 255, 255}, {255, 128, 255, 255}, black};
        for (u32 x = 0; x < 9; ++x) {
            CAPTURE(x);
            CHECK(px(u.get(), x, 15) == want[x]);
        }
    }
    // Scale 2: a logical line along pixel centres becomes a line along pixel boundaries.
    {
        auto u = make_target(8, 8, {0, 0, 0, 0}, 2);
        mt_gfx_draw d = additive(u.get());
        const mt_gfx_vertex v[2] = {win(0.5f, 0.5f, 8, 8), win(4.5f, 0.5f, 8, 8)};
        d.primitive = MT_GFX_LINES;
        d.vertices = v;
        d.count = 2;
        CHECK(mt_gfx_submit(u.get(), &d) == MT_GFX_DRAWN);
        // window (1, 1) -> (9, 1) at scale 2: on the row boundary, the row below; x from the
        // diamond corner at 1: columns 1..8
        CHECK(drawn_pixels(u.get()) == row(1, 8, 0));
    }
    CHECK(mt_gfx_get_stats().total.invalid == 0);
}

TEST_CASE("golden: lines in every octant, strips, loops, blended and textured (gfx_lines)") {
    Fresh fresh;
    constexpr u32 W = 96, H = 64;
    auto t = make_target(W, H);
    // a dark checkerboard background
    {
        u8 *p = mt_gfx_target_pixels(t.get());
        for (u32 y = 0; y < H; ++y) {
            for (u32 x = 0; x < W; ++x) {
                const u8 v = ((x / 8 + y / 8) & 1u) != 0 ? 40 : 24;
                u8 *q = p + (static_cast<std::size_t>(y) * W + x) * 4;
                q[0] = v;
                q[1] = v;
                q[2] = static_cast<u8>(v + 8);
                q[3] = 255;
            }
        }
    }
    auto submit = [&](u32 prim, const std::vector<mt_gfx_vertex> &v, mt_gfx_draw d) {
        d.primitive = prim;
        d.vertices = v.data();
        d.count = static_cast<u32>(v.size());
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
    };
    auto plain = [&](u8 r, u8 g, u8 b, u8 a = 255) {
        mt_gfx_draw d = draw_for(t.get());
        d.color[0] = r;
        d.color[1] = g;
        d.color[2] = b;
        d.color[3] = a;
        if (a != 255) {
            d.blend = 1;
            d.src_factor = MT_GFX_SRC_ALPHA;
            d.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
        }
        return d;
    };
    // A star of 32 spokes from (24, 32), every octant; even spokes start on a pixel centre,
    // odd ones on a pixel corner; ends rounded to 1/16.
    for (int k = 0; k < 32; ++k) {
        const double angle = k * 3.14159265358979323846 / 16.0;
        const float c = k % 2 == 0 ? 24.5f : 24.0f;
        const float ex = std::round(static_cast<float>(c + 21.0 * std::cos(angle)) * 16.0f) / 16.0f;
        const float ey = std::round(static_cast<float>(c + 21.0 * std::sin(angle)) * 16.0f) / 16.0f;
        submit(MT_GFX_LINES, {win(c, c + 7.5f, W, H), win(ex, ey + 7.5f, W, H)},
               plain(static_cast<u8>(128 + k * 4), static_cast<u8>(255 - k * 6),
                     static_cast<u8>(64 + k * 5)));
    }
    // The game's dialogue border: a loop on integer (pixel-corner) vertices, its colour, blended.
    submit(MT_GFX_LINE_LOOP,
           {win(52, 4, W, H), win(92, 4, W, H), win(92, 28, W, H), win(52, 28, W, H)},
           plain(217, 145, 85, 254));
    // a strip zigzag at half alpha, joints on centres and off them
    submit(MT_GFX_LINE_STRIP,
           {win(54.5f, 8.5f, W, H), win(60.5f, 24.5f, W, H), win(66.25f, 8.75f, W, H),
            win(72.0f, 24.0f, W, H), win(78.5f, 8.5f, W, H), win(90.0f, 20.0625f, W, H)},
           plain(90, 200, 255, 128));
    // additive crossing lines
    {
        mt_gfx_draw d = plain(120, 40, 40);
        d.blend = 1;
        d.src_factor = MT_GFX_ONE;
        d.dst_factor = MT_GFX_ONE;
        submit(MT_GFX_LINES,
               {win(52, 34, W, H), win(92, 60, W, H), win(52, 60, W, H), win(92, 34, W, H),
                win(72.5f, 32.5f, W, H), win(72.5f, 61.5f, W, H)},
               d);
    }
    // a textured line: a ramp, bilinear, s over 2 repeats
    {
        std::vector<u8> ramp;
        for (int i = 0; i < 8; ++i) {
            ramp.insert(ramp.end(),
                        {static_cast<u8>(i * 36), 255, static_cast<u8>(255 - i * 36), 255});
        }
        mt_gfx_draw d = plain(255, 255, 255);
        d.texture = make_texture(8, 1, ramp, MT_GFX_LINEAR);
        submit(MT_GFX_LINES, {win(4.5f, 62.5f, W, H, 0, 0.5f), win(91.5f, 62.5f, W, H, 2, 0.5f)},
               d);
    }
    // lines leaving the target on every side (clipped by the viewport)
    submit(MT_GFX_LINES,
           {win(-10, 2, W, H), win(40, 1.5f, W, H), win(95.5f, -6, W, H), win(95.5f, 70, W, H),
            win(30, 70, W, H), win(46, -4, W, H)},
           plain(255, 255, 0));
    check_golden(to_image(t.get()), "gfx_lines");
    // spot checks: a spoke's start pixel, a corner of the border loop (window (52, 3) is the
    // row below y = 4, image row 60), and the clipped yellow line along the right edge
    CHECK(px(t.get(), 95, 32) == golden::Rgba{255, 255, 0, 255});
    const golden::Rgba border = px(t.get(), 60, H - 1 - 3);
    CHECK(border.r > 150);
}

TEST_CASE("golden: the prototype scene, a textured alpha-blended quad (gfx_quad_blend)") {
    Fresh fresh;
    Target t(mt_gfx_target_create(proto::kWidth, proto::kHeight, 1));
    REQUIRE(t != nullptr);
    REQUIRE(proto::render(t.get()));
    const golden::Image first = to_image(t.get());
    check_golden(first, "gfx_quad_blend");
    for (const auto &probe : proto::kProbes) {
        const golden::Rgba want{probe.rgba[0], probe.rgba[1], probe.rgba[2], probe.rgba[3]};
        CHECK_MESSAGE(px(t.get(), probe.x, probe.y) == want, probe.what);
    }
    // Determinism: the same draws give the same bytes, also over a different first frame
    // (the scene overwrites every pixel).
    const u8 junk[4] = {1, 2, 3, 4};
    mt_gfx_target_fill(t.get(), junk);
    REQUIRE(proto::render(t.get()));
    CHECK(to_image(t.get()) == first);
    CHECK(mt_gfx_get_stats().textures == 0); // the scene frees its textures
}

TEST_CASE("internal resolution: a target at scale 2 draws the same logical scene") {
    Fresh fresh;

    SUBCASE("edges land at twice the positions") {
        // Logical 8 x 8 at scale 2: 16 x 16 pixels. The logical rectangle x 2..6, y 1..4 (window)
        // covers pixels x 4..11, window y 2..7 = image rows 8..13.
        auto t = make_target(8, 8, {0, 0, 0, 0}, 2);
        CHECK(mt_gfx_target_width(t.get()) == 8);
        CHECK(mt_gfx_target_scale(t.get()) == 2);
        CHECK(mt_gfx_target_pixel_width(t.get()) == 16);
        CHECK(mt_gfx_target_pixel_height(t.get()) == 16);
        CHECK(submit_rect(t.get(), 2, 1, 6, 4, additive(t.get())) == MT_GFX_DRAWN);
        for (u32 y = 0; y < 16; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                const bool inside = x >= 4 && x <= 11 && y >= 8 && y <= 13;
                CHECK_MESSAGE(px(t.get(), x, y).r == (inside ? 1 : 0), "pixel " << x << "," << y);
            }
        }
        CHECK(mt_gfx_get_stats().total.pixels == 48);
    }

    SUBCASE("half-pixel logical edges fall on pixel boundaries") {
        // Logical x 1.5 .. 3.5 is pixels 3.0 .. 7.0: columns 3..6, nothing on an edge.
        auto t = make_target(6, 6, {0, 0, 0, 0}, 2);
        CHECK(submit_rect(t.get(), 1.5f, 2.5f, 3.5f, 4.5f, additive(t.get())) == MT_GFX_DRAWN);
        for (u32 y = 0; y < 12; ++y) {
            for (u32 x = 0; x < 12; ++x) {
                const bool inside = x >= 3 && x <= 6 && y >= 3 && y <= 6;
                CHECK_MESSAGE(px(t.get(), x, y).r == (inside ? 1 : 0), "pixel " << x << "," << y);
            }
        }
    }

    SUBCASE("the viewport is scaled") {
        auto t = make_target(8, 8, {0, 0, 0, 0}, 3);
        mt_gfx_draw d = additive(t.get());
        d.viewport[0] = 2;
        d.viewport[1] = 1;
        d.viewport[2] = 4;
        d.viewport[3] = 5;
        const mt_gfx_vertex v[3] = {
            {-9, -9, 0, 1, 0, 0}, {30, -9, 0, 1, 0, 0}, {-9, 30, 0, 1, 0, 0}};
        d.vertices = v;
        d.count = 3;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        // Pixels x 6..17, window y 3..17 = image rows 6..20 of 24.
        for (u32 y = 0; y < 24; ++y) {
            for (u32 x = 0; x < 24; ++x) {
                const bool inside = x >= 6 && x <= 17 && y >= 6 && y <= 20;
                CHECK_MESSAGE(px(t.get(), x, y).r == (inside ? 1 : 0), "pixel " << x << "," << y);
            }
        }
    }

    SUBCASE("the prototype scene at scale 2 matches scale 1 at half the resolution") {
        Target one(mt_gfx_target_create(proto::kWidth, proto::kHeight, 1));
        Target two(mt_gfx_target_create(proto::kWidth, proto::kHeight, 2));
        REQUIRE(proto::render(one.get()));
        REQUIRE(proto::render(two.get()));
        // Uniform areas are the same in every pixel of their 2 x 2 blocks.
        for (const auto &probe : proto::kProbes) {
            if (std::string(probe.what).find("quad") != std::string::npos) {
                continue; // filtered texels: not uniform
            }
            const golden::Rgba want = px(one.get(), probe.x, probe.y);
            for (u32 k = 0; k < 4; ++k) {
                CHECK_MESSAGE(px(two.get(), probe.x * 2 + k % 2, probe.y * 2 + k / 2) == want,
                              probe.what);
            }
        }
        // Box-filtered down to 480 x 320, the frame is the scale-1 frame but for its edges (the
        // pixels that straddle an edge, and the filtering of the sprites at twice the samples).
        golden::Image half(static_cast<int>(proto::kWidth), static_cast<int>(proto::kHeight));
        for (u32 y = 0; y < proto::kHeight; ++y) {
            for (u32 x = 0; x < proto::kWidth; ++x) {
                u32 sum[4] = {};
                for (u32 k = 0; k < 4; ++k) {
                    const golden::Rgba p = px(two.get(), x * 2 + k % 2, y * 2 + k / 2);
                    sum[0] += p.r;
                    sum[1] += p.g;
                    sum[2] += p.b;
                    sum[3] += p.a;
                }
                half.set(static_cast<int>(x), static_cast<int>(y),
                         {static_cast<u8>((sum[0] + 2) / 4), static_cast<u8>((sum[1] + 2) / 4),
                          static_cast<u8>((sum[2] + 2) / 4), static_cast<u8>((sum[3] + 2) / 4)});
            }
        }
        const golden::Comparison cmp = golden::compare(half, to_image(one.get()), {24, 0.03});
        CHECK_MESSAGE(cmp.matches, cmp.differing_pixels << " pixels differ by more than 24");
    }
}

namespace {

// The texture replacement hook of the tests: replaces a 2 x 2 upload by `pixels` of `size`.
struct Replacer {
    std::vector<mt_gfx_upload> seen;
    std::vector<u8> pixels;
    u32 width = 0, height = 0;
    bool replace = true;
};

int replace_hook(void *user, const mt_gfx_upload *upload, mt_gfx_replacement *out) {
    auto *r = static_cast<Replacer *>(user);
    r->seen.push_back(*upload);
    if (!r->replace) {
        return 0;
    }
    out->pixels = r->pixels.data();
    out->width = r->width;
    out->height = r->height;
    return 1;
}

// Every pixel of the target's top row-major image.
std::vector<golden::Rgba> all_pixels(const mt_gfx_target *t) {
    std::vector<golden::Rgba> out;
    for (u32 y = 0; y < mt_gfx_target_pixel_height(t); ++y) {
        for (u32 x = 0; x < mt_gfx_target_pixel_width(t); ++x) {
            out.push_back(px(t, x, y));
        }
    }
    return out;
}

} // namespace

TEST_CASE("texture replacement: the hook sees uploads and may supply k times the texels") {
    Fresh fresh;
    // FNV-1a 64 over the texel bytes.
    const std::vector<u8> logical = {10, 20,  30,  40,  50,  60,  70,  80,
                                     90, 100, 110, 120, 130, 140, 150, 160};
    CHECK(mt_gfx_upload_hash(2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, logical.data()) ==
          0xB183856CD4984D25ull);
    CHECK(mt_gfx_upload_hash(2, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_SHORT_4_4_4_4, logical.data()) ==
          0); // no such pair
    CHECK(mt_gfx_upload_hash(2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, nullptr) == 0);

    // A 2x replacement: 4 x 4 texels with a different colour in each.
    Replacer r;
    r.width = r.height = 4;
    for (u32 i = 0; i < 16; ++i) {
        r.pixels.insert(r.pixels.end(),
                        {static_cast<u8>(i * 16), static_cast<u8>(255 - i * 16), 77, 255});
    }
    mt_gfx_set_replace_hook(replace_hook, &r);

    const u32 tex = make_texture(2, 2, logical, MT_GFX_NEAREST, MT_GFX_CLAMP_TO_EDGE);
    REQUIRE(r.seen.size() == 1);
    CHECK(r.seen[0].texture == tex);
    CHECK(r.seen[0].width == 2);
    CHECK(r.seen[0].height == 2);
    CHECK(r.seen[0].format == MT_GFX_RGBA);
    CHECK(r.seen[0].type == MT_GFX_UNSIGNED_BYTE);
    CHECK(r.seen[0].hash == 0xB183856CD4984D25ull);
    u32 w = 0, h = 0;
    CHECK(mt_gfx_texture_size(tex, &w, &h) == 0); // the logical size stays
    CHECK(w == 2);
    CHECK(h == 2);
    const u8 *texels = mt_gfx_texture_texels(tex, &w, &h); // what is sampled: the replacement
    REQUIRE(texels != nullptr);
    CHECK(w == 4);
    CHECK(h == 4);
    CHECK(std::equal(r.pixels.begin(), r.pixels.end(), texels));
    mt_gfx_stats st = mt_gfx_get_stats();
    CHECK(st.replaced_textures == 1);
    CHECK(st.replacements == 1);
    CHECK(st.texture_bytes == 64);

    // Drawn 1:1 on 4 x 4 pixels with the same texture coordinates as the 2 x 2 original, every
    // replacement texel shows: texel (x, row t) at image pixel (x, 3 - t).
    auto t = make_target(4, 4);
    mt_gfx_draw d = draw_for(t.get());
    d.texture = tex;
    CHECK(submit_rect(t.get(), 0, 0, 4, 4, d) == MT_GFX_DRAWN);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u8 *p = r.pixels.data() + ((3 - y) * 4 + x) * 4;
            CHECK(px(t.get(), x, y) == golden::Rgba{p[0], p[1], p[2], p[3]});
        }
    }

    // Filtering happens at the replacement's resolution: bilinear over 8 x 8 pixels equals the
    // same 4 x 4 texels uploaded without a hook.
    mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MIN_FILTER, MT_GFX_LINEAR);
    mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MAG_FILTER, MT_GFX_LINEAR);
    auto replaced = make_target(8, 8);
    d = draw_for(replaced.get());
    d.texture = tex;
    submit_rect(replaced.get(), 0, 0, 8, 8, d);
    mt_gfx_set_replace_hook(nullptr, nullptr);
    const u32 plain = make_texture(4, 4, r.pixels, MT_GFX_LINEAR, MT_GFX_CLAMP_TO_EDGE);
    auto direct = make_target(8, 8);
    d = draw_for(direct.get());
    d.texture = plain;
    submit_rect(direct.get(), 0, 0, 8, 8, d);
    CHECK(all_pixels(replaced.get()) == all_pixels(direct.get()));
    CHECK(r.seen.size() == 1); // no hook, no call

    // The hook may decline; uploads without pixels are not offered; a wrong size is rejected.
    std::vector<std::string> lines;
    mt_diag_config cfg = mt_diag_config_default();
    cfg.log = [](void *user, const char *line) {
        static_cast<std::vector<std::string> *>(user)->emplace_back(line);
    };
    cfg.log_user = &lines;
    mt_diag_configure(&cfg);
    mt_gfx_set_replace_hook(replace_hook, &r);
    r.replace = false;
    const u32 kept = make_texture(2, 2, logical);
    CHECK(r.seen.size() == 2);
    CHECK(mt_gfx_texture_texels(kept, &w, &h) != nullptr);
    CHECK(w == 2);
    CHECK(mt_gfx_texture_image(kept, 2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, nullptr) == 0);
    CHECK(r.seen.size() == 2);
    r.replace = true;
    r.width = 3; // not k times 2
    CHECK(mt_gfx_texture_image(kept, 2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, logical.data()) == 0);
    CHECK(mt_gfx_texture_texels(kept, &w, &h) != nullptr);
    CHECK(w == 2);
    st = mt_gfx_get_stats();
    CHECK(st.rejected_replacements == 1);
    CHECK(st.replacements == 1);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("replacement 3x4 for 2x2 (hash b183856cd4984d25) rejected") !=
          std::string::npos);
    mt_gfx_set_replace_hook(nullptr, nullptr);
    mt_diag_reset();
}

TEST_CASE("golden: every source x destination blend factor (gfx_blend_factors)") {
    Fresh fresh;
    // 11 x 11 tiles of 16 x 16 pixels: row = source factor, column = destination factor. Each
    // tile's destination is a bilinear gradient with alpha, and the source a gradient in the other
    // direction, so every factor meets many colour and alpha values.
    constexpr u32 tile = 16, n = 11, size = tile * n;
    auto t = make_target(size, size);
    const u32 dst_tex =
        make_texture(2, 2, {255, 0, 0, 255, 0, 255, 0, 64, 0, 0, 255, 192, 255, 255, 255, 0},
                     MT_GFX_LINEAR, MT_GFX_CLAMP_TO_EDGE);
    const u32 src_tex =
        make_texture(2, 2, {255, 255, 0, 0, 0, 255, 255, 255, 255, 0, 255, 128, 32, 32, 32, 224},
                     MT_GFX_LINEAR, MT_GFX_CLAMP_TO_EDGE);
    for (u32 row = 0; row < n; ++row) {
        for (u32 col = 0; col < n; ++col) {
            const float x0 = static_cast<float>(col * tile), x1 = x0 + tile;
            const float y1 = static_cast<float>(size - row * tile),
                        y0 = y1 - tile; // row 0 at the top
            mt_gfx_draw d = draw_for(t.get());
            d.texture = dst_tex;
            auto v = rect(x0, y0, x1, y1, size, size);
            d.vertices = v.data();
            d.count = 6;
            REQUIRE(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
            d.texture = src_tex;
            d.blend = 1;
            d.src_factor = k_factors[row];
            d.dst_factor = k_factors[col];
            v = rect(x0, y0, x1, y1, size, size, 1, 0, 0, 1); // mirrored: a different gradient
            d.vertices = v.data();
            REQUIRE(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
        }
    }
    check_golden(to_image(t.get()), "gfx_blend_factors");
}

TEST_CASE("golden: a textured floor in perspective, clipped at the near plane (gfx_perspective)") {
    Fresh fresh;
    // A checkerboard floor seen from above it, through a perspective projection (fovy 90
    // degrees, aspect 4:3, near 0.5, far 50) with no libm: the matrix is written out. Its near
    // edge lies behind the eye, so the clipper cuts it at w = near. Left half nearest, right half
    // bilinear, alpha-blended over grey.
    constexpr u32 w = 160, h = 120;
    auto t = make_target(w, h, {64, 64, 64, 255});
    std::vector<u8> checker;
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const bool light = ((x ^ y) & 1u) == 0;
            checker.insert(checker.end(),
                           {static_cast<u8>(light ? 230 : 30), static_cast<u8>(light ? 200 : 60),
                            static_cast<u8>(light ? 90 : 160),
                            static_cast<u8>(x == 0 || y == 0 ? 128 : 255)});
        }
    }
    const float n = 0.5f, f = 50.0f, fy = 1.0f, fx = 0.75f; // cot(45 deg) = 1, / aspect
    // Eye at height 1 looking along -z; floor y = -1, z from +2 (behind) to -20.
    auto project = [&](float x, float y, float z, float s, float tc) {
        mt_gfx_vertex v;
        v.x = fx * x;
        v.y = fy * y;
        v.z = (f + n) / (n - f) * z + 2.0f * f * n / (n - f);
        v.w = -z;
        v.s = s;
        v.t = tc;
        return v;
    };
    for (int half = 0; half < 2; ++half) {
        const u32 tex =
            make_texture(8, 8, checker, half == 0 ? MT_GFX_NEAREST : MT_GFX_LINEAR, MT_GFX_REPEAT);
        const float xa = half == 0 ? -6.0f : 0.0f, xb = half == 0 ? 0.0f : 6.0f;
        const float sa = half == 0 ? 0.0f : 3.0f, sb = half == 0 ? 3.0f : 6.0f;
        const mt_gfx_vertex v[4] = {project(xa, -1, 2, sa, 0), project(xb, -1, 2, sb, 0),
                                    project(xa, -1, -20, sa, 11), project(xb, -1, -20, sb, 11)};
        mt_gfx_draw d = draw_for(t.get());
        d.primitive = MT_GFX_TRIANGLE_STRIP;
        d.vertices = v;
        d.count = 4;
        d.texture = tex;
        d.blend = 1;
        d.src_factor = MT_GFX_SRC_ALPHA;
        d.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
        REQUIRE(mt_gfx_submit(t.get(), &d) == MT_GFX_DRAWN);
    }
    check_golden(to_image(t.get()), "gfx_perspective");
    // Above the horizon (y = 0 in clip space at z -> -inf) the floor never draws.
    CHECK(px(t.get(), 80, 10) == golden::Rgba{64, 64, 64, 255});
}

TEST_CASE("timing: 300 alpha-blended 32 x 32 sprites at 480 x 320") {
    Fresh fresh;
    // Not a pass/fail check (debug builds and emulated containers are slow): prints the time a
    // busy map frame might take, for ADR 0039's numbers. The scene is redrawn a few times.
    auto t = make_target(480, 320, {0, 0, 0, 255});
    std::vector<u8> sprite(32 * 32 * 4);
    for (std::size_t i = 0; i < sprite.size(); ++i) {
        sprite[i] = static_cast<u8>(i * 7);
    }
    const u32 tex = make_texture(32, 32, sprite, MT_GFX_NEAREST, MT_GFX_CLAMP_TO_EDGE);
    Lcg rng;
    std::vector<std::array<mt_gfx_vertex, 6>> quads;
    for (int i = 0; i < 300; ++i) {
        const float x = rng.unit() * 448.0f, y = rng.unit() * 288.0f;
        quads.push_back(rect(x, y, x + 32, y + 32, 480, 320));
    }
    // Plus a full-screen opaque background, as the game draws first.
    const auto bg = rect(0, 0, 480, 320, 480, 320);
    constexpr int frames = 10;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frames; ++frame) {
        mt_gfx_draw d = draw_for(t.get());
        d.texture = tex;
        d.vertices = bg.data();
        d.count = 6;
        mt_gfx_submit(t.get(), &d);
        d.blend = 1;
        d.src_factor = MT_GFX_SRC_ALPHA;
        d.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
        for (const auto &q : quads) {
            d.vertices = q.data();
            mt_gfx_submit(t.get(), &d);
        }
        mt_gfx_end_frame();
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count() /
        frames;
    const mt_gfx_stats st = mt_gfx_get_stats();
    std::printf("gfx timing: 301 draws, %llu pixels a frame, %.3f ms a frame\n",
                static_cast<unsigned long long>(st.total.pixels / frames), ms);
    CHECK(st.total.draws == frames * 301u);
    CHECK(st.frames == frames);
}

namespace {

// A hook of the chain tests: records its calls in `order` and replaces with `pixels` (size
// width x height) when `replace`.
struct ChainHook {
    std::vector<int> *order = nullptr;
    int id = 0;
    bool replace = false;
    std::vector<u8> pixels;
    u32 width = 0, height = 0;
};

int chain_hook(void *user, const mt_gfx_upload *, mt_gfx_replacement *out) {
    auto *h = static_cast<ChainHook *>(user);
    h->order->push_back(h->id);
    if (!h->replace) {
        return 0;
    }
    out->pixels = h->pixels.data();
    out->width = h->width;
    out->height = h->height;
    return 1;
}

} // namespace

TEST_CASE("texture replacement: hooks form a chain by order, the first valid one counts (#153)") {
    Fresh fresh;
    const std::vector<u8> logical(2 * 2 * 4, 9);
    std::vector<int> order;
    ChainHook dump{&order, 0, false, {}, 0, 0};
    ChainHook text{&order, 100, true, std::vector<u8>(4 * 4 * 4, 100), 4, 4};
    ChainHook pack{&order, 200, true, std::vector<u8>(6 * 6 * 4, 200), 6, 6};
    // Added out of order: they run by order.
    REQUIRE(mt_gfx_add_replace_hook(MT_GFX_REPLACE_ORDER_PACK, chain_hook, &pack) == 0);
    REQUIRE(mt_gfx_add_replace_hook(MT_GFX_REPLACE_ORDER_DUMP, chain_hook, &dump) == 0);
    REQUIRE(mt_gfx_add_replace_hook(MT_GFX_REPLACE_ORDER_TEXT, chain_hook, &text) == 0);
    CHECK(mt_gfx_replace_hook_count() == 3);
    // Adding one again moves it, it does not add a second.
    REQUIRE(mt_gfx_add_replace_hook(MT_GFX_REPLACE_ORDER_TEXT, chain_hook, &text) == 0);
    CHECK(mt_gfx_replace_hook_count() == 3);

    // The text hook replaces: the pack is not asked.
    u32 tex = make_texture(2, 2, logical);
    CHECK(order == std::vector<int>{0, 100});
    u32 w = 0, h = 0;
    const u8 *texels = mt_gfx_texture_texels(tex, &w, &h);
    REQUIRE(texels != nullptr);
    CHECK(w == 4);
    CHECK(texels[0] == 100);

    // The text hook declines: the pack replaces (3x).
    order.clear();
    text.replace = false;
    REQUIRE(mt_gfx_texture_image(tex, 2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, logical.data()) ==
            0);
    CHECK(order == std::vector<int>{0, 100, 200});
    texels = mt_gfx_texture_texels(tex, &w, &h);
    CHECK(w == 6);
    CHECK(texels[0] == 200);

    // A wrong size from the text hook is rejected, and the pack is asked next.
    std::vector<std::string> lines;
    mt_diag_config cfg = mt_diag_config_default();
    cfg.log = [](void *user, const char *line) {
        static_cast<std::vector<std::string> *>(user)->emplace_back(line);
    };
    cfg.log_user = &lines;
    mt_diag_configure(&cfg);
    order.clear();
    text.replace = true;
    text.width = 5;
    REQUIRE(mt_gfx_texture_image(tex, 2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, logical.data()) ==
            0);
    CHECK(order == std::vector<int>{0, 100, 200});
    texels = mt_gfx_texture_texels(tex, &w, &h);
    CHECK(w == 6);
    CHECK(mt_gfx_get_stats().rejected_replacements == 1);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("replacement 5x4 for 2x2") != std::string::npos);
    mt_diag_reset();

    // Removing one leaves the others; set_replace_hook empties the chain first.
    order.clear();
    mt_gfx_remove_replace_hook(chain_hook, &text);
    mt_gfx_remove_replace_hook(chain_hook, &text); // absent: nothing
    CHECK(mt_gfx_replace_hook_count() == 2);
    REQUIRE(mt_gfx_texture_image(tex, 2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, logical.data()) ==
            0);
    CHECK(order == std::vector<int>{0, 200});
    mt_gfx_set_replace_hook(chain_hook, &dump);
    CHECK(mt_gfx_replace_hook_count() == 1);
    mt_gfx_set_replace_hook(nullptr, nullptr);
    CHECK(mt_gfx_replace_hook_count() == 0);

    // At most MT_GFX_MAX_REPLACE_HOOKS; a NULL hook is refused.
    std::vector<ChainHook> many(MT_GFX_MAX_REPLACE_HOOKS + 1,
                                ChainHook{&order, 1, false, {}, 0, 0});
    for (u32 i = 0; i < MT_GFX_MAX_REPLACE_HOOKS; ++i) {
        CHECK(mt_gfx_add_replace_hook(static_cast<int>(i), chain_hook, &many[i]) == 0);
    }
    CHECK(mt_gfx_add_replace_hook(0, chain_hook, &many.back()) == -1);
    CHECK(mt_gfx_add_replace_hook(0, nullptr, nullptr) == -1);
    mt_gfx_set_replace_hook(nullptr, nullptr);
}

TEST_CASE("texture replacement: mt_gfx_upload_rgba8 gives the texels mt_gfx stores (#153)") {
    Fresh fresh;
    // RGB 5-6-5, 3 x 1, rows padded to 4 bytes: 6 bytes + 2 of padding, then the second row.
    const std::vector<u8> rgb565 = {0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0xAA, 0xBB,
                                    0xFF, 0xFF, 0x00, 0x00, 0x10, 0x84, 0xCC, 0xDD};
    std::vector<u8> out(3 * 2 * 4);
    REQUIRE(mt_gfx_upload_rgba8(3, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_SHORT_5_6_5, rgb565.data(),
                                out.data()) == 0);
    const u32 tex = mt_gfx_texture_create();
    REQUIRE(mt_gfx_texture_image(tex, 3, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_SHORT_5_6_5,
                                 rgb565.data()) == 0);
    u32 w = 0, h = 0;
    const u8 *texels = mt_gfx_texture_texels(tex, &w, &h);
    REQUIRE(texels != nullptr);
    CHECK(std::equal(out.begin(), out.end(), texels));
    CHECK(out[0] == 255); // red
    CHECK(out[3] == 255);
    CHECK(mt_gfx_upload_rgba8(3, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_SHORT_4_4_4_4, rgb565.data(),
                              out.data()) == -1);
    CHECK(mt_gfx_upload_rgba8(3, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_SHORT_5_6_5, nullptr, out.data()) ==
          -1);
}

TEST_CASE("texture_filter: the nearest override samples every texture GL_NEAREST (#153)") {
    Fresh fresh;
    // A black-white texel pair, GL_LINEAR both ways, magnified 4x: bilinear gives the ramp of
    // ADR 0039; the override gives two flat halves, and 0 gives the ramp back.
    const u32 tex =
        make_texture(2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, MT_GFX_LINEAR, MT_GFX_CLAMP_TO_EDGE);
    auto draw = [&]() {
        auto t = make_target(8, 1);
        mt_gfx_draw d = draw_for(t.get());
        d.texture = tex;
        REQUIRE(submit_rect(t.get(), 0, 0, 8, 1, d) == MT_GFX_DRAWN);
        std::vector<u8> reds;
        for (u32 x = 0; x < 8; ++x) {
            reds.push_back(px(t.get(), x, 0).r);
        }
        return reds;
    };
    const std::vector<u8> linear = {0, 0, 32, 96, 159, 223, 255, 255};
    CHECK(mt_gfx_texture_filter() == 0);
    CHECK(draw() == linear);
    REQUIRE(mt_gfx_set_texture_filter(MT_GFX_NEAREST) == 0);
    CHECK(mt_gfx_texture_filter() == MT_GFX_NEAREST);
    CHECK(draw() == std::vector<u8>{0, 0, 0, 0, 255, 255, 255, 255});
    // Minified (a 2-texel texture on 1 pixel), the override is nearest too.
    {
        auto t = make_target(1, 1);
        mt_gfx_draw d = draw_for(t.get());
        d.texture = tex;
        REQUIRE(submit_rect(t.get(), 0, 0, 1, 1, d) == MT_GFX_DRAWN);
        CHECK(px(t.get(), 0, 0).r == 255); // the texel under the centre (s = 0.5: texel 1)
    }
    CHECK(mt_gfx_set_texture_filter(MT_GFX_LINEAR) == -1); // only 0 and GL_NEAREST
    CHECK(mt_gfx_texture_filter() == MT_GFX_NEAREST);
    mt_gfx_reset(); // kept across a reset, as the hooks
    CHECK(mt_gfx_texture_filter() == MT_GFX_NEAREST);
    REQUIRE(mt_gfx_set_texture_filter(0) == 0);
}
