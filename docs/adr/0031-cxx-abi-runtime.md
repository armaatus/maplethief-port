# 31. The C++ ABI runtime: new/delete on the guest heap, type_info vtables as guest data with host virtuals, pure virtual calls as a trap

- **Status:** Accepted
- **Date:** 2026-10-27
- **Issue:** #45
- **Builds on:** [ADR 0019](0019-import-thunks-and-hle-abi.md) (registry, data imports), [ADR 0020](0020-sjlj-exceptions.md) (the SjLj personality and its `can_catch` hook), [ADR 0025](0025-guest-memory-and-address-space.md) (guest heap), [ADR 0028](0028-libc-hle.md) (`MT_TRAP_ABORT`, guest stderr), [ADR 0030](0030-runtime-diagnostics.md) (crash report)

## Context

The game's C++ engine was built by LLVM-GCC against `libstdc++.6.dylib` (GCC 4.2's libsupc++). It imports 14 C++ ABI symbols ([ANALYSIS.md → Imports](../ANALYSIS.md#imports-333)): `operator new`/`new[]`/`delete`/`delete[]` (`__Znwm`, `__Znam`, `__ZdlPv`, `__ZdaPv`), `__cxa_atexit`, `__cxa_pure_virtual`, `std::terminate`, the three `__cxxabiv1` type_info vtables, `__gxx_personality_sj0` and the three `_Unwind_SjLj_*` functions. `__cxa_atexit` belongs to process start-up ([ADR 0027](0027-process-start-up.md)); the unwinder, the personality and `std::terminate` to [ADR 0020](0020-sjlj-exceptions.md). #45 has the rest, and the RTTI that exceptions and `dynamic_cast` would depend on.

Facts (measured 2026-10-27 on the armv7 slice):

- **`operator new` runs before `main`.** The two static constructors call `__Znwm` and `__Znam` ([ADR 0027](0027-process-start-up.md)). Together the four functions have 1,245 code sites ([re/imports.md](../re/imports.md#libstdc)).
- **RTTI is data the game never reads.** All 63 `type_info` objects get their vptr from a bind (addend 8) to one of the three vtable imports: 21 `__class_type_info`, 40 `__si_class_type_info`, 2 `__vmi_class_type_info`. `__dynamic_cast` is not imported. No code site references a `type_info` object or its name, and no code loads a vtable's type_info or offset-to-top slot (the vptr-relative loads at -4 or -8 that an inlined `typeid` or `dynamic_cast<void *>` compiles to). See [ANALYSIS.md → RTTI and vtables](../ANALYSIS.md#rtti-and-vtables-9).
- **No typed catch.** The LSDAs have cleanups, `throw()` specifications and Chartboost's `catch (...)`, nothing else ([ADR 0020](0020-sjlj-exceptions.md)). RTTI matters only for an exception that HLE code throws, and for the synthetic tests.
- **31 vtable slots are pure** and bound to `___cxa_pure_virtual` (`IState`, `IStateAdaptor`, `CMapleObject`, `CFloatingBGAlgorithm`). They are reached only if a constructor or destructor calls a pure virtual function, a bug.
- **The vtable imports are 64-byte data objects** (`recomp/imports.py`). GCC 4.2's vtable of a class type_info has offset-to-top, the type_info pointer and nine virtual functions: two destructors, `__is_pointer_p`, `__is_function_p`, `__do_catch`, two `__do_upcast`, `__do_dyncast` and `__do_find_public_src`.

## Options considered

**The virtual functions of the type_info classes.**

1. **Guest-callable host functions.** Each vtable slot would hold an address that `mt_dispatch` maps to a host function. That needs a registration table the dispatch lookup searches, but `dispatch.c` belongs to the generated code's library and the runtime library must link without it, so the table would need a new seam between the two. Nothing in the game would ever call through it.
2. **Guest vtables as data, virtuals on the host.** The vtables hold the ABI's layout, and the runtime knows each vtable's address from its init hook. A type_info's kind is its vptr (vtable + 8), and the host runs that kind's `__do_catch`/`__do_upcast` over guest memory. A guest call through a slot stops with a clear trap.

**The upcast algorithm.**

1. **GCC's `__vmi_class_type_info::__do_upcast` as written.** It walks the bases and uses the `__vmi` flags (`__non_diamond_repeat_mask`, `__diamond_shaped_mask`) to skip non-public bases and stop early.
2. **Every path, compared by where it ends.** Walk every base path from the thrown class, and record where each path that reaches the catch type ends: the last virtual base on the path and the offset from it. One end reached publicly is a match, two different ends are ambiguous, and a non-public path alone is no match. That is the same answer for every hierarchy, since the flags only say which paths GCC may skip, and it needs no trust in the flags. The game has two `__vmi` classes, both with two public non-virtual bases.

**A failed `operator new`.** (a) Throw `std::bad_alloc` through the unwinder, which needs a guest `bad_alloc` object and type_info. (b) Stop the guest. The game has no typed handler, so on the device the exception ends in `std::terminate` and `abort` anyway; only Chartboost's two `catch (...)` blocks could catch it.

**`__cxa_pure_virtual`.** (a) `MT_TRAP_TERMINATE`, which is what libstdc++ calls next. (b) A trap kind of its own, so the crash report names the bug.

## Decision

- **Files.** `runtime/src/hle/cxxabi.cpp` has new/delete and `__cxa_pure_virtual` (it needs the guest heap, so its registry entries are in the `#ifndef MT_HLE_CORE_ONLY` block). `runtime/src/hle/rtti.cpp` has the vtable init hooks and the matcher. It needs only `recomp.h`, so the SjLj test harness compiles it. The public header is `runtime/include/maplethief/hle/cxxabi.h`.
- **new/delete.** `__Znwm`/`__Znam` call `mt_heap_malloc`, with 0 bytes rounded up to 1 (a unique block, as in GCC). When the heap is exhausted, they log `operator new(N) called from 0x…: the guest heap is exhausted (std::bad_alloc): aborting` and stop the guest with `MT_TRAP_ABORT`, detail = the size (option b). `__ZdlPv`/`__ZdaPv` do nothing for NULL and free with `mt_heap_free`. A pointer that is no live block gets the malloc error line `free` logs, naming `operator delete`, then `MT_TRAP_ABORT` with the pointer.
- **`__cxa_pure_virtual`** writes libstdc++'s `pure virtual method called` to guest stderr and stops with the new trap kind **`MT_TRAP_PURE_VIRT` (16)** (option b). It is 16, not 15, because #47 was built at the same time and may take 15. The runtime's `mt_trap` prints the crash report: `pure virtual function called`, the caller (lr), the registers and the symbolized r7 backtrace. `std::terminate` keeps ADR 0020's `MT_TRAP_TERMINATE`, with the same report.
- **Vtables** (option 2 of the first question). The init hooks of the three vtable imports write offset-to-top 0, type_info 0, and in each of the nine virtual slots the vtable import's own thunk address, `MT_IMPORT_THUNK_BASE + 4 × id`. Nothing has a thunk there, so a guest call stops with `MT_TRAP_NO_FUNCTION` at that address, and the id points at the vtable. The hooks call `mt_cxx_set_type_info_vtables`, so the runtime knows the three addresses.
- **Host virtuals.** `mt_cxx_type_kind` reads the vptr. `mt_cxx_same_type` is GCC 4.2's `type_info::operator==`: the same object, the same name pointer, or equal name strings, where a name starting with `*` is file-local and compared by pointer only. Two addresses that are not both class type_infos (an ObjC class, a test's plain address) are equal only as addresses. `mt_cxx_upcast` is `__do_upcast` by option 2: `__si_` bases sit at offset 0, `__vmi_` bases at `offset_flags >> 8`, and a virtual base's offset is read from the object's vtable at that (negative) offset. `mt_cxx_can_catch` is `__class_type_info::__do_catch`: equal types, or an upcast of the thrown object to the catch type.
- **Plugging into the personality. Change to ADR 0020:** `mt_sjlj_can_catch_fn` gains an in/out `uint32_t *object`. A match through a base at a non-zero offset moves the thrown object to that subobject, GCC's `adjustedPtr`. Exception specifications pass NULL. Phase 1 stores the adjusted object in the exception (`mt_sjlj_exception.adjusted`), and `begin_catch` returns it. The boot sets `can_catch = mt_cxx_can_catch` right after `mt_sjlj_init` (`runtime/src/boot/boot.c`).
- **Imports.** The registry gains `__Znwm`, `__Znam`, `__ZdlPv`, `__ZdaPv`, `___cxa_pure_virtual` and the three vtable init hooks. Every C++ ABI import is then implemented: `__gxx_personality_sj0` is never called, and the unwinder binds its address (ADR 0020).

## Consequences

- **Tests.**
  - In CI: `tests/cpp/cxxabi_test.cpp` (doctest) covers new/delete, the abort paths, `__cxa_pure_virtual`'s message, trap and crash report with backtrace, the vtable layout, and the matcher. The matcher cases are equality by name, `__si_` chains, `__vmi_` offsets, non-public, ambiguous and virtual bases (a virtual diamond is one subobject, a non-virtual one is two), and plain addresses.
  - Also in CI, on every compiler: `tests/recomp/sjlj/rtti.sjlj` runs the matcher through the SjLj personality end to end. It throws through a two-level `__si_class_type_info` chain to a catch by base (a type_info from "another object file", matched by name), catches a base at +8 of a `__vmi_class_type_info` (the handler gets object + 8), and sends a private and an ambiguous base to `catch (...)`. The harness installs the matcher for every `.sjlj` program, so the older programs also check that plain type addresses still match only themselves.
  - Locally: `tests/test_hle_cxxabi_real.py` checks that every C++ ABI import of the slice is implemented, and `boot_real_test` boots to `UIApplicationMain` on the real `operator new` with no default-handler call.
- **Dead in normal play.** RTTI matching and `__cxa_pure_virtual` only run on a path that is already a bug or an HLE throw. new/delete are called all over the engine; each costs one `mt_heap_*` call.
- **How we'd know this was wrong:**
  - **A guest call reaches a vtable slot.** It shows as `MT_TRAP_NO_FUNCTION` at `0xFFFE0000 + 4 × id` of a vtable import. Then option 1 of the first question is needed.
  - **An HLE throw is caught in the wrong place.** For example, an `NSException` caught by a C++ clause. The matcher treats non-type_info addresses as plain addresses, so an ObjC typed `@catch` would need an ObjC class walk in `can_catch`. The game has none today.
  - **A heap exhaustion that the game survived on the device.** That would mean Chartboost's `catch (...)` handled a `std::bad_alloc`. Then `operator new` has to throw through `mt_sjlj_throw`.
