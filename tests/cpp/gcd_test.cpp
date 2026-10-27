// GCD (maplethief/gcd.h, #50, ADR 0033) on a real guest address space with the ObjC runtime's
// block classes: the 11 imports through their HLE entries, main-queue ordering, the worker
// threads, groups, timers, dispatch_once under contention and the waits that trap.
//
// Blocks are synthetic: an isa, flags, invoke = a test trampoline, a descriptor, and an action
// index at +20 that the trampoline runs (on whichever guest thread runs the block). The clock
// is virtual: a sleep of the main thread advances it to the deadline.
#include <doctest/doctest.h>

#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "maplethief/gcd.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"

#if defined(_MSC_VER)
// setjmp in C++ (C4611): no object with a destructor lives across it here (see trapped()).
#pragma warning(disable : 4611)
#endif

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// --- test doubles of the runtime's trap and dispatch ----------------------------------------

std::jmp_buf *g_trap_jmp = nullptr;
u32 g_trap[3];
std::map<u32, mt_guest_fn> g_imps;

template <typename F> bool trapped(F fn) {
    std::jmp_buf jmp;
    g_trap_jmp = &jmp;
    if (setjmp(jmp) != 0) {
        g_trap_jmp = nullptr;
        return true;
    }
    fn();
    g_trap_jmp = nullptr;
    return false;
}

} // namespace

extern "C" void mt_trap(mt_cpu *, u32 kind, u32 pc, u32 detail) noexcept {
    g_trap[0] = kind;
    g_trap[1] = pc;
    g_trap[2] = detail;
    if (g_trap_jmp != nullptr && !mt_gcd_on_worker()) {
        std::longjmp(*g_trap_jmp, 1);
    }
    std::abort();
}

extern "C" mt_guest_fn mt_dispatch(mt_cpu *, u32 target, u32) noexcept {
    const auto it = g_imps.find(target);
    if (it == g_imps.end()) {
        std::abort();
    }
    return it->second;
}

namespace {

constexpr u32 k_caller = 0x00C0FFEEu; // lr of the "guest" calls below
constexpr u64 k_ms = 1000000u;

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }

u64 g_clock = 0;
u64 fake_clock(void *) { return g_clock; }
void fake_sleep(void *, u64 deadline) {
    if (deadline > g_clock) {
        g_clock = deadline;
    }
}

// A deque: blocks made inside a running action must not move the running action.
std::deque<std::function<void(mt_cpu *)>> g_actions;
std::vector<std::string> g_trace; // what the blocks did, in order

mt_cpu *g_main_cpu = nullptr;

u32 get32(mt_cpu *cpu, u32 addr) {
    u32 v;
    std::memcpy(&v, cpu->mem + addr, 4);
    return v;
}
void put32(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(cpu->mem + addr, &v, 4); }

// Every test block's invoke: runs its action.
void trampoline(mt_cpu *cpu) {
    const u32 action = get32(cpu, cpu->r[0] + 20u);
    g_actions.at(action)(cpu);
}
constexpr u32 k_trampoline = 0x00F00001u;

// Calls an HLE function the way guest code does (arguments in r0-r3, lr = k_caller).
u32 hle(mt_cpu *cpu, mt_import_fn fn, std::initializer_list<u32> args) {
    const u32 lr = cpu->r[MT_LR];
    u32 i = 0;
    for (u32 a : args) {
        cpu->r[i++] = a;
    }
    cpu->r[MT_LR] = k_caller;
    fn(cpu, 0u);
    cpu->r[MT_LR] = lr;
    return cpu->r[0];
}
u64 hle64(mt_cpu *cpu, mt_import_fn fn, std::initializer_list<u32> args) {
    const u32 lo = hle(cpu, fn, args);
    return (static_cast<u64>(cpu->r[1]) << 32) | lo;
}
u32 lo(u64 v) { return static_cast<u32>(v); }
u32 hi(u64 v) { return static_cast<u32>(v >> 32); }

struct World {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 main_q = 0;
    u32 global_cls = 0, stack_cls = 0, descriptor = 0;

    explicit World(u32 max_workers = 0) {
        mt_gcd_reset();
        g_log.clear();
        g_trace.clear();
        g_actions.clear();
        g_clock = 5000 * k_ms;
        g_imps[k_trampoline] = trampoline;
        mt_gcd_config cfg = mt_gcd_config_default();
        cfg.clock = fake_clock;
        cfg.sleep_until = fake_sleep;
        cfg.log = capture;
        if (max_workers != 0) {
            cfg.max_workers = max_workers;
        }
        REQUIRE(mt_gcd_configure(&cfg) == 0);
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0, &stack) == 0);
        cpu.r[MT_SP] = stack.top;
        cpu.r[MT_LR] = 0x2345u;
        g_main_cpu = &cpu;
        mt_import_config icfg = mt_import_config_default();
        icfg.log = capture;
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &icfg);
        REQUIRE(rt != nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        REQUIRE(mt_objc_add_host_classes(rt) == 0u);
        global_cls = mt_objc_class_named(rt, "__NSGlobalBlock__");
        stack_cls = mt_objc_class_named(rt, "__NSStackBlock__");
        REQUIRE((global_cls != 0u && stack_cls != 0u));
        descriptor = alloc(16);
        put32(&cpu, descriptor + 4, 24u); // reserved, size; no helpers
        // _dispatch_main_q: a zeroed data object of 64 bytes, then its init hook (ADR 0019).
        main_q = alloc(64);
        mt_hle_init_dispatch_main_q(&cpu, 0u, main_q);
    }
    ~World() {
        mt_gcd_reset(); // the worker threads and their guest stacks, before the memory goes
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
        g_main_cpu = nullptr;
    }
    World(const World &) = delete;
    World &operator=(const World &) = delete;

    u32 alloc(u32 n) {
        const u32 at = mt_heap_calloc(mem, 1u, n);
        REQUIRE(at != 0u);
        return at;
    }
    u32 word(u32 addr) { return get32(&cpu, addr); }

    // A global block running `action` (never copied or freed).
    u32 block(std::function<void(mt_cpu *)> action) {
        const u32 b = alloc(24);
        put32(&cpu, b, global_cls);
        put32(&cpu, b + 4, 1u << 28); // BLOCK_IS_GLOBAL
        put32(&cpu, b + 12, k_trampoline);
        put32(&cpu, b + 16, descriptor);
        put32(&cpu, b + 20, static_cast<u32>(g_actions.size()));
        g_actions.push_back(std::move(action));
        return b;
    }
    // A block that records `name` in g_trace.
    u32 note(const std::string &name) {
        return block([name](mt_cpu *) { g_trace.push_back(name); });
    }
    // A stack block (copied by dispatch_async).
    u32 stack_block(std::function<void(mt_cpu *)> action) {
        const u32 b = block(std::move(action));
        put32(&cpu, b, stack_cls);
        put32(&cpu, b + 4, 0u);
        return b;
    }

    u32 global(int priority) {
        return hle(&cpu, mt_hle_dispatch_get_global_queue, {static_cast<u32>(priority), 0u});
    }
    u32 drain() { return mt_gcd_drain_main(&cpu); }
};

// Guest calls from inside a block, on the thread running it.
void async(mt_cpu *cpu, u32 q, u32 b) { hle(cpu, mt_hle_dispatch_async, {q, b}); }
void sync(mt_cpu *cpu, u32 q, u32 b) { hle(cpu, mt_hle_dispatch_sync, {q, b}); }
void once(mt_cpu *cpu, u32 pred, u32 b) { hle(cpu, mt_hle_dispatch_once, {pred, b}); }
u32 group_wait(mt_cpu *cpu, u32 group, u64 timeout) {
    return hle(cpu, mt_hle_dispatch_group_wait, {group, lo(timeout), hi(timeout)});
}
void after(mt_cpu *cpu, u64 when, u32 q, u32 b) {
    hle(cpu, mt_hle_dispatch_after, {lo(when), hi(when), q, b});
}
u64 dtime(mt_cpu *cpu, u64 when, std::int64_t delta) {
    const auto d = static_cast<u64>(delta);
    return hle64(cpu, mt_hle_dispatch_time, {lo(when), hi(when), lo(d), hi(d)});
}

std::string thread_of(mt_cpu *cpu) { return cpu == g_main_cpu ? "main" : "worker"; }

} // namespace

TEST_CASE("dispatch_time: libdispatch's arithmetic on a monotonic nanosecond clock") {
    World w;
    const u64 now = g_clock;
    CHECK(mt_gcd_now() == now);
    CHECK(dtime(&w.cpu, MT_DISPATCH_TIME_NOW, 0) == now);
    CHECK(dtime(&w.cpu, MT_DISPATCH_TIME_NOW, 30 * 1000000000ll) == now + 30000000000ull);
    CHECK(dtime(&w.cpu, 1000u, 500) == 1500u);
    CHECK(dtime(&w.cpu, 1000u, -400) == 600u);
    CHECK(dtime(&w.cpu, 1000u, -5000) == 1u); // never 0 (that is NOW)
    CHECK(dtime(&w.cpu, MT_DISPATCH_TIME_FOREVER, -5) == MT_DISPATCH_TIME_FOREVER);
    CHECK(dtime(&w.cpu, 0x7FFFFFFFFFFFFFF0ull, 0x100) == MT_DISPATCH_TIME_FOREVER); // overflow
    // A wall-clock time (negative, dispatch_walltime's) moves towards -1.
    const u64 wall = static_cast<u64>(-2000000000ll);
    CHECK(dtime(&w.cpu, wall, 1000) == static_cast<u64>(-2000001000ll));
    CHECK(dtime(&w.cpu, wall, -1999999999ll) == 1u);
}

TEST_CASE("dispatch_get_global_queue and the main queue; dispatch_release of queues") {
    World w;
    const u32 high = w.global(MT_DISPATCH_QUEUE_PRIORITY_HIGH);
    const u32 def = w.global(MT_DISPATCH_QUEUE_PRIORITY_DEFAULT);
    const u32 low = w.global(MT_DISPATCH_QUEUE_PRIORITY_LOW);
    const u32 bg = w.global(MT_DISPATCH_QUEUE_PRIORITY_BACKGROUND);
    CHECK((high != 0u && def != 0u && low != 0u && bg != 0u));
    CHECK((high != def && def != low && low != bg && high != w.main_q));
    CHECK(w.global(MT_DISPATCH_QUEUE_PRIORITY_LOW) == low); // one object per priority
    CHECK(hle(&w.cpu, mt_hle_dispatch_get_global_queue, {0u, 2u}) == def); // overcommit flag
    CHECK(hle(&w.cpu, mt_hle_dispatch_get_global_queue, {1u, 0u}) == 0u);  // no such priority
    CHECK(hle(&w.cpu, mt_hle_dispatch_get_global_queue, {0u, 1u}) == 0u);  // unknown flag
    CHECK(w.word(w.main_q) == 0x51444347u); // the init hook tagged _dispatch_main_q
    // Queues are never freed.
    hle(&w.cpu, mt_hle_dispatch_release, {low});
    hle(&w.cpu, mt_hle_dispatch_release, {w.main_q});
    CHECK(mt_heap_usable_size(w.mem, low) != 0u);
    // Anything else is no dispatch object, and no queue.
    CHECK(trapped([&] { hle(&w.cpu, mt_hle_dispatch_release, {w.alloc(16)}); }));
    CHECK(g_trap[0] == MT_TRAP_GCD);
    CHECK(g_trap[1] == k_caller);
    CHECK(trapped([&] { async(&w.cpu, 0x1234u, w.note("x")); }));
    CHECK(g_trap[2] == 0x1234u);
}

TEST_CASE("main queue: FIFO, one drain per turn, blocks queued during a drain wait a turn") {
    World w;
    CHECK(mt_gcd_next_deadline() == UINT64_MAX);
    const u32 d = w.note("D");
    async(&w.cpu, w.main_q, w.note("A"));
    async(&w.cpu, w.main_q, w.block([&](mt_cpu *cpu) {
        g_trace.push_back("B on " + thread_of(cpu));
        async(cpu, w.main_q, d); // queued during the drain
    }));
    async(&w.cpu, w.main_q, w.note("C"));
    CHECK(mt_gcd_main_pending() == 3u);
    CHECK(mt_gcd_next_deadline() == g_clock);
    CHECK(g_trace.empty()); // nothing runs inside the call
    CHECK(w.drain() == 3u);
    CHECK(g_trace == std::vector<std::string>{"A", "B on main", "C"});
    CHECK(mt_gcd_main_pending() == 1u);
    CHECK(w.drain() == 1u);
    CHECK(g_trace.back() == "D");
    CHECK(w.drain() == 0u);
    mt_gcd_stats s;
    mt_gcd_get_stats(&s);
    CHECK(s.main_blocks == 4u);
    CHECK(s.workers == 0u);
}

TEST_CASE("main-queue ordering across dispatch_async, groups, dispatch_after and workers") {
    World w;
    const u32 global = w.global(MT_DISPATCH_QUEUE_PRIORITY_DEFAULT);
    const u32 group = hle(&w.cpu, mt_hle_dispatch_group_create, {});
    REQUIRE(group != 0u);
    const u64 now = g_clock;
    after(&w.cpu, now + 20 * k_ms, w.main_q, w.note("X +20ms"));
    after(&w.cpu, now + 10 * k_ms, w.main_q, w.note("Y +10ms"));
    after(&w.cpu, now + 10 * k_ms, w.main_q, w.note("Z +10ms")); // same deadline: after Y
    after(&w.cpu, MT_DISPATCH_TIME_FOREVER, w.main_q, w.note("never"));
    after(&w.cpu, MT_DISPATCH_TIME_NOW, w.main_q, w.note("N now")); // due: queued at once
    async(&w.cpu, w.main_q, w.note("M1"));
    hle(&w.cpu, mt_hle_dispatch_group_async, {group, w.main_q, w.note("M2 group")});
    async(&w.cpu, global, w.block([&](mt_cpu *cpu) {
        g_trace.push_back("W on " + thread_of(cpu));
        async(cpu, w.main_q, w.note("W1 from worker"));
    }));
    CHECK(mt_gcd_next_deadline() == g_clock); // work is waiting

    // Before the timers are due: the queued blocks and what the worker queued.
    CHECK(w.drain() == 4u);
    CHECK(g_trace ==
          std::vector<std::string>{"W on worker", "N now", "M1", "M2 group", "W1 from worker"});
    CHECK(group_wait(&w.cpu, group, MT_DISPATCH_TIME_NOW) == 0u); // M2 left the group
    CHECK(mt_gcd_next_deadline() == now + 10 * k_ms);
    g_trace.clear();
    g_clock = now + 25 * k_ms;
    CHECK(w.drain() == 3u);
    CHECK(g_trace == std::vector<std::string>{"Y +10ms", "Z +10ms", "X +20ms"});
    CHECK(mt_gcd_next_deadline() == UINT64_MAX); // FOREVER never fires
    mt_gcd_stats s;
    mt_gcd_get_stats(&s);
    CHECK(s.timers_fired == 3u);
    CHECK(s.worker_blocks == 1u);
    hle(&w.cpu, mt_hle_dispatch_release, {group});
    CHECK(mt_heap_usable_size(w.mem, group) == 0u); // the last reference
}

TEST_CASE("dispatch_async copies a stack block and releases the copy after it ran") {
    World w;
    u32 ran_as = 0;
    const u32 b = w.stack_block([&](mt_cpu *cpu) { ran_as = cpu->r[0]; });
    async(&w.cpu, w.main_q, b);
    CHECK(w.drain() == 1u);
    REQUIRE(ran_as != 0u);
    CHECK(ran_as != b);                                   // the heap copy ran
    CHECK(mt_heap_usable_size(w.mem, ran_as) == 0u);      // and was freed afterwards
    CHECK(trapped([&] { async(&w.cpu, w.main_q, 0u); })); // no block
}

TEST_CASE("global queues run on worker threads with their own guest stacks, by priority") {
    World w(1); // one worker: the blocks run one after another, in scheduling order
    struct Seen {
        mt_cpu *cpu;
        u32 sp;
        int worker;
    };
    std::vector<Seen> seen;
    auto record = [&](const std::string &name) {
        return w.block([&seen, name](mt_cpu *cpu) {
            g_trace.push_back(name);
            seen.push_back(Seen{cpu, cpu->r[MT_SP], mt_gcd_on_worker()});
        });
    };
    async(&w.cpu, w.global(MT_DISPATCH_QUEUE_PRIORITY_BACKGROUND), record("background"));
    async(&w.cpu, w.global(MT_DISPATCH_QUEUE_PRIORITY_LOW), record("low"));
    async(&w.cpu, w.global(MT_DISPATCH_QUEUE_PRIORITY_DEFAULT), record("default 1"));
    async(&w.cpu, w.global(MT_DISPATCH_QUEUE_PRIORITY_HIGH), record("high"));
    async(&w.cpu, w.global(MT_DISPATCH_QUEUE_PRIORITY_DEFAULT), record("default 2"));
    CHECK(g_trace.empty()); // global work runs when the main thread drains
    CHECK(w.drain() == 0u);
    CHECK(g_trace ==
          std::vector<std::string>{"high", "default 1", "default 2", "low", "background"});
    REQUIRE(seen.size() == 5u);
    for (const Seen &s : seen) {
        CHECK(s.cpu != &w.cpu);
        CHECK(s.cpu == seen[0].cpu); // the one worker
        CHECK(s.worker == 1);
        CHECK(mt_gcd_is_worker(s.cpu) == 1);
        CHECK(s.sp != 0u);
        CHECK(mt_mem_describe(w.mem, s.sp - 4u, nullptr, 0) == MT_REGION_STACK); // a guest stack
        CHECK((s.sp > w.stack.top || s.sp < w.stack.base));                      // not main's
    }
    CHECK(mt_gcd_is_worker(&w.cpu) == 0);
    CHECK(mt_gcd_on_worker() == 0);

    // dispatch_sync to a global queue runs on the calling thread.
    seen.clear();
    sync(&w.cpu, w.global(0), record("sync from main"));
    REQUIRE(seen.size() == 1u);
    CHECK(seen[0].cpu == &w.cpu);
    mt_gcd_stats s;
    mt_gcd_get_stats(&s);
    CHECK(s.workers == 1u);
    CHECK(s.worker_blocks == 5u);
}

TEST_CASE("groups: dispatch_group_wait on the main thread and on a worker (Chartboost's waiter)") {
    World w;
    const u32 low = w.global(MT_DISPATCH_QUEUE_PRIORITY_LOW);
    // -[CBNativeInterstitialView prepareWithResponse:]: image loads into a group, a waiter on
    // the LOW queue that waits for them, releases the group and goes back to the main queue.
    const u32 group = hle(&w.cpu, mt_hle_dispatch_group_create, {});
    for (int i = 0; i < 3; i++) {
        hle(&w.cpu, mt_hle_dispatch_group_async, {group, low, w.note("load " + std::to_string(i))});
    }
    CHECK(group_wait(&w.cpu, group, MT_DISPATCH_TIME_NOW) == MT_DISPATCH_TIMED_OUT);
    u32 waited = 99;
    async(&w.cpu, low, w.block([&](mt_cpu *cpu) {
        waited = group_wait(cpu, group, MT_DISPATCH_TIME_FOREVER);
        g_trace.push_back("waiter");
        hle(cpu, mt_hle_dispatch_release, {group});
        async(cpu, w.main_q, w.note("done on main"));
    }));
    CHECK(w.drain() == 1u);
    CHECK(waited == 0u);
    CHECK(g_trace ==
          std::vector<std::string>{"load 0", "load 1", "load 2", "waiter", "done on main"});
    CHECK(mt_heap_usable_size(w.mem, group) == 0u);

    // The main thread waits: the workers run until the group is empty.
    g_trace.clear();
    const u32 g2 = hle(&w.cpu, mt_hle_dispatch_group_create, {});
    hle(&w.cpu, mt_hle_dispatch_group_async, {g2, low, w.note("a")});
    hle(&w.cpu, mt_hle_dispatch_group_async, {g2, w.global(2), w.note("b")});
    CHECK(group_wait(&w.cpu, g2, dtime(&w.cpu, 0, 1000 * 1000000ll)) == 0u);
    CHECK(g_trace == std::vector<std::string>{"b", "a"}); // HIGH first

    // A timeout: the group's block waits for the main thread, which is waiting for it.
    g_trace.clear();
    hle(&w.cpu, mt_hle_dispatch_group_async, {g2, low, w.block([&](mt_cpu *cpu) {
                                                  sync(cpu, w.main_q, w.note("sync on main"));
                                                  g_trace.push_back("x done");
                                              })});
    const u64 before = g_clock;
    CHECK(group_wait(&w.cpu, g2, dtime(&w.cpu, 0, 5 * 1000000ll)) == MT_DISPATCH_TIMED_OUT);
    CHECK(g_clock == before + 5 * k_ms); // it slept until the deadline
    CHECK(g_trace.empty());
    CHECK(w.drain() == 1u); // runs the sync block; the worker finishes in the same turn
    CHECK(g_trace == std::vector<std::string>{"sync on main", "x done"});
    CHECK(group_wait(&w.cpu, g2, MT_DISPATCH_TIME_FOREVER) == 0u);
    CHECK(trapped([&] { group_wait(&w.cpu, 0x5000u, MT_DISPATCH_TIME_NOW); }));
    CHECK(trapped([&] { hle(&w.cpu, mt_hle_dispatch_group_async, {0x5000u, low, w.note("x")}); }));
}

TEST_CASE("a worker in a timed wait wakes at the first drain after its deadline") {
    World w(1); // the waiter holds the only worker: the group's block cannot run
    const u32 low = w.global(MT_DISPATCH_QUEUE_PRIORITY_LOW);
    const u32 group = hle(&w.cpu, mt_hle_dispatch_group_create, {});
    u32 result = 99;
    async(&w.cpu, low, w.block([&](mt_cpu *cpu) {
        result = group_wait(cpu, group, dtime(cpu, 0, 10 * 1000000ll));
        g_trace.push_back("waiter woke");
    }));
    hle(&w.cpu, mt_hle_dispatch_group_async, {group, low, w.note("member")});
    const u64 deadline = g_clock + 10 * k_ms;
    CHECK(w.drain() == 0u);
    CHECK(g_trace.empty());
    CHECK(mt_gcd_next_deadline() == g_clock); // the member waits for a worker
    g_clock = deadline - 1;
    CHECK(w.drain() == 0u);
    CHECK(g_trace.empty());
    g_clock = deadline;
    CHECK(w.drain() == 0u);
    CHECK(result == MT_DISPATCH_TIMED_OUT);
    CHECK(g_trace == std::vector<std::string>{"waiter woke", "member"});
    CHECK(std::any_of(g_log.begin(), g_log.end(), [](const std::string &l) {
        return l.find("every worker thread is busy") != std::string::npos;
    }));
}

TEST_CASE("dispatch_sync to the main queue from a worker runs at the next drain") {
    World w;
    async(&w.cpu, w.main_q, w.note("queued before"));
    async(&w.cpu, w.global(0), w.block([&](mt_cpu *cpu) {
        g_trace.push_back("worker start");
        sync(cpu, w.main_q,
             w.block([](mt_cpu *c) { g_trace.push_back("sync on " + thread_of(c)); }));
        g_trace.push_back("worker after sync");
    }));
    CHECK(w.drain() == 2u);
    CHECK(g_trace == std::vector<std::string>{"worker start", "queued before", "sync on main",
                                              "worker after sync"});
}

TEST_CASE("dispatch_once: libdispatch's predicate protocol") {
    World w;
    const u32 pred = w.alloc(4);
    int runs = 0;
    u32 during = 0;
    const u32 b = w.block([&](mt_cpu *) {
        runs++;
        during = w.word(pred);
    });
    once(&w.cpu, pred, b);
    CHECK(runs == 1);
    CHECK(during == MT_DISPATCH_ONCE_RUNNING);
    CHECK(w.word(pred) == MT_DISPATCH_ONCE_DONE); // the guest's inline check: cmp #-1
    once(&w.cpu, pred, b);
    CHECK(runs == 1);
    // A predicate in no known state: nobody will ever finish it.
    const u32 bad = w.alloc(4);
    put32(&w.cpu, bad, 7u);
    CHECK(trapped([&] { once(&w.cpu, bad, b); }));
    CHECK(g_trap[0] == MT_TRAP_GCD);
    CHECK(g_trap[2] == bad);
}

TEST_CASE("dispatch_once under contention: workers wait for the running block") {
    World w(6);
    const u32 pred = w.alloc(4);
    int init_runs = 0, wrong_runs = 0;
    u32 seen_in_m = 0;
    // The initializer blocks half-way: it waits for a main-queue block.
    const u32 m = w.block([&](mt_cpu *) {
        seen_in_m = w.word(pred);
        g_trace.push_back("M on main");
    });
    const u32 init = w.block([&](mt_cpu *cpu) {
        init_runs++;
        g_trace.push_back("init start");
        sync(cpu, w.main_q, m);
        g_trace.push_back("init end");
    });
    const u32 wrong = w.block([&](mt_cpu *) { wrong_runs++; });
    const u32 global = w.global(0);
    async(&w.cpu, global, w.block([&](mt_cpu *cpu) {
        once(cpu, pred, init);
        g_trace.push_back("A returned");
    }));
    std::vector<u32> after_once;
    for (int i = 0; i < 4; i++) {
        async(&w.cpu, global, w.block([&, i](mt_cpu *cpu) {
            g_trace.push_back("B" + std::to_string(i) + " calls");
            once(cpu, pred, wrong);
            after_once.push_back(w.word(pred));
            g_trace.push_back("B" + std::to_string(i) + " returned");
        }));
    }
    CHECK(w.drain() == 1u);
    CHECK(init_runs == 1);
    CHECK(wrong_runs == 0);
    CHECK(seen_in_m == MT_DISPATCH_ONCE_RUNNING);
    CHECK(w.word(pred) == MT_DISPATCH_ONCE_DONE);
    REQUIRE(after_once.size() == 4u);
    for (u32 v : after_once) {
        CHECK(v == MT_DISPATCH_ONCE_DONE);
    }
    CHECK(g_trace == std::vector<std::string>{"init start", "B0 calls", "B1 calls", "B2 calls",
                                              "B3 calls", "M on main", "init end", "A returned",
                                              "B0 returned", "B1 returned", "B2 returned",
                                              "B3 returned"});
    mt_gcd_stats s;
    mt_gcd_get_stats(&s);
    CHECK(s.workers == 5u);
    CHECK(s.worker_blocks == 5u);
}

TEST_CASE("dispatch_once under contention: the main thread waits for a worker's block") {
    World w;
    const u32 pred = w.alloc(4);
    const u32 low = w.global(MT_DISPATCH_QUEUE_PRIORITY_LOW);
    const u32 group = hle(&w.cpu, mt_hle_dispatch_group_create, {});
    int init_runs = 0, wrong_runs = 0;
    // X (in the group) waits for the main thread; the initializer waits for the group, 10 ms.
    hle(&w.cpu, mt_hle_dispatch_group_async, {group, low, w.block([&](mt_cpu *cpu) {
                                                  sync(cpu, w.main_q, w.note("X's sync"));
                                                  g_trace.push_back("X done");
                                              })});
    const u32 init = w.block([&](mt_cpu *cpu) {
        init_runs++;
        g_trace.push_back("init waits " +
                          std::to_string(group_wait(cpu, group, dtime(cpu, 0, 10 * 1000000ll))));
    });
    async(&w.cpu, low, w.block([&](mt_cpu *cpu) { once(cpu, pred, init); }));
    // On the main queue, before X's sync block: the main thread contends.
    const u32 wrong = w.block([&](mt_cpu *) { wrong_runs++; });
    u64 waited = 0;
    async(&w.cpu, w.main_q, w.block([&](mt_cpu *cpu) {
        const u64 t0 = g_clock;
        once(cpu, pred, wrong);
        waited = g_clock - t0;
        g_trace.push_back("main returned, predicate " +
                          std::string(w.word(pred) == MT_DISPATCH_ONCE_DONE ? "done" : "not done"));
    }));
    CHECK(w.drain() == 2u);
    CHECK(init_runs == 1);
    CHECK(wrong_runs == 0);
    CHECK(waited == 10 * k_ms); // the main thread slept until the initializer's timeout
    CHECK(g_trace == std::vector<std::string>{"init waits 49", "main returned, predicate done",
                                              "X's sync", "X done"});
}

TEST_CASE("waits that can never end trap with MT_TRAP_GCD") {
    SUBCASE("dispatch_sync to the main queue from the main thread") {
        World w;
        CHECK(trapped([&] { sync(&w.cpu, w.main_q, w.note("never")); }));
        CHECK(g_trap[0] == MT_TRAP_GCD);
        CHECK(g_trap[1] == k_caller);
        CHECK(g_trap[2] == w.main_q);
        CHECK(g_trace.empty());
    }
    SUBCASE("dispatch_once from inside its own block") {
        World w;
        const u32 pred = w.alloc(4);
        u32 self = 0;
        self = w.block([&](mt_cpu *cpu) { once(cpu, pred, self); });
        CHECK(trapped([&] { once(&w.cpu, pred, self); }));
        CHECK(g_trap[0] == MT_TRAP_GCD);
        CHECK(g_trap[2] == pred);
    }
    SUBCASE("the main thread waits for a group whose block waits for the main thread") {
        World w;
        const u32 group = hle(&w.cpu, mt_hle_dispatch_group_create, {});
        hle(&w.cpu, mt_hle_dispatch_group_async,
            {group, w.global(0), w.block([&](mt_cpu *cpu) { sync(cpu, w.main_q, w.note("s")); })});
        CHECK(trapped([&] { group_wait(&w.cpu, group, MT_DISPATCH_TIME_FOREVER); }));
        CHECK(g_trap[0] == MT_TRAP_GCD);
        CHECK(g_trap[2] == group);
        CHECK(g_log.back().find("deadlock") != std::string::npos);
        // The blocked worker is abandoned by mt_gcd_reset (the World's destructor).
    }
}

TEST_CASE("autorelease pools are per guest thread") {
    World w;
    const u32 nsobject = mt_objc_class_named(w.rt, "NSObject");
    const u32 obj = mt_objc_send0(&w.cpu, nsobject, "new");
    REQUIRE(obj != 0u);
    // The worker autoreleases the object, then blocks until the next drain.
    async(&w.cpu, w.global(0), w.block([&](mt_cpu *cpu) {
        mt_objc_autorelease(cpu, obj);
        sync(cpu, w.main_q, w.note("sync"));
        g_trace.push_back(mt_heap_usable_size(w.mem, obj) != 0u ? "alive after sync" : "freed");
    }));
    // A main-thread pool around the first drain (a run-loop turn's): popping it must not
    // release what the blocked worker autoreleased into its own pool.
    const u32 token = mt_objc_pool_push(w.rt);
    CHECK(w.drain() == 1u); // the worker blocks; then the sync block runs, the worker resumes
    mt_objc_pool_pop(w.rt, token);
    CHECK(g_trace == std::vector<std::string>{"sync", "alive after sync"});
    CHECK(mt_heap_usable_size(w.mem, obj) == 0u); // the worker's pool released it after its block
}

TEST_CASE("mt_gcd_reset abandons blocked workers; configuration only without workers") {
    World w;
    const u32 group = hle(&w.cpu, mt_hle_dispatch_group_create, {});
    hle(&w.cpu, mt_hle_dispatch_group_async, {group, w.global(0), w.block([&](mt_cpu *cpu) {
                                                  g_trace.push_back("blocks");
                                                  sync(cpu, w.main_q, w.note("s"));
                                                  g_trace.push_back("unreachable");
                                              })});
    // The main thread gives up after 1 ms: the worker stays blocked in dispatch_sync.
    CHECK(group_wait(&w.cpu, group, dtime(&w.cpu, 0, 1000000ll)) == MT_DISPATCH_TIMED_OUT);
    CHECK(g_trace == std::vector<std::string>{"blocks"});
    CHECK(mt_gcd_main_pending() == 1u);
    mt_gcd_config cfg = mt_gcd_config_default();
    CHECK(mt_gcd_configure(&cfg) == -1); // a worker exists
    mt_gcd_reset();
    CHECK(g_trace == std::vector<std::string>{"blocks"});
    CHECK(mt_gcd_main_pending() == 0u);
    mt_gcd_stats s;
    mt_gcd_get_stats(&s);
    CHECK(s.workers == 0u);
    CHECK(mt_gcd_configure(nullptr) == 0);
}
