// The colour-blind filter (#158, ADR 0093) with synthetic data only: the matrices (the identity
// when off, greys kept, colours a dichromat confuses with their simulated twin told apart after
// the filter), the CPU way against the float reference, the settings rows, the live change, and
// headless read-backs (SDL's dummy video driver, the software renderer, so the CPU way): off is
// byte-identical to the Presenter before #158, the filter shows in the picture and the blurred
// bars, and switching it live takes effect at the next upload. The GPU way is
// gfx_gpu.present colorblind: * (gfx_gpu_test.cpp), with its GPU time. The picture adjustments
// (#187) in the same colour step: the defaults the identity with no step, the combined matrix
// against a double-precision reference, the CPU way within 1, check_readback and the live rows;
// their GPU way is gfx_gpu.present grade: *.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "color_filter.h"
#include "live_settings.h"
#include "maplethief/hooks.h"
#include "present.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
using fe::ColorblindFilter;

constexpr ColorblindFilter kModes[] = {
    ColorblindFilter::kProtanopia, ColorblindFilter::kDeuteranopia, ColorblindFilter::kTritanopia};

std::array<float, 3> mul(const fe::ColorMatrix &m, const std::array<float, 3> &v) {
    std::array<float, 3> o{};
    for (int i = 0; i < 3; i++) {
        o[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    }
    return o;
}

std::array<float, 3> clamp01(std::array<float, 3> v) {
    for (float &x : v) {
        x = std::clamp(x, 0.0f, 1.0f);
    }
    return v;
}

float dist(const std::array<float, 3> &a, const std::array<float, 3> &b) {
    float s = 0;
    for (int i = 0; i < 3; i++) {
        s += (a[i] - b[i]) * (a[i] - b[i]);
    }
    return std::sqrt(s);
}

std::array<float, 3> linear(int r, int g, int b) {
    return {fe::srgb_to_linear(static_cast<float>(r) / 255.0f),
            fe::srgb_to_linear(static_cast<float>(g) / 255.0f),
            fe::srgb_to_linear(static_cast<float>(b) / 255.0f)};
}

// A headless window (SDL's dummy video driver: the software renderer) of the given size.
struct Window {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    Window(int w, int h) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        REQUIRE_MESSAGE(
            SDL_CreateWindowAndRenderer("color_filter_test", w, h, 0, &window, &renderer),
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

std::vector<std::uint8_t> readback(SDL_Renderer *renderer) {
    SDL_Surface *read = SDL_RenderReadPixels(renderer, nullptr);
    REQUIRE_MESSAGE(read != nullptr, SDL_GetError());
    SDL_Surface *s = SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(read);
    REQUIRE_MESSAGE(s != nullptr, SDL_GetError());
    std::vector<std::uint8_t> px(static_cast<std::size_t>(s->w) * s->h * 4);
    for (int y = 0; y < s->h; y++) {
        std::memcpy(px.data() + static_cast<std::size_t>(y) * s->w * 4,
                    static_cast<const std::uint8_t *>(s->pixels) +
                        static_cast<std::size_t>(y) * s->pitch,
                    static_cast<std::size_t>(s->w) * 4);
    }
    SDL_DestroySurface(s);
    return px;
}

// Every texel its own colour, never black.
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

} // namespace

TEST_CASE("colour-blind filter: off is the identity; greys, black and white are kept") {
    const fe::ColorMatrix id = fe::colorblind_matrix(ColorblindFilter::kOff);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            CHECK(id[i][j] == (i == j ? 1.0f : 0.0f));
        }
    }
    for (const ColorblindFilter f : kModes) {
        INFO(fe::colorblind_filter_name(f));
        const fe::ColorMatrix s = fe::colorblind_simulation(f);
        const fe::ColorMatrix d = fe::colorblind_matrix(f);
        for (int i = 0; i < 3; i++) {
            // Machado et al.'s rows sum to 1, so D's do too: a grey stays that grey.
            CHECK(std::fabs(s[i][0] + s[i][1] + s[i][2] - 1.0f) < 1e-5f);
            CHECK(std::fabs(d[i][0] + d[i][1] + d[i][2] - 1.0f) < 1e-5f);
            // The CPU way's sums stay inside 32 bits: |row| * 65535 * 4096 < 2^31.
            CHECK(std::fabs(d[i][0]) + std::fabs(d[i][1]) + std::fabs(d[i][2]) < 7.0f);
        }
        for (int v = 0; v < 256; v++) {
            const std::uint8_t in[3] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v),
                                        static_cast<std::uint8_t>(v)};
            std::uint8_t out[3];
            fe::colorblind_reference(f, in, out);
            CHECK(out[0] == v);
            CHECK(out[1] == v);
            CHECK(out[2] == v);
        }
    }
    // The matrices as published (spot checks): D = I + E (I - S).
    const fe::ColorMatrix p = fe::colorblind_matrix(ColorblindFilter::kProtanopia);
    CHECK(p[0][0] == doctest::Approx(1.0f));
    CHECK(p[1][0] == doctest::Approx(0.4789f).epsilon(1e-3));
    CHECK(p[2][1] == doctest::Approx(-0.6887f).epsilon(1e-3));
    const fe::ColorMatrix t = fe::colorblind_matrix(ColorblindFilter::kTritanopia);
    CHECK(t[0][2] == doctest::Approx(0.666f).epsilon(1e-3));
    CHECK(t[2][2] == doctest::Approx(1.0f));
}

TEST_CASE("colour-blind filter: colours a dichromat confuses with their twin are told apart") {
    // A colour and its simulated twin (S c) look the same to the dichromat. After the filter
    // (both through D), they don't: the distance the dichromat sees (through S) grows.
    struct Case {
        ColorblindFilter f;
        int r, g, b;
        float gain; // at least this many times further apart
    };
    const Case cases[] = {
        {ColorblindFilter::kProtanopia, 255, 0, 0, 20},   // 0.011 -> 0.83
        {ColorblindFilter::kProtanopia, 200, 40, 40, 20}, // the HP bar's red
        {ColorblindFilter::kDeuteranopia, 255, 0, 0, 10}, // 0.024 -> 0.45
        {ColorblindFilter::kDeuteranopia, 200, 40, 40, 10},
        {ColorblindFilter::kTritanopia, 0, 0, 255, 3}, // 0.16 -> 0.64
        {ColorblindFilter::kTritanopia, 220, 180, 40, 2},
    };
    for (const Case &c : cases) {
        INFO(fe::colorblind_filter_name(c.f) << " " << c.r << "," << c.g << "," << c.b);
        const fe::ColorMatrix s = fe::colorblind_simulation(c.f);
        const fe::ColorMatrix d = fe::colorblind_matrix(c.f);
        const std::array<float, 3> a = linear(c.r, c.g, c.b);
        const std::array<float, 3> twin = clamp01(mul(s, a));
        const float before = dist(mul(s, a), mul(s, twin));
        const float after = dist(mul(s, clamp01(mul(d, a))), mul(s, clamp01(mul(d, twin))));
        CHECK(after > c.gain * before);
    }
}

TEST_CASE("colour-blind filter: the CPU way is within 1 of the float reference, alpha kept") {
    // A grid of the RGB cube (every 4th value per channel, and 255) plus every grey.
    std::vector<std::uint8_t> img;
    for (int r = 0; r <= 256; r += 4) {
        for (int g = 0; g <= 256; g += 4) {
            for (int b = 0; b <= 256; b += 4) {
                img.push_back(static_cast<std::uint8_t>(std::min(r, 255)));
                img.push_back(static_cast<std::uint8_t>(std::min(g, 255)));
                img.push_back(static_cast<std::uint8_t>(std::min(b, 255)));
                img.push_back(static_cast<std::uint8_t>(r & 0xFF));
            }
        }
    }
    for (int v = 0; v < 256; v++) {
        img.insert(img.end(), {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v),
                               static_cast<std::uint8_t>(v), 255});
    }
    const auto n = static_cast<std::uint32_t>(img.size() / 4);
    fe::ColorTransform off;
    std::vector<std::uint8_t> out(img.size());
    off.apply(img.data(), n * 4, out.data(), n * 4, n, 1);
    CHECK(out == img);
    for (const ColorblindFilter f : kModes) {
        INFO(fe::colorblind_filter_name(f));
        fe::ColorTransform t;
        t.set(f);
        CHECK(t.filter() == f);
        t.apply(img.data(), n * 4, out.data(), n * 4, n, 1);
        int worst = 0;
        for (std::uint32_t i = 0; i < n; i++) {
            std::uint8_t ref[3];
            fe::colorblind_reference(f, &img[i * 4], ref);
            for (int c = 0; c < 3; c++) {
                worst = std::max(worst, std::abs(out[i * 4 + c] - ref[c]));
            }
            CHECK(out[i * 4 + 3] == img[i * 4 + 3]);
        }
        CHECK(worst <= 1);
        // In place, the same.
        std::vector<std::uint8_t> same = img;
        t.apply(same.data(), n * 4, same.data(), n * 4, n, 1);
        CHECK(same == out);
    }
}

TEST_CASE("colour-blind filter: the CPU way's time over 480x320 and 1440x960 (logged)") {
    // Logged, not asserted: Debug builds run the loop unoptimised. ADR 0093 has the Release
    // numbers; the CPU way runs only where the renderer has no shaders of ours.
    for (const int k : {1, 3}) {
        const std::uint32_t w = 480u * k;
        const std::uint32_t h = 320u * k;
        const std::vector<std::uint8_t> img =
            unique_image(static_cast<int>(w), static_cast<int>(h));
        std::vector<std::uint8_t> out(img.size());
        fe::ColorTransform t;
        t.set(ColorblindFilter::kDeuteranopia);
        double best = 1e9;
        for (int i = 0; i < 5; i++) {
            const auto t0 = std::chrono::steady_clock::now();
            t.apply(img.data(), w * 4, out.data(), w * 4, w, h);
            const auto t1 = std::chrono::steady_clock::now();
            best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        MESSAGE("colour-blind filter on the CPU: " << w << "x" << h << " in " << best << " ms");
    }
}

TEST_CASE("settings: colorblind_filter and screen_shake (#158), kept by every preset") {
    fe::Settings s;
    CHECK(s.colorblind_filter == ColorblindFilter::kOff);
    CHECK(s.screen_shake == 100u);
    CHECK(fe::parse_settings("colorblind_filter = Deuteranopia\nscreen_shake = 30\n", &s).empty());
    CHECK(s.colorblind_filter == ColorblindFilter::kDeuteranopia);
    CHECK(s.screen_shake == 30u);
    // Bad values keep the old one.
    CHECK(
        fe::parse_settings("colorblind_filter = achromatopsia\nscreen_shake = 101\n", &s).size() ==
        2);
    CHECK(s.colorblind_filter == ColorblindFilter::kDeuteranopia);
    CHECK(s.screen_shake == 30u);
    // Written and read back.
    fe::Settings w;
    w.colorblind_filter = ColorblindFilter::kTritanopia;
    w.screen_shake = 0;
    const std::string text = fe::serialise_settings(w);
    CHECK(text.find("colorblind_filter = tritanopia\n") != std::string::npos);
    CHECK(text.find("screen_shake = 0\n") != std::string::npos);
    fe::Settings r;
    CHECK(fe::parse_settings(text, &r).empty());
    CHECK(r == w);
    // The rows: Accessibility, live, not in the presets.
    for (const char *key : {"colorblind_filter", "screen_shake"}) {
        const fe::SettingInfo *row = fe::find_setting(key);
        REQUIRE(row != nullptr);
        CHECK(row->group == fe::SettingGroup::kAccessibility);
        CHECK(row->applies == fe::Applies::kAtOnce);
        CHECK(row->presets.original == nullptr);
        CHECK(row->presets.enhanced == nullptr);
    }
    CHECK(fe::find_setting_flag("--colorblind-filter") == fe::find_setting("colorblind_filter"));
    CHECK(fe::find_setting_flag("--screen-shake") == fe::find_setting("screen_shake"));
    CHECK(fe::setting_choices(*fe::find_setting("colorblind_filter")) ==
          std::vector<std::string>{"off", "protanopia", "deuteranopia", "tritanopia"});
    // Every preset keeps the player's own values.
    for (const fe::Preset p :
         {fe::Preset::kOriginal, fe::Preset::kOriginalController, fe::Preset::kEnhanced}) {
        fe::Settings mine = w;
        fe::apply_preset(&mine, p);
        CHECK(mine.colorblind_filter == ColorblindFilter::kTritanopia);
        CHECK(mine.screen_shake == 0u);
    }
}

TEST_CASE("settings: flashes (#186), 0 to 100 by 25, kept by every preset, live") {
    fe::Settings s;
    CHECK(s.flashes == 100u);
    CHECK(fe::parse_settings("flashes = 25\n", &s).empty());
    CHECK(s.flashes == 25u);
    CHECK(fe::parse_settings("flashes = 101\n", &s).size() == 1); // the old value stays
    CHECK(s.flashes == 25u);
    fe::Settings w;
    w.flashes = 0;
    const std::string text = fe::serialise_settings(w);
    CHECK(text.find("flashes = 0\n") != std::string::npos);
    fe::Settings r;
    CHECK(fe::parse_settings(text, &r).empty());
    CHECK(r == w);
    const fe::SettingInfo *row = fe::find_setting("flashes");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kAccessibility);
    CHECK(row->applies == fe::Applies::kAtOnce);
    CHECK(row->presets.original == nullptr);
    CHECK(row->presets.enhanced == nullptr);
    CHECK(fe::find_setting_flag("--flashes") == row);
    for (const fe::Preset p :
         {fe::Preset::kOriginal, fe::Preset::kOriginalController, fe::Preset::kEnhanced}) {
        fe::Settings mine = w;
        fe::apply_preset(&mine, p);
        CHECK(mine.flashes == 0u);
    }
    // Live: the hooks on below 100, off at 100 (the original).
    s.flashes = 50;
    CHECK(fe::apply_live_setting(fe::LiveTargets{}, *row, s));
    CHECK(mt_hook_enabled(MT_HOOK_FN_CANVAS_FILL_RECT_COLOR) == 1);
    CHECK(mt_hook_enabled(MT_HOOK_FN_CANVAS_FILL_RECT) == 1);
    s.flashes = 100;
    CHECK(fe::apply_live_setting(fe::LiveTargets{}, *row, s));
    CHECK(mt_hook_enabled(MT_HOOK_FN_CANVAS_FILL_RECT_COLOR) == 0);
    CHECK(mt_hook_enabled(MT_HOOK_FN_CANVAS_FILL_RECT) == 0);
}

TEST_CASE("colour-blind filter: off is byte-identical to the Presenter before #158") {
    // A Presenter that never heard of the filter, one set to off, and one that drew a frame
    // with the filter and was set back to off read back the same bytes.
    Window w(1920, 1080);
    const std::vector<std::uint8_t> image = unique_image(480, 320);
    for (const fe::ScaleMode mode : {fe::ScaleMode::kInteger, fe::ScaleMode::kFit,
                                     fe::ScaleMode::kSmooth, fe::ScaleMode::kSharp}) {
        INFO(fe::scale_mode_name(mode));
        fe::Presenter before;
        REQUIRE(before.start(w.renderer, 480, 320));
        before.set_scaling(mode, fe::Color{});
        REQUIRE(before.upload(image.data(), 480 * 4));
        REQUIRE(before.draw());
        CHECK(before.color_path() == fe::Presenter::ColorPath::kNone);
        const std::vector<std::uint8_t> a = readback(w.renderer);
        before.stop();

        fe::Presenter now;
        REQUIRE(now.start(w.renderer, 480, 320));
        now.set_scaling(mode, fe::Color{});
        now.set_colorblind(ColorblindFilter::kOff);
        REQUIRE(now.upload(image.data(), 480 * 4));
        REQUIRE(now.draw());
        CHECK(readback(w.renderer) == a);
        now.set_colorblind(ColorblindFilter::kProtanopia);
        REQUIRE(now.upload(image.data(), 480 * 4));
        REQUIRE(now.draw());
        CHECK(now.color_path() == fe::Presenter::ColorPath::kCpu);
        CHECK(readback(w.renderer) != a);
        now.set_colorblind(ColorblindFilter::kOff);
        REQUIRE(now.upload(image.data(), 480 * 4));
        REQUIRE(now.draw());
        CHECK(now.color_path() == fe::Presenter::ColorPath::kNone);
        CHECK(readback(w.renderer) == a);
        std::string report;
        CHECK_MESSAGE(now.check_readback(image.data(), 480 * 4, &report) == 0, report);
        CHECK(report.find("colour-blind") == std::string::npos);
        now.stop();
    }
}

TEST_CASE("colour-blind filter: the CPU way's picture and blurred bars, read back") {
    // The software renderer has no shaders of ours: the filter runs on the CPU before the
    // upload, and every step after it (the border's blur, the picture) reads the filtered image.
    Window w(2560, 1080);
    const std::vector<std::uint8_t> image = unique_image(480, 320);
    for (const ColorblindFilter f : kModes) {
        for (const fe::ScaleMode mode : {fe::ScaleMode::kInteger, fe::ScaleMode::kSmooth}) {
            INFO(fe::colorblind_filter_name(f) << " " << fe::scale_mode_name(mode));
            fe::Presenter p;
            REQUIRE(p.start(w.renderer, 480, 320));
            p.set_scaling(mode, fe::Color{});
            p.set_colorblind(f);
            REQUIRE(p.upload(image.data(), 480 * 4));
            REQUIRE(p.draw());
            CHECK(p.color_path() == fe::Presenter::ColorPath::kCpu);
            CHECK(p.color_drawn() == f);
            std::string report;
            CHECK_MESSAGE(p.check_readback(image.data(), 480 * 4, &report) == 0, report);
            CHECK(report.find(std::string("colour-blind filter ") + fe::colorblind_filter_name(f) +
                              " on the CPU") != std::string::npos);
            if (mode == fe::ScaleMode::kInteger) {
                // Each texel exactly the CPU transform of the image's.
                fe::ColorTransform t;
                t.set(f);
                std::vector<std::uint8_t> ref(image.size());
                t.apply(image.data(), 480 * 4, ref.data(), 480 * 4, 480, 320);
                const std::vector<std::uint8_t> out = readback(w.renderer);
                const fe::Rect &r = p.placement().rect;
                const int k = p.placement().integer_scale;
                std::size_t differ = 0;
                for (int ty = 0; ty < 320; ty += 7) {
                    for (int tx = 0; tx < 480; tx += 5) {
                        const int ox = static_cast<int>(r.x) + tx * k + k / 2;
                        const int oy = static_cast<int>(r.y) + ty * k + k / 2;
                        const std::uint8_t *o =
                            &out[(static_cast<std::size_t>(oy) * 2560 + ox) * 4];
                        differ +=
                            std::memcmp(o, &ref[(static_cast<std::size_t>(ty) * 480 + tx) * 4],
                                        3) != 0
                                ? 1
                                : 0;
                    }
                }
                CHECK(differ == 0);
            }
            // The blur reads the filtered image too: its bars differ from the unfiltered blur's.
            p.set_border(fe::BorderMode::kBlur);
            REQUIRE(p.draw());
            const std::vector<std::uint8_t> filtered_bars = readback(w.renderer);
            p.set_colorblind(ColorblindFilter::kOff);
            REQUIRE(p.upload(image.data(), 480 * 4));
            REQUIRE(p.draw());
            const std::vector<std::uint8_t> plain_bars = readback(w.renderer);
            int bar_differ = 0;
            for (int y = 0; y < 1080; y += 9) {
                const std::size_t i = (static_cast<std::size_t>(y) * 2560 + 8) * 4;
                bar_differ += std::memcmp(&filtered_bars[i], &plain_bars[i], 3) != 0 ? 1 : 0;
            }
            CHECK(bar_differ > 0);
            p.stop();
        }
    }
}

TEST_CASE("colour-blind filter and screen_shake are live through apply_live_setting") {
    Window w(960, 640);
    const std::vector<std::uint8_t> image = unique_image(480, 320);
    fe::Presenter p;
    REQUIRE(p.start(w.renderer, 480, 320));
    fe::LiveTargets t;
    t.presenter = &p;
    t.renderer = w.renderer;
    fe::Settings s;
    const fe::SettingInfo &row = *fe::find_setting("colorblind_filter");
    REQUIRE(p.upload(image.data(), 480 * 4));
    REQUIRE(p.draw());
    const std::vector<std::uint8_t> off = readback(w.renderer);
    for (const ColorblindFilter f : kModes) {
        INFO(fe::colorblind_filter_name(f));
        s.colorblind_filter = f;
        CHECK(fe::apply_live_setting(t, row, s));
        CHECK(p.colorblind() == f);
        // The next frame shows it.
        REQUIRE(p.upload(image.data(), 480 * 4));
        REQUIRE(p.draw());
        CHECK(p.color_drawn() == f);
        CHECK(readback(w.renderer) != off);
        std::string report;
        CHECK_MESSAGE(p.check_readback(image.data(), 480 * 4, &report) == 0, report);
    }
    s.colorblind_filter = ColorblindFilter::kOff;
    CHECK(fe::apply_live_setting(t, row, s));
    REQUIRE(p.upload(image.data(), 480 * 4));
    REQUIRE(p.draw());
    CHECK(readback(w.renderer) == off);
    // Without a Presenter it waits for the next start.
    CHECK_FALSE(fe::apply_live_setting(fe::LiveTargets{}, row, s));
    p.stop();

    // screen_shake: the hook on below 100, off at 100 (the original).
    const fe::SettingInfo &shake = *fe::find_setting("screen_shake");
    s.screen_shake = 40;
    CHECK(fe::apply_live_setting(fe::LiveTargets{}, shake, s));
    CHECK(mt_hook_enabled(MT_HOOK_FN_STATE_GAME_PAINT) == 1);
    s.screen_shake = 100;
    CHECK(fe::apply_live_setting(fe::LiveTargets{}, shake, s));
    CHECK(mt_hook_enabled(MT_HOOK_FN_STATE_GAME_PAINT) == 0);
}

// --- the picture adjustments (#187) -------------------------------------------------------------

namespace {

// Grades at the ends of every range, alone, together, and with each colour-blind mode.
std::vector<fe::ColorGrade> adjusted_grades() {
    std::vector<fe::ColorGrade> out;
    const auto with = [](std::uint32_t b, std::uint32_t c, std::uint32_t s, std::uint32_t g,
                         ColorblindFilter f = ColorblindFilter::kOff) {
        return fe::ColorGrade{f, b, c, s, g};
    };
    for (const std::uint32_t v : {50u, 75u, 150u}) {
        out.push_back(with(v, 100, 100, 100));
        out.push_back(with(100, v, 100, 100));
        out.push_back(with(100, 100, v, 100));
    }
    for (const std::uint32_t g : {80u, 90u, 120u, 140u}) {
        out.push_back(with(100, 100, 100, g));
    }
    out.push_back(with(150, 150, 150, 140));
    out.push_back(with(50, 50, 50, 80));
    out.push_back(with(150, 50, 150, 80));
    out.push_back(with(50, 150, 50, 140));
    for (const ColorblindFilter f : kModes) {
        out.push_back(with(120, 130, 70, 115, f));
        out.push_back(with(150, 150, 150, 140, f));
        out.push_back(with(50, 50, 50, 80, f));
    }
    return out;
}

// The step's map computed here in doubles, from its definition (ADR 0093's addendum): saturation
// about Rec. 709's luminance, times the brightness, contrast about linear mid-grey, after D.
void reference_affine(const fe::ColorGrade &g, double m[3][3], double o[3]) {
    const double w[3] = {0.2126, 0.7152, 0.0722};
    const double b = g.brightness / 100.0;
    const double c = g.contrast / 100.0;
    const double s = g.saturation / 100.0;
    const double pivot = std::pow((0.5 + 0.055) / 1.055, 2.4);
    const fe::ColorMatrix d = fe::colorblind_matrix(g.filter);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            double sum = 0;
            for (int k = 0; k < 3; k++) {
                const double sat = (i == k ? s : 0.0) + (1.0 - s) * w[k];
                sum += c * b * sat * d[k][j];
            }
            m[i][j] = sum;
        }
        o[i] = pivot * (1.0 - c);
    }
}

std::vector<std::uint8_t> cube_and_greys() {
    std::vector<std::uint8_t> img;
    for (int r = 0; r <= 256; r += 8) {
        for (int g = 0; g <= 256; g += 8) {
            for (int b = 0; b <= 256; b += 8) {
                img.insert(img.end(), {static_cast<std::uint8_t>(std::min(r, 255)),
                                       static_cast<std::uint8_t>(std::min(g, 255)),
                                       static_cast<std::uint8_t>(std::min(b, 255)),
                                       static_cast<std::uint8_t>(g & 0xFF)});
            }
        }
    }
    for (int v = 0; v < 256; v++) {
        img.insert(img.end(), {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v),
                               static_cast<std::uint8_t>(v), 255});
    }
    return img;
}

fe::ColorGrade adjust(std::uint32_t b, std::uint32_t c, std::uint32_t s, std::uint32_t g) {
    return fe::ColorGrade{ColorblindFilter::kOff, b, c, s, g};
}

} // namespace

TEST_CASE("picture adjustments: the defaults are the identity, with no step installed") {
    const fe::ColorGrade defaults = fe::color_grade(fe::Settings{});
    CHECK(defaults == fe::ColorGrade{});
    CHECK(defaults.is_identity());
    CHECK(fe::color_grade_text(defaults) == "off");
    const fe::ColorAffine a = fe::color_grade_affine(defaults);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            CHECK(a.m[i][j] == (i == j ? 1.0f : 0.0f));
        }
        CHECK(a.offset[i] == 0.0f);
    }
    // With the adjustments at their defaults, the filter's matrix is D exactly (the CPU way's
    // coefficients are #158's, so its bytes are too).
    for (const ColorblindFilter f : kModes) {
        const fe::ColorAffine d = fe::color_grade_affine(fe::ColorGrade{f});
        CHECK(d.m == fe::colorblind_matrix(f));
        CHECK(fe::ColorGrade{f}.filter_only());
    }
    fe::Settings s;
    s.gamma = 105;
    CHECK_FALSE(fe::color_grade(s).is_identity());
    CHECK_FALSE(fe::color_grade(s).filter_only());
    CHECK(fe::color_grade_text(fe::color_grade(s)) == "gamma 1.05");
    s.colorblind_filter = ColorblindFilter::kTritanopia;
    s.brightness = 110;
    CHECK(fe::color_grade_text(fe::color_grade(s)) == "tritanopia, brightness 110 %, gamma 1.05");

    // No step: the Presenter draws as before (no target, no transform), byte for byte, and a
    // Presenter that drew with adjustments and went back to the defaults is the same again.
    Window w(1280, 800);
    const std::vector<std::uint8_t> image = unique_image(480, 320);
    fe::Presenter before;
    REQUIRE(before.start(w.renderer, 480, 320));
    REQUIRE(before.upload(image.data(), 480 * 4));
    REQUIRE(before.draw());
    const std::vector<std::uint8_t> a0 = readback(w.renderer);
    before.stop();
    fe::Presenter p;
    REQUIRE(p.start(w.renderer, 480, 320));
    p.set_grade(defaults);
    REQUIRE(p.upload(image.data(), 480 * 4));
    REQUIRE(p.draw());
    CHECK(p.color_path() == fe::Presenter::ColorPath::kNone);
    CHECK(readback(w.renderer) == a0);
    p.set_grade(adjust(120, 100, 100, 100));
    REQUIRE(p.upload(image.data(), 480 * 4));
    REQUIRE(p.draw());
    CHECK(p.color_path() == fe::Presenter::ColorPath::kCpu);
    CHECK(readback(w.renderer) != a0);
    p.set_grade(defaults);
    REQUIRE(p.upload(image.data(), 480 * 4));
    REQUIRE(p.draw());
    CHECK(p.color_path() == fe::Presenter::ColorPath::kNone);
    CHECK(readback(w.renderer) == a0);
    p.stop();
}

TEST_CASE("picture adjustments: the combined matrix against a float reference; the CPU way "
          "within 1") {
    const std::vector<std::uint8_t> img = cube_and_greys();
    const auto n = static_cast<std::uint32_t>(img.size() / 4);
    std::vector<std::uint8_t> out(img.size());
    for (const fe::ColorGrade &g : adjusted_grades()) {
        INFO(fe::color_grade_text(g));
        CHECK_FALSE(g.is_identity());
        // The matrix and offset: the colour-blind matrix and the adjustments in one map.
        double m[3][3];
        double o[3];
        reference_affine(g, m, o);
        const fe::ColorAffine a = fe::color_grade_affine(g);
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                CHECK(a.m[i][j] == doctest::Approx(m[i][j]).epsilon(1e-5));
            }
            CHECK(a.offset[i] == doctest::Approx(o[i]).epsilon(1e-5));
        }
        // The CPU way against the float reference, and against the doubles' map encoded here.
        fe::ColorTransform t;
        t.set(g);
        CHECK(t.grade() == g);
        t.apply(img.data(), n * 4, out.data(), n * 4, n, 1);
        int worst = 0;
        int worst_double = 0;
        for (std::uint32_t i = 0; i < n; i++) {
            std::uint8_t ref[3];
            fe::color_grade_reference(g, &img[i * 4], ref);
            double lin[3];
            for (int c = 0; c < 3; c++) {
                const double v = img[i * 4 + c] / 255.0;
                lin[c] = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
            }
            for (int c = 0; c < 3; c++) {
                const double v = std::clamp(
                    m[c][0] * lin[0] + m[c][1] * lin[1] + m[c][2] * lin[2] + o[c], 0.0, 1.0);
                const double e = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
                const auto d = static_cast<int>(std::lround(std::pow(e, 100.0 / g.gamma) * 255));
                worst = std::max(worst, std::abs(out[i * 4 + c] - ref[c]));
                worst_double = std::max(worst_double, std::abs(out[i * 4 + c] - d));
            }
            CHECK(out[i * 4 + 3] == img[i * 4 + 3]);
        }
        CHECK(worst <= 1);
        CHECK(worst_double <= 1);
        // In place, the same.
        std::vector<std::uint8_t> same = img;
        t.apply(same.data(), n * 4, same.data(), n * 4, n, 1);
        CHECK(same == out);
    }
}

TEST_CASE("picture adjustments: what each one does; greys stay grey") {
    const auto grey = [](const fe::ColorGrade &g, int v) {
        const std::uint8_t in[3] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v),
                                    static_cast<std::uint8_t>(v)};
        std::uint8_t out[3];
        fe::color_grade_reference(g, in, out);
        CHECK(out[0] == out[1]);
        CHECK(out[1] == out[2]);
        return static_cast<int>(out[0]);
    };
    const fe::ColorGrade bright = adjust(150, 100, 100, 100);
    const fe::ColorGrade dark = adjust(50, 100, 100, 100);
    const fe::ColorGrade punchy = adjust(100, 150, 100, 100);
    const fe::ColorGrade flat = adjust(100, 50, 100, 100);
    const fe::ColorGrade light_mids = adjust(100, 100, 100, 140);
    const fe::ColorGrade dark_mids = adjust(100, 100, 100, 80);
    CHECK(grey(bright, 100) > 100);
    CHECK(grey(dark, 100) < 100);
    CHECK(grey(bright, 0) == 0); // brightness scales the light: black stays black
    CHECK(grey(punchy, 64) < 64);
    CHECK(grey(punchy, 192) > 192);
    CHECK(std::abs(grey(punchy, 128) - 128) <= 1); // about mid-grey
    CHECK(grey(flat, 0) > 0);                      // lower contrast lifts black ...
    CHECK(grey(flat, 255) < 255);                  // ... and dims white
    CHECK(grey(light_mids, 128) > 128);
    CHECK(grey(dark_mids, 128) < 128);
    for (const fe::ColorGrade &g : {light_mids, dark_mids}) {
        CHECK(grey(g, 0) == 0); // gamma keeps black and white
        CHECK(grey(g, 255) == 255);
    }
    for (int v = 0; v < 256; v += 15) {
        grey(adjust(100, 100, 50, 100), v);
        grey(adjust(100, 100, 150, 100), v);
    }
    // Saturation: a colour's channels further apart, or closer together.
    const std::uint8_t red[3] = {200, 80, 60};
    std::uint8_t more[3];
    std::uint8_t less[3];
    fe::color_grade_reference(adjust(100, 100, 150, 100), red, more);
    fe::color_grade_reference(adjust(100, 100, 50, 100), red, less);
    CHECK(more[0] - more[1] > red[0] - red[1]);
    CHECK(less[0] - less[1] < red[0] - red[1]);
}

TEST_CASE("picture adjustments: the CPU way's time over 480x320 and 1440x960 (logged)") {
    for (const int k : {1, 3}) {
        const std::uint32_t w = 480u * k;
        const std::uint32_t h = 320u * k;
        const std::vector<std::uint8_t> img =
            unique_image(static_cast<int>(w), static_cast<int>(h));
        std::vector<std::uint8_t> out(img.size());
        fe::ColorTransform t;
        t.set(fe::ColorGrade{ColorblindFilter::kDeuteranopia, 110, 120, 90, 120});
        double best = 1e9;
        for (int i = 0; i < 5; i++) {
            const auto t0 = std::chrono::steady_clock::now();
            t.apply(img.data(), w * 4, out.data(), w * 4, w, h);
            const auto t1 = std::chrono::steady_clock::now();
            best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        MESSAGE("picture adjustments on the CPU: " << w << "x" << h << " in " << best << " ms");
    }
}

TEST_CASE("settings: brightness, contrast, saturation and gamma (#187): Video, kept by every "
          "preset") {
    fe::Settings s;
    CHECK(s.brightness == 100u);
    CHECK(s.contrast == 100u);
    CHECK(s.saturation == 100u);
    CHECK(s.gamma == 100u);
    CHECK(fe::parse_settings("brightness = 55\ncontrast = 150\nsaturation = 50\ngamma = 1.2\n", &s)
              .empty());
    CHECK(s.brightness == 55u);
    CHECK(s.contrast == 150u);
    CHECK(s.saturation == 50u);
    CHECK(s.gamma == 120u);
    for (const char *ok : {"0.8", "0.80", "1", "1.0", "1.05", "1.40"}) {
        CAPTURE(ok);
        CHECK(fe::parse_settings(std::string("gamma = ") + ok + "\n", &s).empty());
    }
    CHECK(s.gamma == 140u);
    // Out of range or not a gamma: the old value stays.
    CHECK(fe::parse_settings("brightness = 49\ncontrast = 151\nsaturation = x\n", &s).size() == 3);
    for (const char *bad : {"0.79", "1.41", "2", "1.", ".9", "1.005", "-1", "1,2", "a"}) {
        CAPTURE(bad);
        CHECK(fe::parse_settings(std::string("gamma = ") + bad + "\n", &s).size() == 1);
    }
    CHECK(s.brightness == 55u);
    CHECK(s.gamma == 140u);
    fe::Settings w;
    w.brightness = 130;
    w.contrast = 85;
    w.saturation = 145;
    w.gamma = 90;
    const std::string text = fe::serialise_settings(w);
    CHECK(text.find("brightness = 130\n") != std::string::npos);
    CHECK(text.find("contrast = 85\n") != std::string::npos);
    CHECK(text.find("saturation = 145\n") != std::string::npos);
    CHECK(text.find("gamma = 0.90\n") != std::string::npos);
    CHECK(fe::serialise_settings(fe::Settings{}).find("gamma = 1.00\n") != std::string::npos);
    fe::Settings r;
    CHECK(fe::parse_settings(text, &r).empty());
    CHECK(r == w);
    for (const char *key : {"brightness", "contrast", "saturation", "gamma"}) {
        CAPTURE(key);
        const fe::SettingInfo *row = fe::find_setting(key);
        REQUIRE(row != nullptr);
        CHECK(row->group == fe::SettingGroup::kVideo);
        CHECK(row->applies == fe::Applies::kAtOnce);
        CHECK(row->test_pattern);
        CHECK_FALSE(row->presets.in_presets());
        CHECK(fe::find_setting_flag(std::string("--") + key) == row);
    }
    CHECK(fe::setting_choices(*fe::find_setting("brightness")).size() == 21); // 50 ... 150 by 5
    CHECK(fe::setting_choices(*fe::find_setting("gamma")) ==
          std::vector<std::string>{"0.80", "0.85", "0.90", "0.95", "1.00", "1.05", "1.10", "1.15",
                                   "1.20", "1.25", "1.30", "1.35", "1.40"});
    // The menu's steps.
    fe::Settings m;
    CHECK(fe::step_setting(&m, *fe::find_setting("gamma"), 1));
    CHECK(m.gamma == 105u);
    CHECK(fe::step_setting(&m, *fe::find_setting("brightness"), -1));
    CHECK(m.brightness == 95u);
    for (const fe::Preset p :
         {fe::Preset::kOriginal, fe::Preset::kOriginalController, fe::Preset::kEnhanced}) {
        fe::Settings mine = w;
        fe::apply_preset(&mine, p);
        CHECK(fe::color_grade(mine) == fe::color_grade(w));
    }
}

TEST_CASE("picture adjustments: check_readback passes on the CPU way; live through "
          "apply_live_setting") {
    Window w(2560, 1080);
    const std::vector<std::uint8_t> image = unique_image(480, 320);
    for (const fe::ColorGrade &g : adjusted_grades()) {
        if (g.brightness != 150 && g.filter == ColorblindFilter::kOff) {
            continue; // a few are enough here: the transform is checked above
        }
        for (const fe::ScaleMode mode : {fe::ScaleMode::kInteger, fe::ScaleMode::kSmooth}) {
            INFO(fe::color_grade_text(g) << " " << fe::scale_mode_name(mode));
            fe::Presenter p;
            REQUIRE(p.start(w.renderer, 480, 320));
            p.set_scaling(mode, fe::Color{});
            p.set_border(fe::BorderMode::kBlur);
            p.set_grade(g);
            REQUIRE(p.upload(image.data(), 480 * 4));
            REQUIRE(p.draw());
            CHECK(p.color_path() == fe::Presenter::ColorPath::kCpu);
            CHECK(p.grade_drawn() == g);
            std::string report;
            CHECK_MESSAGE(p.check_readback(image.data(), 480 * 4, &report) == 0, report);
            CHECK(report.find("(colour step ") != std::string::npos);
            p.stop();
        }
    }
    // Live: each row's change reaches the Presenter's grade, with the filter kept.
    fe::Presenter p;
    REQUIRE(p.start(w.renderer, 480, 320));
    fe::LiveTargets t;
    t.presenter = &p;
    t.renderer = w.renderer;
    fe::Settings s;
    s.colorblind_filter = ColorblindFilter::kProtanopia;
    CHECK(fe::apply_live_setting(t, *fe::find_setting("colorblind_filter"), s));
    s.brightness = 120;
    CHECK(fe::apply_live_setting(t, *fe::find_setting("brightness"), s));
    s.contrast = 80;
    CHECK(fe::apply_live_setting(t, *fe::find_setting("contrast"), s));
    s.saturation = 60;
    CHECK(fe::apply_live_setting(t, *fe::find_setting("saturation"), s));
    s.gamma = 135;
    CHECK(fe::apply_live_setting(t, *fe::find_setting("gamma"), s));
    CHECK(p.grade() == fe::ColorGrade{ColorblindFilter::kProtanopia, 120, 80, 60, 135});
    REQUIRE(p.upload(image.data(), 480 * 4));
    REQUIRE(p.draw());
    std::string report;
    CHECK_MESSAGE(p.check_readback(image.data(), 480 * 4, &report) == 0, report);
    CHECK(report.find("(colour step protanopia, brightness 120 %") != std::string::npos);
    CHECK_FALSE(fe::apply_live_setting(fe::LiveTargets{}, *fe::find_setting("gamma"), s));
    p.stop();
}
