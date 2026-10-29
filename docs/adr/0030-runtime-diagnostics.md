# 30. Runtime diagnostics: an HLE trace at the import table, one unimplemented report, an optional symbol table and a guest crash reporter

- **Status:** Accepted
- **Date:** 2026-10-27
- **Issue:** #53

## Context

Every M4+ runtime issue debugs the same way: which HLE function did the guest call, with what, from where; which imports and selectors are still missing; and, when the guest dies, where it was. The pieces that exist before this ADR:

- The import table (ADR 0019). Generated code calls every import through `mt_import_table[id](cpu, id)`. `mt_import_unimplemented` logs the first call of a missing import (or every call under `trace`) and stops or returns 0 per `MAPLETHIEF_UNIMPLEMENTED_IMPORT`.
- The ObjC runtime (ADR 0026) logs an undeliverable message (`±[Class sel]`) and follows the same policy (`MT_TRAP_OBJC`).
- Guest memory (ADR 0025). A checked access out of range goes through `mt_mem_fault` to `mt_trap(MT_TRAP_MEMORY)`. `mt_mem_describe` names any guest address. In Release code (`MT_MEM_CHECKED 0`), page zero, stack guards, uncommitted heap and stack space, anything past the arena and writes to sealed `__TEXT` fault **in hardware**. Only the host fault address and the host PC are known there.
- Dispatch (#35) has an r7 backtrace for its own miss message. It reads guest memory only inside `cpu->mem_size`, which is not enough for a crash handler: in the full reservation, uncommitted pages inside the arena fault too.
- Names. Function names live in `work/re/names/*.json` (#8/#9/#12), and the function ranges in `work/recomp/functions.json`. Both are derived from the IPA. The runtime must not read `work/` by itself, and nothing derived may ship (ADR 0024).
- Process start-up ([ADR 0027](0027-process-start-up.md), #43) runs a guest process. Its steps are milestones (`boot: <name>: …`) in the process's log sink (`mt_import_config.log`). Exit, stop and trap end a run by a host `longjmp`. The runtime's `mt_trap` (`runtime/src/boot/trap.c`) records the trap through `mt_process_trap`, which does not return while a process runs.

## Options considered

**Where to trace HLE calls.**

1. Add a logging line to each HLE function. Every implementation (#44 adds hundreds) would have to remember it, and argument formats would drift.
2. Add a check in `MT_CALL_IMPORT`. That costs a branch at every import call in generated code, and it changes the recompiler's contract.
3. **Wrap the import table.** When the trace is on, `mt_imports_link` hands the filled table to the diagnostics, which keep the real functions aside and put one trampoline in each slot whose name passes the filter. Off, the table holds the implementations themselves.

**Where names come from at run time.**

(a) Parse `work/re/*.json` in the runtime: a JSON parser in C++, and the runtime reading `work/`. (b) Compile names into the executable: derived data in a binary that could end up in a kit. (c) **A small text table generated at build time and loaded only from a configured path**, with raw addresses without it.

**Function-entry coverage.**

(a) Emit a call in every lifted function. That changes the recompiler's output and costs a call when off. (b) **A compile-time switch in `MT_FN_ENTER`** (`calls.h`), using `__func__`. Off, it expands to nothing, and the recompiler's output stays byte for byte the same.

## Decision

Options 3, (c) and (b). Interface: `runtime/include/maplethief/diag.h`. Implementation: `runtime/src/diag/`: `diag.cpp` (configuration, channels, HLE trace, report, coverage), `symbols.cpp` and `crash.cpp`. `diag.cpp` and `symbols.cpp` are plain C++ without a C++ runtime library, like `imports.cpp`, so the recompiler's C test harness links them with it.

**Configuration.** `mt_diag_configure(&cfg)`, or `mt_diag_configure_from_env()` with:

| Variable | Effect |
|---|---|
| `MAPLETHIEF_TRACE_HLE=FILTERS` | trace every HLE call whose import name matches: comma-separated globs (`*`, `?`), `!glob` excludes, the last match wins; `memcpy` also matches `_memcpy` |
| `MAPLETHIEF_TRACE=CHANNELS` | enable `mt_tracef(channel, …)` channels (`boot,objc`; `all`). `entry` logs each guest function's first entry |
| `MAPLETHIEF_REPORT=PATH` | write the unimplemented report to PATH at exit, even when empty (`-` = stderr). Unset: stderr, only if not empty |
| `MAPLETHIEF_COVERAGE=PATH` | write the function-entry coverage at exit |
| `MAPLETHIEF_SYMBOLS=PATH` | load the guest symbol table |

Addendum (#56, [ADR 0040](0040-gles-fixed-function-pipeline.md)): `MAPLETHIEF_GL_TRACE_FILE=PATH` writes every OpenGL ES call (decoded arguments, each draw's outcome, presents) to PATH. It is read by the GL layer (`maplethief/gles.h`), not by `mt_diag_configure`; the `gl` channel of `MAPLETHIEF_TRACE` logs the same lines to the log sink.

Everything goes through the configured log function. Without one, it goes to the log sink of the running guest process, or to stderr.

**Wiring into the boot (ADR 0027).** `mt_process_load` calls `mt_diag_process_begin(log, user)` before it links the imports:

- it reads the environment, unless the host already called `mt_diag_configure`;
- it routes the lines to the process's log sink, next to the `boot:` milestones;
- it starts a new report.

`mt_process_run` makes the process's cpu the crash reporter's (`mt_crash_set_cpu`). Every end of a run (exit, stop, trap) writes the report (`mt_diag_finish`). The host adds only `mt_crash_install()`, because installing signal handlers is the executable's decision.

**HLE call trace.** The trampoline `mt_trace_import_call` logs `hle: _name(r0, r1, r2, r3) [sel] from 0xCALLER Symbol+0xOFF`, calls the real function, then logs `hle: _name -> r0 (r1 …)`. For the `objc_msgSend` family it adds the selector, which is the guest address of its name (ADR 0026): `r1`, or `r2` for `_stret`. A call that never returns (a trap, the SjLj unwinder's `longjmp`) has only its first line. The arguments are the four argument registers, raw. The trace knows no signatures; an HLE function that wants more logs it on a channel. Off, there is no cost at all. Linking again without the trace restores the plain table.

**Unimplemented report.** `mt_report_unimplemented_import(name, library, caller)` is called by `mt_import_unimplemented`. `mt_report_unimplemented_selector(class, is_meta, sel, caller)` is called by the ObjC runtime's undeliverable path, so the runtime grown by #47 inherits it. A NULL class means a receiver of no known class. Entries are unique by name or by (class, selector), with a call count and the first caller, in first-seen order:

```
unimplemented report: 1 import (3 calls), 1 selector (2 messages)
import _missing (libFoo.dylib): 3 calls, first from 0x00003021 CFoo::bar+0x20
selector -[UIView setFoo:]: 2 messages, first from 0x00002003 CGame::Update+0x2
```

`mt_diag_finish` writes it once per run: at the end of a guest process's run, at host exit (`atexit`, registered by the first `mt_diag_configure`) if no run wrote it, and right after a hardware crash report, where `atexit` never runs. With `MAPLETHIEF_REPORT` set, an empty run writes `unimplemented report: none`, so a playthrough can check that nothing is missing (#117). `mt_report_get_stats` and `mt_report_format` give it to tests.

**Symbol table.** `python -m recomp symbols` (`recomp/symbols.py`) writes one line per symbol, sorted: function pointer (Thumb bit set), size, name.

```
# maplethief symbols 1
0000280D 0000006C main
00002879 00000034 -[MapleThiefAppDelegate setWindow:]
000F0C90 00000004 _AudioServicesPlaySystemSound (stub)
```

Names come from the names contract, or `f_XXXXXXXX` where there is none, plus every `__symbolstub1` stub. For the real binary that is 2,872 symbols (2,655 functions, 1,676 of them named, plus 217 stubs), 124 KB. When the recompiler's outputs exist next to `MAPLETHIEF_GEN_DIR`, the build writes `<build dir>/maplethief.sym` (target `maplethief_symbols`, `cmake/GeneratedCode.cmake`). It is never installed or packaged. The runtime parses it defensively: skipped lines are counted, there are no overflowing ranges, and it is read-only once loaded, so the crash handler may look names up. `mt_symbolize` gives `Name+0xOFF`. Without a table, addresses stay raw, and the crash report still names regions through `mt_mem_describe`.

**Function-entry coverage.** With `MT_TRACE_ENTRY 1` (CMake option `MAPLETHIEF_TRACE_ENTRY` on `maplethief_gen`), `MT_FN_ENTER` keeps a function-local `static uint64_t` entry counter. On the first entry it calls `mt_trace_fn_first(cpu, __func__, &counter)`. When on, a call costs one increment and one predictable branch; when off, nothing is emitted. `mt_coverage_format` lists `0xPTR entries name` by address.

**Crash reporter.** `mt_crash_format` writes one report for a trap or a hardware fault:

- the kind;
- the guest PC, symbolized, and the detail: the import's name for `MT_TRAP_IMPORT`, the selector for `MT_TRAP_OBJC`, the target for `MT_TRAP_NO_FUNCTION`;
- for a fault, the access and its region;
- r0–pc and cpsr as last stored to `mt_cpu`, each annotated with its symbol or its guest region (heap block, guest stack, segment);
- the backtrace.

`mt_crash_backtrace` walks the r7 chain of `{r7, lr}` frame records. A record is read only if it is 4-byte aligned and both ends lie in a live guest stack (`mt_mem_describe`), or inside `cpu->mem_size` for a plain test buffer. Each next frame must be higher up the stack, a zero lr ends the walk, and it stops after 64 frames. A broken chain can therefore never fault the reporter.

- **Traps.** The runtime's `mt_trap` calls `mt_crash_report_trap(cpu, kind, pc, detail)` before `mt_process_trap` ends the run, and the report goes line by line into the log sink. For `MT_TRAP_MEMORY` it adds the size from `mt_mem_last_fault`.
- **Hardware faults.** `mt_crash_install` installs `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` handlers on a 64 KiB alternate stack (POSIX), or a vectored exception handler (Windows). `mt_crash_set_cpu` names the guest cpu of the thread.
  - A fault whose host address lies in that cpu's reservation (4 GiB + guard, or the arena) is a guest fault. Its guest address is the host address minus `cpu->mem`.
  - The access comes from the fault context: ESR on arm64 macOS, the page-fault error code on x86-64 macOS and Linux, `ExceptionInformation[0]` on Windows. It is unknown elsewhere.
  - The guest PC is not known in generated code. On POSIX, `dladdr` on the host PC gives the generated `f_XXXXXXXX` symbol, which names the guest function. The game executable exports its symbols on Linux the same way the test does (`ENABLE_EXPORTS`); macOS executables export them by default.
  - After the report and `mt_diag_finish`, the previous handler is restored and the faulting instruction runs again, so the process still dies of its signal (or reaches whoever handled it before). A fault outside guest memory, or a second fault while reporting, passes through untouched.
  - The handler formats with `snprintf` and `mt_mem_describe` (which only tries locks), which is not async-signal-safe in the strict sense. It runs once, right before the process dies.

## Consequences

- One `MAPLETHIEF_TRACE_HLE=*` shows every HLE call of a boot without touching any HLE function, and #43, #44 and #47 add none of their own call logging. `mt_tracef` channels are for events that are not HLE calls, such as the boot steps (which ADR 0027 logs as milestones in the same sink).
- Turning the trace on changes the table's function pointers, not its behaviour. A test that compares slots against implementations must link with the trace off (the default).
- The report counts every unimplemented call under every policy. The existing log lines are unchanged, so tests that match them still pass.
- The registers of a hardware-fault report lag the guest in optimized code: generated code keeps values in host registers between stores to `mt_cpu`. The frame chain in guest memory is exact at every call, so the backtrace is reliable from the innermost call outwards. A per-instruction guest PC would need the recompiler to store it, which costs too much for a diagnostic.
- `dladdr` gives no names on Windows, and the vectored handler, written for MSVC `/W4 /WX`, has not run yet. That needs a Windows run (ADR 0023). There the report names the guest address, the region, the registers and the backtrace, but not the guest function.
- The symbol table is derived from the IPA. It lives in `work/recomp/` or the build tree only. `tools/mtport/release.py` rejects any kit file that starts with its header, whatever its name.
- **Verification:**
  - `tests/cpp/diag_test.cpp` (`runtime.*`): symbol parsing; filters; channels; the trace and the report of a test that calls an unimplemented import; selector annotation; coverage over `tests/cpp/trace_entry_fixture.c` compiled with `MT_TRACE_ENTRY 1`; the trap report and the validated backtrace on a real guest stack.
  - Four child-process tests that run `tests/cpp/crash_child.cpp`: the report at exit; a hardware write to page zero and into a stack guard, which give the symbolized backtrace followed by the report; and a checked fault through `mt_trap`.
  - `objc.undeliverable messages go into the unimplemented report`.
  - `boot.a guest trap ends the run with the trap`, which checks the crash report and the run's report in the process's log.
  - The real boot (`runtime.boot_real`, local) with `MAPLETHIEF_TRACE_HLE='*' MAPLETHIEF_TRACE=entry MAPLETHIEF_REPORT=- MAPLETHIEF_SYMBOLS=…` and `MAPLETHIEF_TRACE_ENTRY=ON`. It traces 211 lines of HLE calls up to `UIApplicationMain`, for example `hle: _objc_msgSend(0x01004200, 0x000DA379, …) [alloc] from 0x0000282B main+0x1E`. It records the 7 guest functions entered, and its report reads `unimplemented report: none`.
  - `tests/test_recomp_symbols.py`, plus `test_recomp_symbols_real.py` (local: all 2,655 functions with their exact ranges, every stub, no overlaps).
  - A local build of the real `work/gen` with `MAPLETHIEF_TRACE_ENTRY=ON` and with it off. Warnings are errors in both, and the off build has no trace symbols.
