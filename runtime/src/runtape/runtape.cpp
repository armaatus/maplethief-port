// The run tape (#405, ADR 0156); see maplethief/runtape.h.
#include "maplethief/runtape.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "hle/libc_internal.h"
#include "maplethief/cf.h"
#include "maplethief/gcd.h"
#include "maplethief/hle/libc.h"
#include "maplethief/input.h"
#include "maplethief/memory.h"
#include "maplethief/uikit.h"

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// One stream: its runs, and while playing where the next read is.
struct Stream {
    std::vector<mt_runtape_run> runs;
    std::size_t run = 0; // playing: the run of the next read
    u64 used = 0;        // playing: reads taken from it
    u64 last = 0;        // the last value read (recorded or played)
    bool any = false;    // a value was read

    void record(u64 v) {
        if (!runs.empty() && runs.back().value == v) {
            runs.back().count++;
        } else {
            runs.push_back({v, 1});
        }
        last = v;
        any = true;
    }
    // The next value; false (and the last value) past the end.
    bool play(u64 *v) {
        while (run < runs.size() && used >= runs[run].count) {
            run++;
            used = 0;
        }
        if (run >= runs.size()) {
            *v = last;
            return false;
        }
        used++;
        last = runs[run].value;
        any = true;
        *v = last;
        return true;
    }
};

struct Tape {
    mt_runtape_mode mode = MT_RUNTAPE_OFF;
    Stream streams[MT_RUNTAPE_STREAMS];
    std::vector<mt_runtape_event> events;
    std::size_t next_event = 0; // playing: the next one to deliver
    bool delivering = false;    // playing: mt_runtape_deliver is making a boundary call
    mt_runtape_stats stats{};
    // The clocks a recording reads (never null while recording).
    mt_libc_config libc{};
    mt_gcd_config gcd{};
    // What the stop configures again.
    mt_libc_clock_fn restore_libc = nullptr;
    void *restore_libc_user = nullptr;
    mt_gcd_config restore_gcd{};
    bool wrapped = false;
};

Tape g;
std::mutex g_gcd_mu; // GCD's workers read its clock too

void reset_streams() {
    for (Stream &s : g.streams) {
        s = Stream{};
    }
    g.events.clear();
    g.next_event = 0;
    g.stats = mt_runtape_stats{};
}

u64 take(u32 stream, u64 live) {
    if (g.mode == MT_RUNTAPE_OFF) {
        return live; // stopped while GCD still holds our clock: the wrapped one, untaped
    }
    Stream &s = g.streams[stream];
    g.stats.reads[stream]++;
    if (g.mode == MT_RUNTAPE_PLAY) {
        u64 v = 0;
        if (!s.play(&v)) {
            g.stats.overrun++;
        }
        return v;
    }
    s.record(live);
    return live;
}

// The hosts' clocks, for a recording where the platform configured none: the wall clock as libc
// reads it, and a steady clock as GCD's (its origin is ours, which is all the guest can see:
// installed before the process loads, nothing read GCD's own before).
u64 host_wall_us(void *) {
    using namespace std::chrono;
    return static_cast<u64>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

u64 host_steady_ns(void *) {
    static const auto origin = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - origin)
                        .count();
    return static_cast<u64>(ns) + 1000000000u; // positive, away from DISPATCH_TIME_NOW
}

void host_sleep_ns(void *, u64 deadline) {
    for (u64 now = host_steady_ns(nullptr); now < deadline; now = host_steady_ns(nullptr)) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(deadline - now));
    }
}

u64 tape_libc_clock(void *) {
    if (g.mode == MT_RUNTAPE_PLAY) {
        return take(MT_RUNTAPE_CLOCK, 0);
    }
    if (g.libc.clock == nullptr) {
        return host_wall_us(nullptr);
    }
    return take(MT_RUNTAPE_CLOCK, g.libc.clock(g.libc.clock_user));
}

u64 tape_gcd_clock(void *) {
    const std::lock_guard<std::mutex> lock(g_gcd_mu);
    if (g.mode == MT_RUNTAPE_PLAY) {
        return take(MT_RUNTAPE_GCD, 0);
    }
    if (g.gcd.clock == nullptr) {
        return host_steady_ns(nullptr);
    }
    return take(MT_RUNTAPE_GCD, g.gcd.clock(g.gcd.clock_user));
}

void tape_gcd_sleep(void *, u64 deadline) {
    if (g.mode == MT_RUNTAPE_PLAY) {
        return; // the tape's next value is the time the wait ended
    }
    if (g.gcd.sleep_until == nullptr) {
        host_sleep_ns(nullptr, deadline);
        return;
    }
    g.gcd.sleep_until(g.gcd.sleep_user, deadline);
}

void deliver(mt_cpu *cpu, const mt_runtape_event &e) {
    switch (e.kind) {
    case MT_RUNTAPE_PRESS:
        mt_input_press(e.a);
        break;
    case MT_RUNTAPE_RELEASE:
        mt_input_release(e.a);
        break;
    case MT_RUNTAPE_RELEASE_ALL:
        mt_input_release_all();
        break;
    case MT_RUNTAPE_TOUCH:
        mt_uikit_touch(e.a, e.b, e.x, e.y);
        break;
    case MT_RUNTAPE_LIFECYCLE:
        mt_uikit_lifecycle_event(cpu, e.a);
        break;
    case MT_RUNTAPE_FIRE_TOLERANCE: {
        double d = 0;
        std::memcpy(&d, &e.value, sizeof d);
        mt_runloop_set_fire_tolerance(d);
        break;
    }
    case MT_RUNTAPE_POKE:
        mt_runtape_poke(cpu, e.a, &e.value, e.b);
        break;
    default:
        break;
    }
}

} // namespace

extern "C" int mt_runtape_record(void) noexcept {
    mt_runtape_stop();
    reset_streams();
    g.libc = mt_libc_cfg();
    g.gcd = mt_gcd_get_config();
    // Wrap the clocks in effect, or the hosts' where none is configured.
    if (g.libc.clock == nullptr) {
        g.libc.clock = host_wall_us;
        g.libc.clock_user = nullptr;
    }
    if (g.gcd.clock == nullptr) {
        g.gcd.clock = host_steady_ns;
        g.gcd.clock_user = nullptr;
    }
    if (g.gcd.sleep_until == nullptr) {
        g.gcd.sleep_until = host_sleep_ns;
        g.gcd.sleep_user = nullptr;
    }
    const mt_gcd_config wrapped = mt_gcd_get_config(); // what the stop puts back
    mt_libc_config lc = mt_libc_cfg();
    lc.clock = tape_libc_clock;
    lc.clock_user = nullptr;
    mt_gcd_config gc = wrapped;
    gc.clock = tape_gcd_clock;
    gc.clock_user = nullptr;
    gc.sleep_until = tape_gcd_sleep;
    gc.sleep_user = nullptr;
    if (mt_gcd_configure(&gc) != 0) {
        return -1; // GCD has workers: too late
    }
    g.restore_gcd = wrapped;
    g.restore_libc = g.libc.clock == host_wall_us ? nullptr : g.libc.clock;
    g.restore_libc_user = g.libc.clock_user;
    g.mode = MT_RUNTAPE_RECORD;
    g.wrapped = true;
    mt_libc_configure(&lc);
    return 0;
}

extern "C" int mt_runtape_play(const mt_runtape_run *const runs[MT_RUNTAPE_STREAMS],
                               const size_t run_counts[MT_RUNTAPE_STREAMS],
                               const mt_runtape_event *events, size_t event_count) noexcept {
    for (size_t i = 0; i < event_count; i++) {
        if (events[i].kind == 0 || events[i].kind > MT_RUNTAPE_KIND_LAST ||
            (i > 0 && events[i].turn < events[i - 1].turn)) {
            return -1;
        }
    }
    mt_runtape_stop();
    reset_streams();
    for (u32 s = 0; s < MT_RUNTAPE_STREAMS; s++) {
        g.streams[s].runs.assign(runs[s], runs[s] + run_counts[s]);
    }
    g.events.assign(events, events + event_count);
    g.libc = mt_libc_cfg();
    g.gcd = mt_gcd_get_config();
    const mt_gcd_config wrapped = g.gcd;
    mt_gcd_config gc = wrapped;
    gc.clock = tape_gcd_clock;
    gc.clock_user = nullptr;
    gc.sleep_until = tape_gcd_sleep;
    gc.sleep_user = nullptr;
    if (mt_gcd_configure(&gc) != 0) {
        return -1;
    }
    g.restore_gcd = wrapped;
    g.restore_libc = mt_libc_cfg().clock;
    g.restore_libc_user = mt_libc_cfg().clock_user;
    g.mode = MT_RUNTAPE_PLAY;
    g.wrapped = true;
    mt_libc_config lc = mt_libc_cfg();
    lc.clock = tape_libc_clock;
    lc.clock_user = nullptr;
    mt_libc_configure(&lc);
    return 0;
}

extern "C" void mt_runtape_stop(void) noexcept {
    if (g.wrapped) {
        mt_libc_config lc = mt_libc_cfg();
        lc.clock = g.restore_libc;
        lc.clock_user = g.restore_libc_user;
        mt_libc_configure(&lc);
        mt_gcd_configure(&g.restore_gcd); // refused while GCD has workers: mt_gcd_reset clears it
        g.wrapped = false;
    }
    g.mode = MT_RUNTAPE_OFF;
}

extern "C" mt_runtape_mode mt_runtape_get_mode(void) noexcept { return g.mode; }

extern "C" const mt_runtape_run *mt_runtape_runs(uint32_t stream, size_t *count) noexcept {
    if (stream >= MT_RUNTAPE_STREAMS) {
        *count = 0;
        return nullptr;
    }
    *count = g.streams[stream].runs.size();
    return g.streams[stream].runs.data();
}

extern "C" const mt_runtape_event *mt_runtape_events(size_t *count) noexcept {
    *count = g.events.size();
    return g.events.data();
}

extern "C" mt_runtape_stats mt_runtape_get_stats(void) noexcept { return g.stats; }

extern "C" void mt_runtape_turn(void) noexcept {
    if (g.mode != MT_RUNTAPE_OFF) {
        g.stats.turn++;
    }
}

extern "C" void mt_runtape_deliver(mt_cpu *cpu) noexcept {
    if (g.mode != MT_RUNTAPE_PLAY) {
        return;
    }
    while (g.next_event < g.events.size() && g.events[g.next_event].turn <= g.stats.turn) {
        const mt_runtape_event e = g.events[g.next_event++];
        g.delivering = true;
        deliver(cpu, e);
        g.delivering = false;
        g.stats.events++;
    }
}

extern "C" int mt_runtape_admit(const mt_runtape_event *event) noexcept {
    switch (g.mode) {
    case MT_RUNTAPE_RECORD: {
        mt_runtape_event e = *event;
        e.turn = g.stats.turn;
        g.events.push_back(e);
        g.stats.events++;
        return 1;
    }
    case MT_RUNTAPE_PLAY:
        if (g.delivering) {
            return 1;
        }
        g.stats.dropped++;
        return 0;
    default:
        return 1;
    }
}

extern "C" int mt_runtape_music_playing(int live) noexcept {
    if (g.mode == MT_RUNTAPE_OFF) {
        return live;
    }
    return take(MT_RUNTAPE_MUSIC, live != 0 ? 1u : 0u) != 0 ? 1 : 0;
}

extern "C" int mt_runtape_poke(mt_cpu *cpu, uint32_t addr, const void *bytes,
                               uint32_t length) noexcept {
    if (length == 0 || length > 8 || static_cast<u64>(addr) + length > cpu->mem_size) {
        return 0;
    }
    mt_runtape_event e{};
    e.kind = MT_RUNTAPE_POKE;
    e.a = addr;
    e.b = length;
    std::memcpy(&e.value, bytes, length);
    if (mt_runtape_admit(&e) == 0) {
        return 0;
    }
    std::memcpy(cpu->mem + addr, bytes, length);
    return 1;
}

extern "C" double mt_runtape_host_time(void) noexcept {
    u64 us = 0;
    if (g.mode == MT_RUNTAPE_RECORD) {
        us = g.libc.clock(g.libc.clock_user); // not taped
    } else if (g.mode == MT_RUNTAPE_PLAY && g.streams[MT_RUNTAPE_CLOCK].any) {
        us = g.streams[MT_RUNTAPE_CLOCK].last;
    } else if (g.mode == MT_RUNTAPE_PLAY && !g.streams[MT_RUNTAPE_CLOCK].runs.empty()) {
        us = g.streams[MT_RUNTAPE_CLOCK].runs.front().value;
    } else {
        return mt_cf_absolute_time();
    }
    return static_cast<double>(us) / 1e6 - MT_CF_ABSOLUTE_TIME_1970;
}

extern "C" uint64_t mt_runtape_guest_clock_us(void) noexcept {
    if (g.mode == MT_RUNTAPE_OFF || !g.streams[MT_RUNTAPE_CLOCK].any) {
        return 0;
    }
    return g.streams[MT_RUNTAPE_CLOCK].last;
}
