// The logged performance session (#244, ADR 0145, perf_session.h), synthetic: window frames
// made up as FrameStats records them, so every count and sum of each "perf:" line is known. The
// line's exact text is also the input of tests/test_perfsession.py (kLine below), so the game's
// lines and the judge's parser agree.
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "frame_stats.h"
#include "perf_session.h"

namespace {

namespace fe = maplethief::frontend;

constexpr std::uint64_t kMs = 1'000'000;

// A tick's window frame at 60 Hz, locked: 50 ms, held 3 vblanks, 2 ms of guest, 1 ms of mt_gfx,
// 0.5 ms each of take and compositor, 0.25 of draw and 4 of present; the rest waited.
fe::FrameRecord tick_frame(std::uint64_t at_ms, std::uint64_t interval_ms = 50,
                           std::int32_t vblanks = 3) {
    fe::FrameRecord r;
    r.tick = true;
    r.at_ns = at_ms * kMs;
    r.interval_ns = interval_ms * kMs;
    r.tick_ns = interval_ms * kMs;
    r.vblanks = vblanks;
    r.guest_ns = 2 * kMs;
    r.gfx_ns = 1 * kMs;
    r.take_ns = kMs / 2;
    r.compose_ns = kMs / 2;
    r.draw_ns = kMs / 4;
    r.present_ns = 4 * kMs;
    r.other_ns = kMs / 4;
    r.wait_ns = r.interval_ns - (2 * kMs + kMs + kMs / 2 + kMs / 2 + kMs / 4 + 4 * kMs + kMs / 4);
    return r;
}

fe::PerfContext handheld_map() {
    fe::PerfContext c;
    c.display = "handheld";
    c.scene = "map:2.0";
    c.backend = "gpu";
    return c;
}

// The line of 20 such ticks (one second): tests/test_perfsession.py parses the same text.
const char *const kLine =
    "perf: t=1.050 s=1.000 display=handheld scene=map:2.0 ff=1 scale=1 backend=gpu frames=20 "
    "ticks=20 held=20 held3=20 over=0 tick_max=50.00 guest=40.00 gfx=20.00 take=10.00 "
    "compose=10.00 draw=5.00 present=80.00 wait=830.00 other=5.00";

} // namespace

TEST_CASE("perf session: the scene from the game's mode, area and map") {
    CHECK(fe::perf_scene(-1, -1, -1) == "none");
    CHECK(fe::perf_scene(0, 2, 0) == "map:2.0");
    CHECK(fe::perf_scene(1, 7, 4) == "map:7.4"); // the popup menu over the field
    CHECK(fe::perf_scene(0, -1, -1) == "menu");  // a field mode without a map
    CHECK(fe::perf_scene(6, 2, 0) == "load");
    CHECK(fe::perf_scene(8, 2, 0) == "load");
    CHECK(fe::perf_scene(9, 2, 0) == "scene");
    CHECK(fe::perf_scene(10, 2, 0) == "scene");
    CHECK(fe::perf_scene(3, 2, 0) == "menu");
}

TEST_CASE("perf session: one line a second with the window's counts and sums") {
    fe::PerfSession s;
    std::vector<std::string> lines;
    // The first frame after start has no interval: not counted.
    fe::FrameRecord first = tick_frame(50);
    first.interval_ns = 0;
    CHECK_FALSE(s.add(first, handheld_map()).has_value());
    for (std::uint64_t i = 1; i <= 45; i++) {
        if (std::optional<std::string> l = s.add(tick_frame(50 + i * 50), handheld_map())) {
            lines.push_back(*l);
        }
    }
    REQUIRE(lines.size() == 2); // 20 frames a second
    CHECK(lines[0] == kLine);
    CHECK(lines[1].rfind("perf: t=2.050 s=1.000 ", 0) == 0);
    std::optional<std::string> rest = s.flush();
    REQUIRE(rest.has_value());
    CHECK(rest->find(" s=0.250 ") != std::string::npos);
    CHECK(rest->find(" frames=5 ticks=5 ") != std::string::npos);
    CHECK_FALSE(s.flush().has_value());
    CHECK(s.lines() == 3);
}

TEST_CASE("perf session: a change of screen, scene or rate, or a pause, ends the window") {
    fe::PerfSession s;
    std::vector<std::string> lines;
    auto add = [&](const fe::FrameRecord &r, const fe::PerfContext &c) {
        if (std::optional<std::string> l = s.add(r, c)) {
            lines.push_back(*l);
        }
    };
    std::uint64_t t = 0;
    for (int i = 0; i < 4; i++) {
        add(tick_frame(t += 50), handheld_map());
    }
    fe::PerfContext docked = handheld_map();
    docked.display = "docked";
    add(tick_frame(t += 50), docked); // closes the handheld window
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find(" display=handheld ") != std::string::npos);
    CHECK(lines[0].find(" frames=4 ") != std::string::npos);
    fe::PerfContext ff = docked;
    ff.ff = 2;
    add(tick_frame(t += 25, 25, 2), ff); // fast-forward x2: two ticks a tick's time
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].find(" display=docked ") != std::string::npos);
    CHECK(lines[1].find(" frames=1 ") != std::string::npos);
    // A pause (HOME, sleep): the frame after it has no interval and is not counted.
    fe::FrameRecord after = tick_frame(t += 2000);
    after.interval_ns = 0;
    add(after, ff);
    REQUIRE(lines.size() == 3);
    CHECK(lines[2].find(" ff=2 ") != std::string::npos);
    CHECK(lines[2].find(" frames=1 ticks=1 held=1 held3=0 ") != std::string::npos);
    CHECK_FALSE(s.flush().has_value()); // nothing since the pause
    // A load: its own scene.
    fe::PerfContext load = handheld_map();
    load.scene = "load";
    add(tick_frame(t += 50), load);
    add(tick_frame(t += 50), handheld_map());
    REQUIRE(lines.size() == 4);
    CHECK(lines[3].find(" scene=load ") != std::string::npos);
}

TEST_CASE("perf session: vblanks, late ticks and frames between ticks") {
    fe::PerfSession s;
    fe::PerfContext c = handheld_map();
    std::uint64_t t = 0;
    s.add(tick_frame(t += 50), c);
    s.add(tick_frame(t += 67, 67, 4), c); // a late tick, held 4
    fe::FrameRecord extra = tick_frame(t += 10, 10, 0);
    extra.tick = false; // a window frame without a new GL frame, replaced before it was shown
    s.add(extra, c);
    fe::FrameRecord unknown = tick_frame(t += 40, 40, -1); // no scan-out time known
    s.add(unknown, c);
    const std::optional<std::string> l = s.flush();
    REQUIRE(l.has_value());
    CHECK(l->find(" frames=4 ticks=3 held=3 held3=1 over=1 tick_max=67.00 ") != std::string::npos);
}
