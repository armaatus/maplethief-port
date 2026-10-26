# Recompiler snippet suite

Each `snippets/<group>/<name>.snippet` file is one hand-written Thumb-2 snippet: its code (encoded bytes next to the assembly), the CPU state and memory it starts from, and the state it must end in. `tests/test_recomp_snippets.py` turns every file into one test, `test_snippet[<group>/<name>]`. That test lifts the bytes with `recomp.snippet.lift_snippet`, compiles the C with the host compiler (MSVC `cl` on the Windows runner, clang or gcc elsewhere), runs it and compares the result with the file. The design is [ADR 0015](../../docs/adr/0015-recompiler-snippet-suite.md). No game code goes in here: every snippet is written by hand.

Groups: `flags/` (NZCV), `it/` (IT blocks), `shift/`, `ldst/` (single loads and stores), `ldm/` (load/store multiple, push/pop), `branch/`, `interwork/` (bx/blx/ldr pc to Thumb and ARM code), `dispatch/` (indirect calls, jumps and returns through the dispatch table: interworking, misses, bad returns; [#35](https://github.com/armaatus/maplethief-port/issues/35)), `jumptable/` (`tbb`/`tbh`, `ldr pc`, `add pc` and `mov pc` branch tables, bounded by their `cmp`; [#32](https://github.com/armaatus/maplethief-port/issues/32)), `arm/` (A32 code, `mode: arm`; [#33](https://github.com/armaatus/maplethief-port/issues/33)), `vfp/`, `alu/` (everything else).

**Every integer instruction form of the binary has a snippet.** `tests/recomp/integer_forms.txt` lists the forms of the census (`python -m recomp census`, keys explained in `recomp/census.py`). `test_every_integer_form_of_the_binary_has_a_snippet` fails if a listed form appears in no snippet that is expected to pass. A form counts as covered when one of the snippet's instructions has that form (same `census.form()`), so name the form in the title or a comment and make sure `[out]` checks its result.

## Adding a snippet

Adding a snippet is a one-file change: write the new `.snippet` file and nothing else. pytest picks it up, and no list needs updating.

1. Copy a file from the same group and change it. Write the assembly, and put `?` in the byte column for each instruction.
2. Fill in the bytes with clang (Apple's clang and LLVM's both have the ARM target):

   ```sh
   uv run --group test python tests/recomp_snippets.py tests/recomp/snippets/flags/my_case.snippet
   ```

   Without clang you can encode the bytes by hand, for example with `tests/thumbasm.py`. Capstone checks them on every OS either way.
3. Work out `[out]` from the ARM ARM pseudocode, and cite it in `ref:`. Never copy the values from the lifter's output: the test exists to check the lifter.
4. Run the file's tests, `scripts/test --python-only -- tests/test_recomp_snippets.py -k my_case`. That runs the snippet itself and both byte checks.

If the lifter can't run the snippet yet, add `xfail: #NN <reason>` with the issue that will fix it. The xfail is strict, so the test fails once the lifter can run the snippet, and then the line has to go.

## Format

```
# Comment lines start with '#'.
title: adds sets Z and C when the unsigned sum wraps
ref: A8.8 ADD (register) T1; A2.2.1 AddWithCarry(): 0xFFFFFFFF + 1 = 0x1_00000000

[code]
4218        adds r2, r0, r1     @ a trailing comment in [code] starts with '@'
7047        bx lr

[in]
r0 = 0xFFFFFFFF
r1 = 1

[out]
r2 = 0
flags = nZCv
```

**Headers** (before the first section): `title:` and `ref:` are required. `ref:` names the ARM ARM (DDI 0406C) instruction description and pseudocode that the expected values come from. `xfail:` is optional (see above). `mode: arm` makes the code A32 (ARM state, a PC read is the address + 8): it is lifted as an ARM function, Capstone reads it in ARM mode, and clang assembles it after `.arm`. Headers take no trailing comments, so a `#` there is kept.

**`[code]`**: one instruction per line, `<hex bytes> <assembly>`. The bytes are in memory order, so a 32-bit Thumb instruction is its two halfwords, each little-endian (`50eb0104` is `adcs.w r4, r0, r1`). The code is mapped and lifted at `BASE` = 0x1000. It is also in guest memory there, as `__TEXT` is at run time, so code can read its own tables. Other line forms:
- `name:` on a line of its own labels the next instruction. Branches name the label (`bne loop`). A label must not look like a register or mnemonic.
- `.long`, `.short` and `.byte` lines are data, for example a literal pool.
- `ext_2000` names a Thumb function at guest 0x2000 outside the snippet, and `arm_3000` an ARM one. `bl ext_2000` calls the C function `f_00002001` (the Thumb bit is set) and `blx arm_3000` calls `f_00003000`, both of which `[c]` defines.
- Write the assembly the way Capstone spells it, apart from what the comparison evens out (case, `.w`/`.n`, `r9`-`r15` against `sb`/`sl`/`fp`/`ip`/`sp`/`lr`/`pc`, how immediates are written, labels against addresses). Conditions are `hs`/`lo` (not `cs`/`cc`), and literal loads are `ldr r0, [pc, #8]`.

**`[in]`**: the start state, one `name = value` per line. Anything not set starts at 0, except `sp` = `SP0` (0x1FFFC0) and `lr` = `RET` (0xFFFFFFF1, the return address the harness gives).
- Registers: `r0`-`r14`, `s0`-`s31`, `d0`-`d31` and `fpscr`.
- `flags = NzCv`: the four letters N Z C V, upper case when set and lower case when clear.
- Memory: `byte ADDR = v v ...`, `half ADDR = ...`, `word ADDR = ...` or `dword ADDR = ...`, consecutive little-endian values from `DATA` (0x100000) to the top of the 2 MiB window. The stack below `SP0` is in that window.
- Values are integers (`0x..`, decimal, negative numbers allowed) or the names `BASE`, `DATA`, `SP0` and `RET`, joined by `+` and `-` (`DATA+8`, `SP0-20`). `f32:1.5` and `f64:-2.5` give the bits of a float. A trailing `# ...` is a comment.

**`[out]`**: the expected end state, in the same syntax. `name = ?` means any value, and a `?` in `flags` (for example `flags = ????` after a call, where AAPCS leaves the flags undefined) means that flag can be anything. `trap = <kind> <pc> [<detail>]` says the run must end in `mt_trap` of that kind (`udf`, `unsupported`, `bad_return`, `fell_off`, `unreachable`, `import`, `no_function`) at guest address `pc`, and with that detail if one is given (the target, for `bad_return` and `no_function`); the state is then compared as it is at the trap. **Every register `r0`-`r14`, every half of `d0`-`d31` and every flag that `[out]` doesn't name must keep its `[in]` value.** So a snippet catches stray writes, `sp` must balance, and an instruction that shouldn't set flags is checked by leaving `flags` out. `r15` is never compared. `fpscr` is compared only when it is named.

**`[c]`** (optional): C code added to the test's driver, compiled as C17 by every compiler, MSVC included. It defines the functions that the snippet calls, as `void f_XXXXXXXX(mt_cpu *MT_RESTRICT cpu)`.

**`[dispatch]`** (optional): `0x3001 = thumb_target` lines, the dispatch table for indirect calls and jumps (`blx r3`, `bx r3`, `ldr pc, ...`). Each one maps a guest function pointer (Thumb bit set, or an even ARM address) to a C function from `[c]`. The harness builds it with `recomp.dispatch` and looks it up with the runtime's `runtime/src/recomp/dispatch.c`, so the Thumb bit is part of the key: `0x2000` does not find `0x2001`. A target that is not in it ends the run with `trap = no_function <site> <target>`.

## The checks

- `test_snippet[...]` lifts the code, which must have no unsupported instruction and no lifting problem. It then compiles and runs the code, and lists every value that differs from `[out]`, followed by the listing and the generated C.
- `test_snippet_bytes_match_capstone[...]` disassembles the bytes with Capstone, the decoder oracle of ADR 0003, and compares the text with the assembly line by line. It decodes the instructions in order, so IT blocks give the conditions, and checks data lines by value. It runs on every OS, so CI needs no ARM assembler.
- `test_snippet_bytes_match_clang[...]` assembles the file with `clang --target=thumbv7-none-eabi -march=armv7-a -mfpu=vfpv3` and compares the bytes. It uses `$MAPLETHIEF_CLANG`, `clang` or the newest `clang-NN` on `PATH`, and is skipped when none of them has the ARM target. A `blx` to an ARM function is the one place clang leaves a relocation, so Capstone alone checks it. (`-target thumbv7-apple-ios` no longer works with current Apple clang, so the ELF target is used.)
- `test_suite_size` keeps at least 100 snippets that are expected to pass.

## What the suite doesn't cover yet

- `vcvt` between floating-point and integer, and `vmov` with two single registers: these are the strict xfails, for [#34](https://github.com/armaatus/maplethief-port/issues/34).
- NEON ([#34](https://github.com/armaatus/maplethief-port/issues/34)).
- Integer forms the binary doesn't use (`ssat`, `usat`, `smulbb`, `ldrex`/`strex`, ...): the census has none, so neither the lifter nor the suite needs them.
