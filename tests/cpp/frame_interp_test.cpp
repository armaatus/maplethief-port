// Frame interpolation (#87, ADR 0091, platform/src/frame_interp.h): the matching of two ticks'
// draws (the camera's scroll, repeated tiles, animation frames, snaps), the replay on the
// software rasterizer (alpha 1 is the tick's own frame byte for byte), and the presents between
// two ticks locked to a 60 Hz display (1/3, 2/3 and 1 of the tick, the ticks' cadence kept).
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "frame_interp.h"
#include "maplethief/gfx.h"
#include "pacing.h"

namespace {

namespace fe = maplethief::frontend;
using fe::DrawList;
using fe::DrawMatch;
using fe::MatchKind;

constexpr float kW = 480, kH = 320;

// A quad (a triangle strip) at window rectangle (x, y, w, h) (logical pixels, y up) of a
// 480 x 320 viewport, with texture coordinates (s0, t0)-(s1, t1).
struct Quad {
    mt_gfx_vertex v[4];
    mt_gfx_draw draw;
    Quad(float x, float y, float w, float h, std::uint32_t texture, float s0 = 0, float t0 = 0,
         float s1 = 1, float t1 = 1) {
        const auto cx = [](float px) { return px / kW * 2 - 1; };
        const auto cy = [](float py) { return py / kH * 2 - 1; };
        v[0] = {cx(x), cy(y), 0, 1, s0, t0};
        v[1] = {cx(x + w), cy(y), 0, 1, s1, t0};
        v[2] = {cx(x), cy(y + h), 0, 1, s0, t1};
        v[3] = {cx(x + w), cy(y + h), 0, 1, s1, t1};
        draw = mt_gfx_draw_default();
        draw.primitive = MT_GFX_TRIANGLE_STRIP;
        draw.vertices = v;
        draw.count = 4;
        draw.texture = texture;
        draw.viewport[2] = static_cast<std::int32_t>(kW);
        draw.viewport[3] = static_cast<std::int32_t>(kH);
    }
};

void add(DrawList &list, const Quad &q) { list.add(nullptr, q.draw); }

// The window x of the blended draw's first vertex.
float blended_x(const DrawList &prev, const DrawList &cur, std::size_t i, const DrawMatch &m,
                float alpha) {
    std::vector<mt_gfx_vertex> v;
    std::uint8_t color[4];
    fe::blend_draw(prev, cur, i, m, alpha, &v, color);
    return (v[0].x / v[0].w * 0.5f + 0.5f) * kW;
}

} // namespace

TEST_CASE("frame_interp: draws moved by the camera blend halfway at 0.5 and the HUD stays") {
    DrawList prev, cur;
    // Ten world tiles of their own texture coordinates, scrolled 4 px left; two HUD quads still.
    for (int i = 0; i < 10; i++) {
        const float s = static_cast<float>(i) / 10;
        add(prev, Quad(40.0f * static_cast<float>(i), 100, 32, 32, 1, s, 0, s + 0.1f, 1));
        add(cur, Quad(40.0f * static_cast<float>(i) - 4, 100, 32, 32, 1, s, 0, s + 0.1f, 1));
    }
    add(prev, Quad(0, 0, 100, 20, 2));
    add(cur, Quad(0, 0, 100, 20, 2));
    add(prev, Quad(380, 0, 100, 20, 3));
    add(cur, Quad(380, 0, 100, 20, 3));
    std::vector<DrawMatch> m;
    const fe::MatchStats st = fe::match_draws(prev, cur, fe::MatchLimits{}, &m);
    CHECK_FALSE(st.snapped);
    CHECK(st.exact == 12);
    CHECK(st.camera_dx == doctest::Approx(-4));
    CHECK(st.camera_dy == doctest::Approx(0));
    CHECK(blended_x(prev, cur, 3, m[3], 0.5f) == doctest::Approx(118));
    CHECK(blended_x(prev, cur, 3, m[3], 0.0f) == doctest::Approx(120));
    CHECK(blended_x(prev, cur, 3, m[3], 1.0f) == doctest::Approx(116));
    CHECK(blended_x(prev, cur, 10, m[10], 0.5f) == doctest::Approx(0));
}

TEST_CASE("frame_interp: repeated tiles match the neighbour the camera's scroll predicts") {
    DrawList prev, cur;
    // Thirty identical tiles 16 px apart, scrolled 6 px right (more than a third of the
    // spacing), and five unique landmarks that vote the scroll.
    for (int i = 0; i < 30; i++) {
        add(prev, Quad(16.0f * static_cast<float>(i), 50, 16, 16, 5));
        add(cur, Quad(16.0f * static_cast<float>(i) + 6, 50, 16, 16, 5));
    }
    for (int i = 0; i < 5; i++) {
        const float s = static_cast<float>(i) / 5;
        add(prev, Quad(60.0f * static_cast<float>(i), 200, 40, 40, 6, s, 0, s + 0.2f, 1));
        add(cur, Quad(60.0f * static_cast<float>(i) + 6, 200, 40, 40, 6, s, 0, s + 0.2f, 1));
    }
    std::vector<DrawMatch> m;
    const fe::MatchStats st = fe::match_draws(prev, cur, fe::MatchLimits{}, &m);
    CHECK(st.camera_dx == doctest::Approx(6));
    CHECK(st.exact == 35);
    for (int i = 0; i < 30; i++) {
        REQUIRE(m[static_cast<std::size_t>(i)].kind == MatchKind::kExact);
        CHECK(m[static_cast<std::size_t>(i)].prev == i); // not the neighbour 16 px away
    }
}

TEST_CASE("frame_interp: an animation frame matches loosely and a new draw follows the camera") {
    DrawList prev, cur;
    for (int i = 0; i < 8; i++) {
        const float s = static_cast<float>(i) / 8;
        add(prev, Quad(50.0f * static_cast<float>(i), 10, 30, 30, 1, s, 0, s + 0.125f, 1));
        add(cur, Quad(50.0f * static_cast<float>(i) - 3, 10, 30, 30, 1, s, 0, s + 0.125f, 1));
    }
    // The hero: its next animation frame (other texture coordinates) 2 px on (5 px on screen
    // with the 3 px scroll), and a mob that appears.
    add(prev, Quad(200, 150, 40, 60, 9, 0, 0, 0.25f, 1));
    add(cur, Quad(205, 150, 40, 60, 9, 0.25f, 0, 0.5f, 1));
    add(cur, Quad(300, 150, 40, 40, 10));
    std::vector<DrawMatch> m;
    const fe::MatchStats st = fe::match_draws(prev, cur, fe::MatchLimits{}, &m);
    CHECK(st.exact == 8);
    CHECK(st.loose == 1);
    CHECK(st.camera == 1);
    CHECK(st.none == 0);
    REQUIRE(m[8].kind == MatchKind::kLoose);
    CHECK(m[8].dx == doctest::Approx(-5));
    CHECK(blended_x(prev, cur, 8, m[8], 0.4f) == doctest::Approx(202)); // 205 - 5 x 0.6
    REQUIRE(m[9].kind == MatchKind::kCamera);
    CHECK(blended_x(prev, cur, 9, m[9], 0.0f) == doctest::Approx(303)); // where the camera was
    // Without a scroll a new draw is drawn as it is.
    DrawList still;
    add(still, Quad(300, 150, 40, 40, 10));
    DrawList none;
    add(none, Quad(0, 0, 10, 10, 11));
    const fe::MatchStats st2 = fe::match_draws(none, still, fe::MatchLimits{}, &m);
    CHECK(st2.none == 1);
}

TEST_CASE("frame_interp: a map change snaps, colours blend") {
    DrawList prev, cur;
    for (int i = 0; i < 20; i++) {
        add(prev, Quad(20.0f * static_cast<float>(i), 0, 20, 20, 1));
        add(cur, Quad(20.0f * static_cast<float>(i), 0, 20, 20, 2)); // another map's tiles
    }
    std::vector<DrawMatch> m;
    CHECK(fe::match_draws(prev, cur, fe::MatchLimits{}, &m).snapped);
    // A fade: the colour goes from 0 to 200 alpha; halfway it is 100.
    DrawList a, b;
    Quad qa(0, 0, 480, 320, 0);
    qa.draw.color[3] = 0;
    Quad qb(0, 0, 480, 320, 0);
    qb.draw.color[3] = 200;
    add(a, qa);
    add(b, qb);
    REQUIRE_FALSE(fe::match_draws(a, b, fe::MatchLimits{}, &m).snapped);
    std::vector<mt_gfx_vertex> v;
    std::uint8_t color[4];
    fe::blend_draw(a, b, 0, m[0], 0.5f, &v, color);
    CHECK(color[3] == 100);
    CHECK(fe::frame_alpha(10.025, 10.0, 0.05) == doctest::Approx(0.5));
    CHECK(fe::frame_alpha(9.0, 10.0, 0.05) == 0);
    CHECK(fe::frame_alpha(11.0, 10.0, 0.05) == 1);
    CHECK(fe::frame_alpha(8e8 + 0.04999999, 8e8, 0.05) == 1); // a CF time's rounding
}

TEST_CASE("frame_interp: the replay at alpha 1 is the tick's frame byte for byte, at 0.5 halfway") {
    mt_gfx_reset();
    mt_gfx_target *target = mt_gfx_target_create(480, 320, 1);
    REQUIRE(target != nullptr);
    const std::uint32_t tex = mt_gfx_texture_create();
    const std::uint8_t red[4 * 4] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    REQUIRE(mt_gfx_texture_image(tex, 2, 2, MT_GFX_RGBA, MT_GFX_UNSIGNED_BYTE, red) == 0);
    REQUIRE(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MIN_FILTER, MT_GFX_NEAREST) == 0);
    REQUIRE(mt_gfx_texture_parameter(tex, MT_GFX_TEXTURE_MAG_FILTER, MT_GFX_NEAREST) == 0);
    // A tick: the background (opaque blue, the whole screen) and a 16 x 16 red quad at x.
    const auto tick = [&](float x) {
        Quad bg(0, 0, 480, 320, 0);
        bg.draw.color[0] = 0;
        bg.draw.color[1] = 0;
        bg.draw.color[2] = 255;
        REQUIRE(mt_gfx_submit(target, &bg.draw) == MT_GFX_DRAWN);
        Quad q(x, 100, 16, 16, tex);
        REQUIRE(mt_gfx_submit(target, &q.draw) == MT_GFX_DRAWN);
    };
    // Window pixel (x, y) (y up) of an image.
    const auto red_at = [](const std::uint8_t *px, int x, int y) {
        const std::uint8_t *p = px + (static_cast<std::size_t>(319 - y) * 480 + x) * 4;
        return p[0] == 255 && p[2] == 0;
    };
    fe::FrameInterpolator interp;
    interp.start();
    tick(100);
    CHECK_FALSE(interp.end_tick(target, 1.00, 0.05)); // nothing before it
    CHECK(interp.render(0.5f) == nullptr);
    tick(120);
    REQUIRE(interp.end_tick(target, 1.05, 0.05));
    CHECK(interp.last().exact == 2);
    const mt_gfx_target *out = interp.render(1.0f);
    REQUIRE(out != nullptr);
    CHECK(std::memcmp(mt_gfx_target_cpixels(out), mt_gfx_target_cpixels(target), 480 * 320 * 4) ==
          0);
    out = interp.render(0.5f);
    REQUIRE(out != nullptr);
    const std::uint8_t *px = mt_gfx_target_cpixels(out);
    CHECK(red_at(px, 112, 108)); // the quad at 110 to 126
    CHECK_FALSE(red_at(px, 106, 108));
    CHECK(red_at(px, 124, 108));
    CHECK_FALSE(red_at(px, 128, 108));
    // The replays were not recorded: the next tick has only its own two draws.
    tick(140);
    REQUIRE(interp.end_tick(target, 1.10, 0.05));
    CHECK(interp.last().exact == 2);
    CHECK(interp.last().loose + interp.last().camera + interp.last().none == 0);
    // A gap of more than 2.5 ticks (a load, a pause) snaps.
    tick(160);
    CHECK_FALSE(interp.end_tick(target, 1.30, 0.05));
    CHECK(std::strcmp(interp.last().why, "the ticks are too far apart") == 0);
    // A texture freed since the tick: its draws are skipped.
    tick(180);
    REQUIRE(interp.end_tick(target, 1.35, 0.05));
    mt_gfx_texture_destroy(tex);
    REQUIRE(interp.render(0.5f) != nullptr);
    CHECK(interp.totals().skipped == 1);
    CHECK(interp.totals().ticks == 5);
    CHECK(interp.totals().snaps == 2);
    // Stopped, mt_gfx has no observer: nothing is recorded any more.
    interp.stop();
    Quad bg(0, 0, 480, 320, 0);
    REQUIRE(mt_gfx_submit(target, &bg.draw) == MT_GFX_DRAWN);
    interp.start();
    CHECK_FALSE(interp.end_tick(target, 1.40, 0.05));
    interp.stop();
    mt_gfx_target_destroy(target);
    mt_gfx_reset();
}

TEST_CASE("frame_interp: locked at 60 Hz the refreshes of a tick show 1/3, 2/3 and all of it") {
    struct Host {
        std::uint64_t now = 5'000'000'000ull;
        static std::uint64_t now_fn(void *u) { return static_cast<Host *>(u)->now; }
        static void sleep_fn(void *u, std::uint64_t ns) { static_cast<Host *>(u)->now += ns; }
    } host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, 1735689600ull * 1000000ull);
    const fe::PacingPlan plan = fe::plan_pacing(fe::PacingMode::kDisplay, 60, true, 50);
    REQUIRE(plan.locked);
    REQUIRE(plan.vblanks == 3);
    clock.set_plan(plan);
    fe::PresentHold hold;
    std::mt19937 rng(87);
    std::uniform_int_distribution<std::uint64_t> work(1'000'000, 9'000'000);
    const std::uint64_t k0 = clock.vblank_at(host.now) + 1;
    std::uint64_t interp_presents = 0;
    for (std::uint64_t i = 0; i < 600; i++) {
        // The tick's wake at every third refresh, its work and its present at the next one.
        const std::uint64_t w = clock.vblank_host(k0 + 3 * i);
        REQUIRE(host.now <= w); // the presents between ticks never push a tick later
        host.now = w;
        hold.wake(w);
        const double tick = static_cast<double>(clock.guest_ns()) / 1e9;
        host.now += work(rng);
        double shown = static_cast<double>(clock.guest_at(hold.shown_at(clock, host.now))) / 1e9;
        CHECK(fe::frame_alpha(shown, tick, 0.05) == doctest::Approx(1.0 / 3).epsilon(1e-6));
        host.now = hold.present_at(clock, host.now);
        CHECK(host.now == clock.vblank_host(k0 + 3 * i + 1));
        // present_smooth_locked: one present a refresh until the next tick's wake.
        const std::uint64_t deadline = clock.vblank_host(k0 + 3 * i + 3);
        double alpha = 0;
        for (int k = 2; alpha < 1; k++) {
            hold.wake(clock.vblank_host(clock.vblank_at(host.now)));
            const std::uint64_t at = hold.shown_at(clock, host.now);
            REQUIRE(at <= deadline);
            shown = static_cast<double>(clock.guest_at(at)) / 1e9;
            alpha = fe::frame_alpha(shown, tick, 0.05);
            CHECK(alpha == doctest::Approx(k / 3.0).epsilon(1e-6));
            host.now += 2'000'000; // the replay, the compose
            host.now = hold.present_at(clock, host.now);
            CHECK(host.now == at);
            CHECK(host.now == clock.vblank_host(k0 + 3 * i + static_cast<std::uint64_t>(k)));
            interp_presents++;
        }
    }
    CHECK(interp_presents == 1200);
}
