// The rendering backend's prototype scene (#55, ADR 0039): a textured, alpha-blended quad over a
// background, drawn with mt_gfx (maplethief/gfx.h) into a 480x320 target. `MapleThief
// --gfx-prototype` shows it in the window; tests/cpp/gfx_test.cpp compares it with the golden
// image tests/golden/data/gfx_quad_blend.png. Synthetic: nothing in it comes from the game.
//
// No SDL here, so that the test builds the same scene without the frontend.
#ifndef MAPLETHIEF_PLATFORM_GFX_PROTOTYPE_H
#define MAPLETHIEF_PLATFORM_GFX_PROTOTYPE_H

#include <array>
#include <cstdint>

#include "maplethief/gfx.h"

namespace maplethief::gfx_prototype {

inline constexpr std::uint32_t kWidth = 480;
inline constexpr std::uint32_t kHeight = 320;

// Draws the scene into `target` (kWidth x kHeight), overwriting every pixel. Creates and frees
// its own textures. Returns false if a texture could not be made or a draw was rejected.
bool render(mt_gfx_target *target);

// A pixel of the scene (image coordinates, top row first) and its expected RGBA.
struct Probe {
    const char *what;
    std::uint32_t x, y;
    std::array<std::uint8_t, 4> rgba;
};

// Pixels that prove the quad is there: its middle (texture modulated and blended over the
// background), a background cell of each colour, the multiplied hexagon and the additive
// rectangle. The values are the scene's exact bytes (the golden image has the same); the
// hexagon is (255, 128, 64) x the dark cell (90, 96, 120) / 255 and the rectangle the dark cell
// + (60, 30, 90).
inline constexpr std::array<Probe, 5> kProbes = {{
    {"quad centre (textured, alpha-blended)", 240, 160, {104, 106, 133, 195}},
    {"background, dark cell", 8, 8, {90, 96, 120, 255}},
    {"background, light cell", 40, 8, {200, 200, 210, 255}},
    {"hexagon (DST_COLOR, ZERO)", 400, 70, {90, 48, 30, 255}},
    {"additive rectangle (ONE, ONE)", 440, 290, {150, 126, 210, 255}},
}};

} // namespace maplethief::gfx_prototype

#endif // MAPLETHIEF_PLATFORM_GFX_PROTOTYPE_H
