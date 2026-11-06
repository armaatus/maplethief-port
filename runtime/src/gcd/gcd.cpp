// Grand Central Dispatch (maplethief/gcd.h, #50, ADR 0033): the main queue, the global queues
// on worker threads, timers, groups and dispatch_once.
//
// Threads and the baton. Every guest thread is a host thread: the main thread, and one host
// thread per worker (an 8 MiB host stack for the generated code's recursion, a guest stack, an
// mt_cpu). G.running names the one guest thread that may run; everything else in G is only
// touched by that thread, so it needs no lock of its own. Only the main thread hands the baton
// out (handoff, from the scheduler run_pool); a worker hands it back when its block has run or
// when it blocks (worker_block). The scheduler is deterministic: it wakes the first blocked
// worker whose wait is over, else gives the oldest block of the highest-priority global queue
// to an idle worker (making one if there are fewer than max_workers).
//
// Per-thread runtime state lives in the runtime as "the current thread's" (the SjLj unwinder
// state, the ObjC autorelease pools); handoff exchanges it with the worker's saved copy before
// and after the worker runs.
//
// No C++ object with a destructor is alive in any function here across a call that can leave
// by longjmp (guest code, mt_trap, the end of the run, a worker abandoned by mt_gcd_reset):
// locks are taken and released by hand, and the state is global.
#include "maplethief/gcd.h"

#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "maplethief/boot.h"
#include "maplethief/diag.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/sjlj.h"

#if defined(_MSC_VER)
// setjmp in C++ (C4611): no object with a destructor lives across it (see worker_main).
#pragma warning(disable : 4611)
#endif

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

constexpr u64 k_forever = MT_DISPATCH_TIME_FOREVER;
constexpr u32 k_tag_queue = 0x51444347u; // "GCDQ": the first word of our queue objects
constexpr u32 k_tag_group = 0x47444347u; // "GCDG": ... and of groups
constexpr u32 k_object_size = 64u;       // guest bytes of a queue or group object
constexpr int k_main = 0;                // the main thread's id; workers are 1, 2, ...
constexpr int k_queue_main = 4;          // queue index of the main queue; 0-3 are global
constexpr int k_globals = 4;             // HIGH, DEFAULT, LOW, BACKGROUND: scheduling order
constexpr std::size_t k_host_stack = 8u << 20;

// One queued block. POD: it is copied in and out of the queues.
struct Item {
    u32 block;
    u32 group;       // 0, or the group it leaves when done (dispatch_group_async)
    u32 copied;      // 1: our _Block_copy, released when done
    int sync_worker; // 0, or the worker waiting for it (dispatch_sync to the main queue)
};

struct Timer {
    u64 when;
    u64 seq;
    int queue; // queue index
    u32 block; // copied
};

struct Group {
    u32 refs;
    u32 pending; // blocks of dispatch_group_async not finished yet
};

enum class State { idle, running, blocked, ended };
enum class Wait { none, group, sync, once };

#if defined(_WIN32)
using thread_handle = HANDLE;
#else
using thread_handle = pthread_t;
#endif

struct Worker {
    int id;
    mt_cpu cpu;
    mt_stack stack;
    mt_sjlj_state *sjlj;  // its unwinder state while another thread runs
    mt_objc_pools *pools; // its autorelease pools while another thread runs
    State state;
    Item item;
    bool has_item;
    Wait wait;
    u32 wait_obj;
    u64 deadline;
    bool sync_done;
    bool timed_out;
    std::jmp_buf *exit_jmp; // worker_main's: where an abandoned worker leaves its stack
    thread_handle thread;
};

// What a main-thread wait waits for.
struct Cond {
    Wait kind;
    u32 obj;
};

enum class Outcome { met, timeout, quiet };

struct Gcd {
    std::mutex m;
    std::condition_variable_any cv;
    int running = k_main;  // under m
    bool shutdown = false; // under m

    mt_gcd_config cfg = mt_gcd_config_default();
    bool end_requested = false;
    std::uint8_t *base = nullptr; // host address of guest 0
    mt_mem *mem = nullptr;
    u32 main_q = 0;
    u32 global_q[k_globals] = {};
    std::deque<Item> main_fifo;
    std::deque<Item> global_fifo[k_globals];
    std::vector<Timer> timers; // by (when, seq)
    u64 timer_seq = 0;
    std::map<u32, Group> groups;
    std::map<u32, int> once_owner; // running predicates -> the thread running the block
    std::vector<Worker *> workers;
    mt_sjlj_state *sjlj_tmp = nullptr;
    mt_gcd_stats stats{};
    bool warned_full = false;
};

// Never destroyed: worker threads may still wait on its condition variable at exit.
Gcd &G = *new Gcd;

thread_local Worker *t_worker = nullptr;

// --- small helpers -------------------------------------------------------------------------

void log_line(const char *fmt, ...) {
    char line[400] = "gcd: ";
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + 5, sizeof line - 5u, fmt, ap);
    va_end(ap);
    if (G.cfg.log != nullptr) {
        G.cfg.log(G.cfg.log_user, line);
        return;
    }
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

u64 host_clock(void *) {
    static const auto origin = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - origin)
                        .count();
    // Positive and well away from 0 (DISPATCH_TIME_NOW), like mach_absolute_time.
    return static_cast<u64>(ns) + 1000000000u;
}

void host_sleep(void *, u64 deadline) {
    for (u64 now = host_clock(nullptr); now < deadline; now = host_clock(nullptr)) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(deadline - now));
    }
}

u64 now() { return G.cfg.clock != nullptr ? G.cfg.clock(G.cfg.clock_user) : host_clock(nullptr); }

void sleep_until(u64 deadline) {
    if (G.cfg.sleep_until != nullptr) {
        G.cfg.sleep_until(G.cfg.sleep_user, deadline);
    } else {
        host_sleep(nullptr, deadline);
    }
}

int me() { return t_worker != nullptr ? t_worker->id : k_main; }

void ensure(mt_cpu *cpu) {
    if (G.base == nullptr) {
        G.base = cpu->mem;
        G.mem = mt_mem_from_cpu(cpu);
    }
}

u32 guest_word(u32 addr) {
    u32 v;
    std::memcpy(&v, G.base + addr, 4);
    return v;
}

// Stops the guest: a wait that can never end, or something that is no GCD object.
[[noreturn]] void gcd_trap(mt_cpu *cpu, u32 detail) {
    mt_trap(cpu, MT_TRAP_GCD, cpu->r[MT_LR], detail);
}

// The queue index of `q` (0-3 global, k_queue_main), or -1.
int queue_index(u32 q) {
    if (q == 0) {
        return -1;
    }
    if (q == G.main_q) {
        return k_queue_main;
    }
    for (int i = 0; i < k_globals; i++) {
        if (G.global_q[i] == q) {
            return i;
        }
    }
    return -1;
}

int checked_queue(mt_cpu *cpu, u32 q, const char *what) {
    const int qi = queue_index(q);
    if (qi < 0) {
        log_line("%s from 0x%08X: 0x%08X is not a dispatch queue (the main queue or a global one)",
                 what, cpu->r[MT_LR], q);
        gcd_trap(cpu, q);
    }
    return qi;
}

Group *group_of(u32 addr) {
    const auto it = G.groups.find(addr);
    return it != G.groups.end() ? &it->second : nullptr;
}

u32 new_object(u32 tag, u32 extra) {
    if (G.mem == nullptr) {
        log_line("no guest address space to create a dispatch object in");
        return 0;
    }
    const u32 at = mt_heap_calloc(G.mem, 1u, k_object_size);
    if (at != 0) {
        std::memcpy(G.base + at, &tag, 4);
        std::memcpy(G.base + at + 4u, &extra, 4);
    }
    return at;
}

void group_release(u32 addr) {
    Group *gr = group_of(addr);
    if (gr == nullptr || --gr->refs != 0) {
        return;
    }
    G.groups.erase(addr);
    if (G.mem != nullptr) {
        mt_heap_free(G.mem, addr);
    }
}

void enqueue(int qi, const Item &it) {
    if (qi == k_queue_main) {
        G.main_fifo.push_back(it);
    } else {
        G.global_fifo[qi].push_back(it);
    }
}

// Runs a block on the calling guest thread, then does what its queue entry asks for.
void run_item(mt_cpu *cpu, Item it) {
    mt_block_invoke(cpu, it.block, nullptr, 0u);
    if (it.copied != 0) {
        mt_block_release(cpu, it.block);
    }
    if (it.group != 0) {
        Group *gr = group_of(it.group);
        if (gr != nullptr && gr->pending != 0) {
            gr->pending--;
        }
        group_release(it.group);
    }
    if (it.sync_worker > 0 && static_cast<std::size_t>(it.sync_worker) <= G.workers.size()) {
        G.workers[static_cast<std::size_t>(it.sync_worker) - 1u]->sync_done = true;
    }
}

// Moves the timers that are due to their queues, earliest first.
void fire_timers() {
    if (G.timers.empty()) {
        return;
    }
    const u64 t = now();
    std::size_t n = 0;
    while (n < G.timers.size() && G.timers[n].when <= t) {
        enqueue(G.timers[n].queue, Item{G.timers[n].block, 0u, 1u, 0});
        G.stats.timers_fired++;
        n++;
    }
    G.timers.erase(G.timers.begin(), G.timers.begin() + static_cast<std::ptrdiff_t>(n));
}

void add_timer(u64 when, int qi, u32 block) {
    const Timer t{when, G.timer_seq++, qi, block};
    auto at = G.timers.begin();
    while (at != G.timers.end() && at->when <= when) {
        ++at;
    }
    G.timers.insert(at, t);
}

bool cond_met(Wait kind, u32 obj, const Worker *w) {
    switch (kind) {
    case Wait::group: {
        const Group *gr = group_of(obj);
        return gr == nullptr || gr->pending == 0;
    }
    case Wait::once:
        return guest_word(obj) == MT_DISPATCH_ONCE_DONE;
    case Wait::sync:
        return w != nullptr && w->sync_done;
    case Wait::none:
        break;
    }
    return true;
}

u64 dispatch_time_of(u64 when, i64 delta) {
    if (when == k_forever) {
        return k_forever;
    }
    if (static_cast<i64>(when) < 0) {
        // A wall-clock time (dispatch_walltime), libdispatch's time.c arithmetic.
        const u64 r = when - static_cast<u64>(delta);
        if (delta >= 0) {
            return static_cast<i64>(r) >= 0 ? k_forever : r;
        }
        return static_cast<i64>(r) >= -1 ? 1u : r;
    }
    if (when == 0) {
        when = now();
    }
    const u64 r = when + static_cast<u64>(delta);
    if (delta >= 0) {
        return static_cast<i64>(r) <= 0 ? k_forever : r;
    }
    return static_cast<i64>(r) < 1 ? 1u : r;
}

// A dispatch_time_t as a deadline on our clock.
u64 deadline_of(u64 t) {
    if (t == MT_DISPATCH_TIME_NOW) {
        return now();
    }
    if (t == k_forever || static_cast<i64>(t) >= 0) {
        return t;
    }
    // Wall clock: nanoseconds since the epoch, negated.
    const u64 wall = static_cast<u64>(-static_cast<i64>(t));
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    const u64 wall_now =
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(since).count());
    return now() + (wall > wall_now ? wall - wall_now : 0u);
}

// --- per-thread runtime state --------------------------------------------------------------

// Exchanges the runtime's current per-thread state with worker w's (twice restores).
void swap_state(Worker *w) {
    mt_sjlj_state *cur = mt_sjlj_state_of(&w->cpu);
    std::memcpy(G.sjlj_tmp, cur, sizeof *cur);
    std::memcpy(cur, w->sjlj, sizeof *cur);
    std::memcpy(w->sjlj, G.sjlj_tmp, sizeof *cur);
    mt_objc *rt = mt_objc_current();
    if (rt != nullptr) {
        mt_objc_pools_swap(rt, w->pools);
    }
}

// --- the baton -----------------------------------------------------------------------------

// On a worker: waits for the baton; false on shutdown.
bool wait_baton(Worker *w) {
    G.m.lock();
    while (G.running != w->id && !G.shutdown) {
        G.cv.wait(G.m);
    }
    const bool shutdown = G.shutdown;
    G.m.unlock();
    return !shutdown;
}

// On a worker: hands the baton back to the main thread.
void give_back() {
    G.m.lock();
    G.running = k_main;
    G.cv.notify_all();
    G.m.unlock();
}

// On the main thread: lets worker w run until it hands the baton back.
void handoff(Worker *w) {
    swap_state(w);
    G.stats.handoffs++;
    w->state = State::running;
    G.m.lock();
    G.running = w->id;
    G.cv.notify_all();
    while (G.running != k_main) {
        G.cv.wait(G.m);
    }
    G.m.unlock();
    swap_state(w);
    if (G.end_requested) {
        mt_process_end_recorded(); // the worker ended the run: end it here
    }
}

// On a worker, inside guest code: blocks until the scheduler wakes it (the wait is over or
// timed out); returns whether it timed out. Abandoned (mt_gcd_reset): leaves the thread.
bool worker_block(Worker *w, Wait kind, u32 obj, u64 deadline) {
    w->wait = kind;
    w->wait_obj = obj;
    w->deadline = deadline;
    w->timed_out = false;
    w->state = State::blocked;
    give_back();
    if (!wait_baton(w)) {
        MT_HOST_LONGJMP(*w->exit_jmp, 1);
    }
    w->wait = Wait::none;
    return w->timed_out;
}

// --- worker threads ------------------------------------------------------------------------

void run_global_item(Worker *w, Item it) {
    // libdispatch's worker threads run each block inside an autorelease pool.
    mt_objc *rt = mt_objc_current();
    const u32 token = rt != nullptr ? mt_objc_pool_push(rt) : 0u;
    run_item(&w->cpu, it);
    if (token != 0) {
        mt_objc_pool_pop_on(&w->cpu, rt, token);
    }
    G.stats.worker_blocks++;
}

void worker_loop(Worker *w) {
    while (wait_baton(w)) {
        if (w->has_item) {
            const Item it = w->item;
            w->has_item = false;
            run_global_item(w, it);
        }
        w->state = State::idle;
        give_back();
    }
}

void worker_main(Worker *w) {
    std::jmp_buf jb;
    t_worker = w;
    mt_crash_set_cpu(&w->cpu);
    w->exit_jmp = &jb;
    if (MT_HOST_SETJMP(jb) == 0) {
        worker_loop(w);
    }
    mt_crash_set_cpu(nullptr);
    t_worker = nullptr;
}

#if defined(_WIN32)
unsigned __stdcall thread_entry(void *arg) {
    worker_main(static_cast<Worker *>(arg));
    return 0;
}
bool start_thread(Worker *w) {
    const std::uintptr_t h =
        _beginthreadex(nullptr, static_cast<unsigned>(k_host_stack), thread_entry, w, 0, nullptr);
    if (h == 0) {
        return false;
    }
    w->thread = reinterpret_cast<HANDLE>(h);
    return true;
}
void join_thread(Worker *w) {
    WaitForSingleObject(w->thread, INFINITE);
    CloseHandle(w->thread);
}
#else
void *thread_entry(void *arg) {
    worker_main(static_cast<Worker *>(arg));
    return nullptr;
}
bool start_thread(Worker *w) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        return false;
    }
    pthread_attr_setstacksize(&attr, k_host_stack);
    const int rc = pthread_create(&w->thread, &attr, thread_entry, w);
    pthread_attr_destroy(&attr);
    return rc == 0;
}
void join_thread(Worker *w) { pthread_join(w->thread, nullptr); }
#endif

void free_worker(Worker *w) {
    mt_mem *mem = mt_mem_from_cpu(&w->cpu);
    if (mem != nullptr) {
        mt_stack_destroy(mem, &w->stack);
    }
    std::free(w->sjlj);
    mt_objc_pools_destroy(w->pools);
    delete w;
}

int is_worker_cpu(const mt_cpu *cpu) { return mt_gcd_is_worker(cpu); }
int on_worker() { return mt_gcd_on_worker(); }
void end_run_from_worker() { mt_gcd_worker_end_run(); }

// A new idle worker thread, or nullptr (the pool is full, or no memory).
Worker *spawn() {
    const u32 max = G.cfg.max_workers != 0 ? G.cfg.max_workers : MT_GCD_DEFAULT_WORKERS;
    if (G.workers.size() >= max || G.mem == nullptr) {
        return nullptr;
    }
    if (G.sjlj_tmp == nullptr) {
        G.sjlj_tmp = static_cast<mt_sjlj_state *>(std::calloc(1, sizeof(mt_sjlj_state)));
        if (G.sjlj_tmp == nullptr) {
            return nullptr;
        }
    }
    auto *w = new (std::nothrow) Worker{};
    if (w == nullptr) {
        return nullptr;
    }
    w->id = static_cast<int>(G.workers.size()) + 1;
    if (mt_stack_create(G.mem, G.cfg.stack_size, &w->stack) != 0) {
        log_line("no room for worker thread %d's guest stack", w->id);
        delete w;
        return nullptr;
    }
    mt_mem_attach(G.mem, &w->cpu);
    w->cpu.fpscr = MT_FPSCR_IOS; // a new iOS thread's (ADR 0022)
    w->cpu.r[MT_SP] = w->stack.top;
    w->sjlj = static_cast<mt_sjlj_state *>(std::calloc(1, sizeof(mt_sjlj_state)));
    w->pools = mt_objc_pools_create();
    if (w->sjlj != nullptr) {
        // An empty unwinder state that knows the personalities (the main thread's is current).
        const mt_sjlj_state *main_state = mt_sjlj_state_of(&w->cpu);
        mt_sjlj_init(w->sjlj);
        mt_sjlj_set_personalities(w->sjlj, main_state->gxx_personality,
                                  main_state->objc_personality);
        w->sjlj->can_catch = main_state->can_catch;
    }
    if (w->sjlj == nullptr || w->pools == nullptr || !start_thread(w)) {
        log_line("cannot start worker thread %d", w->id);
        free_worker(w);
        return nullptr;
    }
    w->state = State::idle;
    if (G.workers.empty()) {
        // exit, stop and traps on a worker end the run through the main thread (boot.h).
        const mt_process_threads threads = {is_worker_cpu, on_worker, end_run_from_worker};
        mt_process_set_threads(&threads);
    }
    G.workers.push_back(w);
    G.stats.workers++;
    return w;
}

// --- the scheduler (main thread) -----------------------------------------------------------

// The next worker to run, with its work assigned, or nullptr.
Worker *pick() {
    bool have_time = false;
    u64 t = 0;
    for (Worker *w : G.workers) {
        if (w->state != State::blocked) {
            continue;
        }
        if (cond_met(w->wait, w->wait_obj, w)) {
            w->timed_out = false;
            return w;
        }
        if (w->deadline != k_forever) {
            if (!have_time) {
                t = now();
                have_time = true;
            }
            if (t >= w->deadline) {
                w->timed_out = true;
                return w;
            }
        }
    }
    for (int qi = 0; qi < k_globals; qi++) {
        if (G.global_fifo[qi].empty()) {
            continue;
        }
        Worker *idle = nullptr;
        for (Worker *w : G.workers) {
            if (w->state == State::idle) {
                idle = w;
                break;
            }
        }
        if (idle == nullptr) {
            idle = spawn();
        }
        if (idle == nullptr) {
            if (!G.warned_full) {
                G.warned_full = true;
                log_line("every worker thread is busy (%u): global-queue blocks wait",
                         static_cast<unsigned>(G.workers.size()));
            }
            return nullptr;
        }
        idle->item = G.global_fifo[qi].front();
        idle->has_item = true;
        G.global_fifo[qi].pop_front();
        return idle;
    }
    return nullptr;
}

// Runs worker threads until `until` holds (met), the deadline passes (timeout) or no worker
// can make progress (quiet). With may_sleep, waits for timers and worker timeouts instead of
// returning quiet while one is pending.
Outcome run_pool(const Cond *until, u64 deadline, bool may_sleep) {
    for (;;) {
        fire_timers();
        if (until != nullptr && cond_met(until->kind, until->obj, nullptr)) {
            return Outcome::met;
        }
        if (deadline != k_forever && now() >= deadline) {
            return Outcome::timeout;
        }
        Worker *w = pick();
        if (w != nullptr) {
            handoff(w);
            continue;
        }
        if (!may_sleep) {
            return Outcome::quiet;
        }
        // Only a timer for a global queue or a worker's timeout can still change anything.
        u64 next = deadline;
        for (const Timer &t : G.timers) {
            if (t.queue != k_queue_main && t.when < next) {
                next = t.when;
                break;
            }
        }
        for (const Worker *b : G.workers) {
            if (b->state == State::blocked && b->deadline < next) {
                next = b->deadline;
            }
        }
        if (next == k_forever) {
            return Outcome::quiet;
        }
        sleep_until(next);
    }
}

} // namespace

// --- configuration and the run loop's side ---------------------------------------------------

extern "C" mt_gcd_config mt_gcd_config_default(void) noexcept {
    mt_gcd_config cfg{};
    cfg.max_workers = MT_GCD_DEFAULT_WORKERS;
    cfg.stack_size = MT_STACK_THREAD_SIZE;
    return cfg;
}

extern "C" int mt_gcd_configure(const mt_gcd_config *cfg) noexcept {
    if (!G.workers.empty()) {
        return -1;
    }
    G.cfg = cfg != nullptr ? *cfg : mt_gcd_config_default();
    return 0;
}

extern "C" void mt_gcd_reset(void) noexcept {
    if (t_worker != nullptr) {
        log_line("mt_gcd_reset on a worker thread: ignored");
        return;
    }
    G.m.lock();
    G.shutdown = true;
    G.cv.notify_all();
    G.m.unlock();
    for (Worker *w : G.workers) {
        join_thread(w);
        free_worker(w);
    }
    G.workers.clear();
    G.m.lock();
    G.shutdown = false;
    G.running = k_main;
    G.m.unlock();
    std::free(G.sjlj_tmp);
    G.sjlj_tmp = nullptr;
    G.cfg = mt_gcd_config_default();
    G.end_requested = false;
    G.base = nullptr;
    G.mem = nullptr;
    G.main_q = 0;
    for (int i = 0; i < k_globals; i++) {
        G.global_q[i] = 0;
        G.global_fifo[i].clear();
    }
    G.main_fifo.clear();
    G.timers.clear();
    G.timer_seq = 0;
    G.groups.clear();
    G.once_owner.clear();
    G.stats = mt_gcd_stats{};
    G.warned_full = false;
}

extern "C" uint32_t mt_gcd_drain_main(mt_cpu *cpu) noexcept {
    if (t_worker != nullptr) {
        return 0;
    }
    ensure(cpu);
    run_pool(nullptr, k_forever, false);
    // The blocks queued now; what they (or the workers) queue waits for the next turn.
    std::size_t n = G.main_fifo.size();
    if (n == 0) {
        return 0;
    }
    mt_objc *rt = mt_objc_current();
    const u32 token = rt != nullptr ? mt_objc_pool_push(rt) : 0u;
    u32 ran = 0;
    for (; n != 0 && !G.main_fifo.empty(); n--) {
        const Item it = G.main_fifo.front();
        G.main_fifo.pop_front();
        run_item(cpu, it);
        G.stats.main_blocks++;
        ran++;
        run_pool(nullptr, k_forever, false); // e.g. a worker this block's dispatch_sync freed
    }
    if (token != 0) {
        mt_objc_pool_pop_on(cpu, rt, token);
    }
    return ran;
}

extern "C" uint64_t mt_gcd_now(void) noexcept { return now(); }

extern "C" uint64_t mt_gcd_next_deadline(void) noexcept {
    const u64 t = now();
    if (!G.main_fifo.empty()) {
        return t;
    }
    for (int qi = 0; qi < k_globals; qi++) {
        if (!G.global_fifo[qi].empty()) {
            return t;
        }
    }
    u64 next = UINT64_MAX;
    if (!G.timers.empty()) {
        next = G.timers.front().when;
    }
    for (const Worker *w : G.workers) {
        if (w->state != State::blocked) {
            continue;
        }
        if (cond_met(w->wait, w->wait_obj, w)) {
            return t;
        }
        if (w->deadline < next) {
            next = w->deadline;
        }
    }
    return next;
}

extern "C" uint32_t mt_gcd_main_pending(void) noexcept {
    return static_cast<uint32_t>(G.main_fifo.size());
}

extern "C" int mt_gcd_idle(void) noexcept {
    if (!G.workers.empty() || !G.main_fifo.empty() || !G.timers.empty() || !G.groups.empty() ||
        !G.once_owner.empty()) {
        return 0;
    }
    for (int qi = 0; qi < k_globals; qi++) {
        if (!G.global_fifo[qi].empty()) {
            return 0;
        }
    }
    return 1;
}

extern "C" void mt_gcd_get_stats(mt_gcd_stats *out) noexcept { *out = G.stats; }

extern "C" int mt_gcd_is_worker(const mt_cpu *cpu) noexcept {
    for (const Worker *w : G.workers) {
        if (&w->cpu == cpu) {
            return 1;
        }
    }
    return 0;
}

extern "C" int mt_gcd_on_worker(void) noexcept { return t_worker != nullptr ? 1 : 0; }

extern "C" void mt_gcd_worker_end_run(void) noexcept {
    Worker *w = t_worker;
    if (w == nullptr) {
        std::abort();
    }
    G.end_requested = true;
    w->state = State::ended;
    give_back();
    while (wait_baton(w)) { // an ended worker is never picked again; until mt_gcd_reset
        give_back();
    }
    MT_HOST_LONGJMP(*w->exit_jmp, 1);
}

// --- the imports -----------------------------------------------------------------------------

extern "C" void mt_hle_dispatch_async(mt_cpu *cpu, uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 queue = mt_arg_u32(&a);
    const u32 block = mt_arg_u32(&a);
    ensure(cpu);
    const int qi = checked_queue(cpu, queue, "dispatch_async");
    const u32 copy = mt_block_copy(cpu, block);
    if (copy == 0) {
        log_line("dispatch_async from 0x%08X: no block (0x%08X)", cpu->r[MT_LR], block);
        gcd_trap(cpu, block);
    }
    enqueue(qi, Item{copy, 0u, 1u, 0});
}

extern "C" void mt_hle_dispatch_sync(mt_cpu *cpu, uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 queue = mt_arg_u32(&a);
    const u32 block = mt_arg_u32(&a);
    ensure(cpu);
    const int qi = checked_queue(cpu, queue, "dispatch_sync");
    if (qi != k_queue_main) {
        // A global queue is concurrent: libdispatch runs the block on the calling thread.
        mt_block_invoke(cpu, block, nullptr, 0u);
        return;
    }
    Worker *w = t_worker;
    if (w == nullptr) {
        log_line("dispatch_sync to the main queue from the main thread (0x%08X): a deadlock",
                 cpu->r[MT_LR]);
        gcd_trap(cpu, queue);
    }
    w->sync_done = false;
    G.main_fifo.push_back(Item{block, 0u, 0u, w->id});
    worker_block(w, Wait::sync, 0u, k_forever);
}

extern "C" void mt_hle_dispatch_after(mt_cpu *cpu, uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u64 when = mt_arg_u64(&a);
    const u32 queue = mt_arg_u32(&a);
    const u32 block = mt_arg_u32(&a);
    ensure(cpu);
    const int qi = checked_queue(cpu, queue, "dispatch_after");
    if (when == k_forever) {
        log_line("dispatch_after(DISPATCH_TIME_FOREVER) from 0x%08X: never runs", cpu->r[MT_LR]);
        return;
    }
    const u32 copy = mt_block_copy(cpu, block);
    if (copy == 0) {
        log_line("dispatch_after from 0x%08X: no block (0x%08X)", cpu->r[MT_LR], block);
        gcd_trap(cpu, block);
    }
    const u64 deadline = deadline_of(when);
    if (deadline <= now()) {
        enqueue(qi, Item{copy, 0u, 1u, 0}); // libdispatch: dispatch_async when already due
        return;
    }
    add_timer(deadline, qi, copy);
}

extern "C" void mt_hle_dispatch_once(mt_cpu *cpu, uint32_t) noexcept {
    const u32 pred = cpu->r[0];
    const u32 block = cpu->r[1];
    ensure(cpu);
    std::uint8_t *p = mt_g2h(cpu, pred, 4u);
    u32 v;
    std::memcpy(&v, p, 4);
    if (v == MT_DISPATCH_ONCE_DONE) {
        return;
    }
    if (v == 0) {
        const u32 running = MT_DISPATCH_ONCE_RUNNING, done = MT_DISPATCH_ONCE_DONE;
        std::memcpy(p, &running, 4);
        G.once_owner[pred] = me();
        mt_block_invoke(cpu, block, nullptr, 0u);
        std::memcpy(p, &done, 4); // the handoff to the next thread orders it
        G.once_owner.erase(pred);
        return;
    }
    // Another call is running the block (or the predicate holds garbage).
    const auto it = G.once_owner.find(pred);
    const int owner = it != G.once_owner.end() ? it->second : -1;
    if (owner < 0) {
        log_line("dispatch_once from 0x%08X: predicate 0x%08X holds 0x%08X, but no thread runs "
                 "its block",
                 cpu->r[MT_LR], pred, v);
        gcd_trap(cpu, pred);
    }
    if (owner == me()) {
        log_line("dispatch_once from 0x%08X: predicate 0x%08X from inside its own block: a "
                 "deadlock",
                 cpu->r[MT_LR], pred);
        gcd_trap(cpu, pred);
    }
    if (t_worker != nullptr) {
        worker_block(t_worker, Wait::once, pred, k_forever);
        return;
    }
    const Cond c{Wait::once, pred};
    if (run_pool(&c, k_forever, true) != Outcome::met) {
        log_line("dispatch_once from 0x%08X on the main thread: predicate 0x%08X waits for worker "
                 "%d, which cannot finish: a deadlock",
                 cpu->r[MT_LR], pred, owner);
        gcd_trap(cpu, pred);
    }
}

extern "C" void mt_hle_dispatch_time(mt_cpu *cpu, uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u64 when = mt_arg_u64(&a);
    const i64 delta = mt_arg_i64(&a);
    mt_ret_u64(cpu, dispatch_time_of(when, delta));
}

extern "C" void mt_hle_dispatch_get_global_queue(mt_cpu *cpu, uint32_t) noexcept {
    const auto priority = static_cast<std::int32_t>(cpu->r[0]);
    const u32 flags = cpu->r[1];
    ensure(cpu);
    int qi;
    switch (priority) {
    case MT_DISPATCH_QUEUE_PRIORITY_HIGH:
        qi = 0;
        break;
    case MT_DISPATCH_QUEUE_PRIORITY_DEFAULT:
        qi = 1;
        break;
    case MT_DISPATCH_QUEUE_PRIORITY_LOW:
        qi = 2;
        break;
    case MT_DISPATCH_QUEUE_PRIORITY_BACKGROUND:
        qi = 3;
        break;
    default:
        qi = -1;
        break;
    }
    // libdispatch: NULL for an unknown priority or any flag but (private) overcommit, 2.
    if (qi < 0 || (flags & ~2u) != 0) {
        log_line("dispatch_get_global_queue(%d, 0x%X) from 0x%08X: no such queue", priority, flags,
                 cpu->r[MT_LR]);
        mt_ret_u32(cpu, 0u);
        return;
    }
    if (G.global_q[qi] == 0) {
        G.global_q[qi] = new_object(k_tag_queue, static_cast<u32>(priority));
    }
    mt_ret_u32(cpu, G.global_q[qi]);
}

extern "C" void mt_hle_dispatch_group_create(mt_cpu *cpu, uint32_t) noexcept {
    ensure(cpu);
    const u32 group = new_object(k_tag_group, 0u);
    if (group != 0) {
        G.groups[group] = Group{1u, 0u};
    }
    mt_ret_u32(cpu, group);
}

extern "C" void mt_hle_dispatch_group_async(mt_cpu *cpu, uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 group = mt_arg_u32(&a);
    const u32 queue = mt_arg_u32(&a);
    const u32 block = mt_arg_u32(&a);
    ensure(cpu);
    Group *gr = group_of(group);
    if (gr == nullptr) {
        log_line("dispatch_group_async from 0x%08X: 0x%08X is not a dispatch group", cpu->r[MT_LR],
                 group);
        gcd_trap(cpu, group);
    }
    const int qi = checked_queue(cpu, queue, "dispatch_group_async");
    const u32 copy = mt_block_copy(cpu, block);
    if (copy == 0) {
        log_line("dispatch_group_async from 0x%08X: no block (0x%08X)", cpu->r[MT_LR], block);
        gcd_trap(cpu, block);
    }
    gr = group_of(group); // _Block_copy ran guest code
    if (gr != nullptr) {
        gr->pending++;
        gr->refs++;
    }
    enqueue(qi, Item{copy, group, 1u, 0});
}

extern "C" void mt_hle_dispatch_group_wait(mt_cpu *cpu, uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 group = mt_arg_u32(&a);
    const u64 timeout = mt_arg_u64(&a);
    ensure(cpu);
    const Group *gr = group_of(group);
    if (gr == nullptr) {
        log_line("dispatch_group_wait from 0x%08X: 0x%08X is not a dispatch group", cpu->r[MT_LR],
                 group);
        gcd_trap(cpu, group);
    }
    if (gr->pending == 0) {
        mt_ret_u32(cpu, 0u);
        return;
    }
    if (timeout == MT_DISPATCH_TIME_NOW) {
        mt_ret_u32(cpu, MT_DISPATCH_TIMED_OUT);
        return;
    }
    const u64 deadline = deadline_of(timeout);
    if (t_worker != nullptr) {
        worker_block(t_worker, Wait::group, group, deadline);
        mt_ret_u32(cpu, cond_met(Wait::group, group, nullptr) ? 0u : MT_DISPATCH_TIMED_OUT);
        return;
    }
    const Cond c{Wait::group, group};
    const Outcome o = run_pool(&c, deadline, true);
    if (o == Outcome::quiet) {
        log_line("dispatch_group_wait from 0x%08X on the main thread: group 0x%08X waits for "
                 "blocks nothing can run: a deadlock",
                 cpu->r[MT_LR], group);
        gcd_trap(cpu, group);
    }
    mt_ret_u32(cpu, o == Outcome::met ? 0u : MT_DISPATCH_TIMED_OUT);
}

extern "C" void mt_hle_dispatch_release(mt_cpu *cpu, uint32_t) noexcept {
    const u32 object = cpu->r[0];
    ensure(cpu);
    if (queue_index(object) >= 0) {
        return; // the main and global queues are never freed
    }
    if (group_of(object) == nullptr) {
        log_line("dispatch_release from 0x%08X: 0x%08X is no dispatch object", cpu->r[MT_LR],
                 object);
        gcd_trap(cpu, object);
    }
    group_release(object);
}

extern "C" void mt_hle_init_dispatch_main_q(mt_cpu *cpu, uint32_t, uint32_t addr) noexcept {
    ensure(cpu);
    G.main_q = addr;
    const u32 tag = k_tag_queue;
    std::memcpy(mt_g2h(cpu, addr, 4u), &tag, 4);
}
