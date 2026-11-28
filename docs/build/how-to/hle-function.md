# Adding an HLE function

The game calls iOS's libraries through its import table. The runtime answers each import with a host function, an **HLE function**, registered under the import's symbol name ([ADR 0019](../../adr/0019-import-thunks-and-hle-abi.md)). An import with no entry gets the unimplemented-import handler, which stops the guest by default. This page adds one. For a method of a framework class (`-[NSString length]`) see [Adding a selector on a host class](host-selector.md) instead.

## 1. Find out what is missing

- **From a run.** An unimplemented import stops the guest with a crash report that names it (`MT_TRAP_IMPORT`, [Debugging a trap](trap.md)). To see every missing import of a run instead of the first, run with `MAPLETHIEF_UNIMPLEMENTED_IMPORT=continue` (log the first call of each, return 0 and go on) or `trace` (log every call), and `MAPLETHIEF_REPORT=-`: at exit the run prints the *unimplemented report*, every missing import and selector with its call count and first caller.
- **From the analysis.** [docs/re/imports.md](../../re/imports.md) lists every import of the armv7 slice with its library, its call sites and the functions that use it. The calls' arguments are in the Ghidra project ([naming contract](../../../tools/ghidra/README.md)).

The symbol name is the import's, leading underscore included: C's `ceil` is `_ceil`, `operator new(unsigned long)` is `__Znwm`.

## 2. Write the function

An HLE function is an `mt_import_fn`: `void mt_hle_NAME(mt_cpu *cpu, std::uint32_t id) noexcept`. It reads its arguments from the guest registers and stack with the cursor of `runtime/include/maplethief/hle/abi.h` (iOS armv7 rules: 64-bit values only 4-byte aligned) and writes its result back:

```cpp
// double ceil(double x)
void mt_hle_ceil(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    mt_ret_f64(cpu, std::ceil(mt_arg_f64(&a)));
}
```

- Arguments: `mt_arg_u32`/`_i32` (pointers too), `_u64`, `_f32`, `_f64`, `mt_arg_struct`; a `va_list` through `mt_args_va_list`. Results: `mt_ret_u32`, `_u64`, `_f32`, `_f64`, `mt_ret_struct`.
- Guest pointers become host pointers only through `mt_g2h` and `mt_g2h_str` (`maplethief/memory.h`), which are bounds-checked: never add a guest address to a host pointer yourself.
- Put it in the module of its library: `runtime/src/hle/libc.cpp` or `libc_sys.cpp` (libSystem's C library, [ADR 0028](../../adr/0028-libc-hle.md)), `cxxabi.cpp` (libstdc++), `runtime/src/cf/` (CoreFoundation), `runtime/src/cg/` (CoreGraphics), `runtime/src/foundation/` (Foundation's C functions), `runtime/src/gcd/`, `runtime/src/gles/`, `runtime/src/audio/`, and so on. The comment at the top of `runtime/src/hle/registry.cpp` names each.
- Declare it in the module's header, for libc `runtime/src/hle/hle.h`, next to its neighbours.
- A guest `abort()` never ends the host: stop the guest with `mt_trap` (`MT_TRAP_ABORT`). Anything worth logging goes to a trace channel (`mt_tracef("name", ...)`), off by default.
- A data import (a variable such as `___stderrp`) has no function. It has an *init hook*, `void init(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr)`, which fills the zeroed guest object at `addr`.

## 3. Register it

Add one line to `k_registry` in `runtime/src/hle/registry.cpp`, in its library's group, alphabetical by symbol:

```cpp
    {"_ceil", mt_hle_ceil, nullptr},
```

A data import has `nullptr` as its function and its hook third. An entry the recompiler's C harness can't link (it needs guest memory or ObjC) goes inside `#ifndef MT_HLE_CORE_ONLY`.

## 4. Test it

- **The behaviour**, in CI: a doctest case in the module's test file that calls the function as the guest calls it, arguments in r0–r3 and on the guest stack. `tests/cpp/libc_test.cpp`'s `Guest` (with `call`, `call_f64` and `traps`) does that for libc; `tests/cpp/cf_test.cpp`, `cg_test.cpp`, `foundation_test.cpp` and `cxxabi_test.cpp` have their own. A libc function the game imports also joins the list in `libc_test.cpp`'s case "the registry lists every libc import of the game".
- **Against the game**, locally: the `tests/test_hle_*_real.py` tests check each library's imports against the registry. They are `real_assets` tests and need `scripts/bootstrap` first. When the function closes an import that was owned elsewhere, update the exclusion list or count there.
- **In the game**: run it again with `MAPLETHIEF_REPORT=-`. The report no longer lists the import. `MAPLETHIEF_TRACE_HLE=_ceil` logs every call with its arguments and result ([Debugging a trap](trap.md#4-trace-what-led-up-to-it)).

Then [the quick check](../testing.md#before-you-push): `scripts/lint` and `scripts/test`.

## Worked example

Followed for [#333](https://github.com/armaatus/maplethief-port/issues/333) on a scratch branch: `_ceil` as above, declared in `hle/hle.h`, registered under libSystem's C library, and checked in `libc_test.cpp`'s "floor, trunc and pow take and return doubles in core registers" (`ceil(-2.5) == -2`, `ceil(2.1) == 3`). Four files and one build; `runtime.floor, trunc and pow take and return doubles in core registers` passed, and so did the whole `cmake --workflow --preset mac-debug`. The game doesn't import `ceil`, so the example was not merged.
