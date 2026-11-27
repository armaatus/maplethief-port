/* The script VM on the host (#386, ADR 0226): the decompilation track's first system (ADR 0153).
 *
 * The game's script VM is a C++ port of Lua 2.4 (docs/re/script-vm.md). Its opcode loop,
 * Lua::execute 0x9BC11 (L, tfunc, base), is hooked (MT_HOOK_FN_LUA_EXECUTE, ADR 0069); with the
 * hook on, this host loop runs instead of the recompiled one. The VM's data stays in guest memory
 * in the original layout (the stack 0x1178EC..0x1178F4, the symbol table, the TFunc and its
 * DataReader), so everything around the loop sees what it always saw. The loop does the opcode
 * dispatch, the operand reads, the stack moves and the jumps itself, with the original's quirks
 * kept, and calls the guest's own VM helpers (growStack, the constant and symbol lookups,
 * createString, doCall and the others the original calls) with mt_call_guest, so what they
 * allocate or intern is the original's. CALLFUNC goes through Lua::doCall, which calls
 * Lua::callNative for a native and the hooked Lua::execute for a script function.
 *
 * Checks: a read outside the function's code or outside guest memory, a stack index outside
 * the stack, a write through a pointer outside guest memory. Each is logged on the `scriptvm`
 * trace channel (MAPLETHIEF_TRACE=scriptvm) and counted (mt_scriptvm_stats.checks); then the loop
 * does what the original does. Only where the original would fault on guest memory the host
 * cannot reach, it reads 0 or skips the write instead.
 *
 * Trace: with the channel on, every opcode (function, pc, name, operand, stack depth) and every
 * native call (index, name, arguments) is logged.
 *
 * Off by default: the developer switch --native-script-vm on|off|shadow (platform) registers the
 * hook on or off, or the shadow runs below. C17 that also compiles as C++20; the implementation
 * is runtime/src/scriptvm/. */
#ifndef MAPLETHIEF_SCRIPTVM_H
#define MAPLETHIEF_SCRIPTVM_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* Registers the host VM as Lua::execute's handler, on or off (a later call switches it). */
void mt_hooks_install_native_script_vm(int enabled) MT_NOEXCEPT;

/* Runs Lua::execute (L, tfunc, base) on the host and returns what the original returns (the
 * first result's stack index). The guest's sp is the caller's: the loop keeps its scratch (the
 * original's frame size) below it and restores sp, r4-r11 and lr. */
uint32_t mt_scriptvm_execute(mt_cpu *cpu, uint32_t lua, uint32_t tfunc, uint32_t base) MT_NOEXCEPT;

typedef struct mt_scriptvm_stats {
    uint64_t calls;   /* Lua::execute calls the host ran */
    uint64_t steps;   /* opcodes */
    uint64_t natives; /* CALLFUNCs of a native (a LUA_T_CFUNCTION slot) */
    uint64_t checks;  /* checks that found a problem (logged on the scriptvm channel) */
} mt_scriptvm_stats;

mt_scriptvm_stats mt_scriptvm_get_stats(void) MT_NOEXCEPT;
void mt_scriptvm_reset_stats(void) MT_NOEXCEPT;

/* --- the shadow runs (#430, ADR 0226) ---------------------------------------------------------
 *
 * The proof that the host loop equals the original, call by call: --native-script-vm shadow.
 * At each outermost Lua::execute (one comparison covers its whole call tree), the handler copies
 * guest memory outside the call's frame (the writable image, the heap up to its committed end and
 * its host bookkeeping, the stack from the entry sp up; the dead stack below the entry sp, where
 * the two leave different scratch, is left out) and the registers, then:
 *
 *   1. runs the original (nested executes too). Lua::callNative 0x9B37D is hooked as well
 *      (MT_HOOK_FN_LUA_CALL_NATIVE): each native call is recorded with the state just before
 *      it and just after it.
 *   2. puts the entry state back and runs the rewrite (the host loop, nested calls too). At each
 *      native call the state and the call (L, index, base, sp, lr) must be run 1's at the same
 *      call; the native is then replayed, not run again (its state after it put in), so what a
 *      native does outside guest memory (a sound, a popup, Game Center, an ObjC object) happens
 *      once.
 *   3. compares r0, r4-r11, sp and lr, memory and the heap's bookkeeping with run 1's end, and
 *      the number of native calls. Then the game goes on from run 1's end: the original's.
 *
 * The first difference of a run (where, the address and both words, the native calls so far and
 * the rewrite's last opcodes from the scriptvm recorder) is written to stderr and counted; the
 * process's first is kept (mt_scriptvm_shadow_first_difference). Guest thread only.
 *
 * Without an mt_mem (a synthetic guest's flat memory), all of cpu->mem is compared but the
 * 64 KiB below the entry sp, and there is no heap bookkeeping. */

/* The loop a shadow run checks: mt_scriptvm_execute's signature. */
typedef uint32_t (*mt_scriptvm_execute_fn)(mt_cpu *cpu, uint32_t lua, uint32_t tfunc,
                                           uint32_t base);

/* Registers the shadow runs as the handlers of Lua::execute and Lua::callNative, on or off
 * (replacing mt_hooks_install_native_script_vm's handler). `rewrite` NULL: the host loop,
 * mt_scriptvm_execute; a test passes its own. */
void mt_scriptvm_shadow_install(int enabled, mt_scriptvm_execute_fn rewrite) MT_NOEXCEPT;

typedef struct mt_scriptvm_shadow_stats {
    uint64_t runs;        /* outermost calls run both ways */
    uint64_t natives;     /* native calls recorded in run 1 and replayed in run 2 */
    uint64_t differences; /* runs with a difference */
} mt_scriptvm_shadow_stats;

typedef struct mt_scriptvm_shadow_difference {
    uint64_t run;        /* the shadow run (1 is the process's first) */
    uint32_t checkpoint; /* the native calls before the difference (run 1's count at the end) */
    uint32_t at_end;     /* 1: at the call's end; 0: at native call `checkpoint` + 1 */
    char what[64];       /* "memory", "heap bookkeeping", "r0", "sp", "native call index", ... */
    uint32_t address;    /* memory: the first differing byte */
    uint32_t original;   /* memory: the word at address & ~3 after the original; else its value */
    uint32_t rewrite;    /* the same after the rewrite */
} mt_scriptvm_shadow_difference;

mt_scriptvm_shadow_stats mt_scriptvm_shadow_get_stats(void) MT_NOEXCEPT;
/* 1 and *out if a run has differed since the start (or the last reset), else 0. */
int mt_scriptvm_shadow_first_difference(mt_scriptvm_shadow_difference *out) MT_NOEXCEPT;
/* The numbers and the first difference cleared. */
void mt_scriptvm_shadow_reset(void) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_SCRIPTVM_H */
