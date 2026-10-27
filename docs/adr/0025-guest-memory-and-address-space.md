# 25. Guest memory: one fixed 512 MiB arena in a 4 GiB reservation, host-side heap bookkeeping, and the iOS fallback

- **Status:** Accepted
- **Date:** 2026-10-27
- **Issue:** #42
- **Refines:** "Memory" in [ADR 0014](0014-recompiler-design.md), which fixed the access API (`cpu->mem + addr`, `MT_MEM_CHECKED` 0/1) and left the address space to #42

## Context

ADR 0014 settled how recompiled code reaches guest memory: the host address is `cpu->mem + guest address`, every access goes through `memcpy` in `maplethief/recomp/mem.h`, and the image sits at its link-time addresses (slide 0), so the lifter folds `__TEXT` addresses and literal-pool loads into constants. There are two models behind one API. `MT_MEM_CHECKED 0` has no checks and relies on a 4 GiB reservation. `MT_MEM_CHECKED 1` checks `addr + size <= cpu->mem_size` and calls `mt_mem_fault(cpu, addr, size, pc)` on a miss. This ADR decides what the runtime puts behind `cpu->mem`.

Facts that shape it ([ANALYSIS.md](../ANALYSIS.md)):

- The armv7 image is `__PAGEZERO` 0x0–0x1000, `__TEXT` 0x1000–0xF1000 (r-x), `__DATA` 0xF1000–0x11B000 (rw), and `__LINKEDIT` 0x11B000–0x126000 (r). It is not position-independent: no `MH_PIE`, and the rebase stream is empty. It must therefore load at exactly these addresses, which the runtime does by mapping the Mach-O's `LC_SEGMENT`s.
- Guest pointers are 32 bits everywhere: ObjC objects, CF objects, blocks, and the heap blocks behind `malloc`/`calloc`/`realloc`/`free` (imported, and also reached through a function-pointer table in `__data`). Everything the runtime creates for the guest (data imports, CFStrings, ObjC objects) needs a guest address.
- No thread is ever created ([re/timing-threading.md](../re/timing-threading.md)), but the runtime still needs per-thread stacks for the main thread and for anything run on a queue.
- The game reads uninitialised heap bytes: 20 bytes of `theif` and 9 of `quest` in the save data are never written. So the heap should behave like iOS's: fresh memory is zero, reused memory keeps its old bytes, and nothing is scribbled.
- Host page sizes differ. x86-64, Windows and most Linux hosts use 4 KiB pages; Apple arm64 (macOS and iOS) uses 16 KiB. Guest segments are only 4 KiB aligned (0xF1000 is not a multiple of 0x4000), so a host page can hold parts of two segments.
- iOS limits a process's virtual address space. A reservation of 4 GiB plus guard may be refused unless the app is signed with the extended-virtual-addressing entitlement, which depends on the signing account. The issue requires a fallback that does not need it.

## Options considered

**Address space.**

1. **Reserve all 4 GiB, map things where they fall.** This is simple, but on iOS there is no fallback, and heap and stack addresses depend on the host.
2. **A small bounds-checked window everywhere** (`MT_MEM_CHECKED 1` only). It works on iOS. But Release builds then pay for a compare and a branch on every one of the binary's guest accesses, which ADR 0014's reservation model exists to avoid.
3. **One fixed layout inside a 512 MiB arena at guest 0, backed by a 4 GiB + guard reservation where the host allows it and by the arena alone where it does not.** Guest addresses are the same on every host and in every model, and only the reservation behind them changes. Code compiled with `MT_MEM_CHECKED 0` needs the full reservation (anything beyond the arena faults in hardware). Code compiled with `MT_MEM_CHECKED 1` runs on either.

**Heap.** (a) A classic in-band allocator (dlmalloc-style headers in guest memory). (b) A third-party allocator, which would be a new dependency under [ADR 0003](0003-dependencies.md). (c) Our own allocator with the bookkeeping on the host side.

## Decision

Option 3 for the address space and (c) for the heap. The runtime's interface is `runtime/include/maplethief/memory.h` and the implementation is `runtime/src/mem/` (`memory.cpp`, `heap.cpp`, `vm.cpp`).

**Layout** (guest addresses, the same on every platform):

| Range | Use |
|---|---|
| 0x00000000–0x00000FFF | page zero (`__PAGEZERO`), never mapped |
| 0x00001000–0x00FFFFFF | the image: the Mach-O's segments at their vmaddr |
| 0x01000000–0x1EFFFFFF | the guest heap (480 MiB), grown and committed on demand |
| 0x1F000000–0x1FFFFFFF | guest stacks (16 MiB), each above a 64 KiB no-access guard |
| 0x20000000– | outside guest memory: `cpu->mem_size` = `MT_GUEST_ARENA_SIZE` = 512 MiB |

A device of the game's era had 128–512 MiB of RAM in total, so 480 MiB of heap is more than the game could ever have used.

**Reservation.** `mt_mem_create` reserves address space with no access and no commit charge: `mmap(PROT_NONE, MAP_NORESERVE)` on POSIX, `VirtualAlloc(MEM_RESERVE, PAGE_NOACCESS)` on Windows. Pages are committed read-write only when the image, the heap top or a stack needs them, and stacks are decommitted when freed. `mt_mem_config.reservation` chooses the backing:

- `MT_MEM_RESERVE_AUTO` (the default): 4 GiB plus `MT_GUEST_RESERVE_GUARD` (64 KiB, so that an 8-byte access at 0xFFFFFFFF stays inside) if the host allows it, otherwise the arena alone.
- `MT_MEM_RESERVE_FULL`: the full reservation or failure. Code compiled with `MT_MEM_CHECKED 0` needs this.
- `MT_MEM_RESERVE_ARENA`: the arena alone. This is the iOS model.

`MT_MEM_RESERVE_FOR_BUILD` gives the right value for the `MT_MEM_CHECKED` of the translation unit that includes the header. `cpu->mem_size` is always the arena size, so the runtime's own range checks (`imports.cpp`, the r7 backtrace in `dispatch.c`) stay meaningful in both models. ADR 0014 had it as 2^32 + guard for the full reservation, and `cpu.h` and `mem.h` are updated to match.

**Which model the generated code uses.** The CMake cache string `MAPLETHIEF_MEM_MODEL` (`cmake/GeneratedCode.cmake`) sets `MT_MEM_CHECKED` on `maplethief_gen` as a PUBLIC definition:

- `auto` (the default) is checked in the Debug configuration and reserved otherwise;
- `checked` and `reserved` force one model;
- iOS is always checked, and `reserved` on iOS is a configure error.

The runtime library and the tests always compile with the header default, `MT_MEM_CHECKED 1`. HLE code keeps its checks in Release, where they are cheap.

**The iOS strategy (implemented).** iOS builds compile the generated code with `MT_MEM_CHECKED 1`, and the runtime reserves only the 512 MiB arena (`MT_MEM_RESERVE_ARENA`, which `AUTO` also falls back to). Guest addresses are identical to desktop builds. Every access is bounds-checked against the arena, and an access past it traps with the guest PC. The same code path is tested on macOS by forcing `MT_MEM_RESERVE_ARENA`: the heap stress test gives the same address sequence in both reservations. Running it on a device waits for the first iOS build of the port.

**Image.** `mt_macho_segments` reads the `LC_SEGMENT`s of a thin 32-bit ARM Mach-O. The file is untrusted, so the parser checks the header, the command sizes, the file ranges, `filesize <= vmsize` and address overflow. `mt_mem_map_segment` / `mt_mem_map_macho` commit each segment's pages, copy its file bytes to its vmaddr and leave the rest of vmsize zero (`__bss`). `__PAGEZERO` is skipped, and anything outside 0x1000–0x1000000 or overlapping another segment is refused. After the loader has written its fixups (#43), `mt_mem_seal_image` gives each committed host page the union of the `initprot` of the segments in it: read-only unless a segment in the page is writable. Host-page granularity has two effects:

- On a 4 KiB host, `__TEXT` and `__LINKEDIT` are read-only exactly, and page zero is never mapped, so a null access faults.
- On a 16 KiB host, the page 0xF0000–0xF3FFF holds the end of `__TEXT` and the start of `__DATA` and stays writable. The page 0x0–0x3FFF holds page zero and the start of `__TEXT`, so a null read returns zero bytes and a null write faults.

**Heap.** All bookkeeping lives on the host:

- **Blocks.** `std::map` of live blocks (address → usable size).
- **Small blocks** (up to 1 KiB). 64 size classes of 16 bytes, each with a free list that is refilled by carving 16 KiB of new blocks.
- **Large blocks.** Best-fit free ranges (lowest address on ties) that merge with free neighbours and lower the heap top when they reach it. In-place `realloc` grows into a following free range or the top, and shrinks in place.
- **Locking.** A mutex.

A guest buffer overflow therefore cannot corrupt the allocator, and `mt_heap_free` detects double and invalid frees and returns −1. The HLE `free` (#44) reports them with the guest caller. The heap follows Darwin's rules wherever the guest can see them:

- every block is 16-byte aligned;
- `malloc(0)` returns a unique minimum-sized block;
- `realloc(p, 0)` frees `p` and returns a new minimum-sized block;
- `calloc` checks for overflow;
- fresh memory is zero, and reused blocks keep their bytes.

Addresses are deterministic: the same calls give the same addresses on every host and in both reservations. `mt_heap_memalign` provides power-of-two alignment, and `mt_mem_guest_alloc` adapts the heap to the `mt_guest_alloc_fn` that `mt_imports_place` takes ([ADR 0019](0019-import-thunks-and-hle-abi.md)).

**Stacks.** `mt_stack_create` places a stack first-fit in the stack area. Sizes round up to 64 KiB: `MT_STACK_MAIN_SIZE` is 1 MiB, the iOS main thread's size, and `MT_STACK_THREAD_SIZE` is 512 KiB, the iOS default. Each stack sits above a 64 KiB guard that is never committed. The initial sp is `top`, which is 16-byte aligned. An overflow runs into the guard and faults in hardware in both models.

**Faults and pointer helpers.**

- **Out-of-range accesses.** The runtime implements `mt_mem_fault`. It records the fault for the host thread (`mt_mem_last_fault`: address, size, guest PC, region), logs `guest memory fault: <size>-byte access to 0x<addr> at pc 0x<pc>: <region>`, writes the PC to `r[15]` and stops through `mt_trap(cpu, MT_TRAP_MEMORY, pc, addr)`. `MT_TRAP_MEMORY` (12) is new in `calls.h`.
- **Region names.** `mt_mem_describe` names the region of any guest address, for example `__TEXT+0x17e0`, `heap block 0x01000040+0x8 (32 bytes)` or `guard below guest stack …`. It only tries locks and never waits on them, so the crash reporter (#53) can use it.
- **Pointer helpers for HLE code.** `mt_g2h` and `mt_g2h_str` are always checked, and they also reject page zero, reporting the guest caller (`lr`) as the PC. `mt_h2g` aborts on a host pointer outside guest memory, which would be a runtime bug. `mt_mem_from_cpu` finds the address space of a thread from `cpu->mem` (a registry of up to 16 live spaces), so HLE functions need no global.

## Consequences

- Debug builds trap on every out-of-range guest access with the guest PC. Release desktop builds have no checks. There, an access beyond the arena, or to an unmapped page, a stack guard or sealed `__TEXT`, faults in hardware, and only the guest address can be recovered. The crash handler (#53) catches those faults and names the region with `mt_mem_describe`. Hardware faults in Debug builds (page zero, a guard, writes to `__TEXT`) are the same.
- The bounds check is `addr + size > mem_size`. Page zero is inside the arena, so a checked guest load from a null pointer is not a software trap. On 4 KiB hosts it faults in hardware. On 16 KiB hosts a null read returns zeros. The HLE helpers do reject page zero.
- Every live heap block costs a host `std::map` node. That is fine for this game's allocation rate: the 40,000-call stress test with up to 1,500 live blocks runs twice in under half a second in a Debug build. If profiling shows otherwise, slabs with a per-page class table can replace it without changing the interface.
- The layout is fixed. If the game ever needs more than 480 MiB of heap or more than 16 MiB of stacks (it should not), the constants in `memory.h` change, and so do the guest addresses.
- Freed heap memory is never returned to the host; only stacks are decommitted. The committed heap is therefore the heap's high-water mark.
- **Verification** (`tests/cpp/memory_test.cpp`, `runtime.*` in CTest):
  - the reservations, and hardware faults beyond the arena;
  - a synthetic Mach-O with the real segment layout mapped at its link-time addresses, sealing and malformed files;
  - heap rules, `memalign`, exhaustion, and the randomised stress test against a shadow model;
  - stacks and guards;
  - the trap with the guest PC, the pointer helpers and `describe`.

  `runtime.memory_real_image` (`tests/cpp/memory_real_test.cpp`, label `real_assets`) maps `work/slices/MapleThief.armv7` and checks every segment's address, size and bytes, the entry point at 0x27E0, and read-only `__TEXT`. It is reported as skipped without the slice, which is always the case in CI. These ran on macOS arm64 and in the Linux x86_64 container (`scripts/local-ci`). The Windows path (`VirtualAlloc`) is written for MSVC `/W4 /WX` and needs a Windows run (ADR 0023).
