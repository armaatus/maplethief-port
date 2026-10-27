# 33. GCD: global queues on worker threads that run one at a time, and a main-queue pump

- **Status:** Accepted
- **Date:** 2026-10-27
- **Issue:** #50
- **Builds on:** [ADR 0014](0014-recompiler-design.md) (one `mt_cpu` per guest thread, empty barriers), [ADR 0019](0019-import-thunks-and-hle-abi.md) (registry, data imports), [ADR 0020](0020-sjlj-exceptions.md) (the unwinder state), [ADR 0025](0025-guest-memory-and-address-space.md) (guest stacks), [ADR 0027](0027-process-start-up.md) (how a run ends), [ADR 0029](0029-objective-c-runtime-services.md) (blocks, autorelease pools)

## Context

The binary imports 11 libdispatch symbols: `dispatch_async`, `dispatch_sync`, `dispatch_after`, `dispatch_once`, `dispatch_time`, `dispatch_get_global_queue`, `_dispatch_main_q` (a data import, the main queue object), `dispatch_group_create`, `dispatch_group_async`, `dispatch_group_wait` and `dispatch_release`. Where they are used ([re/timing-threading.md](../re/timing-threading.md#threads-queues-and-callbacks)):

- **The game itself** only sends one thing through GCD: `-[GameCenterManager callDelegateOnMainThread:withArg:error:]` `0xC0145` does `dispatch_async(_dispatch_main_q, …)`.
- **Chartboost** uses the rest.
  - Three `dispatch_once` singletons.
  - A `dispatch_after` request timeout to the main queue.
  - Image loads that `dispatch_group_async` to the LOW global queue. A waiter block, also on that queue, runs `dispatch_group_wait(FOREVER)`, `dispatch_release` and `dispatch_async(main)`.
  - A download block that `dispatch_sync`s its callback to the main queue.
  - With Chartboost stubbed (#52), none of this runs.
- **The inline `dispatch_once` fast path.** At `0xC2E98`, `+[Chartboost sharedChartboost]` compiles to `ldr r0, [pred]; cmp r0, #-1; beq done; … blx _dispatch_once`. The guest calls `dispatch_once` only while the predicate is not `~0`. The 66 `dmb` in the binary are all in Chartboost's atomic property accessors, not here.
- **The rest of the runtime is single-threaded by design.**
  - ADR 0014: barriers emit nothing.
  - ADR 0029: no locks, and `@synchronized` is a counter.
  - `mt_sjlj_state_of` keeps one unwinder state.
  - The ObjC runtime keeps one autorelease pool stack.
  - `mt_process_trap`/`exit` long-jump to the main thread's `mt_process_run`.
- **#50 asks for** global queues on a small host thread pool, each with a guest stack, and a main queue drained once per run-loop turn (by #48's run loop).

## Options considered

1. **Run global-queue blocks inline on the main thread at the next pump.** This is what timing-threading.md proposed. It is the simplest option, and enough for Chartboost's ordering, but:
   - `dispatch_group_wait` inside such a block would have to run the other pending blocks nested on the same stack.
   - A block that waits for the main thread (`dispatch_sync(main)`) or for another block's `dispatch_once` cannot wait: the work it waits for is below it on the same stack.
   - `dispatch_once` contention cannot happen at all.
   - It does not give the guest threads the issue asks for.
2. **Free-running worker threads with a global interpreter lock.** Workers take the lock whenever the main thread releases it. Real threads, but:
   - The interleaving depends on host scheduling, so runs are not reproducible, which golden and replay tests need.
   - The main thread would have to release the lock around every host wait (#48's sleep), coupling the run loop to GCD.
3. **Worker threads that run only when the main thread hands them a baton (chosen).**
   - Each worker is a real host thread with its own `mt_cpu` and guest stack.
   - Only one guest thread runs at a time, and a deterministic scheduler on the main thread decides which. A worker runs from the moment the main thread hands it the baton until its block ends or blocks.
   - Blocking calls keep their real semantics: the blocked worker's host stack simply waits. That covers `dispatch_group_wait`, `dispatch_sync(main)` from a worker, and a contended `dispatch_once`.
   - The single-threaded invariants of the runtime still hold.
4. **Concurrent worker threads.** These would need locks in the heap, the ObjC runtime and the unwinder, and real barriers for `dmb`. Nothing the game or Chartboost does needs parallelism.

## Decision

Option 3. The code is in `runtime/src/gcd/gcd.cpp`, the API in `maplethief/gcd.h`, and the registry entries are in the `#ifndef MT_HLE_CORE_ONLY` block.

- **Threads.**
  - A worker is a host thread with an 8 MiB host stack, enough for the generated code's recursion; `std::thread` cannot size its stack, so it uses `pthread` or `_beginthreadex`.
  - Each worker has an `mt_cpu` attached to the process's address space, with `fpscr = MT_FPSCR_IOS`, and a guest stack of `MT_STACK_THREAD_SIZE` (512 KiB, iOS's).
  - At most `max_workers` workers exist (default 4). One is created when a global block is waiting and no worker is idle.
  - Each worker sets itself as the crash reporter's thread cpu.
- **The baton.**
  - `G.running` names the guest thread that may run (0 is the main thread), under one mutex and condition variable.
  - Only the main thread hands it out. A worker hands it back when its block has run, or when it blocks.
  - All other GCD state is touched only by the baton holder.
  - The mutex orders guest memory between threads, so `dmb` stays empty.
- **Per-thread state.** The runtime holds the running thread's per-thread state. At each handoff the main thread exchanges it with the worker's saved copy, before and after the worker runs. That state is:
  - the SjLj unwinder state: the whole `mt_sjlj_state`, copied;
  - the autorelease pool stack, through the new `mt_objc_pools` API in `maplethief/objc/objc.h`.

  A worker runs each block in an autorelease pool, as libdispatch's worker threads do, and pops it on its own cpu (`mt_objc_pool_pop_on`).
- **The scheduler** (`run_pool`, main thread). It loops:
  1. Fire the due `dispatch_after` timers.
  2. Stop if the main thread's wait is over.
  3. Wake the first blocked worker whose wait is over or timed out.
  4. Otherwise give the oldest block of the highest-priority non-empty global queue (HIGH, DEFAULT, LOW, BACKGROUND) to an idle or new worker.

  When nothing can run, it returns:
  - `mt_gcd_drain_main` never sleeps.
  - A main-thread wait (`dispatch_group_wait`, a contended `dispatch_once`) sleeps until the next timer for a global queue, a worker's timeout or its own deadline.
  - If there is none, the main thread's wait can never end.
- **The main queue** is a FIFO. `mt_gcd_drain_main(cpu)` is the run loop's per-turn call:
  1. Run the pool until nothing can make progress.
  2. Inside an autorelease pool, run the main-queue blocks that were queued when the drain started, oldest first. After each block, run the pool again, for example for a worker that the block's `dispatch_sync` released. Blocks queued during the drain wait for the next turn, as in libdispatch's main-queue drain.

  `mt_gcd_next_deadline()` tells the run loop how long it may sleep, and `mt_gcd_main_pending()` how much is waiting. Global-queue work only advances inside the drain and inside main-thread waits. That is part of the determinism.
- **The imports.**
  - **`dispatch_async`** copies the block (`_Block_copy`), queues it, and releases it after it has run.
  - **`dispatch_sync`**:
    - On a global queue it runs the block on the calling thread, as libdispatch does for concurrent queues.
    - On the main queue from a worker, it queues the block and blocks the worker until the main thread has run it.
  - **`dispatch_after`**: a timer, ordered by deadline with FIFO among equal deadlines, that moves the block to its queue when due. If the time is already past, the block is queued at once. `FOREVER` never runs (logged).
  - **`dispatch_time`**: libdispatch's `time.c` arithmetic over a monotonic clock in nanoseconds. The default clock is the host's steady clock offset by 1 s, so it is never 0 (`NOW`); tests can replace it. The arithmetic saturates to `FOREVER` and to 1, and has the wall-clock branch.
  - **`dispatch_get_global_queue`**: one guest object per priority (2, 0, −2, `INT16_MIN`). An unknown priority, or a flag other than overcommit (2), returns NULL.
  - **Groups** are guest objects with a reference count and a pending count. `dispatch_group_wait` returns 0, or `KERN_OPERATION_TIMED_OUT` (49) as libdispatch does.
  - **`dispatch_release`** frees a group at its last reference; the main and global queues are never freed.
  - **`_dispatch_main_q`**: its init hook records the address and tags the object.
- **`dispatch_once`** keeps libdispatch-187's predicate protocol, so the inline fast path stays correct: 0 is not started, 1 is running, `~0` is done.
  - The caller that finds 0 stores 1, runs the block on its own thread, and stores `~0`.
  - A caller that finds 1 waits. A worker blocks until the predicate is `~0`. The main thread runs the scheduler until it is.
- **Deadlocks and bad objects trap.** A wait that can never end is a hang on iOS. Here it logs and stops the guest with the new trap kind **`MT_TRAP_GCD` (18)**: pc = the caller, detail = the queue, group or predicate. The cases are:
  - `dispatch_sync` to the main queue from the main thread;
  - `dispatch_once` from inside its own block;
  - a predicate that holds neither 0, 1 nor `~0` and that no thread is running;
  - a main-thread `FOREVER` wait with nothing left that could end it.

  A queue or object that is not GCD's traps the same way. Kind 16 is #45's `MT_TRAP_PURE_VIRT`, and 15 and 17 are left to the other M4 issues built at the same time.
- **Ending the run from a worker.**
  - `running_on` in `process.c` accepts worker cpus, so `exit`, `mt_process_stop` and traps on a worker record the result as usual.
  - Then `end_run` calls `mt_gcd_worker_end_run`. That hands the baton back and parks the worker, and the main thread finishes with the new `mt_process_end_recorded`, which long-jumps to `mt_process_run` on the thread that owns it.
  - `mt_process_destroy` calls `mt_gcd_reset`. That ends every worker; a blocked worker is abandoned by a long jump out of its stack. It also frees their guest stacks and forgets all GCD state.

## Consequences

- **Determinism.** Global-queue work runs at fixed points: the per-turn drain and main-thread waits, in a fixed order. With a virtual clock (`mt_gcd_config.clock` and `sleep_until`) a run is reproducible. The cost: a global block runs on the next run-loop turn rather than at once, about 1 ms later.
- **Shared invariants keep working**, so no other module needed a lock:
  - `dmb` is still empty.
  - The ObjC runtime, heap and diagnostics are still accessed by one thread at a time.
  - `mt_sjlj_state_of` still returns one state. If it ever becomes per-cpu, `swap_state` must stop copying it.
- **Limits that are not worth code today.** Chartboost, the only user, is stubbed.
  - `@synchronized` is still a per-object counter: two guest threads that both hold one lock across a GCD wait would not exclude each other.
  - Worker threads have no alternate signal stack, so a host stack overflow on a worker is not reported as a guest crash. Guest memory faults are.
  - The pool does not grow past `max_workers` when every worker is blocked. Global blocks then wait, and this is logged once.
- **Integration with #48.** The run loop calls `mt_gcd_drain_main(cpu)` once per turn, inside its per-turn pool, after its due timers. It may also sleep no later than `mt_gcd_next_deadline()`. Run-loop timers and GCD timers use separate clocks until #48 decides to share one: `mt_gcd_configure` takes a clock.
- **Tests.**
  - `tests/cpp/gcd_test.cpp` (CTest area `gcd`) covers each import through its HLE entry:
    - main-queue ordering;
    - priorities;
    - Chartboost's group-waiter pattern;
    - timeouts;
    - `dispatch_once` contended by five workers and by the main thread;
    - per-thread autorelease pools;
    - the deadlock traps;
    - reset with a blocked worker.
  - `tests/cpp/boot_test.cpp` ends a synthetic process through `exit` and through a trap on a worker.
  - Both pass under ThreadSanitizer.
