# 69. Guest function hooks: the host replaces or wraps a recompiled function

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #143
- **Builds on:** [ADR 0014](0014-recompiler-design.md) (the function shape, `MT_FN_ENTER`, direct calls and tail calls), [ADR 0016](0016-function-discovery.md) (the noreturn fixpoint), [ADR 0019](0019-import-thunks-and-hle-abi.md) (how HLE code reads guest arguments and calls back with `mt_call_guest`), [ADR 0053](0053-observed-functions-game-events-and-rumble.md) (observed functions)

## Context

M10's enhancements change what the engine does: widescreen (#86) moves the screen size, the camera clamp `0xC069` and the follow `0xC175`; fast text and cutscene skip (#90) shorten the scene steps; the Enhanced-only fixes of original bugs (#155) correct single functions; autosave (#150) calls the game's own save. The recompiler could only *observe* a function's entry (ADR 0053), which reads but never changes anything. Generated functions call each other directly in C, and `mt_dispatch` only sees indirect calls, so the host had no place to step in.

The proof case is `OpenPortalURLAndExit` `0x22955` ([docs/re/network.md](../re/network.md)): `CStateMainMenu::OnKeyPressed` `0x9462D` calls it for the main menu's three Nexon "NGB" portal items (popup 1, sub-menu 3, items 2–4). It passes the URL to `0x6015`, an empty stub where the Korean Nexon Game Box hand-off was, and then calls `0x8FF9`, which is `_exit(1)`. The decompiler and the bytes agree on what follows each of the three calls: `b 0x961A2` → `b 0x961EA`, the switch's break. Discovery knew `0x22955` as one of the 5 functions that never return ([ANALYSIS.md](../ANALYSIS.md)), so its callers were lifted with a trap after two of the calls, and with nothing after the third.

What had to be decided:

- **The seam** in the generated code: where the host gets the call, and how a wrapper still calls the original.
- **The host side**: who registers handlers, how they read the guest's arguments, how they are switched on and off.
- **Functions that never return** in the original but should in Enhanced.

## Options considered

For the seam:

1. **Patch the dispatch table only.** Indirect calls already go through `mt_dispatch`; a host function could stand in for a table entry. But almost every call is a direct C call, which never sees the table.
2. **A hook check at the function's entry, as `MT_FN_OBSERVE`.** `if (handled) return;` at the top of the lifted body works for a replacement. A wrapper, though, has to run the original body, and calling `f_X` again would hit the check again. A per-call bypass flag would work but is fragile with recursion.
3. **Split the function. Chosen.** The recompiler lifts a hooked function's unchanged body as `static void f_X_original(mt_cpu *)` and makes `f_X` a two-line wrapper, `MT_FN_HOOK(0xXu, f_X_original)`. Every caller, tail call and dispatch-table entry still names `f_X`, so every way in goes through the hook, and a handler that wraps calls `f_X_original` directly. The body keeps `MT_FN_ENTER`, its return check and its trace name (`f_X_original` still parses as its address).

For the host side:

1. **Handlers with their own calling convention** (arguments as C parameters). Each hook would need generated glue per signature.
2. **The HLE convention. Chosen.** A handler is entered with the guest registers as the caller left them, like an HLE import (ADR 0019), and reads them with `mt_args_begin` and writes results with `mt_ret_*`. One signature fits every function.

For functions that never return:

1. **Leave discovery alone and let the handler jump somewhere.** There is nothing after the call in the lifted caller to jump to: the code was never discovered.
2. **Discovery treats such a function as returning everywhere.** That changes a fact about the binary (the noreturn function count) and the Ghidra cross-check.
3. **A `returns` mark on the hook. Chosen.** Discovery still records the function as noreturn, but does not end its callers' flow at the call. The code after the call is discovered and lifted. With the hook off, the original still exits, so nothing changes in what the game does.

## Decision

**The recompiler** (`recomp/hooks.py`): `HOOKED` maps function pointers to `Hooked(name, why, returns)`, today only `OpenPortalURLAndExit` `0x22955` with `returns=True`. `LiftOptions.hook` holds the pointers that are lifted as body plus wrapper. It is empty by default, so snippets and the differential test (ADR 0017) lift as before. The whole-image lift (`recomp.prototype.IMAGE_OPTIONS`, used by `lift`, `all`, `coverage` and `census`) passes `HOOKED`. `discover(image, may_return=MAY_RETURN)` records a function in `may_return` that never returns as noreturn ("a host hook may return"), but its call sites fall through. For the game, the noreturn calls go from 481 to 478 (the three calls in `0x9462D`). The noreturn functions stay 5, and the Ghidra cross-check stays at 207 differences, 0 unexplained.

**The generated code's ABI** (`maplethief/recomp/calls.h`): `int mt_hook_entry(mt_cpu *, uint32_t fn, mt_guest_fn original)` and `MT_FN_HOOK(fn, original)`. The latter returns when the entry answers nonzero and otherwise tail-calls the original (`MT_TAILCALL`).

**The runtime** (`maplethief/hooks.h`, `runtime/src/hooks/`) has no SDL:

- The registry is keyed by function pointer. `mt_hook_register(fn, handler, user, enabled)` stores one handler per function; a second registration replaces the first. `mt_hook_set_enabled(fn, on)` switches it at run time, from any thread, and the next call sees it. The other calls are `mt_hook_enabled`, `mt_hook_unregister`, `mt_hook_hookable`, `mt_hook_get_stats` (entries, handled, passed per function) and `mt_hook_reset`. The trace channel is `hooks`.
- Only the functions the generated code hooks can be registered (`MT_HOOK_FN_*`, the table in `hooks.cpp`). Registering any other address answers 0. `tests/test_recomp_hooks.py` checks that `HOOKED`, the defines and the table agree.
- A handler is `int (mt_cpu *cpu, const mt_hook_call *call)`, entered like an HLE function. To **replace**, it does the work, writes the result (`mt_ret_*`), keeps r4–r11, sp and lr, and answers `MT_HOOK_HANDLED`. To **wrap**, it calls `call->original(cpu)` with lr unchanged, then answers `MT_HOOK_HANDLED`. To **pass**, it answers `MT_HOOK_PASS` and the original runs after it. Unlike an observer, a handler may write guest memory and call guest functions (`mt_call_guest`).
- **The port's hook:** `mt_hooks_install_portal_no_exit(enabled)` registers `OpenPortalURLAndExit`'s replacement. It logs `hooks: OpenPortalURLAndExit(<url>): not opened, the game goes on (the original quits with exit(1))` and returns. `OnKeyPressed` then takes the switch's break and the menu stays.

**The frontend:** `GameOptions::portal_no_exit`, set by the temporary `--portal-exit quit|stay` (default `quit`, the original). The game registers the hook at start-up, on or off as asked. The settings registry (#144) replaces the flag with a setting under the Enhanced preset (`fix_original_bugs`).

## Consequences

- Hooking another function takes one entry in `HOOKED`, one `MT_HOOK_FN_*` define with its line in `k_hookable`, and a handler registered by whoever owns the enhancement, plus `returns=True` if the function never returns. Regenerating `work/gen` is needed for a new entry. The `returns` mark changes the callers' generated code, but not what the game does while the hook is off.
- An empty list changes nothing: no `MT_FN_HOOK` is emitted, and discovery gives the original noreturn set. The snippet suite and the differential test run with the default options, which hook nothing.
- A hooked function costs one extra call per entry, `mt_hook_entry`'s table lookup, and a tail call. Only listed functions pay it.
- **CI:** `runtime_test`'s `hooks_test.cpp` tests the registry and `MT_FN_HOOK` on synthetic guest functions: replace, wrap with changed arguments and result, pass, switching on and off between calls, re-registration, statistics, an unknown address. It also runs the portal hook on a guest with the URL in an image segment, keeping r4–lr and never running the original. `tests/test_recomp_hooks.py` checks the lifter's body and wrapper and the lists. `tests/test_recomp_discover.py` checks `may_return` on the synthetic image.
- **Real assets:** `runtime.hooks_portal_on` and `runtime.hooks_portal_off` (`hooks_real_test`) boot the game to the main menu. They put `CStateMainMenu` on popup 1, sub-menu 3, items 2–4 and send OK (−5) through the game's own `OnKeyPressed`. On: three calls, all handled, and the game runs 2 more seconds of turns and GL frames in the main menu. Off: item 2 ends the run with `exit(1)`, as on the device.
- Whether the English build can reach that popup with a touch is still unknown (network.md's open question). The hook makes the answer harmless either way.

## Addendum (#155): the fixes of original bugs, under `fix_original_bugs`

- **The setting.** `fix_original_bugs` (Gameplay; Original off, Original + controller off, Enhanced on; `--fix-original-bugs on|off`) switches every fix of an original bug. It replaces the temporary `--portal-exit quit|stay` and `GameOptions::portal_no_exit`, which are gone. The game registers the fixes at start-up with `mt_hooks_install_original_bug_fixes(enabled)`. It covers crashes and dead ends only: a fix that changes gameplay belongs under Assists.
- **The catalogue.** [docs/re/original-bugs.md](../re/original-bugs.md) lists each bug the port found, with its address, trigger, effect and decision: fixed in Enhanced (a hook under the setting), neutralised in every mode (the port's host side never lets the trigger reach the game), or kept. It also has the steps for adding a fix.
- **A second hook:** `-[MainMenuView clickPlay:]` `0xAAAB9` (#122). `mt_hooks_install_early_play_guard` drops a click while the engine's current state (`SkyCanvas` `0x11A950`, `+0xC`) is still a `CStateTitle` (vtable pointer `0x119C98`). Such a click would make a `ChooseCharacterView` whose `drawRect:` reads the still empty `robinImages` and raises `NSRangeException` from `0xA6589`. The hook reads the same three words as #129's focus guard (`focus_engine_at_title`) and reads nothing in a guest whose image lacks the global. Any later click runs the original. An ObjC method is hooked like any function: the dispatch table's entry is the wrapper.
- **Tests.** `runtime_test` (CI): the guard on a synthetic guest (dropped in `CStateTitle`; passed with no canvas, in another state, and off), and both fixes switched together. `platform.early_tap` (real assets, setting off: exit 5 from `0xA6589`, as on the device) and `platform.early_tap_fixed` (`--fix-original-bugs on`: the tap at 0.1 s dropped, then Play at 4.5 s starts a new game). `runtime.hooks_portal_on/off` now take the Enhanced and the Original preset's value of the setting.
