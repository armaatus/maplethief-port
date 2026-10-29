// CFRunLoop, CFRunLoopTimer and CFAbsoluteTime (#48, ADR 0032).
//
// There is one run loop, the main thread's (the game creates no thread). The platform layer
// runs it one turn at a time with mt_runloop_run_once (maplethief/cf.h). Its only timer in
// the game is RunLoopTimer_Arm's 0x8E29 frame timer: created with a 1 ms interval, added in
// kCFRunLoopCommonModes, and re-armed from its own callout with CFRunLoopTimerSetNextFireDate
// (docs/re/program-flow.md).
//
// Timers follow CF: a due timer fires once per turn, earliest fire date first; the callout
// gets (timer, info). Afterwards a repeating timer whose fire date the callout did not change
// moves to the first point of its interval grid after now (missed fires are skipped); a
// one-shot timer (interval <= 0) is invalidated, which removes it from the run loop. A timer
// added in a mode other than the default or common modes never fires: the loop always runs
// in the default mode (UIKit's tracking mode does not exist here).
//
// A turn: autorelease pool push, the per-turn sources, the due timers, the offline stubs'
// deferred deliveries (mt_stubs_pump: GameKit/StoreKit completion handlers and delegate
// callbacks, #52), GCD's main queue (mt_gcd_drain_main: due dispatch_after timers, the worker
// threads, then the main-queue blocks queued so far; ADR 0033), pool pop. The stubs go before
// the drain because the game hands GameKit results to the main queue with dispatch_async: that
// block then runs in the same turn, as iOS services the main queue after the sources of the
// same run-loop pass. The turn returns when the loop next has work, in CFAbsoluteTime: now if
// a delivery or GCD work waits, else the earliest timer or GCD deadline. GCD keeps its own
// monotonic clock (dispatch_time is mach_absolute_time on iOS, CFAbsoluteTime is the wall clock),
// so its deadline is converted by the distance between the two "now"s.
//
// Run-loop sources: the only ones are CMSocket's, made by the offline CFSocket stubs (#52,
// runtime/src/stubs/network.cpp) as plain NSObject instances. CFRunLoopAddSource accepts any
// object, retains it like CF and never signals it.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "cf/cf_hle.h"
#include "cf/cf_objects.h"
#include "hle/libc_internal.h"
#include "maplethief/gcd.h"
#include "maplethief/hle/abi.h"
#include "maplethief/stubs.h"

namespace mt_cf {

namespace {

struct TimerState {
    double fire;
    double interval;
    u32 callout;
    u32 info;
    u32 release; // context.release, called with info at deallocation (0: none)
    bool valid;
};

void release_info(mt_cpu *cpu, u32 release, u32 info) {
    if (release != 0) {
        mt_call_guest(cpu, release, &info, 1, release & ~1u);
    }
}

void destroy_timer(mt_cpu *cpu, u32, void *p) {
    const auto *t = static_cast<const TimerState *>(p);
    release_info(cpu, t->release, t->info);
}

const mt_objc_state_type k_timer_state = {"CFRunLoopTimer", sizeof(TimerState), destroy_timer};
const mt_objc_state_type k_runloop_state = {"CFRunLoop", 4, nullptr};

struct Scheduled {
    u32 obj;     // retained by the run loop
    bool active; // added in the default or common modes
    u32 order;   // insertion order, for equal fire dates
};

struct TurnSource {
    mt_runloop_source_fn fn;
    void *user;
};

struct RunLoop {
    u32 object = 0;
    std::vector<Scheduled> timers;
    std::vector<u32> sources; // retained, never signalled
    u32 next_order = 0;
    bool running = false;
};

RunLoop g_loop;
std::vector<TurnSource> g_turn_sources; // survives rebinding: registered once

template <typename T> T *state(mt_cpu *cpu, u32 obj, const mt_objc_state_type &type) {
    mt_objc *rt = runtime(cpu);
    return rt != nullptr && obj != 0 ? static_cast<T *>(mt_objc_find_state(rt, obj, &type))
                                     : nullptr;
}

TimerState *timer_of(mt_cpu *cpu, u32 obj) { return state<TimerState>(cpu, obj, k_timer_state); }

u32 main_loop(mt_cpu *cpu) {
    if (g_loop.object == 0) {
        g_loop.object = create(cpu, classes(cpu).type);
        if (g_loop.object != 0) {
            mt_objc_state(runtime(cpu), g_loop.object, &k_runloop_state);
            make_immortal(cpu, g_loop.object);
        }
    }
    return g_loop.object;
}

bool runs_in_default_mode(mt_cpu *cpu, u32 mode) {
    const std::u16string *m = string_chars(cpu, mode);
    return m != nullptr && (*m == u"kCFRunLoopDefaultMode" || *m == u"kCFRunLoopCommonModes");
}

void remove_timer(mt_cpu *cpu, u32 obj) {
    const auto it = std::find_if(g_loop.timers.begin(), g_loop.timers.end(),
                                 [obj](const Scheduled &s) { return s.obj == obj; });
    if (it != g_loop.timers.end()) {
        g_loop.timers.erase(it);
        mt_objc_release(cpu, obj);
    }
}

void fire(mt_cpu *cpu, u32 obj, double now) {
    TimerState *t = timer_of(cpu, obj);
    if (t == nullptr || !t->valid || t->fire > now) {
        return;
    }
    mt_objc_retain(cpu, obj); // alive through its callout, whatever the callout does
    const double fired = t->fire;
    const u32 args[2] = {obj, t->info};
    mt_call_guest(cpu, t->callout, args, 2, t->callout & ~1u);
    t = timer_of(cpu, obj);
    if (t != nullptr && t->valid) {
        if (t->interval <= 0.0) {
            t->valid = false; // a one-shot timer is done
            remove_timer(cpu, obj);
        } else if (t->fire == fired) {
            const double missed = std::floor((now - fired) / t->interval);
            t->fire = fired + (missed + 1.0) * t->interval;
        }
    }
    mt_objc_release(cpu, obj);
}

// Reads a CFRunLoopTimerContext {version, info, retain, release,
// copyDescription}: retains info through context.retain as CF does.
void read_context(mt_cpu *cpu, u32 context, u32 *info, u32 *release) {
    *info = 0;
    *release = 0;
    if (context == 0) {
        return;
    }
    const std::uint8_t *c = mt_g2h(cpu, context, 20);
    u32 words[5];
    std::memcpy(words, c, sizeof words);
    *info = words[1];
    *release = words[3];
    if (words[2] != 0 && *info != 0) {
        *info = mt_call_guest(cpu, words[2], info, 1, words[2] & ~1u);
    }
}

} // namespace

void runloop_forget() noexcept { g_loop = RunLoop{}; }

} // namespace mt_cf

using namespace mt_cf;

// --- the platform side -------------------------------------------------------------------------

extern "C" double mt_cf_absolute_time(void) noexcept {
    const mt_libc_config &c = mt_libc_cfg();
    std::uint64_t us;
    if (c.clock != nullptr) {
        us = c.clock(c.clock_user);
    } else {
        using namespace std::chrono;
        us = static_cast<std::uint64_t>(
            duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
    }
    return static_cast<double>(us) / 1e6 - MT_CF_ABSOLUTE_TIME_1970;
}

extern "C" int mt_runloop_add_turn_source(mt_runloop_source_fn fn, void *user) noexcept {
    for (const TurnSource &s : g_turn_sources) {
        if (s.fn == fn && s.user == user) {
            return -1;
        }
    }
    g_turn_sources.push_back(TurnSource{fn, user});
    return 0;
}

extern "C" int mt_runloop_remove_turn_source(mt_runloop_source_fn fn, void *user) noexcept {
    for (auto it = g_turn_sources.begin(); it != g_turn_sources.end(); ++it) {
        if (it->fn == fn && it->user == user) {
            g_turn_sources.erase(it);
            return 0;
        }
    }
    return -1;
}

extern "C" double mt_runloop_next_fire_date(void) noexcept {
    double next = MT_CF_DISTANT_FUTURE;
    mt_objc *rt = mt_objc_current();
    for (const Scheduled &s : g_loop.timers) {
        const auto *t =
            rt != nullptr
                ? static_cast<const TimerState *>(mt_objc_find_state(rt, s.obj, &k_timer_state))
                : nullptr;
        if (s.active && t != nullptr && t->valid) {
            next = std::min(next, t->fire);
        }
    }
    return next;
}

extern "C" double mt_runloop_run_once(mt_cpu *cpu, double now) noexcept {
    if (g_loop.running) {
        log(cpu, "mt_runloop_run_once inside a turn: nested runs are not supported, ignored");
        return mt_runloop_next_wake(now);
    }
    mt_objc *rt = runtime(cpu);
    g_loop.running = true;
    const u32 pool = rt != nullptr ? mt_objc_pool_push(rt) : 0;
    const std::vector<TurnSource> sources = g_turn_sources; // a source may unregister itself
    for (const TurnSource &s : sources) {
        s.fn(cpu, s.user);
    }
    std::vector<Scheduled> due;
    for (const Scheduled &s : g_loop.timers) {
        const TimerState *t = timer_of(cpu, s.obj);
        if (s.active && t != nullptr && t->valid && t->fire <= now) {
            due.push_back(s);
        }
    }
    std::stable_sort(due.begin(), due.end(), [cpu](const Scheduled &a, const Scheduled &b) {
        const double fa = timer_of(cpu, a.obj)->fire, fb = timer_of(cpu, b.obj)->fire;
        return fa != fb ? fa < fb : a.order < b.order;
    });
    for (const Scheduled &s : due) {
        // Still scheduled? An earlier callout may have removed it.
        const bool scheduled = std::any_of(g_loop.timers.begin(), g_loop.timers.end(),
                                           [&s](const Scheduled &x) { return x.obj == s.obj; });
        if (scheduled) {
            fire(cpu, s.obj, now);
        }
    }
    mt_stubs_pump(cpu);
    mt_gcd_drain_main(cpu);
    if (rt != nullptr) {
        mt_objc_pool_pop(rt, pool);
    }
    g_loop.running = false;
    return mt_runloop_next_wake(now);
}

extern "C" double mt_runloop_next_wake(double now) noexcept {
    if (mt_stubs_pending() != 0) {
        return now;
    }
    double next = mt_runloop_next_fire_date();
    const std::uint64_t deadline = mt_gcd_next_deadline();
    if (deadline != UINT64_MAX) {
        const std::uint64_t gcd_now = mt_gcd_now();
        const double wait =
            deadline > gcd_now ? static_cast<double>(deadline - gcd_now) / 1e9 : 0.0;
        next = std::min(next, now + wait);
    }
    return next;
}

extern "C" std::uint32_t mt_runloop_main(mt_cpu *cpu) noexcept { return main_loop(cpu); }

extern "C" std::uint32_t mt_runloop_timer_count(void) noexcept {
    return static_cast<std::uint32_t>(g_loop.timers.size());
}

// --- HLE: CFAbsoluteTime, CFRunLoop -------------------------------------------------------------

// CFAbsoluteTime CFAbsoluteTimeGetCurrent(void): a double in r0:r1.
extern "C" void mt_hle_CFAbsoluteTimeGetCurrent(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_f64(cpu, mt_cf_absolute_time());
}

// CFRunLoopRef CFRunLoopGetMain(void), CFRunLoopGetCurrent(void): the one run loop.
extern "C" void mt_hle_CFRunLoopGetMain(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, main_loop(cpu));
}
extern "C" void mt_hle_CFRunLoopGetCurrent(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, main_loop(cpu));
}

// CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef, CFAbsoluteTime fireDate,
//     CFTimeInterval interval, CFOptionFlags flags, CFIndex order,
//     CFRunLoopTimerCallBack callout, CFRunLoopTimerContext *context)
// The doubles are 4-byte aligned (r1:r2, r3:[sp]), as at the game's call 0x8ED8.
extern "C" void mt_hle_CFRunLoopTimerCreate(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    (void)mt_arg_u32(&a);
    const double fire_date = mt_arg_f64(&a);
    const double interval = mt_arg_f64(&a);
    (void)mt_arg_u32(&a); // flags: unused by CF
    (void)mt_arg_u32(&a); // order: one timer, nothing to order
    const u32 callout = mt_arg_u32(&a);
    const u32 context = mt_arg_u32(&a);
    if (callout == 0) {
        misuse(cpu, "CFRunLoopTimerCreate", "NULL callout", 0);
    }
    u32 info, release;
    read_context(cpu, context, &info, &release);
    const u32 obj = create(cpu, classes(cpu).type);
    auto *t = obj != 0 ? static_cast<TimerState *>(mt_objc_state(runtime(cpu), obj, &k_timer_state))
                       : nullptr;
    if (t == nullptr) {
        mt_ret_u32(cpu, 0);
        return;
    }
    *t = TimerState{fire_date, interval > 0.0 ? interval : 0.0, callout, info, release, true};
    mt_ret_u32(cpu, obj);
}

// void CFRunLoopTimerSetNextFireDate(CFRunLoopTimerRef, CFAbsoluteTime fireDate)
extern "C" void mt_hle_CFRunLoopTimerSetNextFireDate(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 obj = mt_arg_u32(&a);
    const double fire_date = mt_arg_f64(&a);
    TimerState *t = timer_of(cpu, obj);
    if (t == nullptr) {
        misuse(cpu, "CFRunLoopTimerSetNextFireDate", "not a CFRunLoopTimer", obj);
    }
    if (t->valid) {
        t->fire = fire_date;
    }
}

// void CFRunLoopAddTimer(CFRunLoopRef, CFRunLoopTimerRef, CFStringRef mode)
extern "C" void mt_hle_CFRunLoopAddTimer(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 rl = mt_arg_u32(&a), obj = mt_arg_u32(&a), mode = mt_arg_u32(&a);
    if (rl == 0 || rl != main_loop(cpu)) {
        misuse(cpu, "CFRunLoopAddTimer", "not the main run loop", rl);
    }
    const TimerState *t = timer_of(cpu, obj);
    if (t == nullptr) {
        misuse(cpu, "CFRunLoopAddTimer", "not a CFRunLoopTimer", obj);
    }
    if (!t->valid || std::any_of(g_loop.timers.begin(), g_loop.timers.end(),
                                 [obj](const Scheduled &s) { return s.obj == obj; })) {
        return;
    }
    const bool active = runs_in_default_mode(cpu, mode);
    if (!active) {
        log(cpu,
            "CFRunLoopAddTimer: timer 0x%08X added in a mode the run loop never runs: it "
            "will not fire",
            static_cast<unsigned>(obj));
    }
    mt_objc_retain(cpu, obj);
    g_loop.timers.push_back(Scheduled{obj, active, g_loop.next_order++});
}

// void CFRunLoopAddSource(CFRunLoopRef, CFRunLoopSourceRef, CFStringRef mode): any object (the
// offline socket's source is a plain NSObject, #52); retained by the run loop, never signalled.
// The game imports no way to remove one, so it stays until the process ends.
extern "C" void mt_hle_CFRunLoopAddSource(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 rl = mt_arg_u32(&a), source = mt_arg_u32(&a);
    if (rl == 0 || rl != main_loop(cpu)) {
        misuse(cpu, "CFRunLoopAddSource", "not the main run loop", rl);
    }
    if (source == 0 || mt_objc_class_of(runtime(cpu), source) == 0) {
        misuse(cpu, "CFRunLoopAddSource", "not an object", source);
    }
    if (std::find(g_loop.sources.begin(), g_loop.sources.end(), source) == g_loop.sources.end()) {
        mt_objc_retain(cpu, source);
        g_loop.sources.push_back(source);
    }
}
