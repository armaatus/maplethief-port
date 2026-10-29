// Scaling (#59, ADR 0043) with synthetic data only: the placement of the 480x320 screen in every
// scale mode at 1920x1080, 2560x1440 and a 16:10 Retina window (1440x900 points, 2880x1800
// pixels), at odd and tiny sizes and render scale 2; the mouse mapping (window points -> drawable
// pixels -> screen points) and its forward map in every mode, density 1 and 2; settings.ini
// parsing, hostile input included; and headless read-backs (SDL's dummy video driver, the
// software renderer) of a presented synthetic image at the three sizes in every mode: the bars
// in the border colour exactly where expected, every output pixel of a nearest mode equal to the
// texel under it, and the smooth modes' bars, bounds and interior.
//
// The dummy driver has no HiDPI: its 2880x1800 window stands in for the 2880x1800-pixel drawable
// of a 1440x900-point Retina window. The density-2 half is the mapping's (pure arithmetic).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "present.h"
#include "scaling.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
using fe::Filter;
using fe::ScaleMode;

constexpr ScaleMode kModes[] = {ScaleMode::kInteger, ScaleMode::kFit, ScaleMode::kSmooth,
                                ScaleMode::kSharp};

struct Size {
    int w, h;
};
// 1080p, 1440p, and the drawable of a 16:10 Retina window (1440x900 points at 2x).
constexpr Size kSizes[] = {{1920, 1080}, {2560, 1440}, {2880, 1800}};

std::string text(const fe::Rect &r) {
    return std::to_string(r.x) + ", " + std::to_string(r.y) + ", " + std::to_string(r.w) + ", " +
           std::to_string(r.h);
}

void check_rect(const fe::Rect &r, float x, float y, float w, float h) {
    INFO("got (" << text(r) << ")");
    CHECK(r.x == x);
    CHECK(r.y == y);
    CHECK(r.w == w);
    CHECK(r.h == h);
}

} // namespace

TEST_CASE("scaling: mode names and border colours") {
    for (const ScaleMode m : kModes) {
        ScaleMode got{};
        REQUIRE(fe::parse_scale_mode(fe::scale_mode_name(m), &got));
        CHECK(got == m);
    }
    ScaleMode m = ScaleMode::kFit;
    CHECK(fe::parse_scale_mode("Integer", &m));
    CHECK(m == ScaleMode::kInteger);
    CHECK(fe::parse_scale_mode("SMOOTH", &m));
    CHECK(m == ScaleMode::kSmooth);
    for (const char *bad : {"", "int", "integers", " fit", "fit ", "bilinear", "nearest"}) {
        INFO(bad);
        CHECK_FALSE(fe::parse_scale_mode(bad, &m));
    }
    CHECK(m == ScaleMode::kSmooth); // unchanged by the failures

    fe::Color c;
    REQUIRE(fe::parse_color("#1E2a3B", &c));
    CHECK(c == fe::Color{0x1E, 0x2A, 0x3B});
    REQUIRE(fe::parse_color("#000000", &c));
    CHECK(c == fe::Color{});
    REQUIRE(fe::parse_color("#ffffff", &c));
    CHECK(c == fe::Color{255, 255, 255});
    for (const char *bad : {"", "#", "#12345", "#1234567", "123456", "#12345g", "# 12345",
                            "#-12345", "#0x1234", "red", "#FFF"}) {
        INFO(bad);
        CHECK_FALSE(fe::parse_color(bad, &c));
    }
    CHECK(c == fe::Color{255, 255, 255});
}

TEST_CASE("scaling: integer mode, the largest whole multiple of 480x320") {
    // 1080p: 3x (1440x960), bars on all four sides.
    fe::Placement p = fe::place(ScaleMode::kInteger, 1920, 1080, 480, 320);
    check_rect(p.rect, 240, 60, 1440, 960);
    CHECK(p.integer_scale == 3);
    CHECK(p.filter == Filter::kNearest);
    CHECK(p.mode == ScaleMode::kInteger);
    // 1440p: 4x.
    p = fe::place(ScaleMode::kInteger, 2560, 1440, 480, 320);
    check_rect(p.rect, 320, 80, 1920, 1280);
    CHECK(p.integer_scale == 4);
    // The 16:10 Retina window's drawable (1440x900 points at 2x): 5x.
    p = fe::place(ScaleMode::kInteger, 2880, 1800, 480, 320);
    check_rect(p.rect, 240, 100, 2400, 1600);
    CHECK(p.integer_scale == 5);
    // The same window without HiDPI: 2x.
    p = fe::place(ScaleMode::kInteger, 1440, 900, 480, 320);
    check_rect(p.rect, 240, 130, 960, 640);
    CHECK(p.integer_scale == 2);
    // Exact multiples: no bars.
    check_rect(fe::place(ScaleMode::kInteger, 480, 320, 480, 320).rect, 0, 0, 480, 320);
    check_rect(fe::place(ScaleMode::kInteger, 960, 640, 480, 320).rect, 0, 0, 960, 640);
    // Odd sizes: the bars' odd pixel goes right and below.
    check_rect(fe::place(ScaleMode::kInteger, 1001, 667, 480, 320).rect, 20, 13, 960, 640);
    check_rect(fe::place(ScaleMode::kInteger, 959, 639, 480, 320).rect, 239, 159, 480, 320);
    // One axis limits: tall and narrow, wide and flat.
    check_rect(fe::place(ScaleMode::kInteger, 1000, 3000, 480, 320).rect, 20, 1180, 960, 640);
    check_rect(fe::place(ScaleMode::kInteger, 5000, 700, 480, 320).rect, 2020, 30, 960, 640);

    // Render scale 2 (a 960x640 image): the same placements. k scales the screen points, not
    // the texels: at 3x each texel is 1.5 pixels.
    p = fe::place(ScaleMode::kInteger, 1920, 1080, 960, 640);
    check_rect(p.rect, 240, 60, 1440, 960);
    CHECK(p.integer_scale == 3);
    p = fe::place(ScaleMode::kInteger, 2560, 1440, 960, 640);
    check_rect(p.rect, 320, 80, 1920, 1280);
    p = fe::place(ScaleMode::kInteger, 2880, 1800, 960, 640);
    check_rect(p.rect, 240, 100, 2400, 1600);
    // Render scale 3 at 1080p: 3x, one pixel per texel.
    check_rect(fe::place(ScaleMode::kInteger, 1920, 1080, 1440, 960).rect, 240, 60, 1440, 960);

    // Smaller than 1x on either axis: the fit, nearest, integer_scale 0.
    p = fe::place(ScaleMode::kInteger, 479, 400, 480, 320);
    check_rect(p.rect, 0, 40, 479, 319);
    CHECK(p.integer_scale == 0);
    CHECK(p.filter == Filter::kNearest);
    p = fe::place(ScaleMode::kInteger, 600, 319, 480, 320);
    check_rect(p.rect, 60, 0, 479, 319);
    CHECK(p.integer_scale == 0);
    check_rect(fe::place(ScaleMode::kInteger, 100, 50, 480, 320).rect, 12, 0, 75, 50);
    check_rect(fe::place(ScaleMode::kInteger, 1, 1, 480, 320).rect, 0, 0, 1, 1);
    // Nothing to draw into (a minimised window), or nothing to draw.
    check_rect(fe::place(ScaleMode::kInteger, 0, 0, 480, 320).rect, 0, 0, 0, 0);
    check_rect(fe::place(ScaleMode::kInteger, -5, 640, 480, 320).rect, 0, 0, 0, 0);
    check_rect(fe::place(ScaleMode::kInteger, 960, 640, 0, 320).rect, 0, 0, 0, 0);
}

TEST_CASE("scaling: fit, smooth and sharp share the fit rectangle, with their filters") {
    struct Expected {
        Size output;
        int image_w, image_h;
        float x, y, w, h;
    };
    const Expected cases[] = {
        {{1920, 1080}, 480, 320, 150, 0, 1620, 1080}, // 1080p: 3.375x, pillarbox
        {{2560, 1440}, 480, 320, 200, 0, 2160, 1440}, // 1440p: 4.5x
        {{2880, 1800}, 480, 320, 90, 0, 2700, 1800},  // 16:10 Retina: 5.625x
        {{1440, 900}, 480, 320, 45, 0, 1350, 900},    // the same window at 1x
        {{1920, 1080}, 960, 640, 150, 0, 1620, 1080}, // render scale 2
        {{2880, 1800}, 960, 640, 90, 0, 2700, 1800},
        {{1001, 667}, 480, 320, 0, 0, 1001, 667},     // nearly 3:2: rounded, no bars
        {{1000, 3000}, 480, 320, 0, 1166, 1000, 667}, // letterbox
        {{300, 100}, 480, 320, 75, 0, 150, 100},      // smaller than the image
    };
    for (const ScaleMode mode : {ScaleMode::kFit, ScaleMode::kSmooth, ScaleMode::kSharp}) {
        for (const Expected &e : cases) {
            INFO(std::string(fe::scale_mode_name(mode))
                 << " at " << e.output.w << "x" << e.output.h << ", image " << e.image_w << "x"
                 << e.image_h);
            const fe::Placement p = fe::place(mode, e.output.w, e.output.h, e.image_w, e.image_h);
            check_rect(p.rect, e.x, e.y, e.w, e.h);
            CHECK(p.integer_scale == 0);
            CHECK(p.mode == mode);
            CHECK(p.filter == (mode == ScaleMode::kFit      ? Filter::kNearest
                               : mode == ScaleMode::kSmooth ? Filter::kLinear
                                                            : Filter::kPixelArt));
        }
        check_rect(fe::place(mode, 0, 100, 480, 320).rect, 0, 0, 0, 0);
    }
}

// --- the mouse mapping ------------------------------------------------------------------------

TEST_CASE("scaling: window points map to screen points in every mode, density 1 and 2") {
    struct Window {
        int w, h;      // points
        float density; // drawable = points x density
    };
    // 1080p and 1440p windows at 1x, the 16:10 Retina window (1440x900 at 2x), an odd one at 2x.
    const Window windows[] = {{1920, 1080, 1}, {2560, 1440, 1}, {1440, 900, 2}, {1001, 667, 2}};
    for (const ScaleMode mode : kModes) {
        for (const Window &win : windows) {
            for (const int render_scale : {1, 2}) {
                const int dw = static_cast<int>(static_cast<float>(win.w) * win.density);
                const int dh = static_cast<int>(static_cast<float>(win.h) * win.density);
                const fe::Placement p =
                    fe::place(mode, dw, dh, 480 * render_scale, 320 * render_scale);
                const fe::Rect &r = p.rect;
                const float d = win.density;
                INFO(std::string(fe::scale_mode_name(mode))
                     << ", window " << win.w << "x" << win.h << " at " << d << "x, render scale "
                     << render_scale << ", image at (" << text(r) << ")");
                // The image's top-left corner: screen (0, 0).
                fe::ScreenPoint s = fe::window_to_screen(p, r.x / d, r.y / d, d, d);
                CHECK(s.inside);
                CHECK(s.x == doctest::Approx(0).epsilon(1e-5));
                CHECK(s.y == doctest::Approx(0).epsilon(1e-5));
                // The centre of the image: screen (240, 160).
                s = fe::window_to_screen(p, (r.x + r.w / 2) / d, (r.y + r.h / 2) / d, d, d);
                CHECK(s.inside);
                CHECK(s.x == doctest::Approx(240).epsilon(1e-5));
                CHECK(s.y == doctest::Approx(160).epsilon(1e-5));
                // The image's last drawable pixel (its centre): just inside the bottom right.
                s = fe::window_to_screen(p, (r.x + r.w - 0.5f) / d, (r.y + r.h - 0.5f) / d, d, d);
                CHECK(s.inside);
                CHECK(s.x == doctest::Approx(480 - 0.5 * 480 / r.w).epsilon(1e-5));
                CHECK(s.y == doctest::Approx(320 - 0.5 * 320 / r.h).epsilon(1e-5));
                // The bottom-right edge itself is outside (the image covers [0, 480) x [0, 320)).
                s = fe::window_to_screen(p, (r.x + r.w) / d, (r.y + r.h) / d, d, d);
                CHECK_FALSE(s.inside);
                CHECK(s.x == doctest::Approx(480).epsilon(1e-5));
                // The window's corners: in the bars (outside) where there are bars.
                s = fe::window_to_screen(p, 0, 0, d, d);
                CHECK(s.inside == (r.x == 0 && r.y == 0));
                CHECK(s.x <= 0);
                CHECK(s.y <= 0);
                s = fe::window_to_screen(p, static_cast<float>(win.w) - 0.01f,
                                         static_cast<float>(win.h) - 0.01f, d, d);
                CHECK(s.inside ==
                      (r.x + r.w >= static_cast<float>(dw) && r.y + r.h >= static_cast<float>(dh)));
                if (r.x > 0) { // the left and right bars, at mid height
                    s = fe::window_to_screen(p, (r.x - 0.5f) / d, (r.y + r.h / 2) / d, d, d);
                    CHECK_FALSE(s.inside);
                    CHECK(s.x < 0);
                    CHECK(s.y == doctest::Approx(160).epsilon(1e-5));
                    s = fe::window_to_screen(p, (r.x + r.w + 0.5f) / d, (r.y + r.h / 2) / d, d, d);
                    CHECK_FALSE(s.inside);
                    CHECK(s.x > 480);
                }
                if (r.y > 0) { // the top and bottom bars
                    s = fe::window_to_screen(p, (r.x + r.w / 2) / d, (r.y - 0.5f) / d, d, d);
                    CHECK_FALSE(s.inside);
                    CHECK(s.y < 0);
                    s = fe::window_to_screen(p, (r.x + r.w / 2) / d, (r.y + r.h + 0.5f) / d, d, d);
                    CHECK_FALSE(s.inside);
                    CHECK(s.y > 320);
                }
                // Round trips: screen -> window -> screen on a grid of points (the --tap path),
                // and window -> screen -> window.
                int failures = 0;
                for (float sy = 0; sy < 320; sy += 7.25f) {
                    for (float sx = 0; sx < 480; sx += 11.5f) {
                        float wx = 0, wy = 0;
                        fe::screen_to_window(p, sx, sy, d, d, &wx, &wy);
                        const fe::ScreenPoint back = fe::window_to_screen(p, wx, wy, d, d);
                        float wx2 = 0, wy2 = 0;
                        fe::screen_to_window(p, back.x, back.y, d, d, &wx2, &wy2);
                        if (!back.inside || std::fabs(back.x - sx) > 1e-3f ||
                            std::fabs(back.y - sy) > 1e-3f || std::fabs(wx2 - wx) > 1e-3f ||
                            std::fabs(wy2 - wy) > 1e-3f) {
                            failures++;
                        }
                    }
                }
                CHECK(failures == 0);
            }
        }
    }

    // Worked examples. Integer mode, 1080p: the image at (240, 60), 3x.
    fe::Placement p = fe::place(ScaleMode::kInteger, 1920, 1080, 480, 320);
    fe::ScreenPoint s = fe::window_to_screen(p, 240 + 3 * 106, 60 + 3 * 165, 1, 1);
    CHECK(s.x == 106);
    CHECK(s.y == 165);
    CHECK(s.inside);
    // The Retina window: 1440x900 points, 2880x1800 pixels, the image at (240, 100) pixels, 5x.
    // Window point (120, 50) is drawable pixel (240, 100), screen (0, 0); (720, 450) the centre.
    p = fe::place(ScaleMode::kInteger, 2880, 1800, 480, 320);
    s = fe::window_to_screen(p, 120, 50, 2, 2);
    CHECK(s.x == 0);
    CHECK(s.y == 0);
    CHECK(s.inside);
    s = fe::window_to_screen(p, 720, 450, 2, 2);
    CHECK(s.x == 240);
    CHECK(s.y == 160);
    s = fe::window_to_screen(p, 119.5f, 450, 2, 2); // the left bar
    CHECK_FALSE(s.inside);
    // Without the density (window points taken for pixels) the same click lands elsewhere.
    s = fe::window_to_screen(p, 720, 450, 1, 1);
    CHECK(s.x == doctest::Approx(96));
    // Fit mode, 1080p: the image at (150, 0), 1620x1080: 3.375 pixels per point.
    p = fe::place(ScaleMode::kFit, 1920, 1080, 480, 320);
    s = fe::window_to_screen(p, 150 + 3.375f * 106, 3.375f * 165, 1, 1);
    CHECK(s.x == doctest::Approx(106));
    CHECK(s.y == doctest::Approx(165));
    // No image (a minimised window): never inside.
    p = fe::place(ScaleMode::kInteger, 0, 0, 480, 320);
    CHECK_FALSE(fe::window_to_screen(p, 0, 0, 1, 1).inside);
    // A density of 0 (unknown) counts as 1.
    p = fe::place(ScaleMode::kInteger, 960, 640, 480, 320);
    s = fe::window_to_screen(p, 480, 320, 0, 0);
    CHECK(s.x == 240);
    CHECK(s.y == 160);
}

// --- settings.ini -----------------------------------------------------------------------------

TEST_CASE("settings: the defaults, a full file, and the command line's apply_setting") {
    const fe::Settings defaults;
    CHECK(defaults.scale_mode == ScaleMode::kInteger);
    CHECK(defaults.border_color == fe::Color{});
    CHECK(defaults.render_scale == 1);

    fe::Settings s;
    std::vector<std::string> problems = fe::parse_settings("# MapleThief settings\n"
                                                           "scale_mode = smooth\n"
                                                           "\n"
                                                           "   # indented comment\n"
                                                           "border_color=#102030\n"
                                                           "\trender_scale\t=  2 \n",
                                                           &s);
    CHECK(problems.empty());
    CHECK(s.scale_mode == ScaleMode::kSmooth);
    CHECK(s.border_color == fe::Color{0x10, 0x20, 0x30});
    CHECK(s.render_scale == 2);

    // CR LF, a byte order mark, keys and modes in any case, no newline at the end.
    s = {};
    problems = fe::parse_settings("\xEF\xBB\xBFScale_Mode = SHARP\r\nBORDER_COLOR = #abcdef\r\n"
                                  "render_scale = 8",
                                  &s);
    CHECK(problems.empty());
    CHECK(s.scale_mode == ScaleMode::kSharp);
    CHECK(s.border_color == fe::Color{0xAB, 0xCD, 0xEF});
    CHECK(s.render_scale == 8);

    // An empty file, only comments, only blank lines: the defaults, no problems.
    for (const char *t : {"", "\n\n\n", "# a\n#b\n  #c", "\r\n \t \r\n"}) {
        s = {};
        CHECK(fe::parse_settings(t, &s).empty());
        CHECK(s.scale_mode == ScaleMode::kInteger);
        CHECK(s.render_scale == 1);
    }

    // apply_setting, as the command line uses it.
    s = {};
    std::string why;
    CHECK(fe::apply_setting(&s, "scale_mode", "fit", &why));
    CHECK(s.scale_mode == ScaleMode::kFit);
    CHECK_FALSE(fe::apply_setting(&s, "scale_mode", "huge", &why));
    CHECK(why == "scale_mode: 'huge' is not integer, fit, smooth or sharp");
    CHECK(s.scale_mode == ScaleMode::kFit);
    CHECK_FALSE(fe::apply_setting(&s, "volume", "11", &why));
    CHECK(why == "unknown key 'volume'");
}

TEST_CASE("settings: unknown keys, bad values and hostile input are reported and ignored") {
    fe::Settings s;
    s.scale_mode = ScaleMode::kFit;
    s.border_color = fe::Color{1, 2, 3};
    s.render_scale = 3;
    const fe::Settings before = s;
    // Every line here is ignored, each with one problem; the settings keep their values.
    const char *bad[] = {
        "scael_mode = smooth",        // unknown key
        "scale_mode = bilinear",      // not a mode
        "scale_mode =",               // empty value
        "scale_mode = smooth # soft", // no comments after a value
        "scale_mode = \"smooth\"",    // no quoting
        "border_color = #12345",      // too short
        "border_color = 123456",      // no '#'
        "border_color = #GGGGGG",     // not hex
        "border_color = red",         // no names
        "render_scale = 0",           // out of range
        "render_scale = 9",           //
        "render_scale = -1",          //
        "render_scale = +2",          // no sign
        "render_scale = 2.0",         // not an integer
        "render_scale = 0x2",         //
        "render_scale = 99999999999", // would overflow
        "render_scale = 2 3",         //
        "scale_mode smooth",          // no '='
        "= smooth",                   // no key
        "scale mode = smooth",        // a blank in the key
        "scale-mode = smooth",        // not a key character
        "[display]",                  // no sections
        "\x01\x02\x03",               // binary
        "scale_mode = smo\0oth",      // (cut at the NUL by the literal: "smo")
        "\xFF\xFE s\x00",             // a UTF-16 byte order mark
    };
    std::string text;
    for (const char *line : bad) {
        text += line;
        text += "\n";
    }
    const std::vector<std::string> problems = fe::parse_settings(text, &s);
    CHECK(problems.size() == std::size(bad));
    for (std::size_t i = 0; i < problems.size(); i++) {
        INFO(problems[i]);
        CHECK(problems[i].rfind("line " + std::to_string(i + 1) + ": ", 0) == 0);
    }
    CHECK(s.scale_mode == before.scale_mode);
    CHECK(s.border_color == before.border_color);
    CHECK(s.render_scale == before.render_scale);
    REQUIRE(problems.size() >= 23);
    CHECK(problems[0] == "line 1: unknown key 'scael_mode', ignored");
    CHECK(problems[3] == "line 4: scale_mode: 'smooth # soft' is not integer, fit, smooth or "
                         "sharp, ignored");
    CHECK(problems[21] == "line 22: sections are not supported: '[display]' ignored");
    CHECK(problems[22] == "line 23: not key = value: '\\x01\\x02\\x03'");

    // NUL bytes inside a line (std::string, not a literal): the value is not a mode.
    s = before;
    const std::string nul("scale_mode = smo\0oth\nrender_scale = 2\n", 37);
    const std::vector<std::string> nul_problems = fe::parse_settings(nul, &s);
    REQUIRE(nul_problems.size() == 1);
    CHECK(nul_problems[0] ==
          "line 1: scale_mode: 'smo\\x00oth' is not integer, fit, smooth or sharp, ignored");
    CHECK(s.scale_mode == before.scale_mode);
    CHECK(s.render_scale == 2); // the good line still counts

    // Good and bad lines mixed: the good ones apply; a repeated key: the last one counts.
    s = {};
    const std::vector<std::string> mixed = fe::parse_settings("scale_mode = fit\n"
                                                              "volume = 3\n"
                                                              "scale_mode = smooth\n"
                                                              "render_scale = 9\n"
                                                              "render_scale = 4\n",
                                                              &s);
    REQUIRE(mixed.size() == 3);
    CHECK(mixed[0] == "line 2: unknown key 'volume', ignored");
    CHECK(mixed[1] == "line 3: scale_mode again (line 1 set it too): this one counts");
    CHECK(mixed[2] == "line 4: render_scale: '9' is not an integer from 1 to 8, ignored");
    CHECK(s.scale_mode == ScaleMode::kSmooth);
    CHECK(s.render_scale == 4);

    // A very long line: reported with its text cut short.
    s = {};
    const std::string long_line = "scale_mode = " + std::string(10000, 'x') + "\n";
    const std::vector<std::string> long_problems = fe::parse_settings(long_line, &s);
    REQUIRE(long_problems.size() == 1);
    CHECK(long_problems[0].size() < 200);
    INFO(long_problems[0]);
    CHECK(long_problems[0].find("...") != std::string::npos);

    // A file over 64 KiB is ignored as a whole, even its good lines.
    s = {};
    std::string big = "scale_mode = smooth\n";
    big += std::string(fe::kMaxSettingsBytes, '#');
    const std::vector<std::string> big_problems = fe::parse_settings(big, &s);
    REQUIRE(big_problems.size() == 1);
    CHECK(big_problems[0].find("ignored") != std::string::npos);
    CHECK(s.scale_mode == ScaleMode::kInteger);

    // Many lines of garbage: one problem each, never a crash.
    s = {};
    std::string garbage;
    for (int i = 0; i < 2000; i++) {
        garbage += static_cast<char>((i * 37) & 0xFF);
        garbage += (i % 13 == 0) ? "\n" : "=";
    }
    CHECK_NOTHROW(fe::parse_settings(garbage, &s));
    CHECK(s.render_scale == 1);
    CHECK(fe::printable("a\\b\x7F") == "a\\x5Cb\\x7F");
}

// --- read-back --------------------------------------------------------------------------------

namespace {

// A headless window (SDL's dummy video driver: the software renderer) of the given size.
struct Window {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    Window(int w, int h) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        REQUIRE_MESSAGE(SDL_CreateWindowAndRenderer("scaling_test", w, h, 0, &window, &renderer),
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

// An image whose every texel has its own colour: r = x mod 256, g = y mod 256, b = 0x80 plus the
// high bits of x and y (never the border colour's b, 0x10).
std::vector<std::uint8_t> unique_image(int w, int h) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            std::uint8_t *p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<std::uint8_t>(x & 0xFF);
            p[1] = static_cast<std::uint8_t>(y & 0xFF);
            p[2] = static_cast<std::uint8_t>(0x80 | ((x >> 8) << 2) | (y >> 8));
            p[3] = 255;
        }
    }
    return px;
}

// A smooth image (for the blending modes): r and g ramp across and down, b constant.
std::vector<std::uint8_t> ramp_image(int w, int h) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            std::uint8_t *p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
            p[2] = 0xC0;
            p[3] = 255;
        }
    }
    return px;
}

constexpr fe::Color kBorder{0x20, 0x40, 0x10};

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

bool is_border(const std::uint8_t *p) {
    return p[0] == kBorder.r && p[1] == kBorder.g && p[2] == kBorder.b;
}

// Every pixel outside the placement has the border colour, every pixel inside does not; and the
// image's rectangle is exactly where the placement says (its first and last rows and columns).
void check_bars_and_bounds(const Readback &rb, const fe::Rect &r) {
    long bars_wrong = 0;
    long image_is_border = 0;
    const int x0 = static_cast<int>(r.x), y0 = static_cast<int>(r.y);
    const int x1 = static_cast<int>(r.x + r.w), y1 = static_cast<int>(r.y + r.h);
    for (int y = 0; y < rb.s->h; y++) {
        for (int x = 0; x < rb.s->w; x++) {
            const bool in = x >= x0 && x < x1 && y >= y0 && y < y1;
            const bool border = is_border(rb.at(x, y));
            bars_wrong += !in && !border ? 1 : 0;
            image_is_border += in && border ? 1 : 0;
        }
    }
    CHECK(bars_wrong == 0);
    CHECK(image_is_border == 0);
}

// The texel under output pixel centre o (one axis, o counted from the image's edge):
// floor((o + 0.5) * texels / size). A centre that falls (within float error) on a texel edge may
// go to either side.
bool centre_texel(int o, int size, int texels, int got) {
    const double u = (o + 0.5) * texels / size;
    const int t = static_cast<int>(std::floor(u));
    if (got == t) {
        return true;
    }
    return (got == t - 1 && u - t < 1e-4) || (got == t + 1 && t + 1 - u < 1e-4);
}

// Whether the output pixel o (one axis, from the image's edge) covers texel `got` at all.
bool covers_texel(int o, int size, int texels, int got) {
    const long long lo = static_cast<long long>(o) * texels / size;
    const long long hi = (static_cast<long long>(o + 1) * texels - 1) / size;
    return got >= lo && got <= hi;
}

// Nearest: every output pixel inside the placement shows one texel of the image, never a blend
// (the unique image tells which texel). strict: the texel under the pixel's centre. Otherwise one
// the pixel covers: SDL's software renderer steps through the texels in 16.16 fixed point, so at
// a fractional scale (fit: 3.375x, 4.5x, 5.625x) a pixel whose centre is near a texel edge may
// show the neighbour; the count of those is reported.
void check_nearest(const Readback &rb, const fe::Rect &r, int iw, int ih, bool strict) {
    long wrong = 0;
    long off_centre = 0;
    std::string first;
    const int x0 = static_cast<int>(r.x), y0 = static_cast<int>(r.y);
    const int w = static_cast<int>(r.w), h = static_cast<int>(r.h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const std::uint8_t *p = rb.at(x0 + x, y0 + y);
            // Decode the texel from the unique image's colour.
            const int tx = p[0] | (((p[2] >> 2) & 0x7) << 8);
            const int ty = p[1] | ((p[2] & 0x3) << 8);
            const bool texel = (p[2] & 0x80) != 0 && (p[2] & 0x60) == 0 && tx < iw && ty < ih;
            const bool centre = texel && centre_texel(x, w, iw, tx) && centre_texel(y, h, ih, ty);
            const bool covered = texel && covers_texel(x, w, iw, tx) && covers_texel(y, h, ih, ty);
            off_centre += covered && !centre ? 1 : 0;
            if (strict ? !centre : !covered) {
                if (wrong++ == 0) {
                    first = "output (" + std::to_string(x0 + x) + ", " + std::to_string(y0 + y) +
                            ") shows texel (" + std::to_string(tx) + ", " + std::to_string(ty) +
                            ")";
                }
            }
        }
    }
    CHECK_MESSAGE(wrong == 0, "first: " << first);
    if (!strict) {
        MESSAGE(off_centre << " of " << static_cast<long>(w) * h
                           << " pixels show the neighbour of the texel under their centre");
    }
}

// Blending: interior pixels of the ramp image are within 3 of the linear ramp at their centre.
void check_smooth_interior(const Readback &rb, const fe::Rect &r, int iw, int ih) {
    long wrong = 0;
    long checked = 0;
    for (int y = static_cast<int>(r.y) + 8; y < static_cast<int>(r.y + r.h) - 8; y += 37) {
        for (int x = static_cast<int>(r.x) + 8; x < static_cast<int>(r.x + r.w) - 8; x += 41) {
            const double u = (x + 0.5 - r.x) * iw / r.w - 0.5; // texel-centre coordinates
            const double v = (y + 0.5 - r.y) * ih / r.h - 0.5;
            const double er = u * 255 / (iw - 1);
            const double eg = v * 255 / (ih - 1);
            const std::uint8_t *p = rb.at(x, y);
            checked++;
            if (std::fabs(p[0] - er) > 3 || std::fabs(p[1] - eg) > 3 || p[2] != 0xC0) {
                wrong++;
            }
        }
    }
    CHECK(checked > 100);
    CHECK(wrong == 0);
}

} // namespace

TEST_CASE("present: read-back at 1080p, 1440p and the 16:10 Retina drawable in every mode") {
    for (const Size size : kSizes) {
        Window w(size.w, size.h);
        for (const int render_scale : {1, 2}) {
            // Render scale 2 at 1080p (texels of 1.5 pixels in integer mode) and the Retina
            // size; the debug builds' time limit leaves 1440p at 1.
            if (render_scale == 2 && size.w == 2560) {
                continue;
            }
            const int iw = 480 * render_scale, ih = 320 * render_scale;
            const std::vector<std::uint8_t> unique = unique_image(iw, ih);
            const std::vector<std::uint8_t> ramp = ramp_image(iw, ih);
            fe::Presenter p;
            REQUIRE(p.start(w.renderer, static_cast<std::uint32_t>(iw),
                            static_cast<std::uint32_t>(ih)));
            for (const ScaleMode mode : kModes) {
                INFO(std::string(fe::scale_mode_name(mode))
                     << " at " << size.w << "x" << size.h << ", render scale " << render_scale);
                const bool blends = mode == ScaleMode::kSmooth || mode == ScaleMode::kSharp;
                const std::vector<std::uint8_t> &image = blends ? ramp : unique;
                p.set_scaling(mode, kBorder);
                REQUIRE(p.upload(image.data(), static_cast<std::uint32_t>(iw) * 4));
                REQUIRE(p.draw());
                const fe::Placement expected = fe::place(mode, size.w, size.h, iw, ih);
                const fe::Rect &r = p.placement().rect;
                check_rect(r, expected.rect.x, expected.rect.y, expected.rect.w, expected.rect.h);
                // The presenter's own check (what --frames runs).
                std::string report;
                CHECK_MESSAGE(p.check_readback(image.data(), static_cast<std::uint32_t>(iw) * 4,
                                               &report) == 0,
                              report);
                const Readback rb(w.renderer);
                REQUIRE(rb.s->w == size.w);
                REQUIRE(rb.s->h == size.h);
                check_bars_and_bounds(rb, r);
                if (mode == ScaleMode::kSmooth) {
                    check_smooth_interior(rb, r, iw, ih);
                } else if (mode == ScaleMode::kSharp) {
                    // SDL's software renderer draws SDL_SCALEMODE_PIXELART nearest; a GPU
                    // renderer blends at texel edges. Either way within the ramp's tolerance.
                    check_smooth_interior(rb, r, iw, ih);
                } else {
                    // Integer and fit: every output pixel is the texel under its centre. In
                    // integer mode at render scale 1 each texel is exactly k x k pixels.
                    check_nearest(rb, r, iw, ih, mode == ScaleMode::kInteger);
                    if (mode == ScaleMode::kInteger && render_scale == 1) {
                        const int k = p.placement().integer_scale;
                        REQUIRE(k >= 3);
                        const auto x0 = static_cast<int>(r.x);
                        const auto y0 = static_cast<int>(r.y);
                        // Texel (479, 319) fills the last k x k pixels, texel (478, 318) the
                        // block before.
                        CHECK(rb.at(x0 + 479 * k, y0 + 319 * k)[0] == (479 & 0xFF));
                        CHECK(rb.at(x0 + 480 * k - 1, y0 + 320 * k - 1)[1] == (319 & 0xFF));
                        CHECK(rb.at(x0 + 479 * k - 1, y0 + 319 * k - 1)[0] == (478 & 0xFF));
                    }
                }
            }
            p.stop();
        }
    }
}

TEST_CASE("present: the border colour fills the bars, a wrong one fails the check") {
    Window w(1000, 400);
    const std::vector<std::uint8_t> image = unique_image(480, 320);
    fe::Presenter p;
    REQUIRE(p.start(w.renderer, 480, 320));
    p.set_scaling(ScaleMode::kInteger, fe::Color{0xFF, 0x00, 0x80});
    CHECK(p.scale_mode() == ScaleMode::kInteger);
    CHECK(p.border_color() == fe::Color{0xFF, 0x00, 0x80});
    REQUIRE(p.upload(image.data(), 480 * 4));
    REQUIRE(p.draw());
    check_rect(p.placement().rect, 260, 40, 480, 320);
    std::string report;
    CHECK_MESSAGE(p.check_readback(image.data(), 480 * 4, &report) == 0, report);
    {
        const Readback rb(w.renderer);
        const std::uint8_t *corner = rb.at(0, 0);
        CHECK(corner[0] == 0xFF);
        CHECK(corner[1] == 0x00);
        CHECK(corner[2] == 0x80);
        const std::uint8_t *right = rb.at(999, 200);
        CHECK(right[0] == 0xFF);
        CHECK(right[2] == 0x80);
    }
    // The check compares the bars with the colour it was given: draw black bars, then expect
    // magenta.
    p.set_scaling(ScaleMode::kInteger, fe::Color{});
    REQUIRE(p.draw());
    p.set_scaling(ScaleMode::kInteger, fe::Color{0xFF, 0x00, 0x80});
    CHECK(p.check_readback(image.data(), 480 * 4, &report) > 0);
    CHECK(report.find("bar") != std::string::npos);
    // A drawable smaller than 480x320 in integer mode: the fit, nearest, no bars wrong.
    {
        Window small(300, 300);
        fe::Presenter q;
        REQUIRE(q.start(small.renderer, 480, 320));
        q.set_scaling(ScaleMode::kInteger, kBorder);
        REQUIRE(q.upload(image.data(), 480 * 4));
        REQUIRE(q.draw());
        check_rect(q.placement().rect, 0, 50, 300, 200);
        CHECK(q.placement().integer_scale == 0);
        CHECK_MESSAGE(q.check_readback(image.data(), 480 * 4, &report) == 0, report);
        q.stop();
    }
    p.stop();
}

TEST_CASE("present: window points through the SDL window's density (the dummy driver: 1)") {
    Window w(1920, 1080);
    float dx = 0, dy = 0;
    fe::window_density(w.window, &dx, &dy);
    CHECK(dx == 1);
    CHECK(dy == 1);
    fe::window_density(nullptr, &dx, &dy);
    CHECK(dx == 1);
    const std::vector<std::uint8_t> image = unique_image(480, 320);
    fe::Presenter p;
    REQUIRE(p.start(w.renderer, 480, 320));
    REQUIRE(p.upload(image.data(), 480 * 4));
    REQUIRE(p.draw());
    const fe::Placement &pl = p.placement();
    fe::ScreenPoint s = fe::window_point_to_screen(w.window, pl, 960, 540);
    CHECK(s.inside);
    CHECK(s.x == 240);
    CHECK(s.y == 160);
    s = fe::window_point_to_screen(w.window, pl, 10, 10);
    CHECK_FALSE(s.inside);
    float x = 0, y = 0;
    fe::screen_to_window_point(w.window, pl, 106, 165, &x, &y);
    CHECK(x == 240 + 3 * 106);
    CHECK(y == 60 + 3 * 165);
    // The click at that window point is the screen point, read back from the window.
    s = fe::window_point_to_screen(w.window, pl, x + 0.5f, y + 0.5f);
    CHECK(static_cast<int>(s.x) == 106);
    CHECK(static_cast<int>(s.y) == 165);
    const Readback rb(w.renderer);
    const std::uint8_t *px = rb.at(static_cast<int>(x), static_cast<int>(y));
    CHECK(px[0] == 106);
    CHECK(px[1] == 165);
    p.stop();
}
