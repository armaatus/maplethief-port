# 19. Import thunks, the import tables and the HLE calling convention

- **Status:** Accepted
- **Date:** 2026-10-26
- **Issue:** #36

## Context

[ADR 0014](0014-recompiler-design.md) fixed the call side of imports: a call to a `__symbolstub1` stub is `cpu->r[14] = ret; MT_CALL_IMPORT(id);`, which calls `mt_import_table[id](cpu, id)`. The runtime fills the table at start-up, and a default handler logs the name and the caller (`lr`), then continues or stops. Every function import gets a guest code address, so a pointer to it is callable. Data imports are guest objects that the runtime binds. #36 has to settle what ADR 0014 left open: the ids, how imports are classified, how the table gets filled, where the information about data objects and binds comes from, the policy knob, and the calling convention HLE functions read.

Facts from the armv7 slice ([ANALYSIS.md](../ANALYSIS.md), `recomp.loader.Image`, `tests/test_recomp_imports_real.py`):

- **Imports and stubs.** There are 333 undefined `nlist` symbols. 217 have one `__symbolstub1` stub each, and 116 have none (Mach-O kind `address`, `class` or `metaclass`).
- **Function pointers without stubs.** 5 of the stub-less imports are functions that the game only reaches through a pointer. `___cxa_pure_virtual` sits in 31 vtable slots. `___gxx_personality_sj0` and `___objc_personality_v0` are stored in SjLj function contexts. `_realloc` is called through a `__data` pointer by bson (Chartboost). `dyld_stub_binder` is used by `__stub_helper` only. The other 111 are data: 79 ObjC class and metaclass objects, plus 32 variables and constants such as `_kCFBooleanTrue`, `__NSConcreteStackBlock`, `___stderrp`, `_CGRectZero`, `___CFConstantStringClassReference` and the three `__cxxabiv1` type-info vtables.
- **Binds.** dyld writes 1,055 binds and 213 lazy binds. 63 of the binds carry addend 8: type-info objects point 8 bytes into their vtable import. The 4 weak-definition binds (ordinal 0, `operator new`/`delete`) cover words that also have an ordinary bind.
- **64-bit arguments.** The calling convention is not base AAPCS for 64-bit values. At 0x8ED8 the game calls `CFRunLoopTimerCreate(allocator, CFAbsoluteTime fireDate, CFTimeInterval interval, flags, order, callout, context)` with `fireDate` in r1:r2 and `interval` split between r3 and `[sp]`. The rest follows at `[sp+4]` to `[sp+0x10]`. The 64-bit values are 4-byte aligned. This is Apple's iOS armv7 ABI, which clang implements as its APCS variant for Darwin. Base AAPCS would put `fireDate` in r2:r3 and `interval` at `[sp]`.
- **Structure results.** A structure larger than 4 bytes comes back in memory. At 0xD7EC0, `CGRectInset(rect, 5, 5)` gets the result pointer in r0, the CGRect in r1, r2, r3 and `[sp]`, and `dx`/`dy` at `[sp+4]` and `[sp+8]`. The struct-returning imports are `_objc_msgSend_stret`, `_CGRectInset`, `_CGAffineTransformMakeRotation` and the three `_CATransform3D*` functions.

## Options considered

### Ids

1. **Stub order** (the #30 prototype). Ids would cover only the 217 function imports, and data imports would need a second numbering.
2. **`nlist` order over all 333.** This is the order of the loader manifest and of `mtport.imports`, so it is stable for one binary. A single id space covers functions and data.

### Filling the table

1. **Generated static initializer.** `imports.c` would name the HLE function of each slot (`mt_hle__memcpy` or the default handler). The generator would need a list of the functions the runtime implements, and every new HLE function would mean re-running the recompiler and rebuilding the generated code.
2. **By name at start-up.** The runtime keeps a registry `{name, fn, init}` and copies matching entries into `mt_import_table` before the guest runs. Adding an HLE function only touches the runtime, and generated code never depends on what is implemented. The cost is about 333 × registry-size `strcmp` once at start-up, well under a millisecond.

### Data objects and binds

1. **The runtime parses `work/recomp/loader.json`.** That means a JSON reader in the runtime. The sizes of data objects are not in the Mach-O at all, so they would still need a table somewhere.
2. **Generated tables.** `imports.c` carries a descriptor per import (flags, guest code address, data size and alignment) and the bind list (slot, id, addend). The runtime places objects and writes pointers with no parsing. The size and alignment table lives in `recomp/imports.py`, next to the classification it belongs to.

### Guest code addresses of function imports

1. **The stub address for every function import that has one.** This matches the original binary. For the 5 stub-less ones, the runtime would assign addresses at start-up, but then the generated dispatch table could not contain them.
2. **The stub address, else `MT_IMPORT_THUNK_BASE + 4 * id` (0xFFFE0000 + 4 × id), fixed at generation time.** The range lies above any guest mapping. With the bounds-checked memory model, it is outside the window anyway. Guest code never reads it; the address only keys the dispatch.

## Decision

- **Ids** are the `nlist` index over all imports. `MT_IMPORT_COUNT` is 333 for this slice.
- **Classification** lives in `recomp/imports.py`. An import is a *function* if it has a stub or is in `ADDRESS_FUNCTIONS`. Otherwise it is *data*: an ObjC class or metaclass (20-byte `class_t`), or an entry of `DATA_OBJECTS` with its size, alignment and description. An `address` import that is in no table becomes an `ImportTable.problems` entry, and gets a 64-byte, 16-aligned object so that the program still links. The real-asset test requires zero problems. The flags are `MT_IMPORT_FUNCTION`, `MT_IMPORT_DATA`, `MT_IMPORT_WEAK` and `MT_IMPORT_STRET`.
- **Generated tables.** `work/gen/imports.c` holds:
  - `mt_import_names[]`, `mt_import_table[]` (storage) and `mt_import_count`;
  - `mt_import_descs[]`;
  - `mt_import_binds[]`: the bind and lazy-bind streams, ascending by slot. The weak-definition stream is left out.
  - `mt_import_thunks[]`: one `{guest code address, thunk}` per stub and per stub-less function import, ascending. Each thunk is a static `void mt_import_thunk_N(mt_cpu *)` that does `MT_CALL_IMPORT(N)`.

  The types are in `runtime/include/maplethief/recomp/imports.h`, which `recomp.h` includes. `recomp.output` keeps calling `ImportTable.c_source()`.
- **Runtime** (`runtime/include/maplethief/hle/imports.h`, `runtime/src/hle/`):
  - `mt_imports_link` fills every slot by name from `mt_hle_registry()`. Function imports with no implementation, and the never-called slots of data imports, get `mt_import_unimplemented`.
  - `mt_imports_place` gives every import its guest address. A function import gets the address from its descriptor. A data import gets a zeroed object from an allocator the memory model provides (#42), and then its registered `init` hook runs.
  - `mt_imports_apply_binds` writes `address + addend` into every slot.
- **Unimplemented imports.** The default handler logs one line: `unimplemented import <name> (id <id>, <library>) called from 0x<lr>: stopping|returning 0`. With `stop`, the default, it then calls `mt_trap(cpu, MT_TRAP_IMPORT, lr, id)` (new trap kind 7). With `continue`, it sets r0 = r1 = 0 and returns, logging only the first call of each import. `trace` logs every call. The knob is the environment variable `MAPLETHIEF_UNIMPLEMENTED_IMPORT=stop|continue|trace`, read by `mt_import_config_from_env`. The same setting is the `mt_import_config` struct, together with a log sink, for the runtime's own settings and for tests. A data import's slot always stops. The caller is the return address in `lr`. For a guest tail call to an import, that is the return address of the function that made the tail call.
- **Calling convention for HLE functions** (`runtime/include/maplethief/hle/abi.h`, header-only C17/C++20):
  - An `mt_args` cursor reads r0-r3 and then the guest stack, with soft-float floats and doubles in core registers.
  - 64-bit values take the next two words with no alignment, the iOS rule, and may be split between r3 and the stack. `mt_args_begin_aapcs` gives the even-pair rule for code that follows base AAPCS.
  - Structures are passed by value as words. `mt_args_begin_stret` takes the hidden result pointer from r0. `mt_args_va_list` reads a guest `va_list`; variadic arguments follow the same rules as fixed ones.
  - `mt_ret_*` write results: r0, r0:r1, a structure of up to 4 bytes in r0, or a larger one at the result pointer.
  - `mt_call_guest` calls a guest function pointer back through `mt_dispatch`.
- **First implementations.** The libgcc integer helpers (`___divsi3`, `___modsi3`, `___udivsi3`, `___umodsi3`, `___divdi3`, `___udivdi3`, `___floatdisf`) use compiler-rt's results: a quotient of 0 on division by zero, and INT_MIN / -1 wraps. Every other import goes to the default handler until M4.
- **Code style of the runtime's import code.** It is plain C++20 with no exceptions, no `new` and no function-local statics with guards. The recompiler's C test harness can then compile it with `-fno-exceptions` and link it into a C program (`tests/recomp_harness.py`, `IMPORT_RUNTIME`).

## Consequences

- **CI.** It tests every piece without the IPA:
  - doctest in `tests/cpp/imports_test.cpp` covers the ABI cursor, linking, the policies, the environment variable, placement and binds, and the libgcc helpers;
  - pytest in `tests/test_recomp_imports.py` lifts a Thumb snippet that calls an implemented and an unimplemented import stub, links it with the generated table and the runtime, and runs it under each policy.

  Locally, `tests/test_recomp_imports_real.py` checks all 333 imports, the 1,268 binds and every default-handler log line.
- **#35** puts `mt_import_thunks` (or `ImportTable.thunks`) into the dispatch lookup, so that `blx` to a vtable's `___cxa_pure_virtual` or a stub address reaches the import.
- **#37** implements `__Unwind_SjLj_*` as registry entries. Its personality routines are function imports with thunk addresses.
- **#42** reserves `[0xFFFE0000, +4 × count)` and provides the load-time allocator. **#43** calls link, place and bind at start-up.
- **#44–#52** add registry entries, including `init` hooks for data objects such as `_kCFBooleanTrue` or the CFString class. **#53** builds its unimplemented-import report on `mt_import_unimplemented_calls`.
- A new data import in a different binary shows up as a problem, not as a silent default.
- **How we'd know this was wrong:**
  - **Wrong argument order.** An HLE function reads its arguments shifted. The evidence above says the iOS rule is right for this binary; a second call site that disagrees would reopen it.
  - **Objects too small.** A data object turns out bigger than the table says. The ObjC runtime (#47) may need class objects larger than `class_t`, which only means changing the table.
  - **Start-up time.** Name lookup at start-up shows in profiles. Then sort the registry and use binary search.
