/* Control transfer between recompiled functions, imports and the runtime (ADR 0014).
 *
 * Calling convention of generated code. Every guest function is `void f_XXXXXXXX(mt_cpu *)`,
 * named after its guest function pointer (Thumb bit included). Arguments, results and every
 * other register are in cpu->r[] / cpu->d[] exactly as on the hardware; the C call only
 * transfers control:
 *
 *   bl / blx #imm   cpu->r[14] = return address (Thumb bit set); f_XXXXXXXX(cpu);
 *   call to a stub  cpu->r[14] = return address; MT_CALL_IMPORT(id);         (#36)
 *   blx rN          cpu->r[14] = return address; MT_CALL_INDIRECT(t, site);  (#35)
 *   b other_fn      MT_TAILCALL(f_XXXXXXXX(cpu));
 *   bx rN (not lr)  MT_JUMP_INDIRECT(t, site);
 *   bx lr, pop {pc} MT_RETURN(target, site);  a native C return
 *
 * Returns are C returns. A function remembers lr at entry (MT_FN_ENTER) and, with
 * MT_CHECK_RETURNS (debug builds and the tests), checks that it returns to that address; an
 * indirect jump to that address is a return too. Indirect targets are looked up in the
 * generated table (maplethief/recomp/dispatch.h); an unknown one stops the guest with
 * MT_TRAP_NO_FUNCTION. */
#ifndef MAPLETHIEF_RECOMP_CALLS_H
#define MAPLETHIEF_RECOMP_CALLS_H

#include <setjmp.h>
#include <stdint.h>

#include "maplethief/recomp/compiler.h"
#include "maplethief/recomp/cpu.h"

MT_EXTERN_C_BEGIN

#ifndef MT_CHECK_RETURNS
#define MT_CHECK_RETURNS 1
#endif

/* --- traps (implemented by the runtime; they stop the guest) -------------------------- */

enum mt_trap_kind {
    MT_TRAP_UNSUPPORTED = 1, /* instruction the lifter has no translation for; detail = raw */
    MT_TRAP_BAD_RETURN = 2,  /* return to an address other than lr at entry; pc = the return
                              * instruction, detail = target */
    MT_TRAP_FELL_OFF = 3,    /* execution ran past the last instruction of a function */
    MT_TRAP_UNREACHABLE = 4, /* a noreturn call returned, or a switch index was out of range */
    MT_TRAP_UNLIFTED = 5,    /* the function could not be lifted; detail = reason code */
    MT_TRAP_UDF = 6,         /* udf / bkpt / svc in guest code; detail = immediate */
    MT_TRAP_IMPORT = 7,      /* unimplemented import, stop policy (#36); pc = lr, detail = id */
    MT_TRAP_NO_FUNCTION = 8, /* indirect call or jump to an address with no function (#35);
                              * pc = the call or jump, detail = target */
    MT_TRAP_SJLJ = 9,        /* SjLj unwinder misuse (maplethief/sjlj.h); detail = address */
    MT_TRAP_UNCAUGHT = 10,   /* an exception no landing pad handles; detail = thrown type */
    MT_TRAP_TERMINATE = 11,  /* std::terminate: exception specification, call_site 0 */
    MT_TRAP_MEMORY = 12,     /* guest access outside guest memory (mt_mem_fault, ADR 0025);
                              * pc = the access (or an HLE function's caller), detail =
                              * address; maplethief/memory.h has the size */
    MT_TRAP_OBJC = 13,       /* message the ObjC runtime cannot deliver (unrecognized selector,
                              * receiver of no known class), stop policy (ADR 0026); pc = lr,
                              * detail = selector */
    MT_TRAP_ABORT = 14,      /* the guest aborted: abort(), __assert_rtn, a malloc error (an
                              * invalid free/realloc/delete) or a failed __*_chk check (ADR
                              * 0028), operator new without heap (ADR 0031); pc = lr, detail =
                              * the offending address (operator new: the size) or 0 */
    MT_TRAP_PURE_VIRT = 16,  /* __cxa_pure_virtual: a pure virtual function was called (ADR
                              * 0031); pc = lr, detail 0 */
    MT_TRAP_GCD = 18,        /* a GCD wait that can never end (dispatch_sync to the main queue
                              * on the main thread, a recursive dispatch_once, a wait for work
                              * nothing can run) or a bad queue/object (maplethief/gcd.h,
                              * ADR 0033); pc = lr, detail = the queue, group or predicate */
};

/* `pc` is the guest address of the trapping instruction (or the function). */
MT_NORETURN void mt_trap(mt_cpu *cpu, uint32_t kind, uint32_t pc, uint32_t detail) MT_NOEXCEPT;

#define MT_TRAP(kind, pc, detail) mt_trap(cpu, (kind), (pc), (detail))

/* --- function entry and return -------------------------------------------------------- */

/* Function-entry coverage (#53, ADR 0030). Off (0, the default) it adds nothing. With
 * MT_TRACE_ENTRY 1 (CMake: MAPLETHIEF_TRACE_ENTRY on maplethief_gen) every generated function
 * counts its entries in a static counter and registers itself with the runtime on the first
 * one (mt_trace_fn_first, maplethief/diag.h): one increment and one predictable branch per
 * call. No change to the recompiler's output: the name comes from __func__. */
#ifndef MT_TRACE_ENTRY
#define MT_TRACE_ENTRY 0
#endif

#if MT_TRACE_ENTRY
void mt_trace_fn_first(mt_cpu *cpu, const char *fn_name, const uint64_t *calls) MT_NOEXCEPT;
#define MT_FN_TRACE_ENTRY()                                                                        \
    do {                                                                                           \
        static uint64_t mt_fn_calls_;                                                              \
        if (MT_UNLIKELY(mt_fn_calls_++ == 0u)) {                                                   \
            mt_trace_fn_first(cpu, __func__, &mt_fn_calls_);                                       \
        }                                                                                          \
    } while (0)
#else
#define MT_FN_TRACE_ENTRY() ((void)0)
#endif

#define MT_FN_ENTER()                                                                              \
    const uint32_t mt_entry_lr = cpu->r[MT_LR];                                                    \
    (void)mt_entry_lr;                                                                             \
    MT_FN_TRACE_ENTRY()

/* Observed functions (#72, ADR 0053): the recompiler emits MT_FN_OBSERVE(fn) at the start of
 * each function of recomp/observe.py (fn its function pointer), before its first instruction,
 * so the runtime sees the arguments as the caller passed them. mt_observe_entry
 * (runtime/src/events/events.cpp, maplethief/events.h) only reads registers and guest memory:
 * the guest runs the same with or without it. */
void mt_observe_entry(mt_cpu *cpu, uint32_t fn) MT_NOEXCEPT;
#define MT_FN_OBSERVE(fn) mt_observe_entry(cpu, (fn))

/* Hooked functions (#143, ADR 0069): the host can replace or wrap each function of
 * recomp/hooks.py. The recompiler lifts its unchanged body as
 *     static void f_XXXXXXXX_original(mt_cpu *cpu) { MT_FN_ENTER(); ... }
 * and the function itself as
 *     void f_XXXXXXXX(mt_cpu *cpu) { MT_FN_HOOK(0xXXXXXXXXu, f_XXXXXXXX_original); }
 * mt_hook_entry (runtime/src/hooks/hooks.cpp, maplethief/hooks.h) runs the host's handler for
 * fn, if one is registered and enabled, and answers nonzero when the handler did the call; then
 * the function returns. Otherwise (0) the original runs, as a tail call, exactly as without the
 * hook. The handler gets `original` to call it itself (a wrapper). */
int mt_hook_entry(mt_cpu *cpu, uint32_t fn, mt_guest_fn original) MT_NOEXCEPT;
#define MT_FN_HOOK(fn, original)                                                                   \
    if (mt_hook_entry(cpu, (fn), (original)) != 0) {                                               \
        return;                                                                                    \
    }                                                                                              \
    MT_TAILCALL((original)(cpu))

/* `site` is the guest address of the return instruction (bx lr, pop {pc}, ldr pc, ...). */
#if MT_CHECK_RETURNS
#define MT_RETURN(target, site)                                                                    \
    do {                                                                                           \
        uint32_t mt_t_ = (target);                                                                 \
        if (MT_UNLIKELY(mt_t_ != mt_entry_lr)) {                                                   \
            mt_trap(cpu, MT_TRAP_BAD_RETURN, (site), mt_t_);                                       \
        }                                                                                          \
        return;                                                                                    \
    } while (0)
#else
#define MT_RETURN(target, site)                                                                    \
    do {                                                                                           \
        (void)(target);                                                                            \
        return;                                                                                    \
    } while (0)
#endif

/* --- indirect calls (#35) ------------------------------------------------------------- */

/* The host function for guest code address `target` (Thumb bit significant: functions are
 * keyed by their function pointer). For an unknown target it does not return: it stops the
 * guest with MT_TRAP_NO_FUNCTION at `site` (the guest PC of the call or jump), and the
 * runtime reports target, site, lr and the guest backtrace (mt_dispatch_describe). */
mt_guest_fn mt_dispatch(mt_cpu *cpu, uint32_t target, uint32_t site) MT_NOEXCEPT;

/* One entry of the generated table work/gen/dispatch.c, sorted by address. */
typedef struct mt_fn_entry {
    uint32_t addr;
    mt_guest_fn fn;
} mt_fn_entry;

#define MT_CALL_INDIRECT(target, site) mt_dispatch(cpu, (target), (site))(cpu)
#define MT_JUMP_INDIRECT(target, site)                                                             \
    do {                                                                                           \
        uint32_t mt_j_ = (target);                                                                 \
        if (mt_j_ == mt_entry_lr) {                                                                \
            return;                                                                                \
        }                                                                                          \
        MT_TAILCALL(mt_dispatch(cpu, mt_j_, (site))(cpu));                                         \
    } while (0)

/* --- imports (#36) -------------------------------------------------------------------- */

/* Every import has an id (generated, work/gen/mt_gen.h). The generated work/gen/imports.c
 * defines mt_import_names[] and the table storage; the runtime fills each slot at start-up
 * with its HLE implementation or a default handler that reports the import's name and the
 * caller (lr) and then continues or stops, per configuration. A handler reads its arguments
 * from cpu (AAPCS, soft-float: r0-r3, then the guest stack) and writes its results there. */
typedef void (*mt_import_fn)(mt_cpu *cpu, uint32_t id);
extern mt_import_fn mt_import_table[];
extern const char *const mt_import_names[];

#define MT_CALL_IMPORT(id) mt_import_table[(id)](cpu, (id))

/* --- SjLj exception hooks (#37) ------------------------------------------------------- */

/* The game's SjLj code stores a resume address inside the function into the function
 * context's jump buffer (`mov rX, pc; adds rX, #n; str rX, [jbuf, #4]`). At that store the
 * recompiler emits
 *     if (MT_SJLJ_SETJMP(jbuf_guest_addr)) { goto L_<resume address>; }
 * The runtime keeps a host jmp_buf per guest jump buffer (runtime/src/sjlj.c, ADR 0020). When
 * the unwinder installs a landing pad of that function, it restores r7 and sp from the guest
 * jump buffer into cpu and longjmps; generated code then continues at the resume label with
 * all guest state in cpu (no C local is live across the longjmp; the recompiler treats the
 * label as a function entry for the flag check).
 *
 * The host pair is _setjmp/_longjmp on Apple platforms, whose setjmp also saves the signal
 * mask (a system call on every call of an SjLj function); glibc's and MSVC's setjmp do not. */
jmp_buf *mt_sjlj_jmpbuf(mt_cpu *cpu, uint32_t guest_jbuf) MT_NOEXCEPT;

#if defined(__APPLE__)
#define MT_HOST_SETJMP(jb) _setjmp(jb)
#define MT_HOST_LONGJMP(jb, value) _longjmp((jb), (value))
#else
#define MT_HOST_SETJMP(jb) setjmp(jb)
#define MT_HOST_LONGJMP(jb, value) longjmp((jb), (value))
#endif

#define MT_SJLJ_SETJMP(guest_jbuf) MT_HOST_SETJMP(*mt_sjlj_jmpbuf(cpu, (guest_jbuf)))

#if defined(__GNUC__) && !defined(__clang__) && !defined(__cplusplus)
/* GCC's -Wclobbered (in -Wextra) warns about the flag locals of a function with a resume
 * label. They are dead there: the flag check proves no flag is read at the label before it
 * is written, mt_entry_lr is never modified, and every other value lives in cpu. */
#pragma GCC diagnostic ignored "-Wclobbered"
#endif

MT_EXTERN_C_END

#endif /* MAPLETHIEF_RECOMP_CALLS_H */
