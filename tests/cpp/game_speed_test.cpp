// Game speed and fast-forward (#151, ADR 0087), synthetic: the two Assists rows; the game's
// speed table patched in a synthetic guest's memory (at the start and live, with the config's
// Speed byte left alone); the fast_forward action held in the bindings layer and never sent to
// the guest; the guest clocks' rate (the paced clock's wall plan, the launch clock) continuous
// at every toggle, a lock after a hold that does not stall; and the game's own tick gate on the
// fast clock at 2 and 3 times each speed's rate, within 1 %.
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "actions.h"
#include "bindings.h"
#include "clock.h"
#include "frame_stats.h"
#include "game_speed.h"
#include "maplethief/input.h"
#include "maplethief/recomp/cpu.h"
#include "pacing.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
using fe::FastForward;
using fe::GameSpeed;
using fe::PacingMode;
using fe::PacingPlan;

constexpr std::uint64_t kMs = 1'000'000;
constexpr std::uint64_t kStartUnixUs = 1735689600ull * 1000000ull; // 2025-01-01

struct Host {
    std::uint64_t now = 5'000 * kMs;
    static std::uint64_t now_fn(void *u) { return static_cast<Host *>(u)->now; }
    static void sleep_fn(void *u, std::uint64_t ns) { static_cast<Host *>(u)->now += ns; }
};

// A guest's memory up to the game's globals, with the speed table as shipped.
struct Guest {
    static constexpr std::uint32_t kConfig = 0x11B000; // where the config's 6 bytes are
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(0x120000, 0);
    mt_cpu cpu{};
    Guest() {
        cpu.mem = mem.data();
        cpu.mem_size = mem.size();
        const std::int32_t table[4] = {65, 50, 40, -1};
        std::memcpy(mem.data() + fe::kGameSpeedTable, table, sizeof table);
        set_interval(10); // CMTimer's constructor
    }
    std::int32_t table(int i) const {
        std::int32_t v = 0;
        std::memcpy(&v, mem.data() + fe::kGameSpeedTable + 4 * i, 4);
        return v;
    }
    std::int64_t interval() const {
        std::int64_t v = 0;
        std::memcpy(&v, mem.data() + fe::kFrameInterval, 8);
        return v;
    }
    void set_interval(std::int64_t v) { std::memcpy(mem.data() + fe::kFrameInterval, &v, 8); }
    // LoadConfig: the config read (Speed byte `speed`), then ApplyGameSpeed 0x37C41.
    void load_config(std::uint8_t speed) {
        const std::uint32_t p = kConfig;
        std::memcpy(mem.data() + fe::kConfigPointer, &p, 4);
        const std::uint8_t config[6] = {3, speed, 0, 1, 1, 0};
        std::memcpy(mem.data() + kConfig, config, 6);
        set_interval(table(speed));
    }
    std::uint8_t config_speed() const { return mem[kConfig + 1]; }
};

// The game's tick gate (CMTimer::OnRunLoopTimer) on a guest clock, driven as the turn hook's
// wall wait drives it: the 1 ms run-loop timer's next fire date on the guest clock, waited for
// on the host as (fire date - now) / rate, plus `late` of host oversleep. Returns the ticks a
// host second over `ticks` ticks.
template <typename Clock>
double gate_rate(Clock &clock, Host &host, std::int64_t tick_ms, std::uint32_t rate,
                 std::uint64_t ticks, std::uint64_t late = 0) {
    std::int64_t last_frame = fe::guest_time_ms(clock.unix_us());
    std::uint64_t next_fire = clock.unix_us() + 1000;
    std::uint64_t n = 0, first = 0, last = 0;
    while (n <= ticks) {
        const std::uint64_t now_us = clock.unix_us();
        if (next_fire > now_us) {
            host.now += (next_fire - now_us) * 1000 / rate + late;
        }
        const std::int64_t now_ms = fe::guest_time_ms(clock.unix_us());
        if (now_ms - last_frame >= tick_ms) {
            last_frame = now_ms;
            if (n++ == 0) {
                first = host.now;
            }
            last = host.now;
            host.now += 3 * kMs / rate; // the tick's work (the guest's time is the host's x rate)
        }
        next_fire = clock.unix_us() + 1000;
    }
    return static_cast<double>(ticks) / (static_cast<double>(last - first) / 1e9);
}

// The paced clock as the libc clock would read it.
struct PacedUnix {
    fe::PacedClock &c;
    std::uint64_t unix_us() { return c.unix_us(); }
};

} // namespace

TEST_CASE("game speed: the two Assists rows, neutral in every preset") {
    fe::Settings s;
    CHECK(s.game_speed == GameSpeed::kGame);
    CHECK(s.fast_forward == FastForward::kOff);
    for (const char *key : {"game_speed", "fast_forward"}) {
        const fe::SettingInfo *info = fe::find_setting(key);
        REQUIRE(info != nullptr);
        CHECK(info->group == fe::SettingGroup::kAssists);
        CHECK(info->applies == fe::Applies::kAtOnce);
    }
    CHECK(fe::parse_settings("game_speed = 25\nfast_forward = X3\n", &s).empty());
    CHECK(s.game_speed == GameSpeed::k25);
    CHECK(s.fast_forward == FastForward::kX3);
    const std::vector<std::string> bad = fe::parse_settings("game_speed = 20\n", &s);
    REQUIRE(bad.size() == 1);
    CHECK(bad[0] == "line 1: game_speed: '20' is not game, 65, 50, 40, 33 or 25, ignored");
    CHECK(s.game_speed == GameSpeed::k25);
    fe::Settings back;
    fe::parse_settings(fe::serialise_settings(s), &back);
    CHECK(back == s);
    for (const fe::Preset p :
         {fe::Preset::kOriginal, fe::Preset::kOriginalController, fe::Preset::kEnhanced}) {
        fe::Settings t = s;
        fe::apply_preset(&t, p);
        CHECK(t.game_speed == GameSpeed::kGame);
        CHECK(t.fast_forward == FastForward::kOff);
    }
    const std::uint32_t want[] = {0, 65, 50, 40, 33, 25};
    for (int i = 0; i < 6; i++) {
        CHECK(fe::game_speed_ms(static_cast<GameSpeed>(i)) == want[i]);
    }
    CHECK(std::string(fe::game_speed_name(GameSpeed::k33)) == "33");
    CHECK(std::string(fe::fast_forward_name(FastForward::kX2)) == "x2");
}

TEST_CASE("game speed: the table is patched before the config is read, and the config keeps the "
          "game's own Speed byte") {
    for (const GameSpeed speed :
         {GameSpeed::k65, GameSpeed::k50, GameSpeed::k40, GameSpeed::k33, GameSpeed::k25}) {
        for (std::uint8_t option = 0; option < 3; option++) {
            Guest g;
            REQUIRE(fe::patch_game_speed(&g.cpu, speed));
            CHECK(g.table(3) == -1); // the popup id after the table is not touched
            g.load_config(option);
            CAPTURE(option);
            CHECK(g.interval() == fe::game_speed_ms(speed)); // whatever the game's option
            CHECK(g.config_speed() == option);
        }
    }
    // `game`: the game's own table (a patch written before is undone).
    Guest g;
    REQUIRE(fe::patch_game_speed(&g.cpu, GameSpeed::k25));
    REQUIRE(fe::patch_game_speed(&g.cpu, GameSpeed::kGame));
    for (int i = 0; i < 3; i++) {
        CHECK(g.table(i) == fe::kGameSpeedLengths[i]);
    }
    // Not the game's table: nothing written.
    Guest other;
    const std::int32_t junk = 7;
    std::memcpy(other.mem.data() + fe::kGameSpeedTable + 4, &junk, 4);
    CHECK_FALSE(fe::patch_game_speed(&other.cpu, GameSpeed::k25));
    CHECK(other.table(0) == 65);
    // A guest whose memory does not reach the table.
    mt_cpu small{};
    std::vector<std::uint8_t> mem(0x1000);
    small.mem = mem.data();
    small.mem_size = mem.size();
    CHECK_FALSE(fe::patch_game_speed(&small, GameSpeed::k25));
    CHECK(fe::apply_game_speed(&small, GameSpeed::k25) == 0);
}

TEST_CASE("game speed: a change while the game runs sets CMTimer's interval at once") {
    Guest g;
    // Before the config is read: only the table (LoadConfig applies it).
    CHECK(fe::apply_game_speed(&g.cpu, GameSpeed::k33) == 0);
    CHECK(g.interval() == 10);
    g.load_config(1);
    CHECK(g.interval() == 33);
    CHECK(fe::apply_game_speed(&g.cpu, GameSpeed::k65) == 65);
    CHECK(g.interval() == 65);
    // Back to the game's own: its Speed option's length (40 for byte 2), the byte unchanged.
    g.mem[Guest::kConfig + 1] = 2;
    CHECK(fe::apply_game_speed(&g.cpu, GameSpeed::kGame) == 40);
    CHECK(g.interval() == 40);
    CHECK(g.config_speed() == 2);
    // A bad byte reads as 2, as mt_saves_sanitise reads it (#78).
    g.mem[Guest::kConfig + 1] = 3;
    CHECK(fe::apply_game_speed(&g.cpu, GameSpeed::kGame) == 40);
    CHECK(g.config_speed() == 3);
}

TEST_CASE("game speed: the fast-forward rate and the overrun threshold") {
    CHECK(fe::fast_forward_rate(FastForward::kOff, true) == 1);
    CHECK(fe::fast_forward_rate(FastForward::kX2, false) == 1);
    CHECK(fe::fast_forward_rate(FastForward::kX2, true) == 2);
    CHECK(fe::fast_forward_rate(FastForward::kX3, true) == 3);
    CHECK(fe::overrun_ns_for(50, 1) == fe::FrameStats::kDefaultOverrunNs); // as before #151
    CHECK(fe::overrun_ns_for(65, 1) == 71'500'000);
    CHECK(fe::overrun_ns_for(50, 2) == 27'500'000);
    CHECK(fe::overrun_ns_for(25, 3) == 9'166'666);
    CHECK(fe::overrun_ns_for(0, 1) == fe::FrameStats::kDefaultOverrunNs);
    CHECK(fe::overrun_ns_for(10, 1) == 11'000'000);
}

TEST_CASE("game speed: the fast_forward action is held in the bindings layer and never reaches "
          "the guest") {
    struct Sink {
        std::vector<std::pair<std::uint32_t, bool>> sent;
        static void fn(void *u, std::uint32_t a, bool d) {
            static_cast<Sink *>(u)->sent.emplace_back(a, d);
        }
    } sink;
    static std::int32_t panel = 0;
    fe::Input input;
    fe::Actions a(&input);
    a.set_sink(Sink::fn, &sink);
    a.set_panel_source([](void *) { return panel; }, nullptr);
    CHECK(fe::port_action(MT_ACTION_FAST_FORWARD));
    CHECK_FALSE(fe::port_action(MT_ACTION_GRADE_REVIVE));
    const fe::Control tab{fe::ControlKind::kKey, static_cast<int>(SDLK_TAB)};
    const fe::Control r3{fe::ControlKind::kGamepadButton,
                         static_cast<int>(SDL_GAMEPAD_BUTTON_RIGHT_STICK)};
    for (const std::int32_t p : {0, 1}) {
        panel = p;
        for (const fe::Context c : {fe::Context::kField, fe::Context::kMenu}) {
            CHECK(a.action_for(tab, c) == MT_ACTION_FAST_FORWARD);
            CHECK(a.action_for(r3, c) == MT_ACTION_FAST_FORWARD);
        }
        CHECK(a.control(fe::InputSource::kKeyboard, tab, true) == MT_ACTION_FAST_FORWARD);
        CHECK(a.holding(MT_ACTION_FAST_FORWARD));
        CHECK(a.control(fe::InputSource::kGamepad, r3, true) == MT_ACTION_FAST_FORWARD);
        a.control(fe::InputSource::kKeyboard, tab, false);
        CHECK(a.holding(MT_ACTION_FAST_FORWARD)); // the pad still holds it
        a.release_source(fe::InputSource::kGamepad);
        CHECK_FALSE(a.holding(MT_ACTION_FAST_FORWARD));
    }
    CHECK(sink.sent.empty());
    // The runtime ignores it too.
    mt_input_configure(nullptr);
    mt_input_press(MT_ACTION_FAST_FORWARD);
    CHECK(mt_input_held(MT_ACTION_FAST_FORWARD) == 0);
    CHECK(mt_input_get_stats().queued == 0);
    const mt_input_action_info *info = mt_input_action_get_info(MT_ACTION_FAST_FORWARD);
    REQUIRE(info != nullptr);
    CHECK(std::string(info->name) == "fast_forward");
    CHECK(mt_input_action_named("fast_forward") == MT_ACTION_FAST_FORWARD);
    // A bindings file moves it like any action, and writes it back.
    fe::BindingSet set;
    CHECK(fe::parse_bindings("[field]\nfast_forward = key F5\n", "b.ini", &set).empty());
    const fe::Control f5{fe::ControlKind::kKey, static_cast<int>(SDLK_F5)};
    CHECK(fe::controls_for(set.table, fe::Context::kField, MT_ACTION_FAST_FORWARD) ==
          std::vector<fe::Control>{f5});
    CHECK(fe::serialise_bindings(set).find("fast_forward = key F5") != std::string::npos);
}

TEST_CASE("game speed: fast-forward runs the paced clock's wall plan 2 or 3 times as fast, "
          "continuous at every toggle") {
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
    const PacingPlan x2 = fe::plan_pacing(PacingMode::kDisplay, 60, true, 50, 2);
    CHECK_FALSE(x2.locked);
    CHECK(x2.rate == 2);
    CHECK(fe::describe(x2) == "the wall clock x2 (fast-forward)");
    clock.set_plan(fe::plan_pacing(PacingMode::kDisplay, 60, true, 50));
    host.now += 1000 * kMs;
    const std::uint64_t before = clock.guest_ns();
    clock.set_plan(x2);
    CHECK(clock.guest_ns() == before);
    host.now += 1000 * kMs;
    CHECK(clock.guest_ns() - before == 2000 * kMs);
    CHECK(clock.gained_ns() == 1000 * kMs);
    CHECK(clock.fast_ns() == 1000 * kMs);
    // host_when on the fast plan: the first host time the guest clock reads a time.
    std::mt19937_64 rng(151);
    for (int i = 0; i < 2000; i++) {
        const std::uint64_t g = clock.guest_ns() + rng() % (3600 * 1000 * kMs);
        const std::uint64_t h = clock.host_when(g);
        CHECK(clock.guest_at(h) >= g);
        CHECK(clock.guest_at(h - 1) < g);
    }
    // Random toggles between the lock, x2, x3 and the wall clock: continuous, never back, and
    // within a refresh plus half a step of the wall clock plus what fast-forward gained.
    const PacingPlan plans[] = {fe::plan_pacing(PacingMode::kDisplay, 60, true, 50),
                                fe::plan_pacing(PacingMode::kDisplay, 60, true, 50, 3), x2,
                                fe::plan_pacing(PacingMode::kDisplay, 120, true, 50),
                                fe::plan_pacing(PacingMode::kWall, 60, true, 50)};
    std::uint64_t last = clock.guest_ns();
    for (int i = 0; i < 5000; i++) {
        host.now += rng() % (5 * kMs);
        if (i % 41 == 0) {
            const std::uint64_t at = clock.guest_ns();
            clock.set_plan(plans[rng() % std::size(plans)]);
            CHECK(clock.guest_ns() == at);
        }
        const std::uint64_t g = clock.guest_ns();
        CHECK(g >= last);
        last = g;
        const std::int64_t drift = static_cast<std::int64_t>(g) -
                                   static_cast<std::int64_t>(clock.host_elapsed_ns()) -
                                   static_cast<std::int64_t>(clock.gained_ns());
        CHECK(drift < static_cast<std::int64_t>(9 * kMs));
        CHECK(drift > -static_cast<std::int64_t>(30 * kMs));
    }
}

TEST_CASE("game speed: a lock after a fast-forward goes on at once (it does not wait for the wall "
          "clock)") {
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
    const PacingPlan lock = fe::plan_pacing(PacingMode::kDisplay, 60, true, 50);
    REQUIRE(lock.locked);
    clock.set_plan(lock);
    host.now += 2000 * kMs;
    clock.set_plan(fe::plan_pacing(PacingMode::kDisplay, 60, true, 50, 3));
    host.now += 10'000 * kMs; // 10 s held: 20 s gained
    clock.set_plan(lock);
    CHECK(clock.gained_ns() == 20'000 * kMs);
    const std::uint64_t at = clock.guest_ns();
    host.now += 1000 * kMs;
    // A second on: a second of guest time (to a step and a refresh), not a frozen clock.
    const std::uint64_t later = clock.guest_ns();
    CHECK(later - at > 1000 * kMs - 34 * kMs);
    CHECK(later - at < 1000 * kMs + 34 * kMs);
}

TEST_CASE("game speed: the launch clock's rate (pacing = wall) keeps the shift and the time "
          "continuous") {
    static std::uint64_t wall = 1797443813000ull * 1000u; // 0.4 s before the 2^31 jump
    fe::LaunchClock clock;
    clock.install([](void *) { return wall; }, nullptr);
    const std::uint64_t shift = clock.shift_us();
    CHECK(shift == fe::LaunchClock::shift_for(1797443813000ull * 1000u));
    wall += 1'000'000;
    CHECK(clock.unix_us() == wall - shift);
    clock.set_rate(2);
    const std::uint64_t at = clock.unix_us();
    CHECK(at == wall - shift);
    wall += 1'000'000;
    CHECK(clock.unix_us() - at == 2'000'000);
    CHECK(clock.gained_us() == 1'000'000);
    clock.set_rate(1);
    CHECK(clock.unix_us() - at == 2'000'000);
    wall += 1'000'000;
    CHECK(clock.unix_us() - at == 3'000'000);
    CHECK(clock.gained_us() == 1'000'000);
    CHECK(fe::guest_time_ms(clock.unix_us()) >= 0);
    clock.set_rate(3);
    wall += 500'000;
    CHECK(clock.gained_us() == 2'000'000);
    clock.uninstall();
}

TEST_CASE("game speed: the game's tick gate on the fast clock ticks 2 and 3 times each speed's "
          "rate, within 1 %") {
    for (const std::int64_t tick_ms : {65, 50, 40, 33, 25}) {
        for (const std::uint32_t rate : {1u, 2u, 3u}) {
            CAPTURE(tick_ms);
            CAPTURE(rate);
            const double want = 1000.0 / static_cast<double>(tick_ms) * rate;
            {
                Host host;
                fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
                clock.set_plan(fe::plan_pacing(PacingMode::kWall, 0, false, tick_ms, rate));
                PacedUnix u{clock};
                CHECK(gate_rate(u, host, tick_ms, rate, 2000) ==
                      doctest::Approx(want).epsilon(0.01));
            }
            {
                Host host;
                fe::LaunchClock clock;
                clock.install(
                    [](void *u) { return kStartUnixUs + static_cast<Host *>(u)->now / 1000; },
                    &host);
                clock.set_rate(rate);
                CHECK(gate_rate(clock, host, tick_ms, rate, 2000) ==
                      doctest::Approx(want).epsilon(0.01));
                clock.uninstall();
            }
        }
    }
}

// --- Fast-forward's presents and the wait for the gate (ADR 0087 addendum) ---------------------

namespace {

void set_last_frame(Guest &g, std::int64_t ms) {
    std::memcpy(g.mem.data() + fe::kLastFrame, &ms, sizeof ms);
}

// What the turn hook does about the window while fast-forwarding.
enum class Policy {
    kRefresh, // before the fix: a present a refresh apart, wherever it lands
    kGate,    // FastForwardPresents and ff_gate_wait
};

struct FastRun {
    double ticks_per_second = 0;
    std::uint64_t presents = 0;
};

// The game on the paced clock's fast plan, as the frontend drives it in real time: the 1 ms
// run-loop timer polls the gate; a tick's work takes `work` of host time, then its GL frame is
// presented if the policy lets it (`cost` of host time); each turn hook takes `turn` and
// presents a frame still pending if the policy lets it; the wait for the next timer (or, kGate,
// the gate) oversleeps by 50 us. Ticks a host second over `ticks` ticks, and the presents.
FastRun fast_run(Policy policy, std::uint32_t rate, std::int64_t tick_ms, std::uint64_t work,
                 std::uint64_t cost, std::uint64_t turn, std::uint64_t ticks) {
    constexpr std::uint64_t kRefreshNs = 16'666'667;
    Guest guest;
    guest.set_interval(tick_ms);
    Host host;
    fe::PacedClock clock(Host::now_fn, Host::sleep_fn, &host, kStartUnixUs);
    clock.set_plan(fe::plan_pacing(PacingMode::kWall, 0, false, tick_ms, rate));
    fe::FastForwardPresents presents;
    std::int64_t last_frame = fe::guest_time_ms(clock.unix_us());
    set_last_frame(guest, last_frame);
    std::uint64_t next_fire = clock.unix_us() + 1000;
    std::uint64_t last_present = host.now;
    bool pending = false;
    std::uint64_t n = 0, first = 0, last = 0;
    FastRun out;
    auto try_present = [&] {
        if (!pending) {
            return;
        }
        const std::uint64_t since = host.now - last_present;
        bool go = since >= kRefreshNs;
        if (policy == Policy::kGate) {
            const std::int64_t gate_us = fe::gate_in_us(&guest.cpu, clock.unix_us());
            go = presents.allow(host.now, since, kRefreshNs,
                                gate_us == fe::kNoGate ? fe::kNoGate
                                                       : gate_us * 1000 / std::int64_t{rate});
        }
        if (go) {
            host.now += cost;
            presents.presented(cost);
            last_present = host.now;
            pending = false;
            out.presents++;
        }
    };
    while (n <= ticks) {
        if (next_fire <= clock.unix_us()) {
            const std::int64_t now_ms = fe::guest_time_ms(clock.unix_us());
            if (now_ms - last_frame >= tick_ms) {
                last_frame = now_ms;
                set_last_frame(guest, last_frame);
                if (n++ == 0) {
                    first = host.now;
                }
                last = host.now;
                host.now += work;
                pending = true;
                try_present(); // the GL present inside the tick
            }
            next_fire = clock.unix_us() + 1000;
        }
        host.now += turn;
        try_present(); // the turn hook's
        const double now_us = static_cast<double>(clock.unix_us());
        double wait_s = (static_cast<double>(next_fire) - now_us) / 1e6;
        if (policy == Policy::kGate) {
            wait_s = fe::ff_gate_wait(wait_s, fe::gate_in_us(&guest.cpu, clock.unix_us()));
        }
        if (wait_s > 0) {
            host.now += static_cast<std::uint64_t>(wait_s * 1e9 / rate);
        }
        host.now += 50'000;
    }
    out.ticks_per_second = static_cast<double>(ticks) / (static_cast<double>(last - first) / 1e9);
    return out;
}

} // namespace

TEST_CASE("game speed: the time to the game's gate from CMTimer's lastFrame and interval") {
    Guest g;
    g.set_interval(50);
    const std::uint64_t unix_us = kStartUnixUs + 123'456;
    const std::int64_t now_ms = fe::guest_time_ms(unix_us);
    set_last_frame(g, now_ms - 20);
    // 30 ms left in whole milliseconds, less the 456 us of the current one.
    CHECK(fe::gate_in_us(&g.cpu, unix_us) == 30'000 - 456);
    set_last_frame(g, now_ms - 50);
    CHECK(fe::gate_in_us(&g.cpu, unix_us) == -456); // open
    set_last_frame(g, now_ms - 80);
    CHECK(fe::gate_in_us(&g.cpu, unix_us) == -30'456);
    g.set_interval(0);
    CHECK(fe::gate_in_us(&g.cpu, unix_us) == fe::kNoGate);
    g.set_interval(1001);
    CHECK(fe::gate_in_us(&g.cpu, unix_us) == fe::kNoGate);
    mt_cpu small{};
    small.mem = g.mem.data();
    small.mem_size = fe::kLastFrame; // lastFrame not mapped
    CHECK(fe::gate_in_us(&small, unix_us) == fe::kNoGate);
}

TEST_CASE("game speed: fast-forward waits for the gate when it opens within a poll after the "
          "timer") {
    CHECK(fe::ff_gate_wait(0.0008, fe::kNoGate) == 0.0008);
    CHECK(fe::ff_gate_wait(0.0008, 20'000) == 0.0008); // far off: the 1 ms polls go on
    CHECK(fe::ff_gate_wait(0.0008, 500) == 0.0008);    // open before the timer: at the timer
    CHECK(fe::ff_gate_wait(0.0008, 1'500) == doctest::Approx(0.001505));
    CHECK(fe::ff_gate_wait(0.0008, 1'795) == doctest::Approx(0.0018));
    CHECK(fe::ff_gate_wait(0.0008, 1'796) == 0.0008);                   // past a poll after it
    CHECK(fe::ff_gate_wait(-0.0002, 300) == doctest::Approx(0.000305)); // a late timer too
}

TEST_CASE("game speed: fast-forward presents where they end before the gate, else on a 1 % "
          "lateness budget") {
    constexpr std::uint64_t kRefresh = 16'666'667;
    fe::FastForwardPresents p;
    p.presented(5 * kMs);
    CHECK(p.cost_ns() == 6 * kMs); // the longest recent one and the margin
    std::uint64_t now = 1000 * kMs;
    p.restart();
    CHECK(p.allow(now, 20 * kMs, kRefresh, 10 * kMs));       // ends 4 ms before the gate
    CHECK_FALSE(p.allow(now, 10 * kMs, kRefresh, 10 * kMs)); // a refresh apart
    CHECK(p.allow(now, 20 * kMs, kRefresh, fe::kNoGate));    // no gate known: the refresh only
    CHECK_FALSE(p.allow(now, 20 * kMs, kRefresh, 5 * kMs));  // 1 ms over, no budget yet
    // The budget grows by 1 % of the host time: 100 ms pays for 1 ms of lateness.
    now += 99 * kMs;
    CHECK_FALSE(p.allow(now, 20 * kMs, kRefresh, 5 * kMs));
    now += 2 * kMs;
    CHECK(p.allow(now, 20 * kMs, kRefresh, 5 * kMs));
    CHECK(p.overran() == 1);
    CHECK(p.allowed() == 3);
    CHECK(p.budget_ns() < 0.02 * kMs);
    // It never holds more than one present's cost: no burst after a long quiet stretch.
    now += 10'000 * kMs;
    CHECK(p.allow(now, 20 * kMs, kRefresh, 0)); // gate open: the whole 6 ms
    CHECK(p.budget_ns() == doctest::Approx(0.0));
    CHECK_FALSE(p.allow(now, 20 * kMs, kRefresh, 0));
    // A new fast-forward starts with none.
    now += 1000 * kMs;
    p.restart();
    CHECK_FALSE(p.allow(now, 20 * kMs, kRefresh, 0));
    // A longer present raises the cost for the next kCosts presents.
    for (std::size_t i = 0; i < fe::FastForwardPresents::kCosts - 1; i++) {
        p.presented(5 * kMs);
    }
    p.presented(20 * kMs);
    CHECK(p.cost_ns() == 21 * kMs);
    for (std::size_t i = 0; i < fe::FastForwardPresents::kCosts - 1; i++) {
        p.presented(5 * kMs);
    }
    CHECK(p.cost_ns() == 21 * kMs);
    p.presented(5 * kMs);
    CHECK(p.cost_ns() == 6 * kMs);
}

TEST_CASE("game speed: fast-forward holds its rate with the window's presents in the tick's "
          "slack, cheap or dear") {
    // The measured shapes (platform.game_speed on SDL's dummy driver, the CPU composite): a
    // Release present about 6 ms, a Debug one about 21 ms; a tick's work 1 and 6.5 ms; a turn
    // hook 0.05 and 0.3 ms.
    struct Shape {
        std::uint64_t work, cost, turn;
    };
    for (const Shape s : {Shape{1 * kMs, 6 * kMs, 50'000}, Shape{6'500'000, 21 * kMs, 300'000}}) {
        for (const std::uint32_t rate : {2u, 3u}) {
            CAPTURE(s.cost);
            CAPTURE(rate);
            const double want = 20.0 * rate;
            const FastRun fixed = fast_run(Policy::kGate, rate, 50, s.work, s.cost, s.turn, 2000);
            CHECK(fixed.ticks_per_second == doctest::Approx(want).epsilon(0.015));
            // The window still moves: every tick's frame when the present fits, else a few a
            // second on the budget.
            const double seconds = 2000.0 / fixed.ticks_per_second;
            CHECK(static_cast<double>(fixed.presents) / seconds > (s.cost < 10 * kMs ? 15 : 0.5));
        }
    }
    // Before the fix a dear present a refresh after the last landed late in the next tick's
    // slack and ran over its gate: x2 ran about a third slow, as platform.game_speed measured
    // in Debug.
    const FastRun before = fast_run(Policy::kRefresh, 2, 50, 6'500'000, 21 * kMs, 300'000, 2000);
    CHECK(before.ticks_per_second < 40.0 * 0.85);
}
