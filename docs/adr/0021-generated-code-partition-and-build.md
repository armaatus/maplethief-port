# 21. Generated code: address-cell partition into translation units, gen.cmake in the build

- **Status:** Accepted
- **Date:** 2026-10-26
- **Issue:** #38

## Context

[ADR 0014](0014-recompiler-design.md) fixes the files the recompiler writes to `work/gen/` (`mt_gen.h`, `fn_NNN.c`, `imports.c`, `dispatch.c`, `gen.cmake`) and leaves the partition into translation units and the CMake side to #38. Requirements (#38): a full Release build of the generated code in at most 5 minutes on an Apple Silicon Mac, a change to one function's output rebuilds one translation unit, and the code compiles with clang and with clang-cl/MSVC. CI never has the IPA, so the main build must configure and build without `work/gen/`.

Facts, measured on 2026-10-26 on an Apple M4 (10 cores: 4 performance, 6 efficiency), Apple clang 21, after #33:

- **Size.** The 2,655 functions lift to 42.2 MB of C (2,525 of them clean). `__text` is 880,956 bytes (0x27E0–0xD991C), and the functions tile it.
- **Giant functions.** The C size is very uneven. The four largest functions are 1.26 MB, 1.05 MB, 0.96 MB and 0.82 MB of C (`f_0003ED19`, `f_00087501`, `f_0008D4C9`, `f_0003A899`). A translation unit can never be smaller than its largest function, so they set the critical path of a parallel build: about 5 s at `-O3`.
- **One unit.** The whole program as one translation unit takes 31.8 s at `-O3` (one core).
- **The input is immutable.** The function set and every function's guest address come from the shipped binary and never change. What changes, as the recompiler improves, is the C of individual functions.

## Options considered

1. **Cumulative size quantiles.** Sort by address and cut wherever the running C size passes k × total/N. This gives the best balance, but the cuts depend on every size before them. Growing one function shifts every later cut by that amount, so a lifter change in an early function can move functions between many units and rebuild most of the program. Rejected, because it fails "one change rebuilds one unit".
2. **Hash of the function pointer modulo N.** Stable: a function's unit depends only on its pointer. Balance is statistical, though, and with sizes this uneven, units of several giant functions are likely. Callers also end up in different units from their neighbours. Rejected.
3. **Stateful assignment.** Keep a manifest of the previous partition and place only new functions. This is stable, but the output depends on history, not on the input, and two machines can produce different files. Rejected.
4. **Fixed address cells.** Cut the address range of the code (`__text`) into N equal cells, and put each function into the cell that holds its start address: `unit = (address - lo) * N // (hi - lo)`.
   - **Stable.** The assignment depends only on the function's own address, so a change to any function's C, of any size, rewrites exactly one file. Adding or removing a function changes only its own cell, plus `mt_gen.h` and `dispatch.c`, which list the functions.
   - **Balanced.** Equal cells hold equal guest code, and the C size follows it. At N = 64 the largest unit is 1.32 MB (the 1.26 MB function plus its neighbours); every other unit is below a giant function's size. 4 of the 64 cells are empty, because giant functions span them.
   - **Locality.** Callers sit next to their callees, so clang can still inline within a unit.

## Decision

- **Partition: fixed address cells (option 4).** This is `recomp.output.partition`. The span is `__text` for the real image, and the functions' own range otherwise (synthetic programs). The number of cells is `python -m recomp lift --units N`, **64 by default**.
  - **File names.** `fn_NNN.c` is named after its cell index, so an empty cell leaves no file and moves no other name.
  - **Contents.** Each unit starts with `#include "mt_gen.h"` and holds its functions in address order.
- **`mt_gen.h`** holds what every unit shares: it includes `maplethief/recomp/recomp.h` (CPU state, memory API, calls), then `MT_IMPORT_COUNT`, the dispatch table declarations and one prototype per function. It depends only on the function set and the import count.
- **Write if changed.** Every file is written only when its bytes differ, so unchanged files keep their timestamps. A `fn_NNN.c` that an earlier run left in a cell that is now empty is deleted.
- **`gen.cmake`** lists the sources relative to its own directory, in `MAPLETHIEF_GEN_SOURCES`. It changes only when a unit appears or disappears, and then CMake re-configures by itself.
- **The build.** `cmake/GeneratedCode.cmake` defines `maplethief_add_generated_code(<target> <dir>)`, which builds a static library with ADR 0002/0014's flags:
  - **Every compiler:** C17 without extensions, the support headers, warnings as errors under `MAPLETHIEF_WARNINGS_AS_ERRORS`.
  - **clang/gcc:** `-Wall -Wextra -Wpedantic -ffp-contract=off`.
  - **cl:** `/W4 /utf-8 /fp:precise /bigobj`.
  - **clang-cl:** `/W4 /utf-8 /bigobj /clang:-ffp-contract=off`. clang-cl rejects `/fp:precise` together with `-ffp-contract=off` under `/WX` (`-Woverriding-option`).
- **The cache path `MAPLETHIEF_GEN_DIR`.** It defaults to `work/gen`; an empty value leaves the generated code out. When it holds a `gen.cmake`, the top-level build adds the target **`maplethief_gen`** from it. Otherwise the configure step prints that it skipped the target, and it re-runs once the recompiler has written into `work/gen/` (or `work/`). Nothing links `maplethief_gen` yet; the runtime (#43) will.
- **`python -m recomp all`** runs load, discover and lift into `work/` (ADR 0014 assigned it to #38).

**Measured** on the Apple M4, 10 cores, Release (`-O3`), `cmake --build --target maplethief_gen` (Ninja, all cores), after `python -m recomp lift` (3.7 s):

| Units (non-empty) | 1 (1) | 8 (8) | 16 (16) | 32 (31) | **64 (60)** | 128 (116) |
|---|---|---|---|---|---|---|
| Full build, wall | 31.8 s | 7.8 s | 7.1 s | 7.1 s | **6.9–7.6 s** (57.8 s CPU) | 7.3 s |
| Slowest unit | 31.7 s | 7.7 s | 6.5 s | 6.8 s | **4.9–5.3 s** | 5.5 s |

- **Universal build.** The `mac-release-universal` preset (arm64 + x86_64) builds `maplethief_gen` in 22.8 s.
- **Incremental builds.** A change to one function rebuilds one unit: 0.4 s for a typical unit, 1.3 s for the one with a giant function. The real-asset test configures and builds the whole program in Debug (`-O0`) in about 7 s.

With 64 units the full build is limited by the giant functions, not by the number of units. Smaller units make incremental builds cheaper.

## Consequences

- The 5-minute budget is met with a wide margin: about 7 s on the M4. The whole build is 58 s of CPU time, so even one core stays far inside the budget.
- **Tests.** `tests/test_recomp_output.py` checks the partition, write-if-changed and stale-file removal. It also builds a synthetic five-function program (a cross-unit call, an indirect call through dispatch, an import, a tail call) through `GeneratedCode.cmake` in `tests/recomp/gen_project/`. On every OS of CI that build uses the host compiler, MSVC `cl` on Windows, and the test runs the program, changes one function, and asserts that Ninja compiles exactly one unit. `tests/test_recomp_output_real.py` checks the same for the real program locally, and compiles every real unit with clang in clang-cl mode for `x86_64-pc-windows-msvc` at `/W4 /WX /O2`. MSVC `cl` itself on the real program needs a Windows machine with the IPA (#38's `needs-human` hand-off).
- `MAPLETHIEF_GEN_DIR` lets a developer build any other output directory, e.g. `work/gen/proto`, or switch the generated code off.
- **How we'd know this was wrong.**
  - **Balance.** The lifter's C size per guest byte drifts so far between regions that units become badly unbalanced. Then use more cells: the address grid stays stable at any N.
  - **Build time.** Functions grow so large (e.g. after inlining helpers) that one of them alone breaks the budget. Then a function has to be split, which a partition cannot fix.
  - **MSVC.** `cl` is much slower on the giant functions. Then measure on Windows and consider `/O1` for those units.
