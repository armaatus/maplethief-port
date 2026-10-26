# 15. Recompiler snippet suite: one file per snippet, stored bytes, ARM ARM expectations

- **Status:** Accepted
- **Date:** 2026-10-26
- **Issue:** #39

## Context

ROADMAP M3 has "the snippet suite passes" as an exit criterion. #39 asks for at least 100 snippet tests that run in CI on every OS, where adding a snippet is a one-file change. The tests have to cover flags, IT blocks, shifts, ldm/stm, branches, interworking and VFP. [ADR 0014](0014-recompiler-design.md) supplies the two pieces this builds on: `recomp.snippet.lift_snippet` and `tests/recomp_harness.run_snippet`. Run per test, they lift the code, compile it with the host compiler (MSVC `cl` on the Windows runner) and run it.

Constraints:

- **No assembler in CI.** CI can't rely on an ARM assembler. The Windows runner has MSVC. Current Apple clang rejects `-target thumbv7-apple-ios` ("unknown target triple", measured with the Xcode clang on 2026-10-26), though it still assembles `--target=thumbv7-none-eabi` to ELF. Capstone 5.0.9 is already a test dependency on every OS ([ADR 0003](0003-dependencies.md)).
- **Independent expectations.** The expected values must not come from the lifter, or the test only checks the lifter against itself.
- **Old Python.** Tests run on Python 3.10 ([ADR 0002](0002-language-and-toolchain.md)), which has no `tomllib`. There are no new dependencies without an ADR.
- **No game code.** It must never get into the repository.

## Options considered

### Where the bytes come from

1. **Assemble in CI.** This needs clang with the ARM target on every runner, Windows included. It ties the tests to one assembler version and adds a dependency. Rejected.
2. **Store the bytes in each file next to the assembly.** CI runs the stored bytes. Two checks keep the bytes and the assembly in step: Capstone disassembles the bytes on every OS and compares the text, and clang re-assembles the file where it has the ARM target and compares the bytes. In CI that is the macOS runner (Apple clang) and the Windows runner (LLVM clang on PATH); the ubuntu runner has no clang, so it skips that check (measured in CI run 40301118530). A script fills in the bytes from clang, so writing a snippet needs no hand encoding.
3. **Bytes only, built with our `thumbasm.py`.** It would have to grow to cover every form, and it is our own encoder, the very kind of code under test. Rejected.

### File format

1. **TOML.** It reads well, but `tomllib` arrived in 3.11 and a back-port is a new dependency. Rejected.
2. **JSON.** It's in the standard library, but it has no comments and it is awkward for assembly listings and C. Rejected.
3. **A small sectioned text format** (`title:`/`ref:` headers, then `[code]`, `[in]`, `[out]`, `[c]`, `[dispatch]`) with a stdlib parser of about 300 lines and its own tests. It is readable in review and diffs line by line.

### Where the expected values come from

1. **The ARM ARM pseudocode, worked out by hand and cited per file** (`ref:`). Reviewers can follow it. While the suite was being written, every value was also checked locally against Unicorn 2.1.4: all 202 snippets that don't call C agreed. Unicorn is not a dependency, and [#41](https://github.com/armaatus/maplethief-port/issues/41) owns the Unicorn differential test.
2. **Python models in the test**, as `tests/test_recomp_lift.py` does. That works for parametrised arithmetic, but it is a second implementation in every file and makes a snippet more than a one-file data change. Kept for the existing tests, not used for the suite.

## Decision

- **Layout.** Snippets live in `tests/recomp/snippets/<group>/<name>.snippet`. `tests/recomp_snippets.py` parses them, runs them on `run_snippet` and checks their bytes. `tests/test_recomp_snippets.py` parametrises over the files: one `test_snippet[<group>/<name>]` per file, plus a Capstone check and a clang check per file. The format and the steps for adding a snippet are in `tests/recomp/README.md`.
- **The bytes are the test input.** `[code]` lines are `<hex bytes> <assembly>`, with labels, `ext_XXXX`/`arm_XXXX` names for Thumb and ARM functions outside the snippet, and data directives. The Capstone check runs everywhere. The clang check (`--target=thumbv7-none-eabi -march=armv7-a -mfpu=vfpv3`) is skipped without clang. Running `tests/recomp_snippets.py FILE` fills in or rewrites the byte column.
- **Strict end state.** The end state is checked strictly. `[out]` lists the expected values, and every `r0`-`r14`, every 32-bit half of `d0`-`d31` and every NZCV flag it doesn't name must keep its `[in]` value. `?` waives a value. That catches stray writes, unbalanced stacks and instructions that set flags when they shouldn't.
- **Clean lifts.** A snippet must lift with no unsupported instruction and no lifting problem. A snippet that is waiting for another issue says `xfail: #NN ...` and runs as a strict xfail, so it fails once that issue lands and the header has to go. The seven such snippets wait for #34: integer `vcvt` and `vmov` with two single registers.
- **Lifter fix.** In snippet mode (`flags_out`), the flag spill at a return is not a guest read of the flags (`lift/core.py`). Before, `check_flags` reported a false problem after every call.

## Consequences

- **The suite.** 210 snippets: 203 pass and 7 are strict xfails for #34. Each one compiles and runs with MSVC on the Windows runner, as with clang and gcc elsewhere.
- **New instruction forms.** Each new form gets a snippet that is a single file and needs no Python. #32 (`tbb`/`tbh`), #33 and #34 add theirs to the same directory, and the xfail headers tell #34 what it has to make pass.
- **The cost.** Each snippet is one host-compiler run (about 0.3 s with clang on an M4). If that becomes too slow on the Windows runner, the test runtime can be compiled once per session.
- **How we'd know this was wrong:**
  - Capstone and clang disagree with the ARM ARM on some encoding, and then a snippet's bytes need a hand-checked override.
  - The text format grows conditionals or loops, and then it is time for real test code, as in `test_recomp_lift.py`.
  - Hand-worked expectations turn out wrong more often than they catch lifter bugs, and then the expectations should be generated from the Unicorn test of #41.

## Addendum: coverage of the census, A32 and traps (#33, 2026-10-26)

- **Every integer form has a snippet.** #33's census (`recomp/census.py`) names each instruction form the lifter treats differently. `tests/recomp/integer_forms.txt` holds the binary's 189 integer forms, as form names only. `tests/test_recomp_census.py` runs the same `census.form()` over every snippet that is expected to pass, and fails on any listed form that no snippet contains. That keeps the one-file rule: a new form needs one new snippet, and no mapping table. A local test keeps the list equal to the binary's census (`python -m recomp census --forms ...` regenerates it).
- **`mode: arm`.** A snippet can be A32 code. It is lifted as an ARM function, Capstone reads it in ARM mode and clang assembles it after `.arm`. Three snippets in `arm/` cover the entry stub's forms.
- **`trap = <kind> <pc>`** in `[out]`: the run must end in that `mt_trap`. The test runtime `longjmp`s back to the driver, which then prints the state as usual. In snippet mode, the lifter stores the flags to `cpsr` before every trap, so the flags at a trap are checked too.
- **Many vectors, one build.** `recomp_harness.run_cases` lifts once and runs many start states. `tests/test_recomp_flags_model.py` uses it to compare the flags of the arithmetic, the register-controlled shifts (amounts 0 to 257, the bottom-byte rule) and the logical shifter carry with a Python transcription of AddWithCarry and Shift_C. That is a model in the test, so option 2 above returns, but only for this sweep: the snippets stay data.
- **Result.** 240 snippets: 233 pass and the 7 strict xfails still wait for #34. Unicorn 2.1.4 (scratch venv) agrees on all 228 of them that call no C and expect no trap, the xfails included.
