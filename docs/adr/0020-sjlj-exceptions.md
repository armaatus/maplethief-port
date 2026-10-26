# 20. SjLj exceptions in recompiled code: resume hooks, a runtime unwinder over guest memory

- **Status:** Accepted
- **Date:** 2026-10-26
- **Issue:** #37

## Context

[ADR 0014](0014-recompiler-design.md) fixed the interface: where the guest stores a resume address into its jump buffer, the lifter emits `if (MT_SJLJ_SETJMP(jbuf)) { goto L_<resume>; }`; the runtime keeps a host `jmp_buf` per guest jump buffer, restores `r7` and `sp` from the guest buffer and `longjmp`s; `_Unwind_SjLj_*` are ordinary imports. [ADR 0016](0016-function-discovery.md) made each resume address an internal entry of its function, so the landing-pad code is discovered. #37 has to fill in how the hook is found, what the runtime implements, how far its semantics go, and how this is tested when the game itself never throws.

Facts (measured 2026-10-26, [ANALYSIS.md → Exceptions](../ANALYSIS.md#exceptions-37), [formats/lsda.md](../formats/lsda.md)):

- **205 functions** have a context; each registers once and has one resume point. 189 are LLVM-GCC game code (`mov r4, pc; adds r4, r4, #7; ...; str r4, [r5, #4]`, then a compare chain), 16 are Chartboost clang code (`ldr r1, [pc, #k]; orr r1, r1, #1; add r1, pc; ...; str r1, [sp, #k]`, then a bounded `mov pc` into a table of `b.w`). Both store the resume address before calling `_Unwind_SjLj_Register`.
- The context layout is GCC's `SjLj_Function_Context` with LLVM's three-word `__builtin_setjmp` buffer: prev, call_site, data[4], personality, LSDA, r7, resume address, sp. `__builtin_longjmp` restores only r7 and sp; the landing pads reload everything else from the frame.
- The 205 LSDAs have 928 cleanup call sites, 258 empty `throw()` specifications (whose pads call `std::terminate`) and 52 `catch (...)` in two Chartboost `@catch` blocks; no typed catch.
- **No guest code starts an exception**: `__cxa_throw`, `objc_exception_throw` and `_Unwind_SjLj_RaiseException` are not imported. Raising calls (`_Unwind_SjLj_Resume`, `objc_exception_rethrow`) only appear in landing pads. An exception could only come from the HLE side (Foundation's `NSException`, a failing `operator new`).

## Options considered

### How to find the hook

1. **From the census of the prologue** (`recomp.exceptions.find_context`: a symbolic evaluation up to the Register call). Exact, but it needs import knowledge and a whole-function evaluation inside the lifter.
2. **A straight-line scan for the store of the resume value** (`recomp.lift.sjlj.find_hooks`): track registers holding PC-derived values (`mov rX, pc`, `ldr` literal, `add`/`orr` immediates, `add rX, pc`), restart at labels and entries like discovery's `find_code_refs`, and take the first full-word `str` of a value equal to an entry (Thumb bit set). The hook's `jbuf` is the store address - 4. It needs only the function and the literal pool.

### The host jump buffer

1. **A `jmp_buf` local in each SjLj function**, registered with the runtime. No table, but every SjLj function grows by a `jmp_buf` (up to 256 bytes on Windows x64) and needs an extra registration call; ADR 0014's `mt_sjlj_jmpbuf` would change.
2. **Runtime-owned storage keyed by the guest jump buffer** (ADR 0014's `mt_sjlj_jmpbuf(cpu, jbuf)`): a fixed table of `{guest jbuf, jmp_buf}`, released when the context is unregistered or popped by an unwind, garbage-collected against the registered chain when full. Pointers stay stable because the table never moves.

### How much of the ABI

1. **The minimal path** the issue allows when the game never throws: Register/Unregister as a chain, every raise traps.
2. **The full GCC semantics** (`unwind-sjlj.c` and the SjLj personality): two phases, cleanups, catch types, exception specifications, rethrow, the ObjC catch helpers. About 500 lines of C, and the only way the synthetic tests can show the landing pads work.

## Decision

- **Hook.** Option 2: `recomp/lift/sjlj.py` finds one `Hook(site, resume, base, disp)` per resume entry; `lift/core.py` emits `if (MT_SJLJ_SETJMP(cpu->r[base] + disp)) { goto L_<resume>; }` right after the store's own C, labels the resume address and passes the resume addresses to `check_flags` as entries with no flag defined. An entry without a store is a lifting problem (`sjlj resume point without a jump-buffer store`), never a silent miss. The census (option 1, `recomp/exceptions.py`) stays as the independent check: the real-asset test requires the two to agree on all 205 functions.
- **Host buffers.** Option 2, as ADR 0014 specified: `MT_SJLJ_MAX_FRAMES` (64) slots in the per-thread `mt_sjlj_state`, trap `MT_TRAP_SJLJ` when full after collection. **Change to ADR 0014:** on Apple platforms the pair is `_setjmp`/`_longjmp` (`MT_HOST_SETJMP`/`MT_HOST_LONGJMP` in `calls.h`), because Darwin's `setjmp` also saves the signal mask, a system call on every entry of an SjLj function. glibc's and MSVC's `setjmp` do not, so they keep `setjmp`/`longjmp`.
- **ABI.** Option 2, in **`runtime/src/sjlj.c`** (C17, part of the `runtime` library) with the API in **`runtime/include/maplethief/sjlj.h`**:
  - `mt_sjlj_register`/`unregister`/`resume`, `mt_sjlj_throw` (the raise entry point for HLE code), `mt_sjlj_begin_catch`/`end_catch`/`rethrow`;
  - the personality, one routine for `___gxx_personality_sj0` and `___objc_personality_v0` (Apple's ObjC personality forwards to the C++ one for SjLj). It reads the LSDA from guest memory with GCC's rules: call-site records are ULEB128 whatever the header's encoding, a raw 0 type entry is catch-all, negative filters are exception specifications. Type matching is equality unless the runtime sets `can_catch` (M4: RTTI or ObjC class walks; the game has no typed catch);
  - install: data[0] = exception, data[1] = selector, call_site = landing pad, pop the contexts above (GCC's `_Unwind_SjLj_SetContext`), restore r7 and sp, `longjmp`.
  - Phase 1 finds no handler → **`MT_TRAP_UNCAUGHT`** before any cleanup runs (what `__cxa_throw` followed by `std::terminate` does). `std::terminate`, `call_site` 0 → **`MT_TRAP_TERMINATE`**. Inconsistent state (unknown personality, a call site the LSDA lacks, a context without a host buffer, full tables) → **`MT_TRAP_SJLJ`**. These are trap kinds 8, 9 and 10, after #36's `MT_TRAP_IMPORT`.
- **Binding** ([ADR 0019](0019-import-thunks-and-hle-abi.md)). `runtime/src/hle/registry.cpp` registers `mt_sjlj_hle_*` under `__Unwind_SjLj_Register`, `__Unwind_SjLj_Unregister`, `__Unwind_SjLj_Resume`, `__ZSt9terminatev`, `_objc_begin_catch`, `_objc_end_catch` and `_objc_exception_rethrow`; `mt_imports_link` binds them by name. The personalities stay unimplemented as functions (the guest never calls them); `mt_sjlj_bind_personalities(state, mt_import_descs, mt_import_count)` gives the unwinder their thunk addresses. `mt_sjlj_state_of(cpu)` returns one state in `sjlj.c`, since the game runs one guest thread.
- **Normal play.** The game does not throw, so the landing pads are dead in normal play and no HLE function raises. If one ever has to (an `NSException` the game catches), it calls `mt_sjlj_throw`.

## Consequences

- All 205 SjLj functions lift with their hook and label; their C compiles with clang strict flags and for the MSVC target. The flag check covers the resume path.
- **Tests.** `tests/recomp/sjlj/*.sjlj` are hand-written guest programs in the snippet format (bytes checked by Capstone and clang) with synthetic LSDAs: typed try/catch entered twice through a frame without a context, nested cleanups with a non-matching handler and a skipped (`call_site = -1`) frame, rethrow from a catch-all, an uncaught exception, a `throw()` violation, and the Chartboost form. `tests/test_recomp_sjlj.py` lifts and runs them with `sjlj.c` on every CI compiler, MSVC included. Locally, two real functions (`+[EAGLView_Instance Instance]`, Chartboost 0xD33F9) run with the image mapped: an exception thrown by `objc_msgSend` runs their real cleanup and reaches an outer handler.
- **MSVC.** `longjmp` on x64 unwinds with `RtlUnwindEx`; generated C has no destructors or `__finally`, so that only discards frames. No C local is read after the jump before it is written (the flag check), `mt_entry_lr` is never modified, and the guest state is in `*cpu`, which every frame reaches through the pointer passed down, so the `restrict` contract holds. A `jmp_buf` is 16-byte aligned on x64, so `sjlj.h` silences C4324 around its structures. GCC's `-Wclobbered` (in `-Wextra`) is silenced in `calls.h` for the same reasons.
- A function the compiler can prove never returns makes the code after its calls unreachable, which MSVC reports as C4702 under `/W4 /WX` (the first synthetic `nested` program hit it). Discovery's noreturn fixpoint already ends those paths in the game's code; #38/#40 should keep that in mind for the whole-program MSVC build.
- **How we'd know this was wrong:** a landing pad reached with a wrong `sp`/`r7` (the real-asset run checks both), an HLE raise that finds the host buffer gone (`MT_TRAP_SJLJ`), or frames on the SjLj chain outliving their C frames (they would trap on the next unwind, not corrupt memory).
