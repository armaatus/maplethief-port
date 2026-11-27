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
 * Off by default: the developer switch --native-script-vm on|off (platform) registers the hook
 * on or off. C17 that also compiles as C++20; the implementation is runtime/src/scriptvm/. */
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

MT_EXTERN_C_END

#endif /* MAPLETHIEF_SCRIPTVM_H */
