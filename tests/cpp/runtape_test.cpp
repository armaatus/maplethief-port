// The run tape (#405, ADR 0156, maplethief/runtape.h) on synthetic clocks: what the guest's clock
// hooks returned is recorded run-length coded and played back in order (past the end, the last
// value, counted); GCD's sleep does not wait while a tape plays; the boundary's calls are
// recorded at their turn, dropped while a tape plays, and the tape's own delivered at theirs; a
// port write into guest memory goes through it; the host's own reads are not taped.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include "maplethief/cf.h"
#include "maplethief/gcd.h"
#include "maplethief/hle/libc.h"
#include "maplethief/input.h"
#include "maplethief/runtape.h"

namespace {

// A clock the test moves: Unix microseconds, and GCD's nanoseconds beside it.
std::uint64_t g_us = 1735689600ull * 1000000ull;
std::uint64_t g_ns = 1000000000ull;
std::uint64_t g_slept = 0;

std::uint64_t test_unix_us(void *) { return g_us; }
std::uint64_t test_gcd_ns(void *) { return g_ns; }
void test_sleep(void *, std::uint64_t deadline) {
    g_slept++;
    g_ns = deadline > g_ns ? deadline : g_ns;
}

// The test's clocks as the platform's: installed, then the tape around them.
struct Clocks {
    Clocks() {
        mt_libc_config lc = mt_libc_config_default();
        lc.clock = test_unix_us;
        mt_libc_configure(&lc);
        mt_gcd_config gc = mt_gcd_config_default();
        gc.clock = test_gcd_ns;
        gc.sleep_until = test_sleep;
        REQUIRE(mt_gcd_configure(&gc) == 0);
    }
    ~Clocks() {
        mt_runtape_stop();
        mt_input_release_all();
        mt_libc_configure(nullptr);
        mt_gcd_configure(nullptr);
    }
};

std::uint64_t guest_us() {
    return static_cast<std::uint64_t>((mt_cf_absolute_time() + MT_CF_ABSOLUTE_TIME_1970) * 1e6 +
                                      0.5);
}

mt_runtape_event press_event(std::uint32_t action) {
    mt_runtape_event e{};
    e.kind = MT_RUNTAPE_PRESS;
    e.a = action;
    return e;
}

} // namespace

TEST_CASE("runtape: the clocks' values are recorded run-length coded and played back in order") {
    const Clocks clocks;
    REQUIRE(mt_runtape_record() == 0);
    CHECK(mt_runtape_get_mode() == MT_RUNTAPE_RECORD);
    std::vector<std::uint64_t> seen_us, seen_ns;
    for (int refresh = 0; refresh < 5; refresh++) {
        for (int read = 0; read < 4; read++) {
            seen_us.push_back(guest_us());
            seen_ns.push_back(mt_gcd_now());
        }
        g_us += 16667; // the guest's time moves at a display refresh
        g_ns += 16667000;
    }
    std::size_t n = 0;
    const mt_runtape_run *runs = mt_runtape_runs(MT_RUNTAPE_CLOCK, &n);
    REQUIRE(n == 5);
    CHECK(runs[0].count == 4);
    CHECK(runs[4].value == seen_us.back());
    // The host's own read is not taped.
    const double host = mt_runtape_host_time();
    CHECK(host == doctest::Approx(static_cast<double>(g_us) / 1e6 - MT_CF_ABSOLUTE_TIME_1970));
    CHECK(mt_runtape_get_stats().reads[MT_RUNTAPE_CLOCK] == 20);
    mt_runtape_stop();
    CHECK(mt_runtape_get_mode() == MT_RUNTAPE_OFF);

    // Copy the tape, then play it on a clock that has moved on: the guest sees the recording's.
    std::vector<mt_runtape_run> s[MT_RUNTAPE_STREAMS];
    for (std::uint32_t k = 0; k < MT_RUNTAPE_STREAMS; k++) {
        const mt_runtape_run *r = mt_runtape_runs(k, &n);
        s[k].assign(r, r + n);
    }
    const mt_runtape_run *play_runs[MT_RUNTAPE_STREAMS] = {s[0].data(), s[1].data(), s[2].data()};
    const std::size_t counts[MT_RUNTAPE_STREAMS] = {s[0].size(), s[1].size(), s[2].size()};
    g_us += 999999999;
    g_ns += 999999999999;
    REQUIRE(mt_runtape_play(play_runs, counts, nullptr, 0) == 0);
    for (std::size_t i = 0; i < seen_us.size(); i++) {
        CHECK(guest_us() == seen_us[i]);
        CHECK(mt_gcd_now() == seen_ns[i]);
    }
    CHECK(mt_runtape_get_stats().overrun == 0);
    // Past the end: the last value, counted.
    CHECK(guest_us() == seen_us.back());
    CHECK(mt_runtape_get_stats().overrun == 1);
    // GCD's sleep returns at once: the tape's next value is when the wait ended.
    const std::uint64_t slept = g_slept;
    mt_gcd_config cfg = mt_gcd_get_config();
    cfg.sleep_until(cfg.sleep_user, g_ns + 1000000000ull);
    CHECK(g_slept == slept);
    // The host's read while playing: the last value played.
    CHECK(mt_runtape_host_time() ==
          doctest::Approx(static_cast<double>(seen_us.back()) / 1e6 - MT_CF_ABSOLUTE_TIME_1970));
    mt_runtape_stop();
    // The platform's clock is back.
    CHECK(guest_us() == g_us);
}

TEST_CASE("runtape: a recording's sleep waits on the clock it wraps") {
    const Clocks clocks;
    REQUIRE(mt_runtape_record() == 0);
    const std::uint64_t slept = g_slept;
    mt_gcd_config cfg = mt_gcd_get_config();
    cfg.sleep_until(cfg.sleep_user, g_ns + 5);
    CHECK(g_slept == slept + 1);
}

TEST_CASE("runtape: the music's play state is taped") {
    const Clocks clocks;
    CHECK(mt_runtape_music_playing(1) == 1); // off: the live value
    REQUIRE(mt_runtape_record() == 0);
    CHECK(mt_runtape_music_playing(1) == 1);
    CHECK(mt_runtape_music_playing(1) == 1);
    CHECK(mt_runtape_music_playing(0) == 0);
    std::size_t n = 0;
    const mt_runtape_run *runs = mt_runtape_runs(MT_RUNTAPE_MUSIC, &n);
    REQUIRE(n == 2);
    CHECK(runs[0].value == 1);
    CHECK(runs[0].count == 2);
    const std::vector<mt_runtape_run> music(runs, runs + n);
    mt_runtape_stop();
    const std::vector<mt_runtape_run> none;
    const mt_runtape_run *play_runs[MT_RUNTAPE_STREAMS] = {none.data(), none.data(), music.data()};
    const std::size_t counts[MT_RUNTAPE_STREAMS] = {0, 0, music.size()};
    REQUIRE(mt_runtape_play(play_runs, counts, nullptr, 0) == 0);
    // The live value no longer matters: the tape's.
    CHECK(mt_runtape_music_playing(0) == 1);
    CHECK(mt_runtape_music_playing(0) == 1);
    CHECK(mt_runtape_music_playing(1) == 0);
}

TEST_CASE("runtape: boundary calls are recorded at their turn, and only the tape's play") {
    const Clocks clocks;
    REQUIRE(mt_runtape_record() == 0);
    mt_runtape_turn(); // turn 1
    mt_input_press(MT_ACTION_JUMP);
    mt_runtape_turn(); // turn 2
    mt_runtape_turn(); // turn 3
    mt_input_release(MT_ACTION_JUMP);
    mt_input_release(MT_ACTION_JUMP);       // not held: no call reaches the guest, none recorded
    mt_input_press(MT_ACTION_FAST_FORWARD); // the port's own action: never the guest's
    mt_runloop_set_fire_tolerance(0.0005);
    std::size_t n = 0;
    const mt_runtape_event *ev = mt_runtape_events(&n);
    REQUIRE(n == 3);
    CHECK(ev[0].turn == 1);
    CHECK(ev[0].kind == MT_RUNTAPE_PRESS);
    CHECK(ev[0].a == MT_ACTION_JUMP);
    CHECK(ev[1].turn == 3);
    CHECK(ev[1].kind == MT_RUNTAPE_RELEASE);
    CHECK(ev[2].kind == MT_RUNTAPE_FIRE_TOLERANCE);
    const std::vector<mt_runtape_event> events(ev, ev + n);
    mt_runtape_stop();
    mt_runloop_set_fire_tolerance(0.0);

    const mt_runtape_run *play_runs[MT_RUNTAPE_STREAMS] = {nullptr, nullptr, nullptr};
    const std::size_t counts[MT_RUNTAPE_STREAMS] = {0, 0, 0};
    REQUIRE(mt_runtape_play(play_runs, counts, events.data(), events.size()) == 0);
    // The host's own input is dropped while the tape plays.
    mt_input_press(MT_ACTION_ATTACK);
    CHECK(mt_input_held(MT_ACTION_ATTACK) == 0);
    mt_runtape_event e = press_event(MT_ACTION_ATTACK);
    CHECK(mt_runtape_admit(&e) == 0);
    CHECK(mt_runtape_get_stats().dropped == 2);
    // The tape's, at their turns.
    mt_runtape_turn();
    mt_runtape_deliver(nullptr);
    CHECK(mt_input_held(MT_ACTION_JUMP) == 1);
    mt_runtape_turn();
    mt_runtape_deliver(nullptr);
    CHECK(mt_input_held(MT_ACTION_JUMP) == 1);
    mt_runtape_turn();
    mt_runtape_deliver(nullptr);
    CHECK(mt_input_held(MT_ACTION_JUMP) == 0);
    CHECK(mt_runtape_get_stats().events == 3);
    mt_runtape_stop();
    mt_runloop_set_fire_tolerance(0.0);
}

TEST_CASE("runtape: a tape whose events are out of turn order, or of no kind, is refused") {
    const Clocks clocks;
    mt_runtape_event ev[2] = {press_event(MT_ACTION_UP), press_event(MT_ACTION_UP)};
    ev[0].turn = 5;
    ev[1].turn = 4;
    const mt_runtape_run *play_runs[MT_RUNTAPE_STREAMS] = {nullptr, nullptr, nullptr};
    const std::size_t counts[MT_RUNTAPE_STREAMS] = {0, 0, 0};
    CHECK(mt_runtape_play(play_runs, counts, ev, 2) == -1);
    ev[1].turn = 6;
    ev[1].kind = 0;
    CHECK(mt_runtape_play(play_runs, counts, ev, 2) == -1);
    CHECK(mt_runtape_get_mode() == MT_RUNTAPE_OFF);
}

TEST_CASE("runtape: a port write into guest memory is done and recorded, or dropped in play") {
    const Clocks clocks;
    std::vector<std::uint8_t> mem(0x2000, 0);
    mt_cpu cpu{};
    cpu.mem = mem.data();
    cpu.mem_size = mem.size();
    const std::uint64_t v = 0x1122334455667788ull;
    CHECK(mt_runtape_poke(&cpu, 0x100, &v, 8) == 1); // off: written
    CHECK(mem[0x100] == 0x88);
    CHECK(mt_runtape_poke(&cpu, 0x1FFC, &v, 8) == 0); // past the end
    CHECK(mt_runtape_poke(&cpu, 0x100, &v, 9) == 0);
    REQUIRE(mt_runtape_record() == 0);
    const std::uint32_t w = 0xCAFEF00Du;
    CHECK(mt_runtape_poke(&cpu, 0x200, &w, 4) == 1);
    CHECK(std::memcmp(&mem[0x200], &w, 4) == 0);
    std::size_t n = 0;
    const mt_runtape_event *ev = mt_runtape_events(&n);
    REQUIRE(n == 1);
    CHECK(ev[0].kind == MT_RUNTAPE_POKE);
    CHECK(ev[0].a == 0x200);
    CHECK(ev[0].b == 4);
    CHECK(ev[0].value == 0xCAFEF00Du);
    const std::vector<mt_runtape_event> events(ev, ev + n);
    mt_runtape_stop();
    std::memset(&mem[0x200], 0, 4);
    const mt_runtape_run *play_runs[MT_RUNTAPE_STREAMS] = {nullptr, nullptr, nullptr};
    const std::size_t counts[MT_RUNTAPE_STREAMS] = {0, 0, 0};
    REQUIRE(mt_runtape_play(play_runs, counts, events.data(), events.size()) == 0);
    CHECK(mt_runtape_poke(&cpu, 0x300, &w, 4) == 0); // the host's: dropped
    CHECK(mem[0x300] == 0);
    mt_runtape_deliver(&cpu); // turn 0: the tape's
    CHECK(std::memcmp(&mem[0x200], &w, 4) == 0);
}
