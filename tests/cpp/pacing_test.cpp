// Display-locked frame pacing (#80, ADR 0075), synthetic: plan_pacing's choice for 60, 120,
// 144 Hz and the rest; PacedClock on a fake host clock (the wall mapping, the locked steps, the
// correction against the wall clock on 59.94 and 60.06 Hz displays, plan changes, host_when);
// and the game's own tick gate (CMTimer::OnRunLoopTimer's `now - lastFrame >= interval` on
// GetTimeMs) driven by the turn hook's locked wait, with each tick's present fed to FrameStats'
// simulated scan-out: every GL frame is shown for exactly n refreshes, and the game runs at the
// wall clock's speed. The pacing setting in settings.ini and on the command line. The paced
// clock's start with #155's launch shift: GetTimeMs below 2^30 at boot.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "clock.h"
#include "frame_stats.h"
#include "maplethief/cf.h"
#include "pacing.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
using fe::PacingMode;
using fe::PacingPlan;

constexpr std::uint64_t kMs = 1'000'000;
constexpr std::uint64_t kStartUnixUs = 1735689600ull * 1000000ull; // 2025-01-01

// The fake host: a clock the test moves, and a sleep that moves it.
struct Host {
    std::uint64_t now = 5'000 * kMs;
    static std::uint64_t now_fn(void *u) { return static_cast<Host *>(u)->now; }
    static void sleep_fn(void *u, std::uint64_t ns) { static_cast<Host *>(u)->now += ns; }
};

PacingPlan locked(double hz, std::uint32_t vblanks, std::uint32_t tick_ms = 50) {
    const PacingPlan p = fe::plan_pacing(PacingMode::kDisplay, hz, true, tick_ms);
    REQUIRE(p.locked);
    REQUIRE(p.vblanks == vblanks);
    return p;
}

// What a run on a display did.
struct Outcome {
    std::uint64_t ticks = 0;
    double host_s = 0;          // from the first tick to the last
    double guest_s = 0;         // the same span on the guest clock
    std::int64_t max_drift = 0; // |guest - host| since the start, the largest seen (ns)
    fe::FrameStats stats;
};

// The game on the paced clock, as the turn hook drives it locked to the display: wake when the
// guest's 1 ms run-loop timer is due (host_when), let CMTimer gate the tick on GetTimeMs, and
// have a tick's frame ready `work` after the wake (random 1-9 ms: the guest's update and paint,
// the frontend's take, compose and draw), then present it at PresentHold's point. `stall_at`: a
// tick (index) that takes `stall` instead (an overrun or a load). FrameStats simulates the
// scan-out from its own start, `phase` before the clock's refresh grid (the display's real phase
// is unknown). `max_work`: the longest work.
void run(Outcome &o, fe::PacedClock &clock, Host &host, std::int64_t tick_ms, std::uint64_t ticks,
         double refresh_hz, std::uint64_t phase, std::uint64_t stall_at = 0,
         std::uint64_t stall = 0, std::uint64_t max_work = 9 * kMs) {
    std::mt19937 rng(80);
    std::uniform_int_distribution<std::uint64_t> work(1 * kMs, max_work);
    fe::PresentHold hold;
    const std::uint64_t host0 = host.now;
    const std::uint64_t guest_start = clock.guest_ns();
    o.stats.start(host.now - phase, refresh_hz, true, 0);
    std::int64_t last_frame = fe::guest_time_ms(clock.unix_us());
    std::uint64_t next_fire = clock.guest_ns() + 1 * kMs;
    std::uint64_t first_host = 0, first_guest = 0;
    std::uint32_t gl = 0;
    while (o.ticks < ticks) {
        // The turn hook's wait (wait_locked): until the timer's fire date on the guest clock.
        host.now = std::max(host.now, clock.host_when(next_fire));
        hold.wake(clock.vblank_host(clock.vblank_at(host.now)));
        const std::uint64_t woke = host.now;
        // The run loop fires the timer: CMTimer::OnRunLoopTimer.
        const std::uint64_t guest = clock.guest_ns();
        const std::int64_t now_ms = fe::guest_time_ms(clock.unix_us());
        const auto drift = static_cast<std::int64_t>(guest - guest_start) -
                           static_cast<std::int64_t>(host.now - host0);
        o.max_drift = std::max(o.max_drift, drift < 0 ? -drift : drift);
        if (now_ms - last_frame >= tick_ms) {
            last_frame = now_ms; // lastFrame = now, before the tick runs
            if (o.ticks == 0) {
                first_host = woke;
                first_guest = guest;
            }
            o.ticks++;
            host.now += o.ticks == stall_at ? stall : work(rng);
            o.stats.gl_present(host.now);
            host.now = hold.present_at(clock, host.now); // the present point
            o.stats.frame(host.now, o.ticks, ++gl, true, 0);
            o.host_s = static_cast<double>(woke - first_host) / 1e9;
            o.guest_s = static_cast<double>(guest - first_guest) / 1e9;
        }
        // RunLoopTimer_Arm: the next callout 1 ms after this one returns.
        next_fire = clock.guest_ns() + 1 * kMs;
    }
}

double share(const std::array<std::uint64_t, 16> &h, std::size_t n) {
    std::uint64_t total = 0;
    for (const std::uint64_t c : h) {
        total += c;
    }
    return total == 0 ? 0 : static_cast<double>(h[n]) / static_cast<double>(total);
}

} // namespace

TEST_CASE("pacing: the plan locks when the refresh rate is a multiple of the tick rate") {
    // 50 ms (20 ticks a second): 60, 120, 100, 240 Hz lock; 59.94 and 60.06 Hz too (0.1 %).
    CHECK(locked(60, 3).refresh_hz == 60);
    locked(120, 6);
    locked(100, 5);
    locked(240, 12);
    locked(59.94, 3);
    locked(60.06, 3);
    locked(119.88, 6);
    locked(20, 1);
    // 144 Hz (7.2 a tick), 75 Hz (3.75), 165 Hz, 50 Hz (2.5): not locked to the display (the
    // 1 ms grid: the next test).
    for (const double hz : {144.0, 75.0, 165.0, 50.0, 90.0, 30.0}) {
        const PacingPlan p = fe::plan_pacing(PacingMode::kDisplay, hz, true, 50);
        CHECK_FALSE(p.locked);
        CHECK(p.grid);
        CHECK(std::string(p.why) == "the refresh rate is not a multiple of the tick rate");
    }
    // Over the tolerance: 59.5 Hz is 0.83 % from 60.
    CHECK_FALSE(fe::plan_pacing(PacingMode::kDisplay, 59.5, true, 50).locked);
    // The other game speeds: 40 ms (25 ticks a second) locks at 75 and 100 Hz, not at 60; 65 ms
    // (3.9 refreshes at 60 Hz) nowhere common.
    CHECK_FALSE(fe::plan_pacing(PacingMode::kDisplay, 60, true, 40).locked);
    locked(75, 3, 40);
    locked(100, 4, 40);
    CHECK_FALSE(fe::plan_pacing(PacingMode::kDisplay, 60, true, 65).locked);
    CHECK_FALSE(fe::plan_pacing(PacingMode::kDisplay, 144, true, 65).locked);
    // The boot's 10 ms interval (before Clet_Start sets the game speed): 0.6 refreshes.
    CHECK_FALSE(fe::plan_pacing(PacingMode::kDisplay, 60, true, 10).locked);
    // No vsync, no refresh rate, no interval yet, pacing = wall.
    CHECK(std::string(fe::plan_pacing(PacingMode::kDisplay, 60, false, 50).why) == "vsync off");
    CHECK(std::string(fe::plan_pacing(PacingMode::kDisplay, 0, true, 50).why) ==
          "the refresh rate is unknown");
    CHECK(std::string(fe::plan_pacing(PacingMode::kDisplay, 60, true, 0).why) ==
          "the game's tick interval is not known yet");
    CHECK(std::string(fe::plan_pacing(PacingMode::kDisplay, 60, true, -1).why) ==
          "the game's tick interval is not known yet");
    CHECK(std::string(fe::plan_pacing(PacingMode::kWall, 60, true, 50).why) == "pacing = wall");
    CHECK(fe::describe(locked(60, 3)) ==
          "locked to the display: 60.00 Hz, 3 refreshes a 50 ms tick");
    CHECK(fe::describe(fe::plan_pacing(PacingMode::kDisplay, 144, true, 50)) ==
          "a 1 ms grid (the refresh rate is not a multiple of the tick rate), a 50 ms tick");
}

TEST_CASE("pacing: what the display cannot lock gets a 1 ms grid; the rest is unchanged (#165)") {
    // Every fallback of plan_pacing, at every tick length.
    for (const std::int64_t tick : {65, 50, 40, 33, 25}) {
        CAPTURE(tick);
        const PacingPlan novsync = fe::plan_pacing(PacingMode::kDisplay, 60, false, tick);
        CHECK(novsync.grid);
        CHECK_FALSE(novsync.locked);
        CHECK(novsync.stepped());
        CHECK(novsync.vblanks == static_cast<std::uint32_t>(tick));
        CHECK(novsync.refresh_hz == 1000);
        CHECK(novsync.tick_ms == static_cast<std::uint32_t>(tick));
        CHECK(novsync.rate == 1);
        CHECK(std::string(novsync.why) == "vsync off");
        const PacingPlan unknown = fe::plan_pacing(PacingMode::kDisplay, 0, true, tick);
        CHECK(unknown.grid);
        CHECK(std::string(unknown.why) == "the refresh rate is unknown");
        const PacingPlan hz144 = fe::plan_pacing(PacingMode::kDisplay, 144, true, tick);
        CHECK(hz144.grid);
        CHECK(std::string(hz144.why) == "the refresh rate is not a multiple of the tick rate");
    }
    // 60 Hz at the speeds it cannot lock; 50 ms locks and 75 Hz locks 40 ms, as before.
    for (const std::int64_t tick : {65, 40, 33, 25}) {
        CHECK(fe::plan_pacing(PacingMode::kDisplay, 60, true, tick).grid);
    }
    // The boot's 10 ms interval too: every tick length the display cannot lock.
    CHECK(fe::plan_pacing(PacingMode::kDisplay, 60, true, 10).grid);
    // Unchanged: the display's locks, with no grid in them.
    for (const double hz : {60.0, 120.0, 100.0, 240.0, 59.94, 60.06}) {
        const PacingPlan p = fe::plan_pacing(PacingMode::kDisplay, hz, true, 50);
        CHECK(p.locked);
        CHECK_FALSE(p.grid);
    }
    CHECK(locked(75, 3, 40).refresh_hz == 75);
    CHECK(locked(120, 3, 25).vblanks == 3);
    // No grid without an interval to step by, with pacing = wall, or while fast-forwarding.
    for (const PacingPlan &p : {fe::plan_pacing(PacingMode::kDisplay, 60, true, 0),
                                fe::plan_pacing(PacingMode::kDisplay, 60, false, -1),
                                fe::plan_pacing(PacingMode::kWall, 144, true, 50),
                                fe::plan_pacing(PacingMode::kWall, 60, false, 50),
                                fe::plan_pacing(PacingMode::kDisplay, 144, true, 50, 2),
                                fe::plan_pacing(PacingMode::kDisplay, 60, false, 25, 3)}) {
        CHECK_FALSE(p.stepped());
    }
    CHECK(fe::plan_pacing(PacingMode::kDisplay, 60, false, 25, 3).rate == 3);
}

TEST_CASE("pacing: on a 1 ms grid the guest clock steps whole milliseconds at the wall clock's "
          "speed") {
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
    host.now += 1'234'567; // a start that is no whole millisecond
    const PacingPlan grid = fe::plan_pacing(PacingMode::kDisplay, 144, true, 50);
    REQUIRE(grid.grid);
    REQUIRE(clock.set_plan(grid));
    CHECK_FALSE(clock.set_plan(grid));
    const std::uint64_t h0 = host.now;
    const std::uint64_t g0 = clock.guest_ns();
    // It stands still within a millisecond and steps by exactly 1 ms at each.
    host.now = h0 + 400'000;
    CHECK(clock.guest_ns() == g0);
    host.now = h0 + 1'000'000;
    CHECK(clock.guest_ns() == g0 + 1'000'000);
    host.now = h0 + 1'999'999;
    CHECK(clock.guest_ns() == g0 + 1'000'000);
    // Two hours on: the wall clock's speed, within a step; no drift.
    const std::uint64_t later = 7'200'000 * kMs;
    host.now = h0 + later;
    CHECK(clock.guest_ns() == g0 + later);
    // host_when is the first host time the guest clock reads a time, on the grid.
    std::mt19937_64 rng(165);
    for (int i = 0; i < 2000; i++) {
        const std::uint64_t g = g0 + later + rng() % (3600 * 1000 * kMs);
        const std::uint64_t h = clock.host_when(g);
        CAPTURE(g);
        CHECK(clock.guest_at(h) >= g);
        CHECK(clock.guest_at(h - 1) < g);
        CHECK(clock.vblank_host(clock.vblank_at(h)) == h);
    }
    // A grid is not locked to the display.
    CHECK(clock.locked_ns() == 0);
    // PresentHold never holds a frame on a grid.
    fe::PresentHold hold;
    hold.wake(host.now);
    const std::uint64_t ready = host.now + 5 * kMs;
    CHECK(hold.present_at(clock, ready) == ready);
    hold.wake(host.now);
    CHECK(hold.shown_at(clock, ready) == ready);
    // From and to other plans: continuous, the time never goes back.
    const std::uint64_t before = clock.guest_ns();
    host.now += 777'777;
    clock.set_plan(locked(60, 3));
    host.now += 20 * kMs;
    clock.set_plan(fe::plan_pacing(PacingMode::kDisplay, 0, true, 25));
    host.now += 30 * kMs;
    clock.set_plan(fe::plan_pacing(PacingMode::kDisplay, 60, true, 50, 2));
    CHECK(clock.guest_ns() >= before);
    const auto gap =
        static_cast<std::int64_t>(clock.guest_ns()) - static_cast<std::int64_t>(host.now - h0 + g0);
    CHECK(gap < static_cast<std::int64_t>(17 * kMs));
    CHECK(gap > -static_cast<std::int64_t>(17 * kMs));
}

namespace {

// The game's 1 ms timer as it really is, on doubles (RunLoopTimer_Arm,
// CFRunLoopTimerSetNextFireDate at now + 0.001, the run loop's fire <= now) driven by the turn
// hook's grid wait: the wake lands up to `late_ns` after the guest's next timer. The tick gate is
// on GetTimeMs, as in the game.
struct GridTicks {
    std::uint64_t ticks = 0;
    std::uint64_t exact = 0;   // ticks that lasted exactly the interval on the guest clock
    double first_host_s = 0;   // host time of the first tick
    double last_host_s = 0;    // of the last
    std::int64_t max_late = 0; // longest tick - interval, ms
};

GridTicks run_grid(std::int64_t tick_ms, double tolerance, std::uint64_t late_ns,
                   std::uint64_t ticks, std::uint64_t start_unix_us = kStartUnixUs) {
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, start_unix_us);
    host.now += 123'456;
    clock.set_plan(fe::plan_pacing(PacingMode::kDisplay, 144, true, tick_ms));
    std::mt19937 rng(165);
    std::uniform_int_distribution<std::uint64_t> late(0, late_ns);
    const auto cf = [&clock] {
        return static_cast<double>(clock.unix_us()) / 1e6 - MT_CF_ABSOLUTE_TIME_1970;
    };
    GridTicks o;
    std::int64_t last_frame = fe::guest_time_ms(clock.unix_us());
    double fire = cf() + 0.001;
    while (o.ticks < ticks) {
        // wait_locked: the guest's next timer, rounded to whole guest microseconds with the slack.
        const double wait = fire - cf();
        std::uint64_t deadline = host.now;
        if (wait > 0) {
            const double slack_us = tolerance > 0 ? tolerance * 1e6 : 1e-3;
            const std::uint64_t us = clock.guest_ns() / 1000u +
                                     static_cast<std::uint64_t>(std::ceil(wait * 1e6 - slack_us));
            deadline = clock.host_when(us * 1000u);
        }
        host.now = std::max(host.now, deadline) + late(rng);
        const double now = cf();
        if (fire > now + tolerance) {
            continue; // not due: the next turn
        }
        // The callout: CMTimer::OnRunLoopTimer.
        const std::int64_t now_ms = fe::guest_time_ms(clock.unix_us());
        if (now_ms - last_frame >= tick_ms) {
            if (o.ticks == 0) {
                o.first_host_s = static_cast<double>(host.now) / 1e9;
            } else {
                o.exact += now_ms - last_frame == tick_ms ? 1u : 0u;
                o.max_late = std::max(o.max_late, now_ms - last_frame - tick_ms);
            }
            last_frame = now_ms;
            o.ticks++;
            o.last_host_s = static_cast<double>(host.now) / 1e9;
        }
        fire = cf() + 0.001; // RunLoopTimer_Arm(holder, 1): now + 1 ms, in doubles
    }
    return o;
}

} // namespace

TEST_CASE("pacing: on the 1 ms grid every game_speed length ticks at its own rate (#165)") {
    for (const std::int64_t tick : {65, 50, 40, 33, 25}) {
        CAPTURE(tick);
        // The wake under a millisecond late: every poll reads the gate exactly.
        for (const std::uint64_t late : {std::uint64_t{0}, 300 * kMs / 1000, 900 * kMs / 1000}) {
            CAPTURE(late);
            const GridTicks o = run_grid(tick, fe::kGridFireTolerance, late, 2'000);
            CHECK(o.exact == o.ticks - 1);
            CHECK(o.max_late == 0);
            const double per_tick = (o.last_host_s - o.first_host_s) / 1999;
            CHECK(per_tick == doctest::Approx(static_cast<double>(tick) / 1000).epsilon(1e-3));
        }
    }
}

TEST_CASE("pacing: the 1 ms grid needs the run loop's tolerance: a fire date a rounding past the "
          "millisecond loses the poll (#165)") {
    // The game re-arms at now + 0.001 in doubles: the next millisecond reads a rounding either
    // side of that. With no tolerance a poll is missed so often that a tick lasts over 0.5 %
    // longer than its interval, whatever the start time; with the tolerance, none is missed.
    for (const std::uint64_t start :
         {kStartUnixUs, kStartUnixUs + 1'234'567'891ull, fe::paced_start_unix_us(kStartUnixUs)}) {
        CAPTURE(start);
        const GridTicks tied = run_grid(25, 0.0, 300 * kMs / 1000, 2'000, start);
        const GridTicks exact =
            run_grid(25, fe::kGridFireTolerance, 300 * kMs / 1000, 2'000, start);
        const double tied_tick = (tied.last_host_s - tied.first_host_s) / 1999;
        const double exact_tick = (exact.last_host_s - exact.first_host_s) / 1999;
        MESSAGE("tick without the tolerance " << tied_tick * 1000 << " ms, with it "
                                              << exact_tick * 1000 << " ms");
        CHECK(tied_tick > 0.025 * 1.005);
        CHECK(exact_tick == doctest::Approx(0.025).epsilon(1e-3));
    }
}

TEST_CASE("pacing: the paced clock on the wall clock, and locked: steps at the refreshes") {
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
    // The wall clock until a plan locks it: guest time is host time since the start.
    CHECK(clock.guest_ns() == 0);
    host.now += 1234567;
    CHECK(clock.guest_ns() == 1234567);
    CHECK(clock.unix_us() == kStartUnixUs + 1234);
    CHECK(clock.gcd_ns() == fe::PacedClock::kGcdOrigin + 1234567);
    CHECK(clock.host_when(2'000'000) == host.now + 765433);

    // Locked at 60 Hz, 50 ms: the time stands still within a refresh and steps by 16.667 ms at
    // each; three steps are exactly 50 ms.
    REQUIRE(clock.set_plan(locked(60, 3)));
    CHECK_FALSE(clock.set_plan(locked(60, 3))); // the same plan: nothing changes
    const std::uint64_t h0 = host.now;
    const std::uint64_t g0 = clock.guest_ns();
    CHECK(g0 == 1234567);
    host.now = h0 + 16 * kMs;
    CHECK(clock.guest_ns() == g0); // before the first refresh
    CHECK(clock.vblank_at(host.now) == 0);
    CHECK(clock.vblank_host(1) == h0 + 16'666'667);
    host.now = h0 + 16'666'667;
    CHECK(clock.vblank_at(host.now) == 1);
    CHECK(clock.guest_ns() == g0 + 16'666'666);
    host.now = h0 + 33'333'334;
    CHECK(clock.guest_ns() == g0 + 33'333'333);
    host.now = h0 + 50 * kMs;
    CHECK(clock.guest_ns() == g0 + 50 * kMs);
    host.now = h0 + 1000 * kMs;
    CHECK(clock.guest_ns() == g0 + 1000 * kMs); // 60 refreshes, 20 ticks: a second
    // host_when: the refresh at which the guest clock first reads a time.
    CHECK(clock.host_when(g0 + 1000 * kMs + 1) == h0 + 1'016'666'667);
    CHECK(clock.host_when(g0 + 1000 * kMs) == h0 + 1000 * kMs);
    CHECK(clock.host_when(g0) == h0);
    // GCD's sleep_until: the fake sleep moves the host to that refresh.
    clock.sleep_until(g0 + 1100 * kMs);
    CHECK(host.now == h0 + 1'100 * kMs);
    CHECK(clock.locked_ns() == 1'100 * kMs);
}

TEST_CASE("pacing: host_when is the first host time the guest clock reads a time, on any plan") {
    for (const double hz : {60.0, 120.0, 59.94, 60.06, 100.0}) {
        Host host;
        fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
        host.now += 777;
        const PacingPlan plan = fe::plan_pacing(PacingMode::kDisplay, hz, true, 50);
        REQUIRE(plan.locked);
        clock.set_plan(plan);
        std::mt19937_64 rng(static_cast<std::uint64_t>(hz * 100));
        for (int i = 0; i < 2000; i++) {
            const std::uint64_t g = 777 + rng() % (3600 * 1000 * kMs); // within an hour
            const std::uint64_t h = clock.host_when(g);
            CAPTURE(hz);
            CAPTURE(g);
            CHECK(clock.guest_at(h) >= g);
            CHECK(clock.guest_at(h - 1) < g);
            CHECK(clock.vblank_host(clock.vblank_at(h)) == h); // at a refresh
        }
    }
}

TEST_CASE("pacing: at 60 Hz every tick's frame is shown for exactly 3 refreshes, and the game "
          "runs at the wall clock's speed") {
    for (const std::uint64_t phase :
         {1 * kMs, 3 * kMs, std::uint64_t{7'777'777}, 13 * kMs, 16 * kMs}) {
        Host host;
        fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
        clock.set_plan(locked(60, 3));
        Outcome o;
        run(o, clock, host, 50, 6'000, 60, phase); // 5 minutes
        CAPTURE(phase);
        CHECK(share(o.stats.tick_held(), 3) == 1.0);
        CHECK(share(o.stats.held(), 3) == 1.0);
        const fe::TickStats t = o.stats.ticks();
        CHECK(t.p99_ms < 50.0 * 1.5);
        CHECK(t.max_ms < 50.0 + 9.0);
        // Tick to tick exactly 50 ms on both clocks: 20 ticks a second.
        CHECK(o.host_s == doctest::Approx(5999 * 0.05).epsilon(1e-5));
        CHECK(o.guest_s == doctest::Approx(o.host_s).epsilon(1e-9));
        CHECK(o.max_drift <= static_cast<std::int64_t>(17 * kMs));
    }
}

TEST_CASE("pacing: at 120 Hz every tick's frame is shown for exactly 6 refreshes") {
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
    clock.set_plan(locked(120, 6));
    Outcome o;
    run(o, clock, host, 50, 2'000, 120, 4 * kMs);
    CHECK(share(o.stats.tick_held(), 6) > 0.999); // one 7 while the lead learns the work
    CHECK(o.host_s == doctest::Approx(1999 * 0.05).epsilon(1e-5));
    // 40 ms (the fastest game speed) at 75 Hz: 3 refreshes, 25 ticks a second.
    Host h2;
    fe::PacedClock c2(Host::now_fn, Host::sleep_fn, &h2, kStartUnixUs);
    c2.set_plan(locked(75, 3, 40));
    Outcome o2;
    run(o2, c2, h2, 40, 2'000, 75, 1 * kMs);
    CHECK(share(o2.stats.tick_held(), 3) == 1.0);
    CHECK(o2.host_s == doctest::Approx(1999 * 0.04).epsilon(1e-5));
}

TEST_CASE("pacing: at 144 Hz the frames alternate between 7 and 8 refreshes (7.2 a tick), and "
          "on the 1 ms grid each tick still lasts exactly its 50 ms") {
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
    CHECK_FALSE(clock.set_plan(clock.plan())); // starts on the wall clock
    const PacingPlan grid = fe::plan_pacing(PacingMode::kDisplay, 144, true, 50);
    REQUIRE_FALSE(grid.locked);
    REQUIRE(grid.grid);
    clock.set_plan(grid);
    Outcome o;
    run(o, clock, host, 50, 2'000, 144, 0);
    CHECK(share(o.stats.tick_held(), 7) > 0.5);
    CHECK(share(o.stats.tick_held(), 8) > 0.1);
    CHECK(o.host_s == doctest::Approx(1999 * 0.05).epsilon(1e-5));
    CHECK(o.guest_s == doctest::Approx(o.host_s).epsilon(1e-5));
}

TEST_CASE("pacing: a display that is not exactly 60 Hz: a rare held or doubled step keeps the "
          "game at the wall clock's speed") {
    for (const double hz : {59.94, 60.06}) {
        Host host;
        fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
        clock.set_plan(locked(hz, 3));
        Outcome o;
        run(o, clock, host, 50, 24'000, hz, 2 * kMs); // 20 minutes
        CAPTURE(hz);
        // About one tick in 333 (0.1 % of the refreshes, 3 a tick) lasts a refresh more or less.
        CHECK(share(o.stats.tick_held(), 3) > 0.99);
        const std::size_t odd = hz < 60 ? 2 : 4;
        CHECK(share(o.stats.tick_held(), 3) + share(o.stats.tick_held(), odd) == 1.0);
        // The game's speed is the wall clock's (not the display's 0.1 % off): 20 ticks a second.
        CHECK(o.host_s / 23'999 == doctest::Approx(0.05).epsilon(2e-4));
        CHECK(o.guest_s == doctest::Approx(o.host_s).epsilon(1e-4));
        // Never more than half a step plus a refresh from the wall clock.
        CHECK(o.max_drift <= static_cast<std::int64_t>(8'333'334 + 16'700'000));
    }
}

TEST_CASE("pacing: an overrun or a stall: one tick, then the cadence again; no catch-up burst") {
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
    clock.set_plan(locked(60, 3));
    Outcome o;
    // Tick 100 takes 2 s (a map load inside Paint).
    run(o, clock, host, 50, 400, 60, 5 * kMs, 100, 2'000 * kMs);
    const fe::TickStats t = o.stats.ticks();
    // The ticks around the stall: the long one, then the game's `lastFrame = now` ticks once at
    // the first refresh after it, and 3 refreshes a tick again.
    CHECK(t.max_ms > 2'000);
    CHECK(o.stats.tick_held()[3] >= 396);
    // The guest clock followed the wall clock through it.
    CHECK(o.max_drift <= static_cast<std::int64_t>(17 * kMs));
}

TEST_CASE("pacing: plan changes keep the guest clock continuous and never back") {
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
    std::mt19937 rng(7);
    const PacingPlan plans[] = {
        locked(60, 3),     fe::plan_pacing(PacingMode::kDisplay, 144, true, 50),
        locked(120, 6),    locked(59.94, 3),
        locked(75, 3, 40), fe::plan_pacing(PacingMode::kWall, 60, true, 50)};
    std::uint64_t last = 0;
    for (int i = 0; i < 3000; i++) {
        host.now += rng() % (5 * kMs);
        if (i % 97 == 0) {
            const std::uint64_t before = clock.guest_ns();
            clock.set_plan(plans[(i / 97) % std::size(plans)]);
            CHECK(clock.guest_ns() == before); // continuous at the change
        }
        const std::uint64_t g = clock.guest_ns();
        CHECK(g >= last);
        last = g;
        // Within a refresh plus half a step of the wall clock whatever the plans were.
        const std::int64_t drift =
            static_cast<std::int64_t>(g) - static_cast<std::int64_t>(clock.host_elapsed_ns());
        CHECK(drift < static_cast<std::int64_t>(9 * kMs));
        CHECK(drift > -static_cast<std::int64_t>(30 * kMs));
    }
}

TEST_CASE("pacing: the present point is the first refresh boundary after the longest recent work") {
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
    fe::PresentHold hold;
    // On the wall clock nothing is held.
    hold.wake(host.now);
    CHECK(hold.present_at(clock, host.now + 3 * kMs) == host.now + 3 * kMs);
    clock.set_plan(locked(60, 3));
    const std::uint64_t r0 = host.now; // refresh 0 of the new plan
    const std::uint64_t r1 = clock.vblank_host(1), r2 = clock.vblank_host(2);
    // 4 ms of work after the wake at refresh 0: presented at refresh 1.
    hold.wake(r0);
    CHECK(hold.pending());
    CHECK(hold.present_at(clock, r0 + 4 * kMs) == r1);
    CHECK_FALSE(hold.pending());
    // Any other present since the wake goes at once.
    CHECK(hold.present_at(clock, r0 + 5 * kMs) == r0 + 5 * kMs);
    // 18 ms of work: the lead grows, refresh 2 from now on, even for a short work.
    hold.wake(r0);
    CHECK(hold.present_at(clock, r0 + 18 * kMs) == r2);
    CHECK(hold.lead_ns() == 18 * kMs);
    hold.wake(r1);
    CHECK(hold.present_at(clock, r1 + 2 * kMs) == clock.vblank_host(3));
    // A stall longer than n - 1 refreshes: presented at once, not counted.
    hold.wake(r0);
    CHECK(hold.present_at(clock, r0 + 40 * kMs) == r0 + 40 * kMs);
    CHECK(hold.lead_ns() == 18 * kMs);
    // After kWorks wakes without a long work, the lead is short again.
    for (std::size_t i = 0; i < fe::PresentHold::kWorks; i++) {
        hold.wake(r0);
        hold.present_at(clock, r0 + 4 * kMs);
    }
    CHECK(hold.lead_ns() == 4 * kMs);
    // One refresh a tick (20 Hz): the next boundary is the next tick's wake; nothing is held.
    clock.set_plan(locked(20, 1));
    hold.wake(clock.vblank_host(0));
    CHECK(hold.present_at(clock, clock.vblank_host(0) + 4 * kMs) == clock.vblank_host(0) + 4 * kMs);
}

TEST_CASE(
    "pacing: tick_held counts the GL frames' refreshes, whatever window frames come between") {
    // At 60 Hz: ticks presented at 5, 55, 105 ms (shown at refreshes 1, 4, 7), with an event's
    // window frame (no new GL frame) at 30 ms and two at 70 and 80 ms between them.
    fe::FrameStats s;
    s.start(0, 60, true, 0);
    s.frame(5 * kMs, 1, 1, true, 0);
    s.frame(30 * kMs, 2, 1, false, 0);
    s.frame(55 * kMs, 3, 2, true, 0);
    s.frame(70 * kMs, 4, 2, false, 0);
    s.frame(80 * kMs, 5, 2, false, 0);
    s.frame(105 * kMs, 6, 3, true, 0);
    CHECK(s.tick_held()[3] == 2);
    CHECK(s.held()[3] == 0); // the window frames: 2, 1, 1, 1, 1
    CHECK(s.summary(false).back() == "pacing: 2 GL frames (ticks) shown for 3 vblanks 100.00% (2)");
    // A pause starts the count again.
    s.gap(500 * kMs, 0);
    s.frame(505 * kMs, 7, 4, true, 0);
    CHECK(s.tick_held()[3] == 2);
    s.frame(555 * kMs, 8, 5, true, 0);
    CHECK(s.tick_held()[3] == 3);
}

TEST_CASE("pacing: the setting in settings.ini and on the command line") {
    fe::Settings s;
    CHECK(s.pacing == PacingMode::kDisplay); // the default (#80)
    CHECK(fe::parse_settings("pacing = wall\n", &s).empty());
    CHECK(s.pacing == PacingMode::kWall);
    CHECK(fe::parse_settings("PACING = Display\n", &s).empty());
    CHECK(s.pacing == PacingMode::kDisplay);
    const std::vector<std::string> problems = fe::parse_settings("pacing = vsync\n", &s);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0] == "line 1: pacing: 'vsync' is not display or wall, ignored");
    CHECK(s.pacing == PacingMode::kDisplay);
    CHECK(fe::serialise_settings(s).find("\npacing = display\n") != std::string::npos);
    s.pacing = PacingMode::kWall;
    fe::Settings back;
    fe::parse_settings(fe::serialise_settings(s), &back);
    CHECK(back == s);
    std::string why;
    CHECK(fe::apply_setting(&s, "pacing", "display", &why));
    CHECK_FALSE(fe::apply_setting(&s, "pacing", "", &why));
    CHECK(std::string(fe::pacing_mode_name(PacingMode::kWall)) == "wall");
}

TEST_CASE("pacing: with pacing = display the guest's GetTimeMs starts below 2^30 (#155's shift)") {
    // run_game's paced clock starts at paced_start_unix_us(the wall clock) and is the libc clock
    // (no LaunchClock beside it). Launches in each quarter of GetTimeMs' 32 bits, among them 0.4 s
    // before its 2^31 jump (2026-12-16T17:56:53Z, GetTimeMs 0x7FFFFE88), and now.
    const std::uint64_t before_jump = 1797443813000ull * 1000u;
    REQUIRE(fe::guest_time_ms(before_jump) == 0x7FFFFE88);
    constexpr std::uint64_t kQuarterUs = 0x40000000ull * 1000u; // 2^30 ms
    const auto now_us =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    for (const std::uint64_t wall : {kStartUnixUs, before_jump, before_jump + kQuarterUs,
                                     before_jump + 2 * kQuarterUs, now_us}) {
        CAPTURE(wall);
        const std::uint64_t start = fe::paced_start_unix_us(wall);
        CHECK(wall - start == fe::LaunchClock::shift_for(wall));
        CHECK(wall - start <= 3 * kQuarterUs);
        Host host;
        fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, start);
        REQUIRE(clock.install());
        const std::int32_t boot = fe::guest_time_ms(clock.unix_us());
        CHECK(boot >= 0);
        CHECK(boot < (1 << 30));
        // The low 30 bits of the wall clock's GetTimeMs stay: the random seed differs per launch.
        CHECK(static_cast<std::uint32_t>(boot) ==
              (static_cast<std::uint32_t>(fe::guest_time_ms(wall)) & 0x3FFFFFFFu));
        // It is the libc clock: CFAbsoluteTimeGetCurrent reads it.
        const double want = static_cast<double>(clock.unix_us()) / 1e6 - MT_CF_ABSOLUTE_TIME_1970;
        CHECK(mt_cf_absolute_time() == doctest::Approx(want).epsilon(1e-12));
        // Locked to a 60 Hz display, a second on: the guest's second, no jump (within half a step
        // plus a refresh of the wall clock, and GetTimeMs' whole milliseconds).
        clock.set_plan(locked(60, 3));
        host.now += 1000 * kMs;
        const std::int32_t later = fe::guest_time_ms(clock.unix_us());
        CHECK(later - boot >= 1000 - 27);
        CHECK(later - boot <= 1000 + 27);
    }
}

TEST_CASE("pacing: a host up for more than 2^31 ms runs the paced clock as one just booted") {
    // The host's monotonic clock (SDL_GetTicksNS) is the machine's uptime. One up 58 days (more
    // than 2^31 ms, and than 2^32 ms) must give the guest the same clock as one up 5 s: the paced
    // clock keeps 64-bit nanoseconds from its own start. (The game's "N days behind the host's"
    // line is #155's launch shift, from the Unix time's GetTimeMs bits 30 and 31, not uptime.)
    constexpr std::uint64_t kDayMs = 86'400'000ull;
    const std::uint64_t start = fe::paced_start_unix_us(kStartUnixUs);
    const std::uint64_t ups_ms[] = {58 * kDayMs, (std::uint64_t{1} << 31) - 7,
                                    (std::uint64_t{1} << 32) + 3};
    for (const std::uint64_t up_ms : ups_ms) {
        CAPTURE(up_ms);
        Host fresh;
        Host old;
        old.now = up_ms * kMs;
        fe::PacedClock a(Host::now_fn, Host::sleep_fn, &fresh, start);
        fe::PacedClock b(Host::now_fn, Host::sleep_fn, &old, start);
        const auto same = [&] {
            CHECK(a.guest_ns() == b.guest_ns());
            CHECK(a.unix_us() == b.unix_us());
            CHECK(a.gcd_ns() == b.gcd_ns());
            CHECK(fe::guest_time_ms(a.unix_us()) == fe::guest_time_ms(b.unix_us()));
            CHECK(a.host_when(a.guest_ns() + 7 * kMs) - fresh.now ==
                  b.host_when(b.guest_ns() + 7 * kMs) - old.now);
        };
        const auto step = [&](std::uint64_t ns) {
            fresh.now += ns;
            old.now += ns;
        };
        same();
        step(4'000 * kMs); // the wall clock, then fast-forward x3 (game_speed's run), then locked
        same();
        REQUIRE(a.set_plan(fe::plan_pacing(PacingMode::kDisplay, 60, false, 50, 3)));
        REQUIRE(b.set_plan(fe::plan_pacing(PacingMode::kDisplay, 60, false, 50, 3)));
        step(6'700 * kMs);
        same();
        CHECK(b.guest_ns() == 4'000 * kMs + 3 * 6'700 * kMs);
        REQUIRE(a.set_plan(locked(60, 3)));
        REQUIRE(b.set_plan(locked(60, 3)));
        step(1'016 * kMs);
        same();
        CHECK(fe::guest_time_ms(b.unix_us()) >= 0);
    }
}

TEST_CASE("pacing: a save state's restore re-bases the paced clock's Unix time, locked or not") {
    // #204 (ADR 0105's addendum): the guest's Unix time goes on from the state's, back or
    // forward; guest_ns, GCD's clock and the lock go on as they were.
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
    host.now += 10'000 * kMs;
    const std::uint64_t ns = clock.guest_ns();
    const std::uint64_t saved = kStartUnixUs + 2'500'000; // 2.5 s after the start: back
    clock.rebase_to(saved);
    CHECK(clock.unix_us() == saved);
    CHECK(clock.guest_ns() == ns);
    CHECK(clock.gcd_ns() == fe::PacedClock::kGcdOrigin + ns);
    host.now += 50 * kMs;
    CHECK(clock.unix_us() == saved + 50'000);
    REQUIRE(clock.set_plan(locked(60, 3)));
    const std::uint64_t before = clock.unix_us();
    clock.rebase_to(before + 7'000'000); // forward
    CHECK(clock.unix_us() == before + 7'000'000);
    host.now += 50 * kMs; // three refreshes: one 50 ms tick of guest time
    const std::uint64_t step = clock.unix_us() - (before + 7'000'000);
    CHECK(step >= 33'333);
    CHECK(step <= 66'667);
}

// --- the overload guard (#163, ADR 0130) -------------------------------------------------------

namespace {

// Feeds `count` intervals of `ms` each, the time moving with them; returns the time after.
std::uint64_t feed(fe::OverloadGuard &guard, const PacingPlan &plan, std::uint64_t t, int count,
                   double ms) {
    for (int i = 0; i < count; i++) {
        const auto ns = static_cast<std::uint64_t>(ms * 1e6);
        t += ns;
        guard.add(t, ns, plan);
    }
    return t;
}

} // namespace

TEST_CASE("pacing: overload guard: steady ticks and a tick's jitter keep the lock") {
    const PacingPlan plan = locked(60, 3);
    fe::OverloadGuard g;
    std::uint64_t t = 7'000 * kMs;
    t = feed(g, plan, t, 2000, 50.0);
    // The tick's work moving the present by a few ms either way is not lateness.
    for (int i = 0; i < 2000; i++) {
        t = feed(g, plan, t, 1, i % 2 ? 58.0 : 42.0);
    }
    CHECK_FALSE(g.backing_off());
    CHECK(g.unlocks() == 0);
    CHECK(g.apply(plan, t).locked);
}

TEST_CASE("pacing: overload guard: unlocks with the reason when many ticks are late") {
    const PacingPlan plan = locked(60, 3);
    fe::OverloadGuard g;
    std::uint64_t t = 7'000 * kMs;
    // A third of the ticks start a refresh late.
    for (int i = 0; i < 200 && !g.backing_off(); i++) {
        t = feed(g, plan, t, 1, i % 3 == 0 ? 66.7 : 50.0);
    }
    REQUIRE(g.backing_off());
    CHECK(g.unlocks() == 1);
    const PacingPlan wall = g.apply(plan, t);
    CHECK_FALSE(wall.locked);
    CHECK(wall.rate == 1);
    const std::string why = fe::describe(wall);
    CHECK(why.find("the wall clock (overloaded") != std::string::npos);
    CHECK(why.find("late by a refresh or more") != std::string::npos);
    CHECK(why.find("tried again in 30 s") != std::string::npos);
}

TEST_CASE("pacing: overload guard: a small share of late ticks keeps the lock") {
    const PacingPlan plan = locked(60, 3);
    fe::OverloadGuard g;
    std::uint64_t t = 7'000 * kMs;
    // One tick in 10 a refresh late (ADR 0075's load of 10-14 made far fewer): under kLateShare.
    for (int i = 0; i < 4000; i++) {
        t = feed(g, plan, t, 1, i % 10 == 0 ? 66.7 : 50.0);
    }
    CHECK(g.unlocks() == 0);
    CHECK(g.apply(plan, t).locked);
}

TEST_CASE("pacing: overload guard: a map load's stall alone does not unlock") {
    const PacingPlan plan = locked(60, 3);
    SUBCASE("one long stall") {
        fe::OverloadGuard g;
        std::uint64_t t = feed(g, plan, 7'000 * kMs, 200, 50.0);
        t += 2'000 * kMs;
        g.add(t, 2'000 * kMs, plan);
        t = feed(g, plan, t, 200, 50.0);
        CHECK(g.unlocks() == 0);
        CHECK(g.apply(plan, t).locked);
    }
    SUBCASE("a load's many stalls with short presents between them") {
        fe::OverloadGuard g;
        std::uint64_t t = 7'000 * kMs;
        for (int i = 0; i < 100; i++) {
            t = feed(g, plan, t, 1, 340.0); // over 2 x 50 ms: left out
            t = feed(g, plan, t, 2, 4.0);   // the load's own presents
        }
        CHECK(g.unlocks() == 0);
        CHECK(g.apply(plan, t).locked);
    }
    SUBCASE("a stall with only a few ticks in the window decides nothing") {
        fe::OverloadGuard g;
        std::uint64_t t = 7'000 * kMs;
        t = feed(g, plan, t, 1, 500.0);
        t = feed(g, plan, t, 5, 70.0); // late, but under half the window's ticks
        t = feed(g, plan, t, 1, 500.0);
        CHECK(g.unlocks() == 0);
    }
}

TEST_CASE("pacing: overload guard: backs off, retries, and leaves the lock again") {
    const PacingPlan plan = locked(60, 3);
    fe::OverloadGuard g;
    std::uint64_t t = 7'000 * kMs;
    while (!g.backing_off()) {
        t = feed(g, plan, t, 1, 70.0);
    }
    REQUIRE(g.unlocks() == 1);

    // Backing off: still the wall clock until the 30 s are over, and the ticks are not counted.
    const std::uint64_t retry = t + fe::OverloadGuard::kBackoffNs;
    CHECK_FALSE(g.apply(plan, retry - 1).locked);
    feed(g, plan, t, 50, 70.0);
    CHECK(g.unlocks() == 1);
    // The retry: locked again, and the first ticks decide nothing.
    t = retry;
    CHECK(g.apply(plan, t).locked);
    CHECK_FALSE(g.backing_off());
    t = feed(g, plan, t, 10, 70.0);
    CHECK_FALSE(g.backing_off());
    // Still starved: out of the lock again within a window, and the back-off starts over.
    t = feed(g, plan, t, 30, 70.0);
    CHECK(g.backing_off());
    CHECK(g.unlocks() == 2);
    CHECK_FALSE(g.apply(plan, t).locked);
}

TEST_CASE("pacing: overload guard: a recovered machine keeps the lock after the retry") {
    const PacingPlan plan = locked(60, 3);
    fe::OverloadGuard g;
    std::uint64_t t = feed(g, plan, 7'000 * kMs, 200, 70.0);
    REQUIRE(g.unlocks() == 1);
    t += fe::OverloadGuard::kBackoffNs;
    REQUIRE(g.apply(plan, t).locked);
    t = feed(g, plan, t, 400, 50.0);
    CHECK(g.unlocks() == 1);
    CHECK(g.apply(plan, t).locked);
}

TEST_CASE("pacing: overload guard: plans that are not locked pass through") {
    const PacingPlan plan = locked(60, 3);
    fe::OverloadGuard g;
    const PacingPlan wall = fe::plan_pacing(PacingMode::kWall, 60, true, 50);
    feed(g, wall, 7'000 * kMs, 400, 80.0);
    CHECK(g.unlocks() == 0);
    CHECK_FALSE(g.apply(wall, 9'000 * kMs).locked);

    // Backing off, a fast-forward's plan is still its own (rate and reason).
    const std::uint64_t t = feed(g, plan, 7'000 * kMs, 200, 70.0);
    REQUIRE(g.backing_off());
    const PacingPlan ff = fe::plan_pacing(PacingMode::kDisplay, 60, true, 50, 3);
    const PacingPlan out = g.apply(ff, t);
    CHECK(out.rate == 3);
    CHECK(std::string(out.why) == "fast-forward");
}

TEST_CASE("pacing: overload guard: a 1 ms grid is neither counted nor taken over (#165)") {
    const PacingPlan grid = fe::plan_pacing(PacingMode::kDisplay, 144, true, 50);
    REQUIRE(grid.grid);
    fe::OverloadGuard g;
    // Grid ticks, however late, are never counted: no refresh to lose, no unlock.
    std::uint64_t t = feed(g, grid, 7'000 * kMs, 400, 80.0);
    CHECK(g.unlocks() == 0);
    CHECK_FALSE(g.backing_off());
    CHECK(g.apply(grid, t) == grid);
    // A display lock on the same guard starts its count fresh and unlocks as ADR 0130 says.
    const PacingPlan lock = locked(60, 3);
    t = feed(g, lock, t, 200, 70.0);
    REQUIRE(g.backing_off());
    CHECK(g.unlocks() == 1);
    // Backing off: the wall plan is the plain wall clock, with the reason, not the grid.
    const PacingPlan wall = g.apply(lock, t);
    CHECK_FALSE(wall.stepped());
    CHECK(wall.rate == 1);
    CHECK(fe::describe(wall).find("the wall clock (overloaded") != std::string::npos);
    // A display that stops being lockable meanwhile (a move to 144 Hz) keeps its grid plan; and
    // grid ticks fed during the back-off are ignored, as wall ones are.
    CHECK(g.apply(grid, t) == grid);
    t = feed(g, grid, t, 400, 80.0);
    CHECK(g.unlocks() == 1);
    // Back on the lockable display the lock returns after the back-off, and the count restarts.
    const std::uint64_t retry = t + fe::OverloadGuard::kBackoffNs;
    REQUIRE(g.apply(lock, retry).locked);
    CHECK_FALSE(g.backing_off());
    CHECK(g.unlocks() == 1);
}

TEST_CASE("pacing: overload guard: lateness is a refresh of the plan's display") {
    SUBCASE("120 Hz: 8.3 ms is a refresh") {
        const PacingPlan plan = locked(120, 6);
        fe::OverloadGuard g;
        feed(g, plan, 7'000 * kMs, 100, 58.4); // 50 ms + a refresh of 8.33
        CHECK(g.unlocks() == 1);
    }
    SUBCASE("60 Hz: the same interval is not late") {
        const PacingPlan plan = locked(60, 3);
        fe::OverloadGuard g;
        feed(g, plan, 7'000 * kMs, 100, 58.4);
        CHECK(g.unlocks() == 0);
    }
    SUBCASE("a new plan starts the count over") {
        fe::OverloadGuard g;
        const PacingPlan p60 = locked(60, 3);
        const PacingPlan p120 = locked(120, 6);
        std::uint64_t t = feed(g, p60, 7'000 * kMs, 15, 70.0); // under half the window's ticks
        t = feed(g, p120, t, 15, 50.0);
        t = feed(g, p60, t, 15, 70.0);
        CHECK(g.unlocks() == 0);
    }
}
