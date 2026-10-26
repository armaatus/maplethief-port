# 17. Differential testing against Unicorn: a GPL-2.0 dev-only dependency group

- **Status:** Accepted
- **Date:** 2026-10-26
- **Issue:** #41

## Context

The snippet suite ([ADR 0015](0015-recompiler-snippet-suite.md)) checks each instruction form on hand-written code. [#41](https://github.com/armaatus/maplethief-port/issues/41) asks for the other half: run real game leaf functions as machine code in an emulator and as our recompiled C on random inputs, and compare the results. That catches bugs that come from instructions interacting (flags across blocks, IT edge cases, literal pools, stack frames). The acceptance bar is at least 200 real leaf functions passing 1,000 random inputs each with zero mismatches. It runs locally, because it needs the game binary. [ADR 0003](0003-dependencies.md) requires an ADR for a new Python package: its license and the mechanism, `pyproject.toml`, `uv.lock`, `THIRD_PARTY.md` and `tests/test_dependencies.py`.

Facts checked on 2026-10-26:

- **Unicorn 2.1.4** (PyPI, 2025-09-09, tag `2.1.4` → `8028ec436f2d9376525352dd38ed9ed6b9f6be10`) is the latest release. It has wheels for macOS arm64 and x86_64, manylinux and musllinux (x86_64, aarch64, i686) and Windows (x64, x86), so nothing needs compiling. A scratch cross-check during #33 ran the 228 snippets that call no C on it, and it agreed with every one.
- **The license is GPL-2.0.** The wheel's metadata says "BSD License", but that only describes the Python binding. The wheel bundles `libunicorn` (`unicorn/lib/libunicorn.2.dylib` plus `libunicorn.a`), and Unicorn's `COPYING` is the GPL version 2. Its README says "This project is released under the GPL license". The QEMU code inside is GPL-2.0 as a whole (QEMU's `LICENSE`): some files are GPL-2.0-or-later, some LGPL, and the TCG is mostly BSD/MIT. The wheel itself carries no license file. We therefore treat it as GPL-2.0.
- ADR 0003's rule: "Never GPL in anything compiled, linked or shipped." Developer tools under the GPL already exist: shellcheck (GPL-3.0, `scripts/lint`) and the Temurin JDK (GPL-2.0 with the Classpath exception, Ghidra).
- Selecting leaf functions needs no new tool. Whole-image discovery ([ADR 0016](0016-function-discovery.md)) already has, for each function, its calls, tail calls, entries, indirect jumps and noreturn status. The lifter reports unsupported forms and imports.

## Options considered

### The reference

1. **Unicorn (QEMU's TCG behind a small API)**: it implements ARMv7-A with Thumb-2, IT and VFPv3/v4-D32 from QEMU's well-tested ARM front end. It runs in-process at about 150–250 µs per case here, including resetting memory. GPL-2.0, so it can only be a tool.
2. **Run on real ARM hardware**: an iPhone/iPod needs signing and a device in the loop for every run. Apple-silicon Macs can't run AArch32. Rejected as the routine oracle.
3. **qemu-user with an ELF wrapper**: it would mean building an ARM ELF from the Mach-O code, plus an external process per batch. That is heavier to set up and still GPL. Rejected.
4. **Our own interpreter**: a second implementation by the same authors would share our misreadings of the ARM ARM. Rejected.

### Where the dependency lives

1. **Its own group, `difftest = ["unicorn==2.1.4"]`**, not included in `test`, run through `scripts/difftest` in its own environment (`build/python-env/difftest-<os>-<cpu>`). `scripts/test`, CI and the shipped build are unchanged. The GPL code is never installed unless someone asks for it.
2. **Add it to `test`**: every CI job and every `scripts/test` run would download about 15 MB of GPL binaries. And the synthetic part of the test that matters for CI (selection, inputs, the C driver on every compiler) doesn't need Unicorn. Rejected.

### Should CI run the synthetic Unicorn tests?

No. The CI-relevant code (leaf selection, seeded inputs, case files, the C driver compiled with clang, gcc and MSVC, the comparison) is tested in CI without Unicorn, against hand-computed results (`tests/test_recomp_difftest.py`). The Unicorn half of that file (the whole pipeline on a synthetic image, a deliberately broken store caught as a mismatch, run independence) runs locally with `scripts/difftest --pytest`. Running it in CI would add the GPL wheel to three runners to re-check something that only matters on the real binary, which CI never has.

### Second oracle: the armv6 slice

It isn't cheap, so it isn't done. The armv6 slice has the same 2,655 function starts, and they appear to pair up in order. But its `__TEXT` is 1.3 MB and its `__DATA` sits at 0x145000. So the shared window would need a different layout, and inputs that point into `__DATA` would mean different things in the two slices. The function bodies also differ (stack frames, scratch registers, and possibly the compiler). Only a return value and heap writes could be compared, and only for functions that touch no globals, with heuristics to pick those. Unicorn already gives an oracle that is independent of our lifter. The armv6 slice stays available (ARCHITECTURE) if a lifting question ever needs it.

## Decision

- **Dependency.** `unicorn==2.1.4` in the dependency group `difftest` (`pyproject.toml`, `uv.lock`), listed in `THIRD_PARTY.md` as GPL-2.0, test-only and never shipped. `tests/test_dependencies.py` checks the installed version against `THIRD_PARTY.md` when the group is installed. Only `recomp/difftest/emulator.py` imports `unicorn`, and the rest of the recompiler stays standard-library only (ADR 0014). Nothing compiled, linked or shipped contains Unicorn code. It runs as a Python extension in a developer's own environment, and nothing we distribute includes or links it. That is why GPL-2.0 is acceptable here, as with shellcheck. The guest code runs only inside Unicorn's emulator and as our recompiled C, never natively.
- **Leaf selection** (`recomp/difftest/select.py`, standard library). The criteria are applied in order, and each one's count goes into the report:
  1. discovery has no problem;
  2. the function returns and has no noreturn call;
  3. it has no SjLj entry, indirect jump or computed code pointer;
  4. it has no `svc`/`udf`/`bkpt`, no coprocessor or status-register access, and no wait hint;
  5. it lifts cleanly, with no imports and no trap other than a jump table's default;
  6. every call and tail call goes to another selected function (a fixpoint).

  "Bounded memory access" is checked at run time. A run must return within 50,000 instructions. An access outside the guest window must fault at the same address and PC on both sides. Such runs are compared, but they don't count toward the 1,000.
- **Guest window.** One window of 1.25 MiB at address 0, the same bytes on both sides: the image's `__TEXT`/`__DATA` at their link-time addresses (from `recomp.loader`), a 16 KiB heap at 0x120000, the stack below 0x134000 and the return address at 0x13F000. The C side uses the tests' bounds-checked memory (`MT_MEM_CHECKED 1`). Unicorn maps exactly the window, so the fault boundary is the same on both sides. In Unicorn, `__TEXT` is read-only. The game never writes its code, so a case that would write it is dropped before the C side runs. A C write into `__TEXT` still shows, because the driver diffs the whole window.
- **Inputs.** Each case comes from a string seed `"<seed>:<pointer>:<index>"`, so a reported seed reproduces the input exactly. r0–r12 mix small, negative and boundary values with random words and pointers into the heap and `__DATA`. A register the function uses as a memory base gets a heap pointer 85% of the time. NZCV is random. d0–d31 are random doubles, singles and raw bits. Heap images (half of their words heap pointers) come from a per-function pool, and the stack arguments are random.
- **Host side.** One executable (`recomp/difftest/driver.c` plus the lifted C) holds every leaf twice: as production code (`f_`) and lifted with `flags_in`/`flags_out` (`g_`), so NZCV can be compared. It is built with the tests' strict flags and run once per function on a binary case file. A fault, trap or dispatch miss `longjmp`s back to the driver. After each run the driver reports every byte of the window that changed.
- **No memory hooks.** In Unicorn 2.1.4, a memory hook (`UC_HOOK_MEM_WRITE` or `UC_HOOK_MEM_READ`) on a load or store inside an IT block leaves a stale IT state behind. A later instruction outside the block is then skipped as if its condition had failed. This test found it with case `7:0xCAAF1:2135`, where the `mov r0, sb` after a `bge.w` never ran. The same case without hooks, and our C, return the right value. So the emulator finds changed bytes by comparing the writable part of the window with the case's start, and the only hook left is the one for invalid accesses, which fires only on a fault. It also flushes the translation cache after every run: with the write hook installed, a run could take another path after an earlier run on the same emulator. A test checks a long-lived emulator against fresh ones.
- **Compared.** How the run ended (return, or fault address and PC); r0–r12, sp and lr; NZCV for the `g_` variant of functions without calls (a call leaves the flags undefined, AAPCS); FPSCR NZCV; d0–d31; and every changed byte of memory, on either side. A mismatch is reported with the function, the case seed and the first difference. `--only PTR --case N` re-runs that one case and prints every difference.
- **Command and report.** `python -m recomp difftest` (`scripts/difftest`, `scripts/difftest.ps1`) writes `work/recomp/difftest.json` and `work/recomp/difftest.md`. `tests/test_recomp_difftest_real.py` (`real_assets`) runs it and asserts the acceptance bar.

## Consequences

- **Result** (2026-10-26, Apple M4, 8 jobs): 741 of the 2,655 functions are leaves (selection: 2,655 discovered without problem, 2,440 return, 2,321 closed and plain, 1,094 clean; most of the rest call imports). **730 pass 1,000 returning inputs each with zero mismatches.** The run compared 735,051 returning and 454,598 faulting cases. The other 11 get fewer than 1,000 returning runs in 10,000 attempts: pointer chains through random heap words, loops bounded by a random register, or writes into `__TEXT`. 129 of the passing functions call other leaves. Only 1 has VFP code (`TouchKeyPadView_Distance`), because most VFP functions still trap on forms #34 adds or call imports. A second seed (7) also gave zero mismatches. Wall time is about 95 s, of which 4 s is selection, lifting and the C build.
- **Bugs found.** None in the lifter. The two mismatches the first runs reported (`41:0xCAAF1:2603` and `7:0xCAAF1:2135`) were Unicorn's, not ours: the memory-hook IT bug and the stale translation described above. The second was reproduced on a bare emulator with one write hook, which returns `0x23` where the hook-free emulator and our C return 0. Both cases pass now. Injected lifter bugs are caught: a carry flag `>=` → `>` showed up in 48 functions, a wrong byte store in 145, and a wrong store width in the synthetic test (`test_a_wrong_translation_is_caught`).
- **Growth.** Selection is automatic. As #34 (VFP/NEON), #35 (dispatch) and #36 (imports) land, more functions qualify, and the VFP leaves are compared on d0–d31 and FPSCR with no change here. Functions that call imports need import models in the driver before they can be compared, which is a possible later extension.
- **Cost.** One more locally installed wheel (about 15 MB) in its own environment. CI is unchanged.
- **How we'd know this was wrong:** Unicorn disagreeing with the ARM ARM where a snippet's hand-worked expectation is right (then that form needs a waiver in the comparison and a snippet as the authority), or a lifter bug found later in a function this test passed (then the input generator needs a better distribution for that pattern).

## Addendum: after the other M3 children (#40, 2026-10-26)

Re-measured on the epic branch after #34 (VFP/NEON), #35, #36 and #37 had landed, with the iOS FP mode of ADR 0022 as each case's start state (Apple M4, 8 jobs, seed 41): **786 leaves** (selection: 2,655 discovered, 2,440 return, 2,321 closed and plain, 1,136 clean), **775 pass** 1,000 returning inputs each with zero mismatches (780,051 returning and 458,655 faulting cases, 85 s). The same 11 are not bounded. 43 of the passing leaves have VFP code and 134 call other leaves. The "Result" above is the first run.
