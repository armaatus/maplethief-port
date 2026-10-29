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

    // Formats #57 adds, unknown handles and oversized images are refused and change nothing.
    CHECK(mt_gfx_texture_image(tex, 2, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_BYTE, texels.data()) == -1);
    CHECK(mt_gfx_texture_image(tex, 2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_SHORT_4_4_4_4,
                               texels.data()) == -1);
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

TEST_CASE("points and lines are counted and logged, not drawn, and invalid draws rejected") {
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
    for (const u32 prim :
         {MT_GFX_POINTS, MT_GFX_LINES, MT_GFX_LINE_LOOP, MT_GFX_LINE_STRIP, MT_GFX_POINTS}) {
        d.primitive = prim;
        CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_UNSUPPORTED);
    }
    CHECK(mt_gfx_get_stats().total.unsupported == 5);
    CHECK(red_histogram(t.get())[0] == 16);
    REQUIRE(lines.size() == 4); // once per primitive type
    CHECK(lines[0] == "gfx: GL_POINTS draws are not supported: counted, not drawn (logged once)");
    CHECK(lines[3].find("GL_LINE_STRIP") != std::string::npos);

    d.primitive = 7; // GL_QUADS does not exist in GL ES
    CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_INVALID);
    d.primitive = MT_GFX_TRIANGLES;
    d.vertices = nullptr;
    CHECK(mt_gfx_submit(t.get(), &d) == MT_GFX_INVALID);
    CHECK(mt_gfx_submit(nullptr, &d) == MT_GFX_INVALID);
    CHECK(mt_gfx_submit(t.get(), nullptr) == MT_GFX_INVALID);
    CHECK(mt_gfx_get_stats().total.invalid == 4);
    CHECK(lines.size() == 8);
    CHECK(lines[4] == "gfx: invalid draw: unknown primitive");
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
    CHECK(lines[0].rfind("gfx: frame 1: 1 draws, 2 triangles, 16 pixels, 0 unsupported, ", 0) == 0);
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
    CHECK(mt_gfx_upload_hash(2, 2, MT_GFX_RGB, MT_GFX_UNSIGNED_BYTE, logical.data()) == 0);
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
