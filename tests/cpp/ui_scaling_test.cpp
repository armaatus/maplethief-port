// UI scaling across window sizes and modes (ADR 0103), synthetic data only:
//
//   the matrix   window sizes from 480 x 320 to 3840 x 2160 points in 3:2, 4:3, 16:10, 16:9 and
//                21:9 (and an odd size), at densities 1, 1.5, 2 and 3, in every scale mode,
//                widescreen off and on, render scale 1 to 3, with and without a phone's safe
//                area: the picture's rectangles in whole pixels, centred, inside the safe area,
//                the wings flush with the screen image (no seam), and the mouse mapping: every
//                edge pixel of the image hits a screen point inside it, the pixel beyond it
//                none, screen -> window -> screen round trips, the wings' edges map to the wide
//                picture's edge points, and the gear stays inside the drawable and the safe area.
//   text         the port's window-only text (scaling.h's text_frame, place_text_box): its scale
//                follows the density and the picture, a box shrinks until it fits and stays
//                inside the safe area; the frame-time overlay's notice and the cash shop's price
//                line drawn headless at 1080p (2x font) and with a safe area.
//   prompts and the price line (ADR 0114) across the matrix: each button prompt's box on whole
//                pixels inside the placed image, its glyph unit the picture's pixels a point
//                rounded; the price line's box on whole pixels, inside the safe area, and wholly
//                below the picture or wholly inside it at every text scale.
//   the veil     widescreen's wings darkened as the overlays' backdrop darkens the image while
//                the settings menu or the remap overlay is open, the screen image untouched.
//   the EXP figure (#191, ADR 0109) across the matrix: its box on whole pixels inside the placed
//                image, above the HUD's plate between its left end and the level badge, and clear
//                of the minimap (the compass overlay and the corner) and of every key and button
//                label of the field's pad (the original layout, widescreen's edges and narrow).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "canvas.h"
#include "exp_percent.h"
#include "hud_labels.h"
#include "keypad_layout.h"
#include "minimap.h"
#include "perf_overlay.h"
#include "present.h"
#include "price_line.h"
#include "prompts.h"
#include "scaling.h"
#include "settings_menu.h"

namespace {

namespace fe = maplethief::frontend;
using fe::ScaleMode;

constexpr ScaleMode kModes[] = {ScaleMode::kInteger, ScaleMode::kFit, ScaleMode::kSmooth,
                                ScaleMode::kSharp};

struct Size {
    int w, h;
};

// Window sizes in points: 3:2 (the game's own, at 1x and 2x), 4:3, 16:10, 16:9, 21:9, odd.
constexpr Size kWindows[] = {{480, 320},   {960, 640},   {640, 480},   {1024, 768},
                             {1280, 800},  {1440, 900},  {1366, 768},  {1920, 1080},
                             {2560, 1080}, {3440, 1440}, {3840, 2160}, {1001, 701}};
constexpr float kDensities[] = {1.0f, 1.5f, 2.0f, 3.0f};

bool whole(float v) { return v == std::floor(v); }

bool whole(const fe::Rect &r) { return whole(r.x) && whole(r.y) && whole(r.w) && whole(r.h); }

bool inside(const fe::Rect &in, const fe::Rect &out) {
    return in.x >= out.x && in.y >= out.y && in.x + in.w <= out.x + out.w &&
           in.y + in.h <= out.y + out.h;
}

std::string text(const fe::Rect &r) {
    return std::to_string(r.x) + ", " + std::to_string(r.y) + ", " + std::to_string(r.w) + ", " +
           std::to_string(r.h);
}

// A headless window (SDL's dummy video driver: the software renderer).
struct Window {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    Window(int w, int h) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        REQUIRE_MESSAGE(SDL_CreateWindowAndRenderer("ui_scaling_test", w, h, 0, &window, &renderer),
                        SDL_GetError());
    }
    ~Window() {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;
};

// The read-back as R, G, B, A bytes.
struct Readback {
    SDL_Surface *s = nullptr;
    explicit Readback(SDL_Renderer *renderer) {
        SDL_Surface *read = SDL_RenderReadPixels(renderer, nullptr);
        REQUIRE_MESSAGE(read != nullptr, SDL_GetError());
        s = SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888);
        SDL_DestroySurface(read);
        REQUIRE_MESSAGE(s != nullptr, SDL_GetError());
    }
    ~Readback() { SDL_DestroySurface(s); }
    Readback(const Readback &) = delete;
    Readback &operator=(const Readback &) = delete;
    const std::uint8_t *at(int x, int y) const {
        return static_cast<const std::uint8_t *>(s->pixels) +
               static_cast<std::size_t>(y) * s->pitch + static_cast<std::size_t>(x) * 4;
    }
};

long g_price_skipped = 0; // price boxes wider or taller than the picture at scale 1

// The button prompts' boxes (ADR 0114): whole pixels inside the placed image at every edge, for
// glyphs of a few widths, on either side of a control.
void check_prompts(const fe::Placement &p) {
    const int unit = fe::prompt_unit(p.rect.w / 480.0f);
    CHECK(unit == std::clamp(static_cast<int>(std::lround(p.rect.w / 480.0f)), 1, 8));
    for (const int gw : {12, 19, 40}) {
        for (const int before : {0, 3, 240, 470, 480}) {
            for (const int y : {0, 6, 160, 314, 320}) {
                for (const bool right : {false, true}) {
                    const fe::Prompt pr{MT_ACTION_OK, before, before + 10, y, right};
                    const fe::PromptBox b = fe::prompt_box(pr, gw, p.rect.w, p.rect.h);
                    CAPTURE(gw);
                    CAPTURE(before);
                    CAPTURE(y);
                    CAPTURE(right);
                    CHECK(b.unit == unit);
                    CHECK(b.w == (gw + 2) * unit);
                    CHECK(b.h == (fe::kButtonHeight + 2) * unit);
                    const fe::Rect abs{p.rect.x + static_cast<float>(b.x),
                                       p.rect.y + static_cast<float>(b.y), static_cast<float>(b.w),
                                       static_cast<float>(b.h)};
                    CHECK(whole(abs));
                    CHECK(inside(abs, p.rect));
                }
            }
        }
    }
}

// The price line's box (ADR 0114) at every text scale: whole pixels, inside the safe area, and
// either wholly below the picture or wholly inside it, never across its edge.
void check_price_line(const fe::TextFrame &tf, const fe::Rect &picture) {
    // One line (the heading), three, and the five a full shop's quotes wrap to; the widest line.
    for (const int lines : {1, 3, 5}) {
        for (int scale = 1; scale <= 8; scale++) {
            fe::TextFrame f = tf;
            f.scale = scale;
            std::vector<std::string> body(static_cast<std::size_t>(lines), std::string(56, 'x'));
            float w = 0, h = 0;
            fe::price_line_size(body, &w, &h);
            bool below = false;
            const fe::TextBox t = fe::place_price_line(f, picture, w, h, &below);
            const float k = static_cast<float>(t.scale);
            const fe::Rect px{t.box.x * k, t.box.y * k, t.box.w * k, t.box.h * k};
            CAPTURE(lines);
            CAPTURE(scale);
            INFO("price box (" << text(px) << ")");
            CHECK(whole(px));
            CHECK(t.scale <= scale);
            const float pic_bottom = picture.y + picture.h;
            if (below) {
                CHECK(t.scale == scale);
                CHECK(px.y >= pic_bottom);
                CHECK(inside(px, f.area));
                continue;
            }
            // Inside: the part of the safe area the picture covers.
            const float x0 = std::max(f.area.x, picture.x), y0 = std::max(f.area.y, picture.y);
            const float x1 = std::min(f.area.x + f.area.w, picture.x + picture.w);
            const float y1 = std::min(f.area.y + f.area.h, pic_bottom);
            if (w + 4 > x1 - x0 || h + 4 > y1 - y0) {
                g_price_skipped++; // no room at scale 1 even (a synthetic 1x phone)
                continue;
            }
            CHECK(inside(px, fe::Rect{x0, y0, x1 - x0, y1 - y0}));
            // Never across the picture's bottom edge.
            CHECK((px.y + px.h <= pic_bottom || px.y >= pic_bottom));
        }
    }
}

bool overlaps(const fe::Rect &a, const fe::Rect &b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// The EXP figure's box (#191, ADR 0109): whole pixels inside the image, above the HUD's plate
// between its left end and the level badge, clear of the minimap and the pad's keys and labels.
void check_exp_percent(const fe::Placement &p, const fe::Rect &wide, bool widescreen) {
    const std::string t = "EXP 99.99%"; // the widest figure
    const fe::Rect r = fe::exp_percent_rect(p.rect, t);
    INFO("exp box (" << text(r) << ")");
    CHECK(whole(r));
    CHECK(inside(r, p.rect));
    const float sx = p.rect.w / 480.0f, sy = p.rect.h / 320.0f;
    const fe::Rect pt{(r.x - p.rect.x) / sx, (r.y - p.rect.y) / sy, r.w / sx, r.h / sy};
    CHECK(pt.x >= fe::kExpPercentLeft);
    CHECK(pt.x + pt.w <= fe::kExpPercentRight + 0.5f);
    CHECK(pt.y + pt.h <= fe::kExpPercentBottom + 0.5f);
    CHECK(pt.h <= 9.5f);
    // The minimap, as overlay at the compass and in the corner (a 400 x 300 map).
    for (const fe::MinimapAnchor anchor :
         {fe::MinimapAnchor::kCompass, fe::MinimapAnchor::kCorner}) {
        const fe::MinimapLayout m = fe::minimap_layout(
            fe::Minimap::kOverlay, anchor, p, wide,
            fe::minimap_visible(p, wide, widescreen ? 568 : 480, 568), 400 * 8, 300 * 8);
        CHECK_FALSE(overlaps(m.box, r));
    }
    // The pad's keys and their labels (a glyph of 12 x 12 points beside each).
    for (const bool anchored : {true, false}) {
        const fe::KeypadWide kw =
            widescreen ? fe::keypad_wide(568, 568, anchored) : fe::KeypadWide{};
        for (const fe::HudLabel &l : fe::hud_labels(fe::KeypadLayout{}, kw, 1)) {
            const fe::Rect key{l.key.x, l.key.y, l.key.w, l.key.h};
            const fe::KeypadRect b = fe::hud_label_box(l, 12, 12, -44, 524);
            CHECK_FALSE(overlaps(key, pt));
            CHECK_FALSE(overlaps(fe::Rect{b.x, b.y, b.w, b.h}, pt));
        }
    }
}

// The checks of one configuration of the matrix.
void check_config(ScaleMode mode, Size window, float density, bool wide, int render_scale,
                  const fe::Insets &insets) {
    const int ow = static_cast<int>(std::lround(static_cast<float>(window.w) * density));
    const int oh = static_cast<int>(std::lround(static_cast<float>(window.h) * density));
    const float dx = static_cast<float>(ow) / static_cast<float>(window.w);
    const float dy = static_cast<float>(oh) / static_cast<float>(window.h);
    const int points = wide ? 568 : 480;
    const fe::Placement wp =
        fe::place(mode, ow, oh, points * render_scale, 320 * render_scale, insets, dx, dy, points);
    REQUIRE(wp.rect.w > 0);
    // As Presenter::draw: the screen image is the wide picture's middle 480 points.
    fe::Placement p = wp;
    if (wide) {
        p.rect = fe::wide_part(wp.rect, 568, 480);
    }
    INFO("picture (" << text(wp.rect) << "), image (" << text(p.rect) << "), safe (" << text(p.safe)
                     << ")");
    // Whole pixels, inside the safe area and the drawable: no half-covered pixel at a bar.
    CHECK(whole(wp.rect));
    CHECK(whole(p.rect));
    CHECK(inside(wp.rect, p.safe));
    CHECK(inside(wp.rect, fe::Rect{0, 0, static_cast<float>(ow), static_cast<float>(oh)}));
    if (!insets.any()) {
        // Centred: the bars on either side differ by a pixel at most.
        CHECK(std::fabs(wp.rect.x - (static_cast<float>(ow) - wp.rect.x - wp.rect.w)) <= 1);
        CHECK(std::fabs(wp.rect.y - (static_cast<float>(oh) - wp.rect.y - wp.rect.h)) <= 1);
    }
    if (wp.integer_scale > 0) {
        CHECK(wp.rect.w == static_cast<float>(points * wp.integer_scale));
        CHECK(wp.rect.h == static_cast<float>(320 * wp.integer_scale));
        CHECK(p.rect.w == static_cast<float>(480 * wp.integer_scale));
    } else {
        // Fit: one axis fills the safe area; the aspect ratio within a pixel.
        CHECK((wp.rect.w == p.safe.w || wp.rect.h == p.safe.h));
        CHECK(std::fabs(wp.rect.w - wp.rect.h * static_cast<float>(points) / 320.0f) <= 1);
    }
    // The wings flush with the screen image: no seam, no overlap.
    const fe::WingRects wings = fe::wing_rects(wp.rect, p.rect);
    if (wide) {
        CHECK(wings.left.w > 0);
        CHECK(wings.right.w > 0);
        CHECK(wings.left.x == wp.rect.x);
        CHECK(wings.left.x + wings.left.w == p.rect.x);
        CHECK(wings.right.x == p.rect.x + p.rect.w);
        CHECK(wings.right.x + wings.right.w == wp.rect.x + wp.rect.w);
        // The wings' outer pixels map to the wide picture's edge points (44 points beyond the
        // screen), within a point.
        // (A pixel's worth of points, plus the edges' rounding to whole pixels.)
        const float pixel = 480.0f / p.rect.w;
        const fe::ScreenPoint l = fe::drawable_to_screen(p, wp.rect.x + 0.5f, p.rect.y + 1);
        const fe::ScreenPoint r =
            fe::drawable_to_screen(p, wp.rect.x + wp.rect.w - 0.5f, p.rect.y + 1);
        CHECK(std::fabs(l.x - (-44.0f)) < 1.0f + pixel);
        CHECK(std::fabs(r.x - (480.0f + 44.0f)) < 1.0f + pixel);
        CHECK_FALSE(l.inside);
    } else {
        CHECK(wings.left.w == 0);
        CHECK(wings.right.w == 0);
    }
    // Hit-testing: every edge pixel of the image hits a point inside the screen, the pixels
    // beyond it none, and the rows and columns in between in order.
    const float x0 = p.rect.x, y0 = p.rect.y, x1 = p.rect.x + p.rect.w, y1 = p.rect.y + p.rect.h;
    for (const float py : {y0 + 0.5f, (y0 + y1) / 2, y1 - 0.5f}) {
        CHECK(fe::drawable_to_screen(p, x0 + 0.5f, py).inside);
        CHECK(fe::drawable_to_screen(p, x1 - 0.5f, py).inside);
        CHECK_FALSE(fe::drawable_to_screen(p, x0 - 0.5f, py).inside);
        CHECK_FALSE(fe::drawable_to_screen(p, x1 + 0.5f, py).inside);
    }
    for (const float px : {x0 + 0.5f, (x0 + x1) / 2, x1 - 0.5f}) {
        CHECK(fe::drawable_to_screen(p, px, y0 + 0.5f).inside);
        CHECK(fe::drawable_to_screen(p, px, y1 - 0.5f).inside);
        CHECK_FALSE(fe::drawable_to_screen(p, px, y0 - 0.5f).inside);
        CHECK_FALSE(fe::drawable_to_screen(p, px, y1 + 0.5f).inside);
    }
    // Screen -> window -> screen, in window points with the density.
    for (const float sx : {0.0f, 1.0f, 106.0f, 240.0f, 420.0f, 479.5f}) {
        for (const float sy : {0.0f, 79.0f, 165.0f, 319.5f}) {
            float wx = 0, wy = 0;
            fe::screen_to_window(p, sx, sy, dx, dy, &wx, &wy);
            const fe::ScreenPoint back = fe::window_to_screen(p, wx, wy, dx, dy);
            CHECK(std::fabs(back.x - sx) < 1e-3f);
            CHECK(std::fabs(back.y - sy) < 1e-3f);
            CHECK(back.inside);
        }
    }
    // The gear: inside the drawable and the safe area, never over the screen image unless no
    // bar fits (over_image; with widescreen it may sit over the wings' corner, ADR 0090).
    const fe::GearPlace gear = fe::gear_place(p, wp, dy);
    CHECK(inside(gear.rect, p.safe));
    if (!gear.over_image) {
        const bool overlaps = gear.rect.x < x1 && gear.rect.x + gear.rect.w > x0 &&
                              gear.rect.y < y1 && gear.rect.y + gear.rect.h > y0;
        CHECK_FALSE(overlaps);
    }
    // The window text: at least the density's size, inside the safe area.
    const fe::TextFrame tf = fe::text_frame(p, dy, ow, oh);
    CHECK(tf.scale >= std::max(1L, std::lround(dy)));
    CHECK(tf.scale <= 8);
    const fe::TextBox box = fe::place_text_box(tf, 200, 40, fe::TextCorner::kBottomLeft, 2);
    const float k = static_cast<float>(box.scale);
    CHECK(inside(fe::Rect{box.box.x * k, box.box.y * k, box.box.w * k, box.box.h * k}, p.safe));
    check_prompts(p);
    check_price_line(tf, wp.rect);
    check_exp_percent(p, wp.rect, wide);
}

} // namespace

TEST_CASE("ui scaling: the picture, the mapping, the gear and the text across the matrix") {
    // An iPhone in landscape at 3x: the Dynamic Island's 59 points and the home indicator's 21.
    const fe::Insets phone{0, 59, 21, 59};
    long configs = 0;
    for (const Size window : kWindows) {
        for (const float density : kDensities) {
            for (const ScaleMode mode : kModes) {
                for (const bool wide : {false, true}) {
                    for (const int n : {1, 2, 3}) {
                        for (const bool notch : {false, true}) {
                            CAPTURE(window.w);
                            CAPTURE(window.h);
                            CAPTURE(density);
                            CAPTURE(fe::scale_mode_name(mode));
                            CAPTURE(wide);
                            CAPTURE(n);
                            CAPTURE(notch);
                            check_config(mode, window, density, wide, n,
                                         notch ? phone : fe::Insets{});
                            configs++;
                        }
                    }
                }
            }
        }
    }
    MESSAGE(configs << " configurations");
    CHECK(configs == 12 * 4 * 4 * 2 * 3 * 2);
    // The price line had room in the picture everywhere but in the 480 x 320 window at density
    // 1 with a phone's insets (a safe area 362 pixels wide).
    MESSAGE(g_price_skipped << " price-line boxes without room at scale 1");
    // (that one window and density: 4 modes x widescreen off and on x 3 render scales x 3 boxes x
    // 8 text scales).
    CHECK(g_price_skipped == 4 * 2 * 3 * 3 * 8);
}

TEST_CASE("ui scaling: the window text's scale follows the density and the picture") {
    struct Case {
        int ow, oh;
        float density;
        ScaleMode mode;
        int scale;
    };
    // Drawable pixels; the picture's pixels per point decides with the density.
    const Case cases[] = {
        {480, 320, 1, ScaleMode::kInteger, 1},    // the game's size: 8 pixels a line
        {960, 640, 1, ScaleMode::kInteger, 1},    // the default window at 1x: as before
        {1920, 1280, 2, ScaleMode::kInteger, 2},  // the default window on Retina: as before
        {1920, 1080, 1, ScaleMode::kInteger, 2},  // 1080p: 16 pixels, not 8
        {1920, 1080, 1, ScaleMode::kFit, 2},      // 1080p fit (3.375x)
        {2560, 1440, 1, ScaleMode::kInteger, 2},  // 1440p (4x)
        {3840, 2160, 1, ScaleMode::kInteger, 3},  // 4K at 100 % (6x): 24 pixels, not 8
        {3840, 2160, 1.5f, ScaleMode::kFit, 3},   // 4K at 150 % (6.75x)
        {3024, 1964, 2, ScaleMode::kInteger, 3},  // a 14" MacBook Pro fullscreen (6x)
        {2532, 1170, 3, ScaleMode::kInteger, 3},  // an iPhone at 3x (3x): the density's 3
        {5120, 2880, 1, ScaleMode::kInteger, 5},  // 5K at 100 % (9x)
        {15360, 8640, 1, ScaleMode::kInteger, 8}, // never above 8
    };
    for (const Case &c : cases) {
        CAPTURE(c.ow);
        CAPTURE(c.oh);
        CAPTURE(c.density);
        const fe::Placement p =
            fe::place(c.mode, c.ow, c.oh, 480, 320, fe::Insets{}, c.density, c.density);
        const fe::TextFrame f = fe::text_frame(p, c.density, c.ow, c.oh);
        CHECK(f.scale == c.scale);
        CHECK(f.area.w == static_cast<float>(c.ow));
    }
    // No picture yet (a minimised window): the density, and the whole output as the area.
    const fe::TextFrame none = fe::text_frame(fe::Placement{}, 2, 800, 600);
    CHECK(none.scale == 2);
    CHECK(none.area.w == 800);
    CHECK(none.area.h == 600);
}

TEST_CASE("ui scaling: a text box shrinks until it fits, inside the safe area's corner") {
    fe::TextFrame f;
    f.scale = 3;
    f.area = fe::Rect{100, 20, 960, 600};
    // Fits at 3: 2 font pixels in from the area's first whole font pixel (100 / 3 -> 34).
    fe::TextBox b = fe::place_text_box(f, 100, 40, fe::TextCorner::kTopLeft, 2);
    CHECK(b.scale == 3);
    CHECK(b.box.x == 36); // drawable 108
    CHECK(b.box.y == 9);  // 20 / 3 -> 7, + 2: drawable 27
    b = fe::place_text_box(f, 100, 40, fe::TextCorner::kBottomLeft, 2);
    CHECK(b.box.y + b.box.h == 204); // 620 / 3 -> 206, - 2: drawable 612
    b = fe::place_text_box(f, 100, 40, fe::TextCorner::kTopRight, 4);
    CHECK(b.box.x + b.box.w == 349); // 1060 / 3 -> 353, - 4: drawable 1047
    b = fe::place_text_box(f, 100, 40, fe::TextCorner::kCentre, 0);
    CHECK(b.box.x * 3 >= 100);
    CHECK(std::fabs((b.box.x + b.box.w / 2) * 3 - 580) <= 3);
    // The frame-time overlay's box (about 400 font pixels wide) at 3 needs 1212: 2 fits.
    b = fe::place_text_box(f, 400, 60, fe::TextCorner::kTopLeft, 2);
    CHECK(b.scale == 2);
    // Too tall at 2 as well: 1.
    b = fe::place_text_box(f, 400, 400, fe::TextCorner::kTopLeft, 2);
    CHECK(b.scale == 1);
    // Wider than the area at 1: kept at 1, from the corner (clipped on the far side).
    b = fe::place_text_box(f, 2000, 40, fe::TextCorner::kTopRight, 2);
    CHECK(b.scale == 1);
    CHECK(b.box.x == 102);
    // Every box inside the area.
    for (const fe::TextCorner c : {fe::TextCorner::kTopLeft, fe::TextCorner::kTopRight,
                                   fe::TextCorner::kBottomLeft, fe::TextCorner::kCentre}) {
        const fe::TextBox t = fe::place_text_box(f, 77, 23, c, 2);
        const float k = static_cast<float>(t.scale);
        CHECK(inside(fe::Rect{t.box.x * k, t.box.y * k, t.box.w * k, t.box.h * k}, f.area));
    }
    // Every corner lands on whole drawable pixels (crisp glyphs).
    for (const fe::TextCorner c : {fe::TextCorner::kTopLeft, fe::TextCorner::kTopRight,
                                   fe::TextCorner::kBottomLeft, fe::TextCorner::kCentre}) {
        const fe::TextBox t = fe::place_text_box(f, 77, 23, c, 2);
        CHECK(whole(t.box.x));
        CHECK(whole(t.box.y));
    }
}

TEST_CASE("ui scaling: the notice and the price line drawn at 1080p and inside a safe area") {
    Window w(1920, 1080);
    const auto dark = [](const std::uint8_t *p) { return p[0] < 128 && p[1] < 128; };
    const auto white = [](const std::uint8_t *p) {
        return p[0] == 255 && p[1] == 255 && p[2] == 255;
    };
    for (const bool notch : {false, true}) {
        CAPTURE(notch);
        const fe::Insets insets = notch ? fe::Insets{0, 100, 30, 0} : fe::Insets{};
        const fe::Placement p =
            fe::place(ScaleMode::kInteger, 1920, 1080, 480, 320, insets, 1.0f, 1.0f);
        const fe::TextFrame f = fe::text_frame(p, 1.0f, 1920, 1080);
        REQUIRE(f.scale == 2);
        const float left = notch ? 100.0f : 0.0f;
        const float bottom = notch ? 1050.0f : 1080.0f;
        // The notice "ABC": (3 x 8 + 2 x 4) x (8 + 2 x 4) font pixels, 2 in from the corner.
        SDL_SetRenderDrawColor(w.renderer, 255, 255, 255, 255);
        SDL_RenderClear(w.renderer);
        fe::PerfOverlay::draw_notice(w.renderer, "ABC", f);
        {
            const Readback rb(w.renderer);
            const int x0 = static_cast<int>(left) + 4;
            const int y1 = static_cast<int>(bottom) - 4; // one past the box's last row
            CHECK(dark(rb.at(x0, y1 - 1)));
            CHECK(dark(rb.at(x0 + 63, y1 - 32)));
            CHECK(white(rb.at(x0 - 1, y1 - 1)));
            CHECK(white(rb.at(x0 + 64, y1 - 1)));
            CHECK(white(rb.at(x0, y1)));
            CHECK(white(rb.at(x0, y1 - 33)));
        }
        // The renderer's scale is left as it was.
        float sx = 0, sy = 0;
        SDL_GetRenderScale(w.renderer, &sx, &sy);
        CHECK(sx == 1);
        CHECK(sy == 1);
        // The price line: the shop's lines, at 2x, inside the safe area.
        mt_cash_view view{};
        view.screen = MT_CASH_SCREEN_SHOP;
        view.meso = 590;
        view.count = 2;
        view.quotes[0].price = 10000;
        view.quotes[1].price = -1; // the meso packs: not sold
        const std::vector<std::string> lines = fe::price_lines(view);
        REQUIRE(!lines.empty());
        SDL_SetRenderDrawColor(w.renderer, 255, 255, 255, 255);
        SDL_RenderClear(w.renderer);
        fe::draw_price_line(w.renderer, view, f, p.rect);
        const Readback rb(w.renderer);
        // The picture 3x: 1440 x 960 from y 60 (no notch: a 60-pixel bar below it, which the
        // two lines fit at 2x) or moved up into the safe area (notch: 30 pixels, too few, so
        // inside the picture's bottom-left corner).
        const float pic_bottom = p.rect.y + p.rect.h;
        const int x0 = static_cast<int>(notch ? std::max(left, p.rect.x) : left) + 4;
        const int y1 = static_cast<int>(notch ? pic_bottom : bottom) - 4;
        const int h = (10 * static_cast<int>(lines.size()) - 2 + 8) * 2;
        CHECK(dark(rb.at(x0, y1 - 1)));
        CHECK(dark(rb.at(x0, y1 - h)));
        CHECK(white(rb.at(x0, y1 - h - 1)));
        CHECK(white(rb.at(x0 - 1, y1 - 1)));
        bool below = false;
        float bw = 0, bh = 0;
        fe::price_line_size(lines, &bw, &bh);
        fe::place_price_line(f, p.rect, bw, bh, &below);
        CHECK(below == !notch);
        if (!notch) {
            CHECK(y1 - h >= static_cast<int>(pic_bottom)); // wholly below the picture
        }
    }
}

TEST_CASE("ui scaling: the veil darkens widescreen's wings while an overlay is open") {
    Window w(1920, 1080);
    std::vector<std::uint8_t> gl(568 * 320 * 4);
    for (std::size_t i = 0; i < gl.size(); i += 4) {
        gl[i] = 0;
        gl[i + 1] = 255;
        gl[i + 2] = 255;
        gl[i + 3] = 255;
    }
    std::vector<std::uint8_t> image(480 * 320 * 4);
    for (std::size_t i = 0; i < image.size(); i += 4) {
        image[i] = 40;
        image[i + 1] = 80;
        image[i + 2] = 120;
        image[i + 3] = 255;
    }
    fe::Presenter p;
    REQUIRE(p.start(w.renderer, 480, 320));
    p.set_scaling(ScaleMode::kInteger, fe::Color{});
    p.set_wide(568);
    for (const std::uint8_t veil : {std::uint8_t{0}, fe::kOverlayBackdropAlpha}) {
        CAPTURE(static_cast<int>(veil));
        p.set_veil(veil);
        REQUIRE(p.upload_wings(gl.data(), 568, 320, 568));
        REQUIRE(p.upload(image.data(), 480 * 4));
        REQUIRE(p.draw());
        // 3x: the wide picture 1704 x 960 at (108, 60), the image 1440 wide at 240.
        CHECK(p.wide_rect().x == 108);
        CHECK(p.placement().rect.x == 240);
        const Readback rb(w.renderer);
        // The wings: as drawn, or 255 x (255 - 170) / 255 = 85 under the veil (within 2).
        const int g = veil == 0 ? 255 : 85;
        for (const int x : {108, 200, 239, 1680, 1811}) {
            CAPTURE(x);
            CHECK(std::abs(rb.at(x, 500)[1] - g) <= 2);
        }
        // The screen image and the bars are left alone (the overlay draws its own backdrop).
        for (const int x : {240, 960, 1679}) {
            CAPTURE(x);
            const std::uint8_t *q = rb.at(x, 500);
            CHECK((q[0] == 40 && q[1] == 80 && q[2] == 120));
        }
        CHECK(rb.at(50, 500)[1] == 0);
        CHECK(rb.at(960, 30)[1] == 0);
    }
    // Off again: the wings as drawn.
    p.set_veil(0);
    REQUIRE(p.draw());
    const Readback rb(w.renderer);
    CHECK(rb.at(200, 500)[1] == 255);
    p.stop();
}
