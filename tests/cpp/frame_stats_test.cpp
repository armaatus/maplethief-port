// Frame telemetry and the frame-time overlay (#137, ADR 0071), synthetic: FrameStats driven
// through a real-time run as game.cpp's hooks drive it (the guest, the GL present hook's
// phases, the turn hook's wait, a pause), with host timestamps made up so that every number of
// the exit summary is known; the CSV rows and the file; the last second's means; and
// PerfOverlay on SDL's dummy video driver: F3 toggles it live, it draws into the window only
// when visible, and never in a run that writes images (--fixed-step, --shot, --capture).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "frame_stats.h"
#include "game.h"
#include "perf_overlay.h"

namespace {

namespace fe = maplethief::frontend;
using fe::Phase;

constexpr std::uint64_t kMs = 1'000'000;
constexpr std::uint64_t kUs = 1'000;

// A real-time run as game.cpp's hooks report it. Each tick: the guest runs 3 ms (1 ms of it in
// mt_gfx_submit), then presents: the GL present hook takes the frame (0.1 ms), composites
// (5 ms), uploads and draws (0.2 ms) and presents (0.5 ms), the window frame is done, and the
// hook spends 0.05 ms more before it returns. 8.85 ms in all; the turn hook then waits the rest
// of the tick interval.
struct Run {
    static constexpr std::uint64_t kTickWork =
        3 * kMs + 100 * kUs + 5 * kMs + 200 * kUs + 500 * kUs + 50 * kUs;
    fe::FrameStats s;
    std::uint64_t t = 1'000 * kMs; // SDL_GetTicksNS at the start
    std::uint64_t gfx = 0;         // mt_gfx_submit's running total
    std::uint64_t index = 0;
    std::uint32_t gl = 0;

    explicit Run(double refresh_hz = 60, bool vsync = true) { s.start(t, refresh_hz, vsync, gfx); }

    void tick() {
        t += 3 * kMs; // the guest: the timer fires, the game updates and draws
        gfx += 1 * kMs;
        s.enter_host(t); // -presentRenderbuffer: -> the GL present hook
        s.gl_present(t);
        s.add(Phase::kTake, 100 * kUs);
        t += 100 * kUs;
        s.add(Phase::kCompose, 5 * kMs);
        t += 5 * kMs;
        s.add(Phase::kDraw, 200 * kUs);
        t += 200 * kUs;
        s.add(Phase::kPresent, 500 * kUs);
        t += 500 * kUs;
        s.frame(t, ++index, ++gl, true, gfx);
        t += 50 * kUs;
        s.leave_host(t);
    }
    // The turn hook after the tick: waits so that the next tick comes `interval` after this one.
    void wait_for(std::uint64_t interval) {
        s.enter_host(t);
        const std::uint64_t w = interval - kTickWork;
        s.add(Phase::kWait, w);
        t += w;
        s.leave_host(t);
    }
    // The turn hook waits while the window is minimised (wait_hidden), then the run goes on.
    void hidden(std::uint64_t ns) {
        s.enter_host(t);
        t += ns;
        s.gap(t, gfx);
        s.leave_host(t);
    }
};

// 203 ticks: 99 at 50 ms, then 56, 70 and 80 ms (three overruns), a 2 s minimise (a pause, not
// an overrun), then 99 more at 50 ms.
void overrun_run(Run &r) {
    for (int i = 0; i < 99; i++) {
        r.tick();
        r.wait_for(50 * kMs);
    }
    for (const std::uint64_t interval : {56 * kMs, 70 * kMs, 80 * kMs}) {
        r.tick();
        r.wait_for(interval);
    }
    r.tick();
    r.hidden(2'000 * kMs);
    for (int i = 0; i < 99; i++) {
        r.tick();
        r.wait_for(50 * kMs);
    }
    r.tick();
}

} // namespace

TEST_CASE("frame_stats: a synthetic real-time run's exit summary: tick p50, p99, max, GL frames "
          "a second, overruns, the split and the vblanks (#137)") {
    Run r;
    overrun_run(r);

    const fe::TickStats t = r.s.ticks();
    CHECK(t.ticks == 203);
    CHECK(t.intervals == 201); // none across the pause, none before the first
    CHECK(t.overruns == 3);
    CHECK(t.p50_ms == doctest::Approx(50.0));
    CHECK(t.p99_ms == doctest::Approx(56.0)); // the 199th of 201 sorted
    CHECK(t.max_ms == doctest::Approx(80.0));
    CHECK(t.mean_ms == doctest::Approx(10106.0 / 201));
    CHECK(t.per_second == doctest::Approx(201 / 10.106));
    CHECK(r.s.frames() == 203);

    // The vblanks each frame was shown for (scan-out simulated at 60 Hz from the present
    // times): 3 at 50 ms; the 70 ms tick's frame 5 and the 80 ms tick's 4 (the phase carries:
    // 56 ms stays at 3); none across the pause.
    CHECK(r.s.held()[3] == 199);
    CHECK(r.s.held()[4] == 1);
    CHECK(r.s.held()[5] == 1);

    const std::vector<std::string> lines = r.s.summary(false);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "ticks: 203 GL frames, 19.89 a second; tick interval p50 50.00 ms, p99 56.00 "
                      "ms, max 80.00 ms, mean 50.28 ms; 3 over 55 ms");
    // Waiting: 198 waits of 41.15 ms and 47.15, 61.15, 71.15 over 203 frames (the first frame
    // and the first after the pause waited for nothing). Other: the 0.05 ms after each present,
    // in the next frame, but not across the pause.
    CHECK(lines[1] == "frame split: 203 window frames; per frame guest 2.00 ms, mt_gfx 1.00 ms, "
                      "taking the GL frame 0.10 ms, compositor 5.00 ms, upload and draw 0.20 ms, "
                      "SDL_RenderPresent 0.50 ms, waiting 41.02 ms, other 0.05 ms");
    CHECK(lines[2] == "pacing: 60.00 Hz display, vsync on, scan-out simulated from the present "
                      "times: 201 window frames shown for 3 vblanks 99.00% (199), 4 vblanks "
                      "0.50% (1), 5 vblanks 0.50% (1)");

    // The fixed-step mode says that its wall-clock times are not the game's speed.
    CHECK(r.s.summary(true)[0].find("(wall clock: the fixed-step mode is not paced)") !=
          std::string::npos);
    // Overruns by another limit (#151's slower game speeds).
    Run slow;
    slow.s.set_overrun_ns(75 * kMs);
    overrun_run(slow);
    CHECK(slow.s.ticks().overruns == 1);
}

TEST_CASE("frame_stats: each frame's split adds up to its interval; the guest's mt_gfx time is "
          "mt_gfx's") {
    Run r;
    for (int i = 0; i < 5; i++) {
        r.tick();
        r.wait_for(50 * kMs);
    }
    r.tick();
    const fe::FrameRecord *f = r.s.last();
    REQUIRE(f != nullptr);
    CHECK(f->index == 6);
    CHECK(f->gl_frame == 6);
    CHECK(f->tick);
    CHECK(f->interval_ns == 50 * kMs);
    CHECK(f->tick_ns == 50 * kMs);
    CHECK(f->at_ns == 5 * 50 * kMs + Run::kTickWork - 50 * kUs);
    CHECK(f->guest_ns == 2 * kMs);
    CHECK(f->gfx_ns == 1 * kMs);
    CHECK(f->take_ns == 100 * kUs);
    CHECK(f->compose_ns == 5 * kMs);
    CHECK(f->draw_ns == 200 * kUs);
    CHECK(f->present_ns == 500 * kUs);
    CHECK(f->wait_ns == 50 * kMs - Run::kTickWork);
    CHECK(f->other_ns == 50 * kUs);
    CHECK(f->frontend_ns() == 5300 * kUs);
    CHECK(f->guest_ns + f->gfx_ns + f->take_ns + f->compose_ns + f->draw_ns + f->present_ns +
              f->wait_ns + f->other_ns ==
          f->interval_ns);

    // A window frame without a new GL frame (a UIKit animation, from the turn hook): no tick.
    r.s.enter_host(r.t);
    r.s.add(Phase::kCompose, 2 * kMs);
    r.t += 2 * kMs;
    const fe::FrameRecord &anim = r.s.frame(r.t, ++r.index, r.gl, false, r.gfx);
    CHECK_FALSE(anim.tick);
    CHECK(anim.tick_ns == 0);
    CHECK(anim.guest_ns == 0);
    CHECK(anim.compose_ns == 2 * kMs);
    CHECK(anim.other_ns == 50 * kUs); // what the GL present hook did after the last frame
    r.s.leave_host(r.t);

    // A hook inside a hook (the turn hook calls the guest, which presents): the guest code in
    // between counts as the outer hook's, and the guest's own time starts again only when the
    // outer hook returns.
    fe::FrameStats s;
    s.start(0, 0, false, 0);
    s.enter_host(10 * kMs); // the turn hook: 10 ms of guest before it
    s.enter_host(12 * kMs); // the GL present hook, inside it
    s.leave_host(13 * kMs);
    s.leave_host(15 * kMs); // the turn hook returns
    s.enter_host(18 * kMs); // 3 ms more of guest
    const fe::FrameRecord &n = s.frame(19 * kMs, 1, 1, true, 0);
    CHECK(n.guest_ns == 13 * kMs);
    CHECK(n.other_ns == 6 * kMs);
    CHECK(n.vblanks == -1); // no vsync, no refresh rate
}

TEST_CASE("frame_stats: the vblanks a frame was shown for, from the present times") {
    // At 60 Hz the vblanks fall at multiples of 16.667 ms after the start; a frame is first
    // shown at the next one after its present.
    fe::FrameStats s;
    s.start(0, 60, true, 0);
    const std::uint64_t at[] = {10 * kMs, 60 * kMs, 110 * kMs, 112 * kMs, 180 * kMs, 300 * kMs};
    // vblank:                   1          4          7          7           11          19
    const std::int32_t held[] = {-1, 3, 3, 0, 4, 8};
    for (std::size_t i = 0; i < std::size(at); i++) {
        CHECK(s.frame(at[i], i + 1, static_cast<std::uint32_t>(i + 1), true, 0).vblanks == held[i]);
    }
    CHECK(s.held()[0] == 1); // replaced before it was shown
    CHECK(s.held()[3] == 2);
    CHECK(s.held()[4] == 1);
    CHECK(s.held()[8] == 1);
    // 120 Hz and 144 Hz: the same frames on other displays.
    fe::FrameStats f;
    f.start(0, 144, true, 0);
    f.frame(0, 1, 1, true, 0);
    CHECK(f.frame(50 * kMs, 2, 2, true, 0).vblanks == 7);  // 7.2 refreshes
    CHECK(f.frame(100 * kMs, 3, 3, true, 0).vblanks == 7); // 14.4
    CHECK(f.frame(150 * kMs, 4, 4, true, 0).vblanks == 7); // 21.6
    CHECK(f.frame(200 * kMs, 5, 5, true, 0).vblanks == 7); // 28.8
    CHECK(f.frame(250 * kMs, 6, 6, true, 0).vblanks == 8); // 36: 7 and 8 alternate
    // Without vsync (or a refresh rate) nothing is counted.
    fe::FrameStats n;
    n.start(0, 60, false, 0);
    n.frame(10 * kMs, 1, 1, true, 0);
    CHECK(n.frame(60 * kMs, 2, 2, true, 0).vblanks == -1);
    CHECK(n.summary(false).back() == "pacing: no vblank counts (vsync off)");
    fe::FrameStats u;
    u.start(0, 0, true, 0);
    u.frame(10 * kMs, 1, 1, true, 0);
    u.frame(60 * kMs, 2, 2, true, 0);
    CHECK(u.summary(false).back() == "pacing: no vblank counts (the refresh rate is unknown)");
}

TEST_CASE("frame_stats: the last second's means, for the overlay and run-time decisions") {
    Run r;
    CHECK(r.s.last() == nullptr);
    CHECK(r.s.recent().frames == 0);
    for (int i = 0; i < 30; i++) {
        r.tick();
        r.wait_for(50 * kMs);
    }
    r.tick();
    const fe::RecentStats s = r.s.recent();
    CHECK(s.frames == 21); // the newest and the 20 within 1 s before it
    CHECK(s.seconds == doctest::Approx(1.05));
    CHECK(s.fps == doctest::Approx(20.0));
    CHECK(s.tps == doctest::Approx(20.0));
    CHECK(s.tick_ms == doctest::Approx(50.0));
    CHECK(s.max_tick_ms == doctest::Approx(50.0));
    CHECK(s.guest_ms == doctest::Approx(2.0));
    CHECK(s.gfx_ms == doctest::Approx(1.0));
    CHECK(s.compose_ms == doctest::Approx(5.0));
    CHECK(s.frontend_ms == doctest::Approx(5.3));
    CHECK(s.max_frontend_ms == doctest::Approx(5.3));
    CHECK(s.wait_ms == doctest::Approx(41.15));
    // A pause starts a new stretch: nothing before it counts.
    r.wait_for(50 * kMs);
    r.hidden(500 * kMs);
    r.tick();
    r.wait_for(50 * kMs);
    r.tick();
    const fe::RecentStats after = r.s.recent();
    CHECK(after.frames == 2);
    CHECK(after.fps == doctest::Approx(20.0));
    // More frames than the ring holds: still the last second's.
    Run fast(120);
    for (std::size_t i = 0; i < fe::FrameStats::kRecentFrames + 40; i++) {
        fast.tick();
        fast.wait_for(10 * kMs);
    }
    CHECK(fast.s.recent().frames == 101);
    CHECK(fast.s.recent().fps == doctest::Approx(100.0));
    CHECK(fast.s.last()->index == fe::FrameStats::kRecentFrames + 40);
}

TEST_CASE("frame_stats: --perf-csv: a header and a row per window frame") {
    CHECK(fe::FrameStats::csv_header() ==
          "frame,gl_frame,tick,present_s,interval_ms,tick_ms,vblanks,guest_ms,gfx_ms,take_ms,"
          "compose_ms,draw_ms,present_ms,wait_ms,other_ms");
    Run r;
    r.tick();
    const fe::FrameRecord first = *r.s.last();
    r.wait_for(50 * kMs);
    r.tick();
    const fe::FrameRecord second = *r.s.last();
    // The first frame has no interval, tick interval or vblank count: empty cells.
    CHECK(fe::FrameStats::csv_row(first) ==
          "1,1,1,0.008800,,,,2.000,1.000,0.100,5.000,0.200,0.500,0.000,0.000");
    CHECK(fe::FrameStats::csv_row(second) ==
          "2,2,1,0.058800,50.000,50.000,3,2.000,1.000,0.100,5.000,0.200,0.500,41.150,0.050");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("maplethief-perf-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "perf.csv";
    {
        fe::PerfCsv csv;
        REQUIRE(csv.open(file.string()));
        csv.write(first);
        csv.write(second);
        CHECK(csv.rows() == 2);
        CHECK(csv.close());
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text == fe::FrameStats::csv_header() + "\n" + fe::FrameStats::csv_row(first) + "\n" +
                      fe::FrameStats::csv_row(second) + "\n");
    in.close();
    fe::PerfCsv bad;
    CHECK_FALSE(bad.open((dir / "no-such-dir" / "perf.csv").string()));
    std::filesystem::remove_all(dir);
}

namespace {

// A dummy-driver window with a software renderer, 480x320.
struct OverlayWindow {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    OverlayWindow() {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        REQUIRE_MESSAGE(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        REQUIRE_MESSAGE(
            SDL_CreateWindowAndRenderer("frame_stats_test", 480, 320, 0, &window, &renderer),
            SDL_GetError());
    }
    ~OverlayWindow() {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    OverlayWindow(const OverlayWindow &) = delete;
    OverlayWindow &operator=(const OverlayWindow &) = delete;

    // Clears to black, draws the overlay over it and counts the pixels that are not black.
    long draw(const fe::PerfOverlay &overlay, const fe::FrameStats &stats) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
        SDL_RenderClear(renderer);
        overlay.draw(renderer, stats, 1);
        SDL_Surface *read = SDL_RenderReadPixels(renderer, nullptr);
        REQUIRE(read != nullptr);
        long lit = 0;
        for (int y = 0; y < read->h; y++) {
            for (int x = 0; x < read->w; x++) {
                Uint8 r = 0, g = 0, b = 0, a = 0;
                SDL_ReadSurfacePixel(read, x, y, &r, &g, &b, &a);
                if (r != 0 || g != 0 || b != 0) {
                    lit++;
                    CHECK(y < 320 / 5); // along the top
                }
            }
        }
        SDL_DestroySurface(read);
        return lit;
    }
};

SDL_Event key(SDL_EventType type, SDL_Keycode k, SDL_Keymod mod = SDL_KMOD_NONE,
              bool repeat = false) {
    SDL_Event e{};
    e.type = type;
    e.key.key = k;
    e.key.mod = mod;
    e.key.repeat = repeat;
    e.key.down = type == SDL_EVENT_KEY_DOWN;
    return e;
}

} // namespace

TEST_CASE("perf_overlay: F3 toggles it live; it draws into the window only while visible") {
    OverlayWindow w;
    Run r;
    for (int i = 0; i < 25; i++) {
        r.tick();
        r.wait_for(50 * kMs);
    }
    fe::PerfOverlay overlay; // perf_overlay = off (the default)
    CHECK_FALSE(overlay.visible());
    CHECK(w.draw(overlay, r.s) == 0);

    CHECK(overlay.handle(key(SDL_EVENT_KEY_DOWN, SDLK_F3)));
    CHECK(overlay.visible());
    const long lit = w.draw(overlay, r.s);
    CHECK(lit > 500); // a box of text
    // A held key repeats: no toggle; its release: no toggle. Both are the overlay's.
    CHECK(overlay.handle(key(SDL_EVENT_KEY_DOWN, SDLK_F3, SDL_KMOD_NONE, true)));
    CHECK(overlay.handle(key(SDL_EVENT_KEY_UP, SDLK_F3)));
    CHECK(overlay.visible());
    // Ctrl+F3 and other keys are not its.
    CHECK_FALSE(overlay.handle(key(SDL_EVENT_KEY_DOWN, SDLK_F3, SDL_KMOD_LCTRL)));
    CHECK_FALSE(overlay.handle(key(SDL_EVENT_KEY_DOWN, SDLK_F1)));
    CHECK(overlay.visible());
    CHECK(overlay.presses() == 1);
    // Shift+F3 toggles too (a keyboard whose F keys need it); then F3 shows it again.
    CHECK(overlay.handle(key(SDL_EVENT_KEY_DOWN, SDLK_F3, SDL_KMOD_LSHIFT)));
    CHECK_FALSE(overlay.visible());
    CHECK(w.draw(overlay, r.s) == 0);
    CHECK(overlay.handle(key(SDL_EVENT_KEY_DOWN, SDLK_F3)));
    CHECK(w.draw(overlay, r.s) == lit);

    // The setting shows it from the start; drawing leaves the renderer's state as it was.
    const fe::PerfOverlay on(true, true);
    CHECK(on.visible());
    SDL_SetRenderDrawBlendMode(w.renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(w.renderer, 1, 2, 3, 4);
    on.draw(w.renderer, r.s, 2);
    float sx = 0, sy = 0;
    SDL_GetRenderScale(w.renderer, &sx, &sy);
    CHECK(sx == 1.0f);
    CHECK(sy == 1.0f);
    SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
    SDL_GetRenderDrawBlendMode(w.renderer, &blend);
    CHECK(blend == SDL_BLENDMODE_NONE);
    Uint8 c[4] = {};
    SDL_GetRenderDrawColor(w.renderer, &c[0], &c[1], &c[2], &c[3]);
    CHECK(c[0] == 1);
    CHECK(c[3] == 4);

    // What it shows: the last second's numbers.
    const std::vector<std::string> text = fe::PerfOverlay::lines(r.s.recent(), r.s.last());
    REQUIRE(text.size() == 4);
    CHECK(text[0] == " 20.0 fps  20.0 ticks/s  tick  50.0 ms (max 50.0)");
    CHECK(text[1] == "guest  2.00  gfx  1.00  take  0.10 ms");
    CHECK(text[2] == "comp   5.00  draw  0.20  frontend  5.30 (max 5.3)");
    CHECK(text[3] == "present  0.50  wait 41.15 ms  vblanks 3");
}

TEST_CASE("perf_overlay: never in a run that writes images: --fixed-step, --shot, --capture") {
    fe::GameOptions o;
    CHECK(fe::perf_overlay_allowed(o)); // a real-time run that writes nothing
    o.fixed_step_us = 1000;
    CHECK_FALSE(fe::perf_overlay_allowed(o));
    o = {};
    o.shots.push_back(fe::Shot{3.0, "main_menu"});
    CHECK_FALSE(fe::perf_overlay_allowed(o));
    o = {};
    o.capture_dir = "captures";
    CHECK_FALSE(fe::perf_overlay_allowed(o));
    o = {};
    o.frames = 150; // --frames checks the window's read-back, drawn before the overlay
    o.perf_csv = "perf.csv";
    CHECK(fe::perf_overlay_allowed(o));

    // Not allowed: hidden even with the setting on; F3 is taken (no binding sees it) and
    // changes nothing; nothing is drawn.
    OverlayWindow w;
    Run r;
    r.tick();
    fe::PerfOverlay overlay(true, false);
    CHECK_FALSE(overlay.visible());
    CHECK(overlay.handle(key(SDL_EVENT_KEY_DOWN, SDLK_F3)));
    CHECK(overlay.presses() == 1);
    CHECK_FALSE(overlay.visible());
    CHECK(w.draw(overlay, r.s) == 0);
}
