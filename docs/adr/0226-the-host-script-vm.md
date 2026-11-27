# 226. The host script VM: Lua::execute's loop on the host, the guest's helpers kept

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#386](https://github.com/armaatus/maplethief-port/issues/386); the proofs it leaves: [#430](https://github.com/armaatus/maplethief-port/issues/430) (shadow runs, the regress digest), [#431](https://github.com/armaatus/maplethief-port/issues/431) (every shipped script through both)
- **Builds on:** [ADR 0153](0153-decompilation-track-script-vm-first.md) (the decompilation track, the script VM first, the three proofs), [ADR 0069](0069-guest-function-hooks.md) (guest function hooks)

## Context

ADR 0153 opened the decompilation track with the script VM: "`Lua::execute`, its call and stack machinery and the undump, hooked at their entries; the native handlers stay guest code behind `Lua::callNative`", behind a developer switch that stays off until three proofs hold. #386 builds it.

What the original is, measured on the recompiled code (`work/gen`, 2026-11-27):

- `Lua::execute` `0x9BC11` (L, tfunc, base) is the opcode loop: 1,527 Thumb instructions of unoptimised code, a `tbh` over 0x00–0x42, every temporary spilled to its own 0x2BC-byte frame (6,760 lines of lifted C). It returns the first result's stack index.
- The loop itself only dispatches, moves 8-byte slots on the stack (`0x1178EC`..`0x1178F4`) and moves the TFunc's operand count (`+0x1C`) and its DataReader (`+0x20`: buffer `+4`, position `+8`, start `+0xC`). Everything else is a call: `growStack`, `toNumberCheck`, `comparison`, `equalObjects`, `pushGlobal`, `pushIndexed`, `storeIndexed0`, `Table::constant` / `symbol` / `createString`, `FunctionSlots::get`, `linkFunction`, `adjustTop`, `setLine`, `doCall`, `error`, `sprintf` and `operator new[]` (the addresses are in [re/script-vm.md](../re/script-vm.md#the-host-vm)).
- Reading every case against the documented opcode table found five behaviours the docs did not have: CONCOP runs the arithmetic's number check on both operands (so two strings become numbers) and then formats their value words as string pointers; RETCODE rewinds the reader before reading its operand, so it reads the function's first byte; STOREINDEXED on a non-array builds Lua 2.4's fallback frame and leaves its two extra slots on the stack, with `Lua::error(L, 6)` where the fallback call was; EQOP and NOTOP give nil with a value word of 1; PUSHNIL writes the tag only. None occurs in the shipped files (the opcodes involved don't, or only in the shape that hides it).

What had to be decided: which entries to hook in this first step, where the rewrite stops, what "checked" means while it must stay equal, and the switch.

## Options considered

For the entries:

1. **All of ADR 0153's list at once:** `execute`, `doCall`, `callFunction`, `undump`, `reset`. The undump (41 functions) and `reset` allocate, intern and free; rewriting them on the host means a host copy of the string table's hashing and the allocator's order, and each must be proved equal by the same harness that does not exist yet. Too much for one step, and it gains nothing that a checked loop does not already give.
2. **`Lua::execute` only. Chosen.** It is where the bytecode is interpreted: every operand read, every stack index and every one of the eight differences from Lua 2.4 is in it, so it is where checks and a trace pay off. `doCall` and the natives are reached through it: CALLFUNC calls the guest's `doCall`, which calls `callNative` for a native and the hooked `execute` for a script function, so nested calls run on the host too.

For the boundary inside the loop:

1. **Everything on the host**, the helpers rewritten. Then equality depends on reproducing the allocator, the string table and `growStack`'s copying exactly.
2. **The loop on the host, the guest's helpers called. Chosen.** The host does the dispatch, the operand reads (the same two words the original's `TFunc::fetchU8/U16/U32` move), the stack moves, the jumps and the quirks, and calls each helper with `mt_call_guest` with the original's arguments in the original's order. What a helper writes is then the original's by construction, and a later step can move a helper to the host behind the same harness.

For the checks: the host checks an operand read against the function's code size, a stack index against the stack, and every guest address against guest memory. A problem is logged on the `scriptvm` trace channel and counted, and then the loop does what the original does (reads past the code, writes below the stack). Only where the original would fault on an address the host cannot reach does it read 0 or skip the write. Refusing bad bytecode outright would make the rewrite differ from the original on exactly the inputs a check is for; that choice belongs to a later script pack (M13), not to a rewrite that must prove itself equal first.

## Decision

- **The hook:** `Lua::execute` `0x9BC11` joins `HOOKED` (`recomp/hooks.py`), `MT_HOOK_FN_LUA_EXECUTE` and `k_hookable`. `Lua::doCall`, `Lua::callFunction`, `Undump::undump` and `Lua::reset` stay guest code in this step.
- **The host VM** (`runtime/src/scriptvm/scriptvm.cpp`, `maplethief/scriptvm.h`): `mt_scriptvm_execute(cpu, L, tfunc, base)` runs the loop over guest memory; `mt_hooks_install_native_script_vm(enabled)` registers it as the handler. It keeps the original's frame size below the caller's sp as its scratch (CONCOP's 100-byte buffer at the original's offset `+0x17C`, `comparison`'s fifth argument at sp), restores sp and r4–r11, and keeps every quirk: the eight differences, and the five behaviours above. `mt_scriptvm_get_stats` counts calls, opcodes, native calls and checks.
- **Trace:** `MAPLETHIEF_TRACE=scriptvm` logs every opcode (TFunc, pc, name, the next four bytes, base, depth), every native call (index, name from `g_scriptNativeNames` `0x117950`, the argument slots) and every check.
- **The switch:** `--native-script-vm on|off` (`GameOptions::native_script_vm`), a developer switch, not a setting; **off by default**. It stays off until #430 (shadow runs at the hook over `scripts/regress` and the NPC routes, and the same regress digest on and off) and #431 (every function of the 307 shipped scripts through both) hold; their addendum here decides the default.

## Consequences

- Hooking `execute` regenerates `work/gen` (one new wrapper and body); the game is unchanged while the switch is off, which is the default and what every scripted run uses.
- The shadow-run comparison of #430 must leave out the dead stack below the hooked call's entry sp: the original's frame scratch and the host's differ there by design, and nothing reads it after the call.
- **CI:** `runtime_test`'s `scriptvm_test.cpp` runs hand-made bytecode for each of the 67 opcodes, the eight differences as far as they live in the loop (1–5 in full; 6 as PUSHFUNCTION's `linkFunction` onto `L+0xC`; 7 as a builtin's CALLFUNC handed to `doCall` unchanged, its no-op being `callNative`'s; 8 is `Lua::reset`'s, outside the loop) and the five behaviours on a synthetic guest, with recording stand-ins for the guest helpers (`scriptvm_test_dispatch`): the stack, the TFunc and reader words, the helpers' arguments, the checks, and the hook on and off. `tests/test_recomp_hooks.py` checks that `HOOKED`, the define and the table agree.
- **Not proved here:** equality on the real game. That is #430 and #431; until they pass, the switch is a developer's tool and the trace is its use.
- How we'd know this was wrong: #430's shadow runs find differences inside the loop that come from the helper boundary (a helper that reads the original's frame, for example), which would mean the boundary has to move.

## Addendum (#430): the shadow runs and the regress digest

ADR 0153's proofs 1 and 3, decided and measured on 2026-11-27 (delegated by Joris).

**The harness.** `--native-script-vm shadow` (`mt_scriptvm_shadow_install`, `runtime/src/scriptvm/shadow.cpp`, `MAPLETHIEF_NATIVE_SCRIPT_VM=shadow` in the scripted runs, `scripts/regress --native-script-vm shadow`) registers itself as `Lua::execute`'s handler. At the outermost call (nested calls belong to the run they are in) it copies guest memory outside the call's frame: the writable image, the heap up to its committed end with its host bookkeeping, the stack from the entry sp to its top. The dead stack below the entry sp is not compared. It runs the original (`call->original`), puts the copy back, runs the host loop, and compares r0, r4–r8, r10, r11, sp, memory and the heap's bookkeeping (its top, live blocks and free lists; not the committed end, since pages one run committed stay committed when the copy goes back). The game then goes on from the original's end state, so a shadow run plays the original.

Options for the natives: run them in both runs, or **record them in the first run and replay them in the second (chosen)**. A native reaches state outside guest memory: sounds, popups, Game Center reports, ObjC host objects keyed by guest addresses. Running it twice would do those twice and leave the host's tables out of step with a restored heap. So `Lua::callNative` `0x9B37D` joins `HOOKED` (`MT_HOOK_FN_LUA_CALL_NATIVE`). In run 1 each native call is recorded with its arguments (L, index, base, sp, lr) and the state just before and just after it. In run 2 the same call must come in the same state, a checkpoint compared like the end. Then run 1's after-state is put in instead of running the native. A difference is therefore found at the first native call after it (or at the end), at its lowest address, and reported with both words, the native calls so far and the host loop's last opcodes from the `scriptvm` recorder. It goes to stderr and is counted, and a scripted run with any difference fails.

**r9.** The first regress run in shadow mode found 6 differences in 54 calls, all in r9. Apple's armv7 ABI makes r9 a scratch register, not a callee-saved one: the original's natives leave it changed and the host loop puts back the caller's. Nothing can rely on it after the call, so the comparison leaves it out.

**Results** (Debug for the replay, Release for the playthrough, the Mac):

| Run | Outermost calls | Native calls | Differences |
|---|---|---|---|
| `scripts/regress --native-script-vm shadow` (title to the first fight) | 54 | 224 | 0 |
| `MAPLETHIEF_NATIVE_SCRIPT_VM=shadow scripts/playthrough`, chapter 1 a–m and chapter 2 a–f (19 parts) | 2,305 | 9,943 | 0 |
| the same, chapter 3, its side quests, the ending and the post-game (19 parts, from chapter 2 f's save) | 1,894 | 8,717 | 0 |

The 38 parts are the whole played chain, every route that talks to NPCs: 4,199 outermost calls and 18,660 native calls, none different. One part, `play.chapter_2_f_synthetic`, failed a check of its own ("the trip kept the hero": one item stack 80 before the trip, 77 after). It fails the same way with the switch off, so the cause is in the branch the run was built from, not in the host loop. Its shadow runs (70 calls) showed no difference, and chapter 3 was played from its save.

`scripts/regress` gives `8a2f05733c8d4603` with `--native-script-vm on`, `off` and `shadow`. That is the default's digest since 2026-11-05. With `on`, the host loop ran 66 calls, 1,620 opcodes and 224 native calls with 0 checks. No original fault had to be listed: the host loop refuses nothing the shipped scripts do.

**The default.** It stays **off**. ADR 0153 asks for three proofs and #431's (every function of the 307 shipped scripts through both) is still open. The shadow harness is what #431 can run them through. When #431 holds, its addendum here turns the switch on by default.

**CI.** `runtime_test`'s `scriptvm shadow:` cases run the harness on a synthetic guest whose `execute` body is the host loop and whose `doCall` reaches a hooked `callNative`. The host loop against itself, with scratch below the entry sp, shows no difference, and the native runs once. Deliberately different rewrites are caught at their first differing write: a late write at its address and both words at the end, an early one at the native call after it (though a lower address differs later), a wrong r0. Every case leaves the original's state behind.
