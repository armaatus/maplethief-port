// render_scale = auto and the frame-budget guard (#152, ADR 0080), synthetic: the cap from the
// image's place in the drawable; RenderScaleGovernor fed made-up seconds of FrameStats::recent()
// (climbing from 1x while the scaled cost fits, the software backend's numbers stopping at 2x,
// a drop after two seconds over the budget and the wait before climbing again, a resize, too
// few frames, a fixed scale's warning given once); the overlay's line; and the `auto` value of
// the render_scale row (the file, the flag, the presets).
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "frame_stats.h"
#include "perf_overlay.h"
#include "render_scale.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
using fe::RenderScaleGovernor;

// One second of window frames: `frames` of them, means per frame.
fe::RecentStats second(double frontend_ms, double gfx_ms, std::uint32_t frames = 20) {
    fe::RecentStats r;
    r.frames = frames;
    r.seconds = 1.0;
    r.fps = frames;
    r.tps = frames;
    r.frontend_ms = frontend_ms;
    r.max_frontend_ms = frontend_ms * 2;
    r.gfx_ms = gfx_ms;
    return r;
}

} // namespace

TEST_CASE("render_scale: the cap is the smallest scale whose image covers its place (#152)") {
    CHECK(fe::render_scale_cap(0, 0) == 1);
    CHECK(fe::render_scale_cap(-5, 100) == 1);
    CHECK(fe::render_scale_cap(480, 320) == 1);
    CHECK(fe::render_scale_cap(240, 160) == 1); // under 1x (the fit fallback)
    CHECK(fe::render_scale_cap(481, 320) == 2);
    CHECK(fe::render_scale_cap(960, 640) == 2);
    CHECK(fe::render_scale_cap(1440, 960) == 3);              // 1080p, integer 3x
    CHECK(fe::render_scale_cap(1439.9999999, 959.9999) == 3); // a hair under counts as it
    CHECK(fe::render_scale_cap(1620, 1080) == 4);             // 1080p, fit: 3.375x
    CHECK(fe::render_scale_cap(1920, 1280) == 4);             // the default window on Retina
    CHECK(fe::render_scale_cap(2400, 1600) == 5);             // a 16:10 Retina drawable, 5x
    CHECK(fe::render_scale_cap(3840, 2560) == 8);
    CHECK(fe::render_scale_cap(7680, 5120) == 8); // at most MT_GFX_MAX_SCALE
}

TEST_CASE("render_scale: auto climbs one scale at a time while the scaled cost fits") {
    RenderScaleGovernor g(true, 1);
    CHECK(g.automatic());
    CHECK(g.scale() == 1);
    CHECK(g.set_cap(4, 0.0) == 0); // 1x is under the cap
    CHECK(g.cap() == 4);
    // GPU-like numbers: the compositor's time grows with N squared, mt_gfx records only.
    CHECK(g.update(second(0.5, 0.05), 1.0) == 2);
    CHECK(g.scale() == 2);
    CHECK(g.reason().find("2x should fit") != std::string::npos);
    CHECK(g.update(second(2.0, 0.05), 1.5) == 0); // settling after the change
    CHECK(g.update(second(2.0, 0.05), 2.3) == 3); // 3x: 4.5 ms predicted
    CHECK(g.update(second(4.0, 0.05), 3.6) == 4); // 4x: 7.1 ms predicted
    CHECK(g.update(second(7.0, 0.05), 4.9) == 0); // the cap
    CHECK(g.update(second(7.0, 0.05), 6.0) == 0);
    CHECK(g.scale() == 4);
    CHECK(g.status() == "render 4x auto (window 4x)");
    CHECK_FALSE(g.warned());

    // The frontend's own budget stops it: 6 ms at 3x is 10.7 ms at 4x.
    RenderScaleGovernor h(true, 3);
    h.set_cap(8, 0.0);
    CHECK(h.update(second(6.0, 0.0), 2.0) == 0);
    CHECK(h.scale() == 3);
}

TEST_CASE("render_scale: auto on the software backend's numbers stops at 2x") {
    // The real-time fresh run's means on a map (Release, Apple M4): at 1x the frontend 0.3 ms
    // and mt_gfx 3.3 ms a frame; at 2x 0.7 ms and 12 ms. 3x would be about 28.6 ms with mt_gfx,
    // over the 20 ms budget.
    RenderScaleGovernor g(true, 1);
    g.set_cap(4, 0.0);
    CHECK(g.update(second(0.3, 3.3), 1.0) == 2);
    CHECK(g.update(second(0.7, 12.0), 2.5) == 0);
    CHECK(g.update(second(0.7, 12.0), 3.5) == 0);
    CHECK(g.scale() == 2);
    // The main menu's heavier compositing at 1x (6 ms: 24 ms predicted at 2x) does not climb.
    RenderScaleGovernor menu(true, 1);
    menu.set_cap(4, 0.0);
    CHECK(menu.update(second(6.0, 0.3), 1.0) == 0);
}

TEST_CASE("render_scale: auto drops after two seconds over the budget, then waits to climb") {
    RenderScaleGovernor g(true, 4);
    g.set_cap(4, 0.0);
    // One second over (a spike): nothing.
    CHECK(g.update(second(3.0, 40.0), 10.0) == 0);
    CHECK(g.update(second(1.0, 4.0), 11.0) == 0);
    // Two in a row: down to the largest scale whose scaled cost fits (43 ms at 4x: 24.2 at 3x,
    // 10.75 at 2x).
    CHECK(g.update(second(3.0, 40.0), 12.0) == 0);
    CHECK(g.update(second(3.0, 40.0), 13.0) == 2);
    CHECK(g.scale() == 2);
    CHECK(g.reason().find("at 4x: down to 2x") != std::string::npos);
    // Cheap now, but no climbing for kRetrySeconds after a drop.
    CHECK(g.update(second(0.5, 2.0), 15.0) == 0);
    CHECK(g.update(second(0.5, 2.0), 13.0 + RenderScaleGovernor::kRetrySeconds - 0.5) == 0);
    CHECK(g.update(second(0.5, 2.0), 13.0 + RenderScaleGovernor::kRetrySeconds + 0.5) == 3);

    // Over the budget at every scale: down to 1x, never below.
    RenderScaleGovernor slow(true, 3);
    slow.set_cap(8, 0.0);
    slow.update(second(30.0, 60.0), 1.0);
    CHECK(slow.update(second(30.0, 60.0), 2.0) == 1);
    CHECK(slow.update(second(30.0, 60.0), 4.0) == 0);
    CHECK(slow.update(second(30.0, 60.0), 5.0) == 0);
    CHECK(slow.scale() == 1);
    CHECK_FALSE(slow.warned()); // auto does what it can: no warning
}

TEST_CASE("render_scale: a resize caps auto at once and lets it climb again") {
    RenderScaleGovernor g(true, 4);
    CHECK(g.set_cap(4, 0.0) == 0);
    CHECK(g.set_cap(4, 0.5) == 0);
    CHECK(g.set_cap(2, 1.0) == 2); // the window shrank
    CHECK(g.scale() == 2);
    CHECK(g.reason() == "the window's image needs at most 2x");
    // A drop for the budget, then the window grows: no wait.
    RenderScaleGovernor h(true, 3);
    h.set_cap(3, 0.0);
    h.update(second(8.0, 30.0), 2.0);
    CHECK(h.update(second(8.0, 30.0), 3.0) == 2);
    CHECK(h.update(second(0.5, 1.0), 5.0) == 0); // waiting
    CHECK(h.set_cap(5, 6.0) == 0);
    CHECK(h.update(second(0.5, 1.0), 7.5) == 3);
    // A fixed scale ignores the window.
    RenderScaleGovernor fixed(false, 4);
    CHECK(fixed.set_cap(1, 0.0) == 0);
    CHECK(fixed.scale() == 4);
}

TEST_CASE("render_scale: seconds with too few frames do not count") {
    RenderScaleGovernor g(true, 1);
    g.set_cap(4, 0.0);
    CHECK(g.update(second(0.5, 0.5, RenderScaleGovernor::kMinFrames - 1), 1.0) == 0);
    fe::RecentStats half = second(0.5, 0.5);
    half.seconds = 0.4; // just after a pause
    CHECK(g.update(half, 2.0) == 0);
    CHECK(g.update(second(0.5, 0.5), 3.0) == 2);
}

TEST_CASE("render_scale: a fixed scale over the budget warns once, after three seconds") {
    RenderScaleGovernor g(false, 4);
    g.set_cap(4, 0.0);
    std::string text;
    CHECK(g.update(second(3.79, 41.0), 1.0) == 0);
    CHECK(g.update(second(3.79, 41.0), 2.0) == 0);
    CHECK_FALSE(g.warned());
    CHECK_FALSE(g.take_warning(&text));
    CHECK(g.update(second(3.79, 41.0), 3.0) == 0);
    REQUIRE(g.warned());
    CHECK(g.take_warning(&text));
    CHECK(text == "Render scale 4x is too slow here (45 ms a frame): try auto or less");
    CHECK(text.size() <= 80); // one line of the notice
    CHECK(g.reason().find("frontend 3.79 ms, with mt_gfx 44.79 ms") != std::string::npos);
    CHECK_FALSE(g.take_warning(&text)); // once
    CHECK(g.status() == "render 4x: OVER BUDGET");
    CHECK(g.scale() == 4); // a fixed scale never changes
    for (int i = 4; i < 10; i++) {
        CHECK(g.update(second(3.79, 41.0), i) == 0);
    }
    // At 1x there is nothing to warn about.
    RenderScaleGovernor one(false, 1);
    for (int i = 1; i < 10; i++) {
        one.update(second(30.0, 60.0), i);
    }
    CHECK_FALSE(one.warned());
    CHECK(one.status() == "render 1x");
    // The frontend's budget alone counts too (#152: 10 ms a frame).
    RenderScaleGovernor fe_only(false, 2);
    for (int i = 1; i <= 3; i++) {
        fe_only.update(second(10.5, 0.0), i);
    }
    CHECK(fe_only.warned());
}

TEST_CASE("render_scale: a scale the frontend cannot set stops auto climbing") {
    RenderScaleGovernor g(true, 1);
    g.set_cap(4, 0.0);
    CHECK(g.update(second(0.5, 0.5), 1.0) == 2);
    g.failed(1);
    CHECK(g.scale() == 1);
    CHECK(g.update(second(0.5, 0.5), 3.0) == 0);
    CHECK(g.update(second(0.5, 0.5), 60.0) == 0);
    CHECK(g.set_cap(5, 61.0) == 0); // a bigger window: try again
    CHECK(g.update(second(0.5, 0.5), 62.5) == 2);
}

TEST_CASE("render_scale: the frame-time overlay shows the governor's line") {
    const fe::RecentStats r = second(1.0, 2.0);
    CHECK(fe::PerfOverlay::lines(r, nullptr).size() == 4);
    const std::vector<std::string> with = fe::PerfOverlay::lines(r, nullptr, "render 2x auto");
    REQUIRE(with.size() == 5);
    CHECK(with[4] == "render 2x auto");
}

TEST_CASE("render_scale: auto in settings.ini, on the command line and in the presets") {
    fe::Settings s;
    CHECK(fe::parse_settings("render_scale = auto\n", &s).empty());
    CHECK(s.render_scale == fe::kRenderScaleAuto);
    CHECK(fe::serialise_settings(s).find("\nrender_scale = auto\n") != std::string::npos);
    CHECK(fe::parse_settings("render_scale = AUTO\n", &s).empty()); // any case
    CHECK(s.render_scale == fe::kRenderScaleAuto);
    s.render_scale = 3;
    for (const char *bad : {"autox", "0", "auto 2", "9"}) {
        INFO(bad);
        std::string why;
        CHECK_FALSE(fe::apply_setting(&s, "render_scale", bad, &why));
        CHECK(why.find("is not auto or an integer from 1 to 8") != std::string::npos);
        CHECK(s.render_scale == 3);
    }
    CHECK(fe::apply_setting(&s, "render_scale", "auto", nullptr));
    CHECK(s.render_scale == fe::kRenderScaleAuto);
    const fe::SettingInfo *row = fe::find_setting_flag("--render-scale");
    REQUIRE(row != nullptr);
    CHECK(fe::setting_text(s, *row) == "auto");
    // Original and Original + controller keep 1x; Enhanced is auto (#89's table, ADR 0070).
    fe::apply_preset(&s, fe::Preset::kOriginal);
    CHECK(s.render_scale == 1);
    fe::apply_preset(&s, fe::Preset::kOriginalController);
    CHECK(s.render_scale == 1);
    fe::apply_preset(&s, fe::Preset::kEnhanced);
    CHECK(s.render_scale == fe::kRenderScaleAuto);
    // A fixed scale under Enhanced makes it custom.
    CHECK(fe::apply_setting(&s, "render_scale", "4", nullptr));
    CHECK(s.preset == fe::Preset::kCustom);
    CHECK(fe::check_registry(fe::settings_registry()).empty());
}
