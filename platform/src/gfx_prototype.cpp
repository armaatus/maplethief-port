// The rendering backend's prototype scene (#55, ADR 0039); see gfx_prototype.h.
//
// Positions are given in image pixels (origin top left, as the golden image shows them) and
// turned into clip space for an ortho draw over the whole 480x320 viewport. The rotation uses
// constant sines and cosines, not libm, so the vertices are the same floats everywhere.
#include "gfx_prototype.h"

#include <vector>

namespace maplethief::gfx_prototype {
namespace {

// Image pixel (x, y) with texture coordinates (s, t) as a clip-space vertex.
mt_gfx_vertex at(float x, float y, float s, float t) {
    const float w = static_cast<float>(kWidth), h = static_cast<float>(kHeight);
    return {x / w * 2.0f - 1.0f, 1.0f - y / h * 2.0f, 0.0f, 1.0f, s, t};
}

mt_gfx_draw base_draw() {
    mt_gfx_draw d = mt_gfx_draw_default();
    d.viewport[2] = static_cast<int32_t>(kWidth);
    d.viewport[3] = static_cast<int32_t>(kHeight);
    return d;
}

// A 2x2 checkerboard (REPEAT, NEAREST), for the background.
std::uint32_t make_checker() {
    const std::uint8_t texels[16] = {200, 200, 210, 255, 90,  96,  120, 255,
                                     90,  96,  120, 255, 200, 200, 210, 255};
    const std::uint32_t tex = mt_gfx_texture_create();
    if (tex == 0 ||
        mt_gfx_texture_image(tex, 2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texels) != 0 ||
        mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MIN_FILTER, MT_GFX_NEAREST) != 0 ||
        mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MAG_FILTER, MT_GFX_NEAREST) != 0) {
        mt_gfx_texture_destroy(tex);
        return 0;
    }
    return tex;
}

// A 64x64 sprite: a red/green gradient over x/y with blue, darker 8x8 cells in a checkerboard,
// an opaque 4-texel frame, a translucent inside (alpha 176) and a transparent round hole off the
// middle. Integer maths.
std::uint32_t make_sprite() {
    constexpr int n = 64;
    std::vector<std::uint8_t> texels(n * n * 4);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            std::uint8_t *p = texels.data() + (y * n + x) * 4;
            const bool frame = x < 4 || y < 4 || x >= n - 4 || y >= n - 4;
            const int shade = !frame && ((x / 8 + y / 8) % 2) != 0 ? 3 : 4; // quarters
            p[0] = static_cast<std::uint8_t>(255 * x / (n - 1) * shade / 4);
            p[1] = static_cast<std::uint8_t>(255 * y / (n - 1) * shade / 4);
            p[2] = static_cast<std::uint8_t>(160 * shade / 4);
            const int dx = x - 44, dy = y - 20;
            p[3] = frame ? 255 : dx * dx + dy * dy < 100 ? 0 : 176;
        }
    }
    const std::uint32_t tex = mt_gfx_texture_create();
    if (tex == 0 ||
        mt_gfx_texture_image(tex, n, n, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, texels.data()) != 0 ||
        mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_WRAP_S, MT_GFX_CLAMP_TO_EDGE) != 0 ||
        mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_WRAP_T, MT_GFX_CLAMP_TO_EDGE) != 0) {
        mt_gfx_texture_destroy(tex);
        return 0;
    }
    return tex;
}

} // namespace

bool render(mt_gfx_target *target) {
    const std::uint32_t checker = make_checker();
    const std::uint32_t sprite = make_sprite();
    bool ok = checker != 0 && sprite != 0;

    auto submit = [&](const mt_gfx_draw &d) {
        ok = ok && mt_gfx_submit(target, &d) == MT_GFX_DRAWN;
    };

    // Background: the checkerboard in 32-pixel cells over the whole screen, opaque.
    {
        const float w = static_cast<float>(kWidth), h = static_cast<float>(kHeight);
        const mt_gfx_vertex v[4] = {at(0, h, 0, 0), at(w, h, w / 64, 0), at(0, 0, 0, h / 64),
                                    at(w, 0, w / 64, h / 64)};
        mt_gfx_draw d = base_draw();
        d.primitive = MT_GFX_TRIANGLE_STRIP;
        d.vertices = v;
        d.count = 4;
        d.texture = checker;
        submit(d);
    }

    // The quad: the sprite at 3x, turned by 30 degrees about the middle of the screen, bilinear,
    // modulated by a slightly transparent white, blended with SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
    {
        constexpr float c = 0.8660254f, s = 0.5f, r = 96.0f; // cos 30, sin 30, half size
        const float cx = 240.0f, cy = 160.0f;
        auto corner = [&](float u, float v) { // u, v in {-1, 1}
            const float x = u * r, y = v * r;
            return at(cx + x * c - y * s, cy + x * s + y * c, (u + 1) / 2, (v + 1) / 2);
        };
        const mt_gfx_vertex v[6] = {corner(-1, -1), corner(1, -1), corner(1, 1),
                                    corner(-1, -1), corner(1, 1),  corner(-1, 1)};
        mt_gfx_texture_parameter(sprite, MT_GFX_TEXTURE_MIN_FILTER, MT_GFX_LINEAR);
        mt_gfx_texture_parameter(sprite, MT_GFX_TEXTURE_MAG_FILTER, MT_GFX_LINEAR);
        mt_gfx_draw d = base_draw();
        d.vertices = v;
        d.count = 6;
        d.texture = sprite;
        d.color[3] = 230;
        d.blend = 1;
        d.src_factor = MT_GFX_SRC_ALPHA;
        d.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
        submit(d);
    }

    // The sprite again at 1.5x, nearest, tinted orange, alpha-blended.
    {
        const mt_gfx_vertex v[4] = {at(24, 296, 0, 0), at(120, 296, 1, 0), at(24, 200, 0, 1),
                                    at(120, 200, 1, 1)};
        mt_gfx_texture_parameter(sprite, MT_GFX_TEXTURE_MIN_FILTER, MT_GFX_NEAREST);
        mt_gfx_texture_parameter(sprite, MT_GFX_TEXTURE_MAG_FILTER, MT_GFX_NEAREST);
        mt_gfx_draw d = base_draw();
        d.primitive = MT_GFX_TRIANGLE_STRIP;
        d.vertices = v;
        d.count = 4;
        d.texture = sprite;
        d.color[1] = 200;
        d.color[2] = 120;
        d.blend = 1;
        d.src_factor = MT_GFX_SRC_ALPHA;
        d.dst_factor = MT_GFX_ONE_MINUS_SRC_ALPHA;
        submit(d);
    }

    // A hexagon (a fan) multiplied into the background: DST_COLOR / ZERO.
    {
        constexpr float k = 0.8660254f; // sin 60
        const float cx = 400.0f, cy = 70.0f, r = 50.0f;
        const mt_gfx_vertex v[8] = {
            at(cx, cy, 0, 0),
            at(cx + r, cy, 0, 0),
            at(cx + r / 2, cy - r * k, 0, 0),
            at(cx - r / 2, cy - r * k, 0, 0),
            at(cx - r, cy, 0, 0),
            at(cx - r / 2, cy + r * k, 0, 0),
            at(cx + r / 2, cy + r * k, 0, 0),
            at(cx + r, cy, 0, 0),
        };
        mt_gfx_draw d = base_draw();
        d.primitive = MT_GFX_TRIANGLE_FAN;
        d.vertices = v;
        d.count = 8;
        d.color[1] = 128;
        d.color[2] = 64;
        d.blend = 1;
        d.src_factor = MT_GFX_DST_COLOR;
        d.dst_factor = MT_GFX_ZERO;
        submit(d);
    }

    // An additive rectangle (ONE / ONE), untextured, in the bottom right corner.
    {
        const mt_gfx_vertex v[4] = {at(380, 310, 0, 0), at(470, 310, 0, 0), at(380, 240, 0, 0),
                                    at(470, 240, 0, 0)};
        mt_gfx_draw d = base_draw();
        d.primitive = MT_GFX_TRIANGLE_STRIP;
        d.vertices = v;
        d.count = 4;
        d.color[0] = 60;
        d.color[1] = 30;
        d.color[2] = 90;
        d.blend = 1;
        d.src_factor = MT_GFX_ONE;
        d.dst_factor = MT_GFX_ONE;
        submit(d);
    }

    mt_gfx_texture_destroy(checker);
    mt_gfx_texture_destroy(sprite);
    return ok;
}

} // namespace maplethief::gfx_prototype
