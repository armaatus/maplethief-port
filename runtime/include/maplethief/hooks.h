/* Guest function hooks (#143, ADR 0069): the host replaces or wraps a recompiled guest function,
 * so an enhancement or a fix of an original bug changes the engine in one contained place.
 *
 * Which functions can be hooked is decided when the code is generated: the recompiler lifts each
 * function of recomp/hooks.py (HOOKED) so that every call to it first asks mt_hook_entry
 * (MT_FN_HOOK in maplethief/recomp/calls.h). The functions are named here as MT_HOOK_FN_*;
 * tests/test_recomp_hooks.py checks that the two lists agree. Registering a handler for any
 * other address is refused: no call would ever reach it.
 *
 * A handler is entered like an HLE function (ADR 0019, maplethief/hle/abi.h): the guest
 * registers exactly as the caller left them, the arguments in r0-r3 and on the guest stack at
 * sp, the return address in lr. It reads them with mt_args_begin(cpu) and friends.
 *
 *   replace  do the function's work, write the result with mt_ret_* and answer
 *            MT_HOOK_HANDLED: the original does not run. As any function, keep r4-r11, sp and
 *            lr as they were (r0-r3 and r12 are free).
 *   wrap     change the arguments if needed, call call->original(cpu) (lr unchanged: the
 *            original returns to the caller's return address), look at or change the result,
 *            and answer MT_HOOK_HANDLED.
 *   pass     answer MT_HOOK_PASS without calling the original: it runs after the handler, as
 *            if there were no hook (a handler that decides per call).
 *
 * A handler may call other guest functions (mt_call_guest) and read and write guest memory.
 * Example (a replacement that makes the function return 0):
 *
 *     static int no_op(mt_cpu *cpu, const mt_hook_call *call) {
 *         (void)call;
 *         mt_ret_u32(cpu, 0u);
 *         return MT_HOOK_HANDLED;
 *     }
 *     mt_hook_register(MT_HOOK_FN_..., no_op, NULL, 1);
 *     mt_hook_set_enabled(MT_HOOK_FN_..., 0);  // back to the original, at any time
 *
 * The registry is process-wide and outlives guest processes. Register before the guest runs
 * or on the guest thread; mt_hook_set_enabled may be called from any thread at any time (the
 * next call of the function sees it). Trace: MAPLETHIEF_TRACE=hooks (every handled call).
 * C17 that also compiles as C++20; the implementation is runtime/src/hooks/. */
#ifndef MAPLETHIEF_HOOKS_H
#define MAPLETHIEF_HOOKS_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* The hookable functions' pointers (recomp/hooks.py lists the same). */
#define MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT 0x22955u /* (owner, url) */
#define MT_HOOK_FN_SAVE_QUEST_DATA 0x38F55u /* CDataMgr::SaveQuestData (data manager, hero) */

/* A call the handler is asked to handle. */
typedef struct mt_hook_call {
    uint32_t fn;          /* the hooked function's pointer (Thumb bit set) */
    mt_guest_fn original; /* the function's unchanged body: call it to wrap */
    void *user;           /* as registered */
} mt_hook_call;

enum { MT_HOOK_PASS = 0, MT_HOOK_HANDLED = 1 };

/* Called on the guest thread, inside the guest's call. Answers MT_HOOK_HANDLED or MT_HOOK_PASS. */
typedef int (*mt_hook_fn)(mt_cpu *cpu, const mt_hook_call *call);

/* Registers `handler` for function pointer `fn` (one handler per function: a second
 * registration replaces the first and keeps its statistics), enabled or not. Returns 0 if `fn`
 * is not a hookable function (MT_HOOK_FN_*) or `handler` is NULL, else 1. */
int mt_hook_register(uint32_t fn, mt_hook_fn handler, void *user, int enabled) MT_NOEXCEPT;
/* Removes fn's handler; the original runs again. */
void mt_hook_unregister(uint32_t fn) MT_NOEXCEPT;
/* Switches fn's handler on or off at run time. Returns 0 if fn has no handler. */
int mt_hook_set_enabled(uint32_t fn, int enabled) MT_NOEXCEPT;
/* 1 if fn has a handler and it is on. */
int mt_hook_enabled(uint32_t fn) MT_NOEXCEPT;
/* 1 if fn can be hooked: the generated code calls mt_hook_entry for it. */
int mt_hook_hookable(uint32_t fn) MT_NOEXCEPT;

typedef struct mt_hook_stats {
    uint64_t entries; /* calls of the function (mt_hook_entry) */
    uint64_t handled; /* calls the handler did (MT_HOOK_HANDLED) */
    uint64_t passed;  /* calls the original ran: no handler, off, or MT_HOOK_PASS */
} mt_hook_stats;

/* fn's numbers since the start of the host process (or the last reset). */
mt_hook_stats mt_hook_get_stats(uint32_t fn) MT_NOEXCEPT;
/* Every handler removed and every number zeroed (tests). */
void mt_hook_reset(void) MT_NOEXCEPT;

/* --- the port's own hooks (runtime/src/hooks/builtin.cpp) ---------------------------------- */

/* OpenPortalURLAndExit 0x22955: the main menu's three Nexon "NGB" portal items
 * (CStateMainMenu::OnKeyPressed 0x9462D: popup 1, sub-menu 3, items 2-4) pass a
 * w.nexonmobile.com/CP/ URL to an empty stub, and then the original quits with exit(1). The
 * hook logs the URL and returns, and the game goes on in the menu. Registers it, `enabled` or
 * not (the original, on the device). */
void mt_hooks_install_portal_no_exit(int enabled) MT_NOEXCEPT;

/* mt_hook_entry(cpu, fn, original) is declared in maplethief/recomp/calls.h: the generated
 * code's ABI. Tests call it as the generated code does. */

MT_EXTERN_C_END

#endif /* MAPLETHIEF_HOOKS_H */
