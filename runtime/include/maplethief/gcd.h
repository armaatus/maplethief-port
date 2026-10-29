/* Grand Central Dispatch: the subset of libdispatch the binary imports (#50, ADR 0033).
 *
 * The 11 imports, all from libSystem.B.dylib: dispatch_async, dispatch_sync, dispatch_after,
 * dispatch_once, dispatch_time, dispatch_get_global_queue, _dispatch_main_q (the main queue
 * object, a data import), dispatch_group_create, dispatch_group_async, dispatch_group_wait
 * and dispatch_release. The game itself only sends GameCenterManager's completion handlers to
 * the main queue; everything else is Chartboost's (docs/re/timing-threading.md).
 *
 * Threads. The main queue runs on the main thread, drained by the run loop. The global
 * queues run on a small pool of host worker threads, each a guest thread of its own: an
 * mt_cpu and a guest stack (MT_STACK_THREAD_SIZE). Only one guest thread runs at a time: the
 * rest of the runtime is single-threaded (ADR 0014, ADR 0029), so the main thread holds a
 * baton and hands it to one worker at a time, which hands it back when its block ends or
 * blocks (dispatch_group_wait, dispatch_sync to the main queue, a contended dispatch_once).
 * The order is deterministic: it depends only on the guest's calls and the clock. At every
 * handoff the per-thread state of the runtime (the SjLj unwinder's, the autorelease pools)
 * is exchanged, and the handoff itself (a mutex) orders guest memory, so the guest's dmb
 * barriers can stay empty.
 *
 * The run loop (#48) drives it:
 *
 *     // each run-loop turn, inside the turn's autorelease pool, after the due timers:
 *     mt_gcd_drain_main(cpu);
 *     // before sleeping: wake no later than this (UINT64_MAX: no GCD deadline)
 *     uint64_t wake = mt_gcd_next_deadline();   // in mt_gcd_now() time
 *
 * mt_gcd_drain_main fires the due dispatch_after timers, lets the worker threads run until
 * none can make progress, then runs the main-queue blocks that were queued when it started,
 * oldest first (one queued during the drain waits for the next turn, as with libdispatch).
 *
 * A wait that can never end is a guest deadlock on iOS too (dispatch_sync to the main queue
 * from the main thread, a dispatch_once that recurses into itself, waiting on the main thread
 * for work nothing can run): here it traps with MT_TRAP_GCD instead of hanging. So does a
 * queue or object that is none of GCD's.
 *
 * A worker thread that ends the run (exit, a trap) hands that to the main thread, which ends
 * mt_process_run as usual. The run's process destroys the pool (mt_gcd_reset).
 *
 * C17 that also compiles as C++20; the implementation is runtime/src/gcd/gcd.cpp. Call
 * everything here on the main thread (the one that runs the process), unless it says
 * otherwise. */
#ifndef MAPLETHIEF_GCD_H
#define MAPLETHIEF_GCD_H

#include <stdint.h>

#include "maplethief/hle/imports.h" /* mt_log_fn */
#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* dispatch_time_t values (libdispatch: nanoseconds of a monotonic clock; negative ones are
 * dispatch_walltime's, which the binary does not import). */
#define MT_DISPATCH_TIME_NOW 0ull
#define MT_DISPATCH_TIME_FOREVER (~0ull)

/* dispatch_get_global_queue priorities (long). */
#define MT_DISPATCH_QUEUE_PRIORITY_HIGH 2
#define MT_DISPATCH_QUEUE_PRIORITY_DEFAULT 0
#define MT_DISPATCH_QUEUE_PRIORITY_LOW (-2)
#define MT_DISPATCH_QUEUE_PRIORITY_BACKGROUND (-32768) /* INT16_MIN, iOS 5 */

/* dispatch_once predicates (libdispatch's protocol, which the guest's inline fast path relies
 * on: it calls dispatch_once only while the predicate is not ~0). */
#define MT_DISPATCH_ONCE_RUNNING 1u
#define MT_DISPATCH_ONCE_DONE 0xFFFFFFFFu

/* What dispatch_group_wait returns on a timeout (KERN_OPERATION_TIMED_OUT, libdispatch's
 * semaphore.c). */
#define MT_DISPATCH_TIMED_OUT 49

#define MT_GCD_DEFAULT_WORKERS 4u

/* --- configuration ------------------------------------------------------------------------ */

/* A monotonic clock in nanoseconds; it must never return 0. */
typedef uint64_t (*mt_gcd_clock_fn)(void *user);
/* Blocks the calling (main) thread until the clock reads at least `deadline`. */
typedef void (*mt_gcd_sleep_fn)(void *user, uint64_t deadline);

typedef struct mt_gcd_config {
    mt_gcd_clock_fn clock; /* NULL: the host's steady clock */
    void *clock_user;
    mt_gcd_sleep_fn sleep_until; /* NULL: sleep on the host (with the host clock) */
    void *sleep_user;
    uint32_t max_workers; /* worker threads at most; 0: MT_GCD_DEFAULT_WORKERS */
    uint32_t stack_size;  /* each worker's guest stack; 0: MT_STACK_THREAD_SIZE */
    mt_log_fn log;        /* "gcd: ..." lines; NULL: stderr */
    void *log_user;
} mt_gcd_config;

/* The host clocks, MT_GCD_DEFAULT_WORKERS, MT_STACK_THREAD_SIZE, stderr. */
mt_gcd_config mt_gcd_config_default(void) MT_NOEXCEPT;

/* Sets the configuration (NULL: the default). Only while no worker thread exists (at start,
 * or after mt_gcd_reset); returns 0, or -1 otherwise. */
int mt_gcd_configure(const mt_gcd_config *cfg) MT_NOEXCEPT;

/* Ends every worker thread (a blocked one is abandoned: its guest stack is freed without
 * unwinding), frees their guest stacks and forgets all queued work, timers, groups and the
 * queue objects; the configuration goes back to the default. Call it before the guest
 * memory goes away; mt_process_destroy does. */
void mt_gcd_reset(void) MT_NOEXCEPT;

/* --- the run loop's side ------------------------------------------------------------------ */

/* One turn of the main queue on the main thread's cpu (see above). Returns the number of
 * main-queue blocks it ran. From a worker thread it does nothing and returns 0. */
uint32_t mt_gcd_drain_main(mt_cpu *cpu) MT_NOEXCEPT;

/* The GCD clock now (dispatch_time's time base). */
uint64_t mt_gcd_now(void) MT_NOEXCEPT;

/* When mt_gcd_drain_main next has something to do, in mt_gcd_now() time: at most now if a
 * main-queue block or global-queue work is waiting, else the earliest dispatch_after timer or
 * worker timeout; UINT64_MAX if nothing is pending. */
uint64_t mt_gcd_next_deadline(void) MT_NOEXCEPT;

/* Main-queue blocks waiting to run. */
uint32_t mt_gcd_main_pending(void) MT_NOEXCEPT;

typedef struct mt_gcd_stats {
    uint32_t main_blocks;   /* blocks run from the main queue */
    uint32_t worker_blocks; /* blocks run from global queues (on worker threads) */
    uint32_t workers;       /* worker threads created */
    uint32_t handoffs;      /* times the main thread handed the baton to a worker */
    uint32_t timers_fired;  /* dispatch_after timers that came due */
} mt_gcd_stats;

void mt_gcd_get_stats(mt_gcd_stats *out) MT_NOEXCEPT;

/* --- worker threads (for the rest of the runtime) ----------------------------------------- */

/* 1 if `cpu` is a GCD worker thread's (any thread may ask). */
int mt_gcd_is_worker(const mt_cpu *cpu) MT_NOEXCEPT;
/* 1 if the calling host thread is a GCD worker thread. */
int mt_gcd_on_worker(void) MT_NOEXCEPT;
/* On a worker thread whose guest code ended the run (the process recorded the result): hands
 * the end to the main thread, which then ends mt_process_run (mt_process_end_recorded), and
 * parks this thread until mt_gcd_reset. */
MT_NORETURN void mt_gcd_worker_end_run(void) MT_NOEXCEPT;

/* --- the imports (mt_import_fn; runtime/src/hle/registry.cpp) ----------------------------- */

/* void dispatch_async(dispatch_queue_t queue, dispatch_block_t block): copies the block,
 * queues it, releases it after it ran. */
void mt_hle_dispatch_async(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* void dispatch_sync(dispatch_queue_t queue, dispatch_block_t block): a global queue runs it
 * at once on the calling thread; the main queue from a worker runs it on the main thread at
 * the next drain while the worker waits; the main queue from the main thread traps. */
void mt_hle_dispatch_sync(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* void dispatch_after(dispatch_time_t when, dispatch_queue_t queue, dispatch_block_t block):
 * when r0:r1, queue r2, block r3. */
void mt_hle_dispatch_after(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* void dispatch_once(dispatch_once_t *predicate, dispatch_block_t block) */
void mt_hle_dispatch_once(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta): when r0:r1, delta r2:r3,
 * the result in r0:r1. */
void mt_hle_dispatch_time(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* dispatch_queue_t dispatch_get_global_queue(long priority, unsigned long flags) */
void mt_hle_dispatch_get_global_queue(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* dispatch_group_t dispatch_group_create(void) */
void mt_hle_dispatch_group_create(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* void dispatch_group_async(dispatch_group_t group, dispatch_queue_t queue,
 *                           dispatch_block_t block) */
void mt_hle_dispatch_group_async(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* long dispatch_group_wait(dispatch_group_t group, dispatch_time_t timeout): group r0, timeout
 * r1:r2; 0, or MT_DISPATCH_TIMED_OUT. */
void mt_hle_dispatch_group_wait(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* void dispatch_release(dispatch_object_t object): groups are freed at the last release; the
 * main and global queues are never freed. */
void mt_hle_dispatch_release(mt_cpu *cpu, uint32_t id) MT_NOEXCEPT;
/* The data import _dispatch_main_q (struct dispatch_queue_s, opaque to the guest): its
 * address is the main queue (dispatch_get_main_queue() is &_dispatch_main_q). */
void mt_hle_init_dispatch_main_q(mt_cpu *cpu, uint32_t id, uint32_t addr) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_GCD_H */
