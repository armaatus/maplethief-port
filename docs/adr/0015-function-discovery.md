# 15. Function discovery: recursive descent with our decoder, Ghidra as a cross-check only

- **Status:** Accepted
- **Date:** 2026-10-26
- **Issue:** #32

## Context

The lifter ([ADR 0014](0014-recompiler-design.md)) needs, for every guest function, the instructions that belong to it, their successors, the branch targets that need labels, and what it cannot follow. The #30 prototype only did recursive descent from each `LC_FUNCTION_STARTS` entry up to the next one. On the whole binary, 279 of the 2,654 Thumb functions did not lift cleanly. It could not follow 173 `tbb`/`tbh` sites, and 3 functions ran past their end after a call that never returns.

Facts about the input ([ANALYSIS.md](../ANALYSIS.md#instruction-mix), measured 2026-10-26 on an Apple M4):

- `LC_FUNCTION_STARTS` is complete (2,655 entries, 2,654 Thumb plus the ARM entry stub). Ghidra finds no function in `__text` outside it except three data tables.
- The linear census (`mtport.armscan`, Capstone) finds **132 `tbb` and 52 `tbh`**. They are all -O0 switches: `cmp rX, #N` + `bhi default`, often with the index going through stack slots first (`str r0, [sp, #12]; bhi; ldr r1, [sp, #12]; ldr r1, [sp, #12]; tbb [pc, r1]`), and sometimes with *another* register holding the same value being compared (`ldr r1, [sp, #16]; cmp r1, #7; str r0, [sp, #12]`). There is no `ldr pc` or `add pc` table.
- Chartboost's clang code has a third shape: the SjLj dispatch of a landing pad jumps through a table of `b.w` instructions (`addw r1, pc, #k; add.w r1, r1, r0, lsl #2; mov pc, r1`, 16 of them). Neither the census nor Ghidra's function bodies see these, because no branch reaches the landing pads.
- Ghidra's own switch recovery depends on the host's speed under its default settings ([#112](https://github.com/armaatus/maplethief-port/issues/112), [ADR 0013](0013-ghidra-analysis-without-time-limits.md)). Recompiled code must not.
- NEON (381 instructions) is decoded only by #34. An undecodable instruction used to stop discovery, so the rest of the function was never seen, and the function looked as if it never returned.
- 205 functions use SjLj. Their resume code is reached only through the unwinder, never through a branch. The game code computes the resume address with `mov r4, pc; adds r4, #7`, and Chartboost with `ldr r1, =k; orr r1, #1; add r1, pc`.

## Options considered

### Where function bodies come from

1. **Ghidra's export (`work/ghidra/functions.json` bodies, `xrefs.json` switch targets).** It is already there and agrees with LC_FUNCTION_STARTS. But users never run Ghidra ([ADR 0007](0007-re-tooling-ghidra.md)), the result depended on host speed before ADR 0013, and a different Ghidra would change the recompiled program. Rejected as an input; kept as an oracle.
2. **Capstone through `mtport.armscan`.** Its linear sweep skips pools and tables. But it bounds a table only by a `cmp` directly before the `tbb`, which none of this binary's 184 tables has, and Capstone is not allowed on the recompiler's path ([ADR 0014](0014-recompiler-design.md)). Kept as the census oracle.
3. **Recursive descent with our own decoder**, in `recomp/discover.py`, bounded by `LC_FUNCTION_STARTS`. Chosen.

### How a table is bounded

1. **Scan entries until one points outside the function or into the table.** This needs no pattern, but it overruns into padding bytes and into the next table, and `add pc` tables hold arbitrary offsets.
2. **The guarding `cmp` found by pattern** (`cmp` / `bhi` / `tbb` adjacent). This misses every -O0 table here.
3. **The guarding `cmp` found by a small symbolic evaluation** of the single-predecessor path before the table. Registers and stack slots (`[sp, #k]`, `[r7, #k]`) hold value numbers, copied by `mov`, `ldr` and `str` of a slot, and fresh after any other write. A `cmp rX, #N` followed on the path by the `ls`/`lo` side of a branch bounds rX's value, and the bound of the value the index holds at the table is the entry count. Chosen. The result is checked: the table must lie inside the function, no target may be inside the table, and the bound is computed again once the whole function is known.

## Decision

**Discovery** (`recomp/discover.py`) is recursive descent with our decoder. Every function runs from its pointer (the Thumb bit gives the mode) up to the next `LC_FUNCTION_STARTS` entry, and IT conditions are carried along the fall-through path.

- **Calls** record the callee's pointer with its mode: `bl` calls Thumb code, `blx #imm` ARM code (the `__symbolstub1` stubs). `bx rN` and `blx rN` stay dynamic (#35).
- **Jump tables** come in four forms, with `rB` from `adr`/`addw rB, pc`:
  - `tbb`/`tbh [pc, rI]`;
  - `ldr pc, [rB, rI, lsl #2]`, with absolute entries;
  - `ldr rT, [rB, rI, lsl #2]; add pc, rT`, with entries relative to the `add`;
  - `add.w rT, rB, rI, lsl #2; mov pc, rT`, a *branch table* of 4-byte branches, which is code, not data.

  Each is bounded as in option 3 above. The cases map the switched register's value to a target (the index, or the loaded offset for `add pc`). Every target gets a label and a successor edge. A table that cannot be bounded is a problem, never a guess. `lift.core` emits a `switch` with one `goto` per case, and an index outside the table traps with `MT_TRAP_UNREACHABLE`.
- **Noreturn calls.** A call to `_abort`, `_exit`, `___assert_rtn`, `std::terminate`, `_Unwind_SjLj_Resume`, `objc_exception_rethrow` and similar imports (`NORETURN_IMPORTS`) does not fall through. The same holds for a call to a function proven never to return, found by a whole-program fixpoint: a function with no return, no indirect jump and no tail call to a returning function is noreturn, and its callers are discovered again. The lifter emits `MT_TRAP_UNREACHABLE` after such a call. The bytes after it (alignment padding, pools) are never decoded.
- **Tail calls** are `b` to another function's start. A branch out of the function to anything else is a problem.
- **Data.** Literal-pool words (the targets of PC-relative `ldr`/`ldrd`/`vldr`) and tables are recorded as data. An instruction that overlaps data is a problem.
- **Unknown NEON encodings** (the Advanced SIMD spaces, always 32 bits, never a write to the PC) are stepped over until #34 decodes them. The lifter still traps there.
- **Internal entries.** Code addresses computed in a function are tracked along straight-line code: `movw`/`movt` or a literal plus `add rX, pc`, `adr`, `mov rX, pc`, and then `add`/`orr` with an immediate. An odd one that points into the function itself is the SjLj resume address, and it becomes an **internal entry** (`Function.entries`). Its code, the landing pads and their dispatch, is discovered from there. An entry gets no label, because no branch goes there; #37 adds the resume hook and its label. An even address into the function is a `data_ref` (a constant table read through a register).
- **Seeds.** The seeds are the function starts, the entry point, the init functions, ObjC IMPs (`mtport.objc`), vtable slots (`mtport.rtti_analysis`), call and tail-call targets, and two weak sources: odd words in data sections that point into `__text`, and code addresses computed in code (`movw`/`movt` or a literal, then `add rX, pc`). A strong seed outside the starts would be discovered as an extra function. A weak one is only reported, because a data word can look like a pointer (18 do here: Korean UTF-8 bytes and CFString lengths).
- **API.** `discover_function(code, base, ptr, end, *, starts=None, noreturn=())` handles one function (the snippet tests use it as before). `discover(image)` returns a `Discovery` with `functions` (by pointer), `seeds`, `noreturn`, `failures()`, `containing(addr)`, `counts()` and `to_json()`. `Function` keeps ADR 0014's `insns`, `succ`, `labels` and `problems`, and adds `tables`, `data`, `calls`, `tail_calls`, `noreturn_calls`, `entries`, `code_refs`, `data_refs`, `indirect`, `noreturn`, `blocks()` and `extent`.
- **Outputs.** `python -m recomp discover` writes `work/recomp/functions.json` (schema 1, in the docstring of `recomp/discover.py`) and `work/recomp/discovery-report.md` (`recomp/discovery_report.py`). The exit status is 1 if more than 1% of the functions differ from Ghidra without an explanation.
- **Ghidra is a cross-check only.** The report compares every function's body with `work/ghidra/functions.json`, halfword by halfword, and gives each difference a reason:
  - **arm:** the ARM stub, which is not decoded yet (#33);
  - **after-noreturn:** dead code or padding after a noreturn call;
  - **data:** a pool or table that Ghidra decoded as code;
  - **sjlj-entry:** code only an internal entry reaches, which Ghidra never disassembles.

  Anything else is *unexplained*. The report also checks every table's targets against Ghidra's `COMPUTED_JUMP` xrefs. As a check that Ghidra does not share, it classifies every byte inside a function's bounds that is neither code nor data: padding, after a noreturn call, data addressed in code, or *unreached*.

Results on the armv7 slice:

- **Functions.** All 2,655 seeds are analysed. The one function discovery cannot finish is the ARM entry stub (A32 decoder: #33).
- **Tables.** All 184 census tables are resolved, plus the 16 branch tables. Their 2,103 targets match Ghidra's `COMPUTED_JUMP` xrefs exactly.
- **Ghidra.** Of 208 functions that differ from Ghidra, 0 are unexplained. 205 of them differ by the SjLj landing code (2,102 bytes) that only discovery reaches. `CDataMgr::OnServerPacket`, `CStateGame::Paint` and `CStateGame::OnKeyPressed` have all of Ghidra's code.
- **Unreached bytes.** Inside the function bounds, the only bytes neither reached nor marked as data are three dead `b` instructions after an unconditional branch (6 bytes). The rest is 2,604 bytes of alignment padding, 18 bytes after noreturn calls and one 32-byte float table.
- **Speed.** Discovery takes 1.5 s.
- **Lifting.** The clean lift yield goes from 2,375 to **2,524 of 2,654** Thumb functions. The remaining 130 are NEON and VFP integer conversions (#34) only.

## Consequences

- The lifter gets complete bodies: switch cases, code after a NEON instruction, and SjLj landing code with its dispatch tables. #37 finds its resume points in `Function.entries`.
- #35 has the address-taken code pointers (`code_refs`) and knows there are no indirect jumps other than returns (`indirect` is empty on this binary). #40 can report `Discovery.failures()` and the lifter problems side by side.
- The ARM entry stub stays undecoded until #33 adds `decode/arm.py`, and then `discover_function` needs an ARM path.
- A new table shape (a guard this evaluation does not follow) shows up as a problem and in the census test, never as a wrong switch.
- **How we'd know this was wrong:**
  - **Ghidra.** The cross-check reports unexplained differences, or table targets that differ from Ghidra's.
  - **Runtime.** `MT_TRAP_UNREACHABLE` fires at run time. Then a callee marked noreturn returned, or a switch index got past its bound.
  - **Speed.** Discovery's time approaches the 60 s budget (ADR 0002).
