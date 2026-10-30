// The safe area (#73, ADR 0055) with synthetic data only: the placement of the 480x320 screen
// inside a phone's safe area, in every scale mode, at densities 1, 2 and 3 and render scales 1
// and 2, with asymmetric insets and their mirror image (the two landscape orientations: SDL turns
// the window's coordinates with the phone, so the insets swap sides), with the real landscape
// insets of notch and Dynamic Island iPhones, and with zero insets (exactly the placement without
// them). Then the touch inverse through those placements (window points -> screen points and
// back), --safe-area's parser, and a headless read-back (SDL's dummy driver) of a presenter with
// forced insets: the bars, in the border colour, cover the inset strips.
//
// The phones' numbers are UIKit's (points, landscape): iPhone 15 (Dynamic Island) 852x393 at 3x,
// insets 59 left and right, 21 bottom; iPhone X / 11 Pro (notch) 812x375 at 3x, 44/44/21; iPhone
// 11 / XR 896x414 at 2x, 48/48/21; iPhone SE 667x375 at 2x, none.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "focus.h"
#include "present.h"
#include "scaling.h"

namespace {

namespace fe = maplethief::frontend;
using fe::Insets;
using fe::ScaleMode;

constexpr ScaleMode kModes[] = {ScaleMode::kInteger, ScaleMode::kFit, ScaleMode::kSmooth,
                                ScaleMode::kSharp};

std::string text(const fe::Rect &r) {
    return std::to_string(r.x) + ", " + std::to_string(r.y) + ", " + std::to_string(r.w) + ", " +
           std::to_string(r.h);
}

bool same(const fe::Rect &a, const fe::Rect &b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// The other landscape orientation: left and right swap.
Insets mirrored(const Insets &in) { return Insets{in.top, in.right, in.bottom, in.left}; }

struct Screen {
    const char *name;
    int w, h;      // points
    float density; // drawable = points x density
    Insets insets; // points
};

// The phones (header), desktop windows at 1x and 2x (no insets), and asymmetric cases: a notch on
// one side only, a top strip, everything at once, a 1.5x density, and strips that leave less
// than 1x of room.
const Screen kScreens[] = {
    {"iPhone 15", 852, 393, 3, {0, 59, 21, 59}},
    {"iPhone X", 812, 375, 3, {0, 44, 21, 44}},
    {"iPhone XR", 896, 414, 2, {0, 48, 21, 48}},
    {"iPhone SE", 667, 375, 2, {}},
    {"desktop 960x640", 960, 640, 1, {}},
    {"desktop Retina", 1440, 900, 2, {}},
    {"notch left only", 852, 393, 3, {0, 59, 0, 0}},
    {"notch left, home indicator", 812, 375, 3, {0, 44, 21, 0}},
    {"top strip", 1000, 700, 1, {37, 0, 0, 0}},
    {"all four", 1000, 700, 2, {11, 83, 29, 7.5f}},
    {"1.5x", 1200, 800, 1.5f, {0, 33.3f, 10, 5}},
    {"under 1x", 600, 400, 1, {0, 100, 50, 30}},
    {"wide strips", 2000, 400, 1, {0, 700, 0, 100}},
};

int drawable(int points, float density) {
    return static_cast<int>(std::lround(static_cast<double>(points) * density));
}

// The safe area in drawable pixels, worked out independently: each inset x density rounded up.
fe::Rect expected_safe(const Screen &s) {
    const auto px = [](float pts, float d) {
        return static_cast<float>(std::ceil(static_cast<double>(pts) * d - 1e-3));
    };
    const float l = px(s.insets.left, s.density), r = px(s.insets.right, s.density);
    const float t = px(s.insets.top, s.density), b = px(s.insets.bottom, s.density);
    const auto dw = static_cast<float>(drawable(s.w, s.density));
    const auto dh = static_cast<float>(drawable(s.h, s.density));
    return fe::Rect{l, t, dw - l - r, dh - t - b};
}

// Where an image of `size` whole pixels is centred in `output` (the odd pixel right/below).
float centred(int output, float size) {
    return std::floor((static_cast<float>(output) - size) / 2);
}

// Checks one placement against the safe area: inside it, the mode's size for it, and the
// touch inverse round trip.
void check_placement(const Screen &s, const Insets &insets, ScaleMode mode, int render_scale,
                     const fe::Placement &p) {
    const int dw = drawable(s.w, s.density), dh = drawable(s.h, s.density);
    const float d = s.density;
    const fe::Rect &r = p.rect;
    const fe::Rect &a = p.safe;
    INFO(s.name << ", " << fe::scale_mode_name(mode) << ", render scale " << render_scale
                << ", insets " << insets.top << "," << insets.left << "," << insets.bottom << ","
                << insets.right << ": image (" << text(r) << "), safe (" << text(a) << ")");
    REQUIRE(r.w > 0);
    REQUIRE(r.h > 0);
    // The safe area: the drawable less the strips, in whole pixels.
    Screen with = s;
    with.insets = insets;
    CHECK(same(a, expected_safe(with)));
    // The image lies wholly inside the safe area, in whole pixels.
    CHECK(r.x >= a.x);
    CHECK(r.y >= a.y);
    CHECK(r.x + r.w <= a.x + a.w);
    CHECK(r.y + r.h <= a.y + a.h);
    CHECK(r.x == std::floor(r.x));
    CHECK(r.y == std::floor(r.y));
    CHECK(r.w == std::floor(r.w));
    CHECK(r.h == std::floor(r.h));
    // Its size is the mode's for the safe area.
    const int sw = static_cast<int>(a.w), sh = static_cast<int>(a.h);
    const int k = std::min(sw / 480, sh / 320);
    if (mode == ScaleMode::kInteger && k >= 1) {
        CHECK(p.integer_scale == k);
        CHECK(r.w == static_cast<float>(480 * k));
        CHECK(r.h == static_cast<float>(320 * k));
    } else {
        CHECK(p.integer_scale == 0);
        const fe::Rect f = fe::fit_rect(sw, sh, 480 * render_scale, 320 * render_scale);
        CHECK(r.w == f.w);
        CHECK(r.h == f.h);
        CHECK((r.w == a.w || r.h == a.h)); // it fills the safe area on one axis
    }
    // As centred in the whole drawable as the safe area allows: unmoved where it fits, else
    // against the strip it would overlap.
    CHECK(r.x == std::clamp(centred(dw, r.w), a.x, a.x + a.w - r.w));
    CHECK(r.y == std::clamp(centred(dh, r.h), a.y, a.y + a.h - r.h));

    // The touch inverse: window points over the image map to screen points and back; each one
    // is inside the safe area in window points.
    int failures = 0;
    for (float sy = 0; sy < 320; sy += 9.75f) {
        for (float sx = 0; sx < 480; sx += 13.5f) {
            float wx = 0, wy = 0;
            fe::screen_to_window(p, sx, sy, d, d, &wx, &wy);
            const fe::ScreenPoint back = fe::window_to_screen(p, wx, wy, d, d);
            float wx2 = 0, wy2 = 0;
            fe::screen_to_window(p, back.x, back.y, d, d, &wx2, &wy2);
            const bool in_safe = wx >= insets.left - 1e-3f && wy >= insets.top - 1e-3f &&
                                 wx <= static_cast<float>(s.w) - insets.right + 1e-3f &&
                                 wy <= static_cast<float>(s.h) - insets.bottom + 1e-3f;
            if (!back.inside || !in_safe || std::fabs(back.x - sx) > 1e-3f ||
                std::fabs(back.y - sy) > 1e-3f || std::fabs(wx2 - wx) > 1e-3f ||
                std::fabs(wy2 - wy) > 1e-3f) {
                failures++;
            }
        }
    }
    CHECK(failures == 0);
    // A touch in a strip is over the bars: outside the image.
    const float mid_y = (r.y + r.h / 2) / d;
    const float mid_x = (r.x + r.w / 2) / d;
    if (insets.left >= 1) {
        CHECK_FALSE(fe::window_to_screen(p, insets.left / 2, mid_y, d, d).inside);
    }
    if (insets.right >= 1) {
        CHECK_FALSE(fe::window_to_screen(p, static_cast<float>(s.w) - insets.right / 2, mid_y, d, d)
                        .inside);
    }
    if (insets.top >= 1) {
        CHECK_FALSE(fe::window_to_screen(p, mid_x, insets.top / 2, d, d).inside);
    }
    if (insets.bottom >= 1) {
        CHECK_FALSE(
            fe::window_to_screen(p, mid_x, static_cast<float>(s.h) - insets.bottom / 2, d, d)
                .inside);
    }
}

} // namespace

TEST_CASE("safe_area: the image lies inside the safe area in every mode, both orientations") {
    for (const Screen &s : kScreens) {
        const int dw = drawable(s.w, s.density), dh = drawable(s.h, s.density);
        for (const ScaleMode mode : kModes) {
            for (const int render_scale : {1, 2}) {
                const int iw = 480 * render_scale, ih = 320 * render_scale;
                // Landscape left, and landscape right (the insets mirrored).
                const fe::Placement left =
                    fe::place(mode, dw, dh, iw, ih, s.insets, s.density, s.density);
                const fe::Placement right =
                    fe::place(mode, dw, dh, iw, ih, mirrored(s.insets), s.density, s.density);
                check_placement(s, s.insets, mode, render_scale, left);
                check_placement(s, mirrored(s.insets), mode, render_scale, right);
                // The two are mirror images: the same size, and x mirrored (to the pixel where
                // the strips moved the image; within the odd bar's pixel where they did not).
                INFO(s.name << ", " << fe::scale_mode_name(mode) << ": left (" << text(left.rect)
                            << "), right (" << text(right.rect) << ")");
                CHECK(left.rect.w == right.rect.w);
                CHECK(left.rect.h == right.rect.h);
                CHECK(left.rect.y == right.rect.y);
                const float mirror_x = static_cast<float>(dw) - right.rect.x - right.rect.w;
                CHECK(std::fabs(left.rect.x - mirror_x) <= 1);
                if (left.rect.x != centred(dw, left.rect.w)) { // a strip moved it
                    CHECK(left.rect.x == mirror_x);
                }
            }
        }
    }
}

TEST_CASE("safe_area: worked examples on the iPhone 15 (Dynamic Island) and a one-sided notch") {
    // 2556x1179 pixels; insets 59, 59, 21 points: the safe area is 2202x1116 at (177, 0).
    const Insets phone{0, 59, 21, 59};
    fe::Placement p = fe::place(ScaleMode::kInteger, 2556, 1179, 480, 320, phone, 3, 3);
    CHECK(same(p.safe, fe::Rect{177, 0, 2202, 1116}));
    // Integer 3x (as without insets): 1440x960, centred at (558, 109), clear of every strip.
    CHECK(p.integer_scale == 3);
    CHECK(same(p.rect, fe::Rect{558, 109, 1440, 960}));
    CHECK(same(p.rect, fe::place(ScaleMode::kInteger, 2556, 1179, 480, 320).rect));
    // Fit: without insets 1769x1179 at (393, 0), under the island and the home indicator; with
    // them 1674x1116 at (441, 0), above the home indicator's strip.
    CHECK(
        same(fe::place(ScaleMode::kFit, 2556, 1179, 480, 320).rect, fe::Rect{393, 0, 1769, 1179}));
    p = fe::place(ScaleMode::kFit, 2556, 1179, 480, 320, phone, 3, 3);
    CHECK(same(p.rect, fe::Rect{441, 0, 1674, 1116}));
    // 1674 / 480 = 3.4875 drawable pixels per screen point: drawable pixel (441 + 3.4875 x 106,
    // 3.4875 x 165), a third of it in window points, is screen point (106, 165).
    fe::ScreenPoint s = fe::window_to_screen(p, (441 + 3.4875f * 106) / 3, 3.4875f * 165 / 3, 3, 3);
    CHECK(s.inside);
    CHECK(s.x == doctest::Approx(106));
    CHECK(s.y == doctest::Approx(165));

    // A notch on one side only, 1000x700 at 1x: integer 2x needs 960 of the 970 points left.
    // Centred it would be at x 20, under a 30-point strip: moved to 30 on the left, and in the
    // other orientation it stays at 20 (the strip on the right ends at 970, the image at 980:
    // moved to 10).
    p = fe::place(ScaleMode::kInteger, 1000, 700, 480, 320, Insets{0, 30, 0, 0}, 1, 1);
    CHECK(same(p.rect, fe::Rect{30, 30, 960, 640}));
    p = fe::place(ScaleMode::kInteger, 1000, 700, 480, 320, Insets{0, 0, 0, 30}, 1, 1);
    CHECK(same(p.rect, fe::Rect{10, 30, 960, 640}));
    // A strip under 20 points does not move it.
    p = fe::place(ScaleMode::kInteger, 1000, 700, 480, 320, Insets{0, 20, 0, 0}, 1, 1);
    CHECK(same(p.rect, fe::Rect{20, 30, 960, 640}));
    // Strips that leave no room: nothing to draw into.
    p = fe::place(ScaleMode::kFit, 1000, 700, 480, 320, Insets{0, 600, 0, 400}, 1, 1);
    CHECK(p.rect.w == 0);
    CHECK(p.safe.w == 0);
    CHECK_FALSE(fe::window_to_screen(p, 500, 350, 1, 1).inside);
    // Rounded up to whole pixels: 33.3 points at 1.5x are 49.95 pixels, so 50.
    CHECK(same(fe::safe_rect(1800, 1200, Insets{0, 33.3f, 0, 0}, 1.5f, 1.5f),
               fe::Rect{50, 0, 1750, 1200}));
    // 59 points at 3x are exactly 177 (no float noise turns them into 178).
    CHECK(fe::safe_rect(2556, 1179, Insets{0, 59, 0, 0}, 3, 3).x == 177);
    // Negative, non-finite and unknown-density sides: 0, and density 1.
    CHECK(same(fe::safe_rect(1000, 700, Insets{-5, NAN, INFINITY, 10}, 0, -1),
               fe::Rect{0, 0, 990, 700}));
}

TEST_CASE("safe_area: zero insets give exactly the placement without them (the desktop)") {
    std::mt19937 rng(73);
    std::uniform_int_distribution<int> side(1, 4000);
    std::vector<std::pair<int, int>> sizes = {{1920, 1080}, {2560, 1440}, {2880, 1800},
                                              {960, 640},   {1001, 667},  {959, 639},
                                              {300, 300},   {0, 0},       {480, 320}};
    for (int i = 0; i < 300; i++) {
        sizes.emplace_back(side(rng), side(rng));
    }
    for (const auto &[w, h] : sizes) {
        for (const ScaleMode mode : kModes) {
            for (const int render_scale : {1, 3}) {
                for (const float d : {1.0f, 2.0f, 3.0f}) {
                    const fe::Placement a =
                        fe::place(mode, w, h, 480 * render_scale, 320 * render_scale);
                    const fe::Placement b = fe::place(mode, w, h, 480 * render_scale,
                                                      320 * render_scale, Insets{}, d, d);
                    INFO(w << "x" << h << " " << fe::scale_mode_name(mode) << " (" << text(a.rect)
                           << ") vs (" << text(b.rect) << ")");
                    CHECK(same(a.rect, b.rect));
                    CHECK(a.integer_scale == b.integer_scale);
                    CHECK(a.filter == b.filter);
                    if (w > 0 && h > 0) {
                        CHECK(same(b.safe,
                                   fe::Rect{0, 0, static_cast<float>(w), static_cast<float>(h)}));
                    }
                }
            }
        }
    }
}

TEST_CASE("safe_area: the focus highlight (#69) stays inside the image, so inside the safe area") {
    // focus_highlight maps a screen rectangle through the placement and clamps it to the image:
    // with a safe-area placement the outline never reaches into a strip, even for a control on
    // the screen's edge with the outset around it.
    struct Box {
        float x, y, w, h;
    };
    const Box boxes[] = {{0, 0, 480, 320}, {0, 0, 40, 30},     {440, 290, 40, 30},
                         {0, 150, 20, 20}, {460, 150, 20, 20}, {200, 100, 80, 40}};
    for (const Screen &s : kScreens) {
        const int dw = drawable(s.w, s.density), dh = drawable(s.h, s.density);
        for (const ScaleMode mode : kModes) {
            for (const Insets &in : {s.insets, mirrored(s.insets)}) {
                const fe::Placement p = fe::place(mode, dw, dh, 480, 320, in, s.density, s.density);
                const float width = fe::focus_highlight_width(p);
                for (const Box &b : boxes) {
                    const fe::Rect r = fe::focus_highlight(p, b.x, b.y, b.w, b.h, width);
                    INFO(s.name << ", " << fe::scale_mode_name(mode) << ": highlight (" << text(r)
                                << "), image (" << text(p.rect) << "), safe (" << text(p.safe)
                                << ")");
                    REQUIRE(r.w > 0);
                    CHECK(r.x >= p.rect.x);
                    CHECK(r.y >= p.rect.y);
                    CHECK(r.x + r.w <= p.rect.x + p.rect.w);
                    CHECK(r.y + r.h <= p.rect.y + p.rect.h);
                    CHECK(r.x >= p.safe.x);
                    CHECK(r.x + r.w <= p.safe.x + p.safe.w);
                    CHECK(r.y >= p.safe.y);
                    CHECK(r.y + r.h <= p.safe.y + p.safe.h);
                }
            }
        }
    }
}

TEST_CASE("safe_area: --safe-area T,L,B,R") {
    Insets in;
    CHECK(fe::parse_insets("0,59,21,59", &in));
    CHECK(in == Insets{0, 59, 21, 59});
    CHECK(in.any());
    CHECK(fe::parse_insets("1.5,0,0.25,4096", &in));
    CHECK(in == Insets{1.5f, 0, 0.25f, 4096});
    CHECK(fe::parse_insets("0,0,0,0", &in));
    CHECK_FALSE(in.any());
    const Insets before = in;
    for (const char *bad :
         {"", "0,0,0", "0,0,0,0,0", "-1,0,0,0", "+1,0,0,0", "1e2,0,0,0", "1.,0,0,0", ".5,0,0,0",
          "0, 0,0,0", "0,0,0,0 ", "4096.5,0,0,0", "99999999999999999999,0,0,0", "a,b,c,d",
          "0;0;0;0", "0,0,,0", "nan,0,0,0"}) {
        INFO(bad);
        CHECK_FALSE(fe::parse_insets(bad, &in));
        CHECK(in == before);
    }
}

TEST_CASE("safe_area: a presenter with forced insets draws the bars over the strips") {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    REQUIRE_MESSAGE(SDL_CreateWindowAndRenderer("safe_area_test", 1000, 700, SDL_WINDOW_HIDDEN,
                                                &window, &renderer),
                    SDL_GetError());
    // The dummy driver (like every desktop window) reports no insets.
    CHECK(fe::window_insets(window) == Insets{});
    CHECK(fe::window_insets(nullptr) == Insets{});

    // A synthetic image whose every texel has its own colour.
    std::vector<std::uint8_t> img(480 * 320 * 4);
    for (std::uint32_t y = 0; y < 320; y++) {
        for (std::uint32_t x = 0; x < 480; x++) {
            std::uint8_t *px = &img[(y * 480 + x) * 4];
            px[0] = static_cast<std::uint8_t>(x * 7 + 1);
            px[1] = static_cast<std::uint8_t>(y * 5 + 3);
            px[2] = static_cast<std::uint8_t>((x + y) * 3 + 50);
            px[3] = 255;
        }
    }
    fe::Presenter presenter;
    REQUIRE(presenter.start(renderer, 480, 320));
    const fe::Color border{0x20, 0x30, 0x40};
    for (const ScaleMode mode : kModes) {
        for (const Insets &in :
             {Insets{}, Insets{0, 30, 0, 0}, Insets{0, 0, 0, 30}, Insets{40, 59, 21, 59}}) {
            INFO(fe::scale_mode_name(mode)
                 << ", insets " << in.top << "," << in.left << "," << in.bottom << "," << in.right);
            presenter.set_scaling(mode, border);
            presenter.set_forced_insets(in);
            REQUIRE(presenter.upload(img.data(), 480 * 4));
            REQUIRE(presenter.draw());
            CHECK(presenter.insets() == in);
            const fe::Placement &p = presenter.placement();
            CHECK(same(p.rect, fe::place(mode, 1000, 700, 480, 320, in, 1, 1).rect));
            // Every pixel outside the image has the border colour (the strips included), every
            // one inside shows the image.
            std::string report;
            CHECK_MESSAGE(presenter.check_readback(img.data(), 480 * 4, &report) == 0, report);
            CHECK(p.rect.x >= in.left);
            CHECK(p.rect.x + p.rect.w <= 1000 - in.right);
            CHECK(p.rect.y >= in.top);
            CHECK(p.rect.y + p.rect.h <= 700 - in.bottom);
        }
    }
    // Back to the window's own (none): the desktop placement.
    presenter.set_forced_insets(std::nullopt);
    presenter.set_scaling(ScaleMode::kInteger, border);
    REQUIRE(presenter.draw());
    CHECK(same(presenter.placement().rect, fe::Rect{20, 30, 960, 640}));
    CHECK(presenter.insets() == Insets{});
    presenter.stop();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
