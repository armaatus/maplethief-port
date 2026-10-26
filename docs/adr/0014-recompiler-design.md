# 14. Static recompiler design: own decoder, C17 output, CPU state, calls and module layout

- **Status:** Accepted
- **Date:** 2026-10-25
- **Issue:** #30

## Context

M3 turns the armv7 slice into C that compiles with clang and MSVC (ROADMAP M3). This ADR fixes how the recompiler is built and what the generated code looks like. It is the contract the other M3 children build on: loader #31, discovery #32, integer lifting #33, VFP/NEON #34, indirect dispatch #35, imports #36, SjLj exceptions #37, output #38, snippet tests #39, coverage #40, the Unicorn differential test #41, and, in M4, the memory model #42.

Constraints from earlier decisions:

- [ADR 0002](0002-language-and-toolchain.md): `recomp/` is Python 3.10+ with a **60 s** budget for a full recompile. Generated code is **C17** that clang compiles in shipping builds and **MSVC `cl /std:c17`** must also compile, so no VLAs, no statement expressions, no computed goto, and compiler features only behind macros with a fallback. `recomp/` imports the Mach-O parser from `tools/` and does not copy it ([ADR 0012](0012-re-module-layout-and-shared-helpers.md)).
- [ADR 0003](0003-dependencies.md): Capstone 5.0.9 is a tooling and test dependency, the "independent oracle for the recompiler's own decoder". Putting it in the recompiler's decode path would need a new decision.
- [ADR 0007](0007-re-tooling-ghidra.md): Ghidra is a developer tool. End users, who build from their own IPA, never run it.
- No game code is committed. Generated C lives in `work/gen/`, and tests use synthetic bytes.

Facts about the input. They come from [ANALYSIS.md](../ANALYSIS.md) and from measurements for this ADR on 2026-10-25, Apple M4, CPython 3.12/3.14. The scripts used Capstone through `mtport.armscan`, which decodes every `LC_FUNCTION_STARTS` function linearly and skips literal pools.

| | armv7 (Thumb-2) | armv6 (ARM) |
|---|---|---|
| Functions | 2,655 (2,654 Thumb, 1 ARM entry stub) | 2,655, all ARM |
| `__text` | 880,956 B, **330,063 instructions** | 1,224,284 B, 286,599 instructions |
| Conditional execution | 986 IT instructions covering **1,018** instructions (306 engine, 50 app, 65 Chartboost functions) | **2,382** conditional non-branch instructions, about 1,084 of them `andeq r0, r0, r0` (zero words in literal pools), so about 1,300 real |
| Jump tables | 132 `tbb`, 52 `tbh` | no `addls pc` / `ldrls pc` form |
| VFP | VFPv3-D32, d16-d31 used | VFPv2: d0-d4 and d8-d15 only |
| NEON | **381 instructions in 121 functions** (Chartboost 192, app-objc 112, engine 77): 103 `vorr dN, dM, dM` moves, 67 `vmov.i32 qN, #0`, 129 f32 lane operations on D registers (`vadd/vsub/vmul/vneg.f32`, 8 of them inside IT blocks), 82 `vcvt` between f32 and s32/u32 on D registers | none |
| FPSCR | 121 `vmrs APSR_nzcv, fpscr`, **no `vmsr`** | the same 121 `vmrs`, no `vmsr` |
| SjLj | 205 functions call `_Unwind_SjLj_Register` | the same |

The SjLj functions use LLVM's inline `__builtin_setjmp`. For example, `+[EAGLView_Instance Instance]` (0x5839) keeps a function context at `sp+0x60`: `prev` +0, `call_site` +4, `data[4]` +8, personality +0x18, LSDA +0x1C, then the jump buffer at +0x20: `r7`, the resume address, `sp`. It writes the resume address with `mov r4, pc; adds r4, #7; str r4, [r5, #4]`, which is 0x58CD, inside the function.

## Options considered

### How to lift

1. **Ghidra P-code exported to C.** Ghidra's SLEIGH spec already models Thumb-2, VFP and NEON. Evidence from a throwaway PyGhidra project, no analysis, over the five functions of the prototype plus `-[CBUnderfinedProgressBar step:]` (0xC9581):
   - **Size.** 1.8 to 4.5 P-code ops per instruction, 3.2 on average (590 ops for 183 instructions). Each op would be a C statement, against one to three statements per instruction when lifting directly.
   - **Opaque NEON.** NEON is opaque: `vadd.f32 d0, d1, d0` becomes a user-defined `CALLOTHER` with no semantics, and so does the Thumb-mode switch of every `bx`. Those would need hand-written semantics anyway.
   - **Wrong code.** In the `itttt gt` block at 0xC95EE, P-code applies the condition only to the first instruction. `vmulgt.f32`, `vaddgt.f32` and **`vstrgt s0, [r0]`** at 0xC95F4–0xC95FC come out unconditional, so a guest store would run when it must not.
   - **Rounding modes.** Conversions are generic `FLOAT2FLOAT`/`TRUNC` without the FPSCR rounding mode.
   - **Build cost.** Every user build would need JDK 25 and Ghidra 12.2 (893 MB + 294 MB unpacked), and minutes of import, which goes against ADR 0007.
   - Rejected.
2. **remill/McSema-style lifting to LLVM IR.** remill's README lists x86/amd64, AArch64 and SPARC, and says "AArch32 (32-bit ARMv8 / ARMv7) support is underway"; it says nothing about Thumb-2. McSema has been archived since 2022-08 and supported x86/amd64, AArch64 and SPARC only. The output is LLVM bitcode, not C, so there would be no MSVC check. It needs LLVM 15+, a C++ toolchain and XED, so ADR 0002 would have to move `recomp/` to C++. Rejected.
3. **An existing ARM32 static recompiler.** gbarecomp (the GBA recompilation framework behind Mother3Recomp and others) handles ARM7TDMI code only: ARMv4T ARM plus Thumb-1, with no Thumb-2, IT, VFP or NEON. It emits C++ for a GBA runtime. dynarmic covers A32/T32 up to ARMv7-A, but it is a JIT (it is what touchHLE uses), not an ahead-of-time translator. A web search found no static recompiler for Thumb-2 with VFP. Neither fits, though both are design references: gbarecomp logs code it missed and runs it in an interpreter, which is relevant to #35.
4. **Capstone decode with our own Python C emitter.**
   - **What it gives.** No decoder to write. Capstone decodes all 330,052 Thumb instructions, at 7.4 µs per instruction through armscan (2.45 s for the binary).
   - **IT state.** Capstone knows the IT state only within one `disasm()` call. The bytes `01 22` decode as `movs r2, #1` (sets flags) alone, but as `movle r2, #1` (no flags) after `it le`. A recursive-descent discovery would have to restart Capstone at every IT.
   - **Lost encoding facts.** Its operands are values, not encodings, so the ThumbExpandImm carry-out has to be re-derived.
   - **Future churn.** Capstone 6's ARM module renames instruction and operand enums, so an upgrade would rewrite the adapter.
   - **Dependency and oracle.** The recompiler would need the native wheel on the build path, and CI would lose its independent decode oracle: only Unicorn, local only (#41), would be left.
5. **Our own decoder and our own Python C emitter, with Capstone as the oracle in tests.** The decoder follows the ARM ARM's encoding tables (A6.2, A6.3, A7) and produces the fields its decode pseudo-code produces. Measured with the prototype:
   - **Size and speed.** About 1,100 lines of Python, standard library only. **1.73 µs per instruction** (0.58 s for all 330,052).
   - **Real binary.** Identical text to Capstone on **all 329,671 non-NEON instructions** of `__text`.
   - **Exhaustive 16-bit check.** Of all 65,536 16-bit encodings, where both decode (58,002), the text is identical.
   - **Random 32-bit check.** On 98,728 random 32-bit encodings that it decodes, Capstone agrees on all of them, up to the number format.
   - **Bugs found.** That fuzz found and fixed about ten encoding-constraint bugs in the first draft, which is the oracle doing its job.
   - **Cost.** The decoder is ours to maintain. The forms are bounded, because both the census of this binary and Capstone tell us what exists.

### Input slice

1. **armv7**, the primary target. Every M1/M2 product (names files, tags, vtables, ObjC IMPs, the Ghidra export, ANALYSIS.md) uses armv7 addresses. Conditional execution is about the same either way: about 1,300 conditional instructions in armv6 against 1,018 IT-predicated ones in armv7, and the lifter needs the same predicate machinery for both. IT is a decode-time detail, about 30 lines. Downside: NEON and d16-d31 (#34).
2. **armv6**: no Thumb-2 and no NEON, VFPv2 with d0-d15 only. But it has 39% more code bytes, the same conditional density, a different memory layout, and none of the RE work applies, since every address differs. Its one real advantage, no NEON, saves #34 a few forms.

### Where guest registers live

1. **C locals per function, written to the state struct at calls and returns.** The compiler optimizes them freely. But being exact needs spill and reload code at all 19,000 call sites; the alternative is to trust AAPCS for r4-r11, which is unchecked. Locals are also indeterminate after a host `longjmp` (SjLj, 205 functions), and traps need a spill first.
2. **The `mt_cpu` struct, reached through a `restrict` pointer**, the N64Recomp-style `ctx->r[n]`. The state is exact at every call, return, trap and SjLj resume with no extra code, and generated code stays short. `restrict` tells the compiler that guest memory stores (through `cpu->mem + addr`) never alias the struct, so it keeps registers in host registers between calls anyway. Measured: the lifted `Rand_Next` (29 guest instructions) compiles at `-O2` to **21 arm64 instructions**. The seed address is folded to `0x11A510`, the LCG is one `madd`, the registers are stored once at the end, and only the stack stores the guest can observe remain.

### Flags

1. **In the struct, like registers.** That costs four stores per flag-setting instruction, and the compiler can only drop those that are overwritten before the next call.
2. **C locals, computed eagerly in C, so the compiler drops the dead ones (effectively lazy).** The lifter emits only flags some instruction of the function reads. Exact only if no flag is live into a function or across a call. AAPCS guarantees that for compiled code, and a per-function check proves it, so it is never just assumed. Over the whole binary, **0 of the 2,654 functions** violate it.
3. **Lazy flags at run time** (store the operands, compute on demand). This is more machinery for a C compiler that already removes dead computations. Rejected.

## Decision

### Lifter and decoder

`recomp/` has **its own Thumb-2/VFP decoder** and its own C emitter, both in standard-library Python 3.10+, so CMake or a user can run the recompiler with any Python and no `uv`. **Capstone stays where ADR 0003 put it**: an independent oracle in tests, and the census and listing tool. The CI tests compare the decoder with Capstone over every 16-bit encoding and 60,000 random 32-bit ones; a local test compares it over the whole binary. ADR 0003 and THIRD_PARTY.md therefore need no change. Unicorn adds an execution oracle in #41 (its own ADR, per ADR 0003).

### Input

The **armv7 slice**, mapped at its link-time addresses. The armv6 slice stays available to #41 as an optional second oracle.

### Generated code

The committed support headers in **`runtime/include/maplethief/recomp/`** are the contract between generated code and the runtime. They are C17 that also compiles as C++20, and `tests/cpp/recomp_headers_test.cpp` checks that on every CI compiler. Generated code includes only `work/gen/mt_gen.h`, which includes `maplethief/recomp/recomp.h`.

| Header | Holds |
|---|---|
| `compiler.h` | `MT_RESTRICT`, `MT_INLINE`, `MT_NORETURN`, `MT_LIKELY`; `MT_TAILCALL(f(cpu))` (clang `musttail`, else call plus return); compile-time checks of the host: two's complement, arithmetic right shift, 32-bit `int`, little-endian, `FLT_EVAL_METHOD` 0 |
| `cpu.h` | `mt_cpu`, `mt_guest_fn` |
| `mem.h` | `MT_LD8/16/32/64(addr, pc)`, `MT_ST8/16/32/64(addr, value, pc)`, `mt_mem_fault` |
| `alu.h` | Shifts by register with ARM semantics and their carry-out, `mt_ror`, sign extension, `clz`/`rev`, `mt_smul64` |
| `vfp.h` | `mt_s_get`/`mt_s_set`, f32/f64 bit casts, `mt_v{add,sub,mul,div,sqrt}_{f32,f64}`, the f32↔f64 conversions, `mt_vcmp_*`; FP contraction off |
| `calls.h` | `MT_FN_ENTER`, `MT_RETURN`, `MT_CALL_INDIRECT`, `MT_JUMP_INDIRECT`, `mt_dispatch`, `mt_fn_entry`, `MT_CALL_IMPORT`, `mt_import_table`/`mt_import_names`, `mt_trap` and its kinds, `MT_SJLJ_SETJMP` / `mt_sjlj_jmpbuf` |

**Functions.** Every guest function becomes `void f_XXXXXXXX(mt_cpu *MT_RESTRICT cpu)`. The name is the guest **function pointer**, so the Thumb bit is set for Thumb code (`f_000225E5`, `Rand_Next` at 0x225E4), matching `LC_FUNCTION_STARTS` and the names files. Each guest instruction becomes a comment `/* 000225E4: sub sp, #8 */` and a few statements. A branch target gets the label `L_XXXXXXXX:;` (instruction address, no Thumb bit). An instruction in an IT block, or one with a condition field, is wrapped in `if (cond) { ... }`.

**CPU state.** `mt_cpu` holds `uint32_t r[16]`, `cpsr`, `fpscr`, `uint64_t d[32]`, `uint8_t *mem`, `uint64_t mem_size` and `void *host`, with the offsets fixed by static asserts. Generated code reads and writes `cpu->r[n]` and `cpu->d[n]` directly. The runtime must never touch a running thread's `mt_cpu` through another pointer (the `restrict` contract). A read of the PC is the constant `address + 4`; `r[15]` is written only for diagnostics.

**Flags.** N, Z, C and V are the C locals `nf zf cf vf` (0 or 1), declared only if the function reads them. Every flag-setting instruction computes them inline: AddWithCarry for arithmetic, the shifter carry-out (including ThumbExpandImm_C) for logic, V unchanged by logic. Only flags some instruction of the function reads are computed. `lift.core.check_flags` proves per function, with a forward must-be-defined analysis that treats calls as killing every flag, that **no flag is read where it may be undefined**: live into the function or across a call. A violation is a lifting problem in the coverage report, never silent. `cpsr` is touched only in snippet mode (see Tests).

**VFP.** `d[0..31]` are bit patterns, and `s[2n]` and `s[2n+1]` are the low and high halves of `d[n]`, read and written through `mt_s_get`/`mt_s_set`. Arithmetic converts with `memcpy` and uses the host's IEEE operations. Those give the same results as VFP for round-to-nearest with denormals kept. The `mt_v*` helpers apply ARM's NaN rules (FPProcessNaNs, default NaN `0x7FC00000`), because x86 creates NaNs with the sign bit set. VMLA and friends are two rounded operations. Floating-point contraction is off: a pragma in `vfp.h` plus `-ffp-contract=off`. **FPSCR** is `cpu->fpscr`. Only its NZCV bits change at run time (`vcmp`/`vcmpe` write them, `vmrs APSR_nzcv` copies them to the flag locals). The binary never writes FPSCR (no `vmsr`), so the rounding mode stays nearest-even and FZ/DN stay 0; the cumulative exception bits are not modelled. #34 adds the NEON f32 lane operations, which always flush denormals and use the default NaN ("standard FPSCR value"), and the integer conversions with saturation.

**Memory.** Guest memory is little-endian. The host address is `cpu->mem + guest address`, and every access goes through `memcpy`, so unaligned accesses (allowed for LDR/STR/LDRH/STRH on ARMv7) are correct and aliasing-safe. The **image is mapped at its link-time addresses (slide 0)**, so the lifter folds every PC-relative address and every literal-pool load from `__TEXT`, which is immutable, into a constant. One API serves both models #42 chooses between:
- **`MT_MEM_CHECKED 0`**: a 4 GiB reservation plus a guard area. No check; an unmapped page faults in hardware.
- **`MT_MEM_CHECKED 1`**, the default, for debug builds, tests and iOS: a bounds-checked window of `mem_size` bytes. A miss calls `mt_mem_fault(cpu, addr, size, pc)`.

Every access passes the guest PC as a constant, and only the fault path uses it. Barriers (`dmb/dsb/isb`) emit nothing, because the guest is single-threaded ([re/timing-threading.md](../re/timing-threading.md)).

**Calls and returns.**
- **Calls.** Arguments, results and every register stay in `cpu`; the C call only transfers control. `bl`/`blx #imm` set `cpu->r[14]` to the return address (Thumb bit set) and call the target's C function directly.
- **Returns** are **native C returns**. `MT_FN_ENTER` remembers `lr` at entry. `bx lr`, `pop {..., pc}`, `ldm sp!, {..., pc}` and `ldr pc, [sp], #4` are `MT_RETURN(target)`. With `MT_CHECK_RETURNS` (debug builds and tests) it traps if the target is not the entry `lr`.
- **Tail calls.** A `b` to another function is `MT_TAILCALL(f_X(cpu))`, which guarantees a tail call under clang, the shipping compiler.
- **Indirect jumps.** `bx rN` (not lr), `mov pc, rN` and `ldr pc` (not a return) are `MT_JUMP_INDIRECT(target, site)`. A jump to the entry `lr` is a return; anything else is a tail call through the dispatch table.

**Indirect dispatch (#35).** `blx rN` is `MT_CALL_INDIRECT(target, site)`, which calls `mt_dispatch(cpu, target, site)(cpu)`. `mt_dispatch` maps a guest function pointer to its host function. The Thumb bit is significant, because functions are keyed by their pointers. For an unknown target it returns a function that reports the target, the site and `lr`, then stops. The generated `work/gen/dispatch.c` lists `mt_fn_table[]` (`{addr, fn}`, sorted) and `mt_fn_count`. #35 owns the fast lookup, the stubs' entries and the fallback policy (trap, or an interpreter for code that was not lifted).

**Imports (#36).** Every import has a generated id. A call to a `__symbolstub1` stub is `cpu->r[14] = ret; MT_CALL_IMPORT(id);`, which calls `mt_import_table[id](cpu, id)`. `work/gen/imports.c` defines `mt_import_names[]` and the table's storage. The runtime fills each slot at start-up with an HLE function, which reads its AAPCS soft-float arguments from `cpu` and writes its results back, or with a default handler that logs the name and the caller (`lr`) and then continues or stops. Every function import also gets a guest code address in the dispatch table (its stub, or one the runtime assigns), so a pointer to an import is callable. Data imports are guest objects that the runtime binds.

**SjLj (#37).** Where the guest stores a resume address inside the function into a jump buffer, the lifter emits `if (MT_SJLJ_SETJMP(jbuf)) { goto L_<resume address>; }`, a host `setjmp` keyed by the guest jump buffer. When the guest unwinder resumes there, the runtime restores `r7` and `sp` from the guest jump buffer into `cpu` and `longjmp`s. No C local is live across that jump: all guest state is in `cpu`, and the flag check treats the resume label as a function entry. `_Unwind_SjLj_Register`/`Unregister`/`Resume` are ordinary import calls.

**Traps and gaps.**
- **Unknown instruction.** An instruction the lifter cannot translate becomes `MT_TRAP(MT_TRAP_UNSUPPORTED, pc, raw)` in place and is reported.
- **Falling off the end.** Code past the last decoded instruction gets `MT_TRAP_FELL_OFF`.
- **Undefined encodings.** `udf`/`bkpt`/`svc` (the compiler's `trap`) become `MT_TRAP_UDF`.
- **Unlifted functions.** A function that cannot be lifted at all still gets an `f_XXXXXXXX` that calls `mt_trap(..., MT_TRAP_UNLIFTED, ...)`, so the program always links (#40 lists them).

**Portability.** Arithmetic is on `uint32_t`, where wrap-around is defined. Constants carry `u`. There is no unary minus on unsigned values and no shift of 32 or more (the helpers clamp). There are no unused temporaries or labels, so `-Wall -Wextra -Werror -Wpedantic` and `/W4 /WX` pass. `setjmp.h` and `math.h` (for `sqrt`/`sqrtf`, correctly rounded) are the only headers beyond `stdint.h`/`stddef.h`/`string.h`/`float.h`.

### Module layout

Each later child works in its own modules. A stage imports only earlier stages, never later ones. A lifter module imports `lift.core`, `decode` and `discover`, plus another lifter module's helpers where they are shared (`vfp` uses `ldst.address`).

| Module | Owner | Contents |
|---|---|---|
| `recomp/__init__.py` | #30 | Package docstring, `REPO`; puts `tools/` on `sys.path` for `python -m recomp` |
| `recomp/__main__.py` | #30, each stage adds its command | CLI |
| `recomp/loader.py` | #31 | `Image` on `mtport.macho`: immutable bytes, function bounds, stubs, rebases and binds (decoded in `mtport.macho_dyld`), the 333 imports (`mtport.imports.import_table`), init functions, ObjC ranges, consistency checks and the loader manifest |
| `recomp/decode/` | `insn.py`, `bits.py`: #30; `thumb16.py`, `thumb32.py`: #33; `vfp.py`: #34; `arm.py`: #33 (the entry stub) | `insn.py`: `Insn`, the operand types and Capstone-style `text()`; `bits.py`: ThumbExpandImm and friends |
| `recomp/discover.py` | #32 | `Function` (`insns`, `succ`, `labels`, `problems`), `discover_function`, `flow`, `is_return`; #32 adds jump tables, tail calls, noreturn calls, literal pools, internal entries, `discover` and `functions.json` ([ADR 0016](0016-function-discovery.md)) |
| `recomp/discovery_report.py` | #32 | `discovery-report.md`: failures, seeds, tables, unreached bytes and the Ghidra cross-check |
| `recomp/lift/core.py` | #30 | `Ctx` (the emission API), `lifter()` registry, `lift_function`, `check_flags`, `LiftOptions`, `Lifted` |
| `recomp/lift/alu.py`, `mul.py`, `ldst.py`, `branch.py`, `misc.py` | #33 (may split by class) | One lifter per mnemonic |
| `recomp/lift/vfp.py` | #34 | VFP and NEON |
| `recomp/lift/sjlj.py`, `recomp/exceptions.py` | #37 | Resume-point detection and hook emission; context census and LSDA parsing |
| `recomp/dispatch.py` | #35 | `dispatch.c` |
| `recomp/imports.py` | #36 | `ImportTable`, `imports.c` |
| `recomp/output.py` | #38 | `write_program`, `write_if_changed`, the `mt_gen.h` prototypes; #38 adds the partitioner and `gen.cmake` |
| `recomp/census.py` | #33/#34 | `census.json` |
| `recomp/coverage.py` | #40 | `coverage.json` / `coverage.md`, the summary line |
| `recomp/snippet.py` | #30 (#39 builds on it) | `lift_snippet` |
| `recomp/prototype.py` | #30 | The five functions of this ADR, kept as a regression set |

**Command line**, run from the repository root: `python -m recomp lift [--slice PATH] [--only PTR,...] [--out DIR]` and `python -m recomp prototype`. #31 adds `load`, #32 `discover`, #33 `census`, #40 `coverage`, and #38 `all` for the full pipeline. A thin `scripts/` wrapper can follow with #38.

**Outputs**, git-ignored and never committed:
- `work/recomp/`: `loader.json`, `functions.json`, `discovery-report.md`, `census.json`, `coverage.json`, `coverage.md`. `loader.json` is the loader manifest the runtime (#43) maps the image and applies its binds from; its schema is in the docstring of `recomp/loader.py`, the one place it is specified (#31).
- `work/gen/`: `mt_gen.h`, `fn_NNN.c`, `imports.c`, `dispatch.c`, `gen.cmake`.
- `work/gen/proto/`: the prototype's output.

**pytest** imports `recomp` through `pythonpath = ["tools", "."]` in `pyproject.toml`, and ruff treats `.` as first-party.

### Tests

- **Snippet interface (#39).** `recomp.snippet.lift_snippet(code, address, name="snippet")` lifts a byte string as one function and returns a complete translation unit plus the callees, unsupported instructions and problems. Running past the end returns. Flags come from and go back to `cpu->cpsr`, so tests can set and check them (`LiftOptions(flags_in, flags_out, fall_off_returns)`). Literal loads from inside the snippet are folded, and direct calls go to `f_XXXXXXXX` functions the test defines.
- **Harness.** `tests/recomp_harness.py` finds the host C compiler (MSVC `cl` on the Windows runner, else clang, cc or gcc), builds with ADR 0002's strict flags against `tests/recomp/test_runtime.c`, runs the program, and returns the final state. `run_snippet(tmp_path, code, state, memory, read, extra_c, functions)` does all of that in one call.
- **Synthetic and real-asset tests.** `tests/test_recomp_lift.py` holds the synthetic cases. `tests/test_recomp_prototype_real.py` holds the local real-asset ones.

### Prototype

Five real functions, lifted end to end from the armv7 slice into `work/gen/proto/` by `python -m recomp prototype`:

| Function | Pointer | Exercises |
|---|---|---|
| `Rand_Next` (MSVC `rand()` LCG) | 0x225E5 | Leaf: literal-pool loads folded, a global in `__DATA`, `mul`, register shift, stack round trips |
| `Rand_Percent` | 0x226B9 | **IT block** (`movle`), direct call, `smmul` + `mls` (modulo by 100), `push`/`pop {pc}` |
| `TouchKeyPadView_Distance` | 0xB45FD | **VFP**: f32 `vsub/vmul/vadd`, `vcvt.f64.f32`, `vsqrt.f64`, `vcvt.f32.f64`, soft-float return in r0 |
| `-[CBReachability networkStatusForFlags:]` | 0xCA049 | Five IT blocks, including a conditional return `bxeq lr`, `ubfx`, `tst.w` |
| `Rand_Chance` | 0x22685 | An import call through a stub (`___modsi3`) |

Results:
- **Clean lift and compile.** All five lift with no problem. They compile with clang `-std=c17 -Wall -Wextra -Werror -Wpedantic`, and pass `clang --target=x86_64-pc-windows-msvc -fms-compatibility` with both the clang and the `_MSC_VER` macro paths (`-U__clang__`), using shim CRT headers because there is no Windows SDK on the Mac.
- **Same results as references.** With the image mapped in the test runtime, 1,610 calls give exactly what independent references compute: the MSVC LCG, Apple's Reachability sample logic, and IEEE f32/f64 arithmetic in Python.
- **Real MSVC.** MSVC `cl /std:c17 /W4 /WX` itself compiles the synthetic snippets in CI on the Windows runner.

The same prototype on the **whole binary**:
- **Coverage.** It lifts all 2,654 Thumb functions in **2.3 s** (145 MB peak memory). 2,375 (89.5%) lift with no problem; the rest are exactly the later children's scope: TBB/TBH tables (#32), NEON and VFP integer conversions (#34), and three tail-call fall-throughs (#32).
- **Output.** The 34 MB of C (1.25 M lines) passes clang `-Wall -Wextra -Werror -Wpedantic`. One unpartitioned `-O2` build of it takes 17 s on one core and yields 3.0 MB of arm64 code.

## Consequences

- **#31** extends `loader.Image` and keeps building on `mtport.macho`. **#32** keeps `Function`'s fields and `flow()`, and adds tables and tail calls; the decoder already decodes every control-flow form. **#33** finds every integer form of the binary already decoded and identical to Capstone. Its work is lifting and testing each form, plus the A32 decoder for the entry stub, and splitting `alu.py`/`ldst.py` is fine. **#34** adds NEON decode (381 instructions, all in the Advanced SIMD space) and lifting with FZ/DN semantics.
- **#35, #36 and #37** implement the runtime side of `calls.h` and the generators. **#38** owns `fn_NNN.c` partitioning and `gen.cmake`, and the 5-minute budget looks safe (17 s unpartitioned). **#39** builds snippet files on `lift_snippet` and `run_snippet`. **#40** reports `Lifted.problems` and `unsupported`. **#42** implements `mem.h`'s two models.
- ADR 0003 and THIRD_PARTY.md are unchanged: Capstone stays a test and tooling dependency.
- The recompiler is pure Python with no dependencies, so CMake can run it with `find_package(Python3)` and no `uv`.
- New instruction forms go into their class's lifter module, and a missing one is visible as a trap in place plus a coverage entry.
- **How we'd know this was wrong:**
  - **The flag check.** It reports functions whose flags are live across a call; then those functions spill flags to `cpsr`.
  - **Optimized code.** The host compiler fails to optimize the struct-based state well enough for the game's speed. Then measure first, and move hot registers into locals per function in a later ADR, keeping the call contract.
  - **The time budget.** The full pipeline breaks ADR 0002's 60 s budget. It is at 2.3 s now.
  - **MSVC.** It cannot compile the generated volume; ADR 0002 already allows dropping that check.
  - **The decoder.** Maintaining it costs more than adapting Capstone would. Then put Capstone behind `decode/`'s `Insn` interface; the lifters would not change.

## Addendum: integer lifting and the A32 path (#33, 2026-10-26)

- **Census.** `python -m recomp census` writes `work/recomp/census.json`: every instruction form with its count, its class (integer, vfp, neon, undefined) and its trapped instances. The form keys and the schema are in `recomp/census.py`, and #34 refines the VFP and NEON keys there. On the armv7 slice, **no integer instruction traps**. All 189 integer forms have a lifter and a snippet (ADR 0015 addendum).
- **A32.** `recomp/decode/arm.py` decodes the classes the entry stub uses and their close relatives: data processing, multiplies, `clz`, single and extra loads and stores, `ldm`/`stm`, and the branches. Anything else is `<undefined>`. Capstone is its oracle in a per-class fuzz test. `Insn.arm` marks an A32 instruction and `Insn.pc` is the value a PC read returns. In the lifter, `bl` keeps the instruction set and `blx #imm` switches it. An A32 ALU write to the PC interworks (BXWritePC), while a Thumb one stays in Thumb. An ARM function is keyed by its even pointer (`f_000027E0`).
- **Whole program.** `python -m recomp lift` now lifts every `LC_FUNCTION_STARTS` function, the ARM stub included: **2,525 of 2,655 clean**, and the rest are #34's. The 42 MB of C compiles with clang `-Wall -Wextra -Werror -Wpedantic` in 14 s at `-O0` and 30 s at `-O2` on one core. It also passes the MSVC-target syntax check with both macro paths.

## Addendum: indirect dispatch, returns and the miss policy (#35, 2026-10-26)

**Indirect sites.** Discovery finds **371 `blx rN`** in 154 functions, plus 2,052 `pop {..., pc}` and 635 `bx lr` returns. Every other write to the PC is a jump table (`Function.indirect` is empty). The other ways in are ObjC IMPs (1,064), vtable slots, init functions and callbacks. Those are discovery's strong seeds, and they are all `LC_FUNCTION_STARTS` entries. Code also computes 79 code pointers (`Function.code_refs`), all of them function starts.

**The table.** `recomp/dispatch.py` generates `work/gen/dispatch.c`:
- **Entries.** One `{pointer, function}` entry per lifted function, sorted, and one per `__symbolstub1` stub. A stub's entry is a static thunk `mt_stub_XXXXXXXX` that calls `MT_CALL_IMPORT(id)`, so a pointer to a stub is callable too.
- **Keys.** Keys are function pointers: Thumb code with bit 0 set, ARM code (the entry stub, the stubs) word-aligned with bit 0 clear. The generator rejects two entries at one address and a misaligned ARM pointer.
- **Code.** The lookup code is committed: `runtime/src/recomp/dispatch.c`, declared in `runtime/include/maplethief/recomp/dispatch.h`. The runtime links it with the generated data, and the test runtime compiles it in, so the snippet harness's `[dispatch]` tables use the same index and lookup.

**Fast lookup: a bucket index.** `mt_fn_idx` splits the table's address range into 16-byte buckets. `first[b]` is the first entry at or after bucket `b`. A lookup clears bit 0 for the bucket and compares the full pointer, so a Thumb function reached in ARM state, or an ARM one in Thumb state, is a miss. Measured on 2026-10-26 (Apple M4, clang -O2, 4 M lookups of the 2,872 real addresses at random):

| Lookup | Random target | Same 16 targets | Size |
|---|---|---|---|
| Binary search over the sorted table | 19.1 ns | 11.2 ns | 0 (the table, 46 KB) |
| Dense `uint16_t` per halfword of `__text` and the stubs | 1.0 ns | 0.9 ns | 977 KB |
| **Buckets of 16 B** (`uint16_t` first index, at most 4 entries per bucket) | **1.9 ns** | **1.2 ns** | **122 KB** |
| Buckets of 64 B (at most 7) | 4.4 ns | 1.6 ns | 31 KB |

- **Choice.** The 16-byte buckets are within 1 ns of the dense array at an eighth of its size. A 61,058-entry `uint16_t` array is about 0.4 MB of generated C, against 3 MB for the dense array.
- **Perfect hash rejected.** It would be a third generator for nothing: the bucket walk is already constant time, and its worst case is four compares.
- **Lookup cost.** At 2 ns, a lookup costs about as much as the indirect call itself.

**Returns stay native C returns, checked.** Two changes:
- **`MT_RETURN(target, site)`.** It now takes the guest address of the return instruction. With `MT_CHECK_RETURNS`, a return to anything other than the entry `lr` traps with `MT_TRAP_BAD_RETURN` at that instruction (detail = target). Before, it reported the entry `lr` as the PC.
- **What the snippets cover.** `bx lr`, `pop {pc}`, `ldr pc, [sp], #4`, ARM `pop {pc}`, `mov rN, lr; bx rN` and an `ldr pc` from memory that loads the entry `lr` (an indirect jump to the entry `lr` is a return) each have a passing snippet. A changed `lr` or a changed saved `lr` traps.

**Miss policy: trap, no interpreter.**
- **The trap.** `mt_dispatch` returns the host function, or records the site in `r[15]` and calls `mt_trap(cpu, MT_TRAP_NO_FUNCTION, site, target)`. The new trap kind is 7.
- **The diagnostic.** The runtime's trap handler reports it with `mt_dispatch_describe`, one line with:
  - the target and the site, with the function it lies in (`f_00001001+0x2`, the caller);
  - `lr`;
  - why the target is not a function: null, wrong instruction set, an ARM address not word-aligned, an offset into a function, or outside the code;
  - a guest backtrace from the r7 frame chain (Apple's ARM ABI keeps `r7` as the frame pointer, with the saved `lr` above the saved `r7`). The backtrace is read only at the trap, so it costs nothing at run time, unlike a shadow call stack.

  The test runtime prints that line plus `miss_caller` and `miss_frame<n>`.
- **No interpreter.** An interpreter for unlifted code (gbarecomp's fallback) would be a second implementation of every instruction form, and the CPU state would have to cross between it and the lifted code. On this binary there is nothing for it to run: every function and every known indirect target is lifted, and an unknown target can only be a bug (a corrupted pointer, a missed seed). The trap names it exactly, so the fix goes into discovery. If M4/M5 play-testing finds real misses, an interpreter can plug in at the miss path of `mt_dispatch`, which knows target and site, without changing generated code.

**Evidence.**
- **CI.** `tests/test_recomp_dispatch.py` checks the bucket index against a dictionary for every address near every entry. It compiles the C lookup with the host compiler (MSVC on the Windows runner) for Thumb and ARM pointers, mode-flipped and mid-function neighbours and import thunks, and checks the miss description and the backtrace.
- **Snippets.** 16 snippets in `tests/recomp/snippets/dispatch/`: `trap = no_function <site> <target>` misses (Thumb and A32, wrong mode, misaligned, mid-function, null), interworking through the table, and bad returns.
- **Local.** `tests/test_recomp_dispatch_real.py` (real assets) checks the binary: all 2,655 functions and 217 stubs are in the table, all 2,655 strong seeds and 79 code pointers too. The generated `dispatch.c`, compiled, finds every one of 2,872 entries and rejects its neighbours. The only 18 seeds outside the table are weak "data" words (ADR 0016's false pointers).
