# Debugging a trap

A **trap** is the runtime stopping the guest because it can't go on: an instruction the recompiler couldn't translate, a call to an address with no function, an unimplemented import or selector, a guest `abort()`, a guest access outside guest memory. The run ends with exit code 5 and a crash report ([ADR 0030](../../adr/0030-runtime-diagnostics.md)). This page goes from that report to the cause.

## 1. Get a run that traps, with names

Run the game from a build with the generated code ([Build from source](../README.md)), with the symbol table, which the build writes next to it as `build/<preset>/maplethief.sym`:

```sh
MAPLETHIEF_SYMBOLS=build/mac-debug/maplethief.sym MAPLETHIEF_REPORT=- \
  build/mac-debug/platform/MapleThief.app/Contents/MacOS/MapleThief
```

(On Linux the program is `build/linux-debug/platform/MapleThief`, on Windows `build/win-debug/platform/MapleThief.exe`.) Without `MAPLETHIEF_SYMBOLS`, guest addresses stay raw. The table is made from your copy of the game: it lives in `build/` only and is never packaged.

To reproduce a trap the same way every time, script the run: `--fixed-step 50` makes it deterministic, `--seconds S` or `--frames N` ends it, and `--tap`, `--hold` and `--swipe` play the input ([Test and debugging aids](../running.md#test-and-debugging-aids)). `SDL_VIDEO_DRIVER=dummy` runs it without a window, and `MAPLETHIEF_DATA_DIR` keeps its saves and settings away from your own. `--force-crash trap` traps on purpose 2 seconds in, to see the whole path once.

## 2. Read the crash report

The report goes to the log (stderr) and into `crashes/<time>.txt` in the [data folder](../running.md#the-data-folder), which also holds the version, the system, the renderer and the last 200 log lines. An interactive run then shows a dialog with the file's path. Its parts:

- **`=== guest crash: <kind> ===`**: what stopped the guest. The kinds are `MT_TRAP_*` in `runtime/include/maplethief/recomp/calls.h`, each with what its `detail` means.
- **`pc ...; detail ...`**: the guest address, as `Function+0xOFFSET` with symbols, and the kind's detail, named where it can be: `(import _name)` for an unimplemented import, `(selector name)` for an undeliverable message, the target's region for a call to nowhere or a bad return.
- **The registers**, r0 to pc, as last stored to `mt_cpu`, each annotated with its symbol or its region of guest memory: a heap block and its size, the guest stack, a segment (`__TEXT+0x...`).
- **The backtrace**, the r7 frame chain, innermost call first.

After the report the log has `boot: trap: ...` and `game: trap kind N at pc ...`, and the run exits with code 5 ([Exit codes](../running.md#exit-codes)).

| Kind | Usually means | Where to look |
|---|---|---|
| `MT_TRAP_IMPORT` | an import with no HLE function | [Adding an HLE function](hle-function.md) |
| `MT_TRAP_OBJC` | a selector a host class doesn't implement, or a message to a bad object | [Adding a selector on a host class](host-selector.md) |
| `MT_TRAP_NO_FUNCTION` | an indirect call to an address the recompiler didn't find as a function | the recompiler's function list (`recomp/`) and the target in Ghidra |
| `MT_TRAP_UNSUPPORTED`, `MT_TRAP_UNLIFTED` | an instruction or function the lifter can't translate | `recomp/`; `scripts/difftest` compares lifted leaf functions with an emulator |
| `MT_TRAP_MEMORY` | a guest access outside guest memory | the register that holds the bad address, and the HLE function that made the pointer |
| `MT_TRAP_ABORT` | the game called `abort()` or failed an assertion | the backtrace: what the game checked |
| `MT_TRAP_UDF` | a `udf`, `bkpt` or `svc` in guest code (and `--force-crash trap`) | the instruction at `pc` |

A **hardware fault** (a real `SIGSEGV` or similar in the runtime or the generated code) writes the same kind of report. Instead of a guest `pc` it names the host pc and, inside generated code, the guest function it belongs to; the process then dies of the signal instead of exiting with 5. Its guest registers can lag in optimized builds; the backtrace is exact.

## 3. Look the address up

The names come from the names contract: `tools/ghidra/` builds a Ghidra project of the game with every name applied ([its README](../../../tools/ghidra/README.md)), and `scripts/re-analyze` rebuilds all of it. Open the `pc` and each frame of the backtrace there. A name like `f_0001A2B4` means no one has named the function yet; the [function names](../../re/function-names.md) page says how many are left. When you work out what a function does, name it through a names file ([the naming contract](../../../tools/ghidra/README.md#naming-contract)), never in the Ghidra GUI alone, so the name survives a re-import.

## 4. Trace what led up to it

| Variable | What it shows |
|---|---|
| `MAPLETHIEF_TRACE_HLE=FILTERS` | every HLE call whose import name matches: comma-separated globs, `!glob` excludes, the last match wins. `'*,!_objc_msgSend*'` is every call but the message sends; `_objc_msgSend*` adds the selector to each send. Each line has the caller, symbolized. |
| `MAPLETHIEF_TRACE=CHANNELS` | the runtime's trace channels, comma-separated, or `all`: `audio`, `composite`, `diag`, `events`, `focus`, `gfx`, `gl`, `hooks`, `input`, `interp`, `uikit` and `widescreen` (the first argument of each `mt_tracef` call). `entry` logs the first entry of each guest function, in a build configured with `MAPLETHIEF_TRACE_ENTRY=ON`. |
| `MAPLETHIEF_REPORT=PATH` | the unimplemented report at exit, to a file or `-` for stderr, even when it is empty |
| `MAPLETHIEF_UNIMPLEMENTED_IMPORT=continue` | an unimplemented import or selector returns 0 instead of stopping, so the run goes on to the next problem (`trace`: and logs every call). A data import still stops. |
| `MAPLETHIEF_GL_TRACE_FILE=PATH` | every GL call, to a file |
| `MAPLETHIEF_HW_MACHINE=iPhone3,1` | the device the game sees, for code that branches on it |

Every variable is in [the table](../running.md#environment-variables). The traces are long: send stderr to a file and search it.

## 5. Fix it, and keep it fixed

Add a test that would have trapped: a doctest case for an HLE function or a selector, or a scripted run for a game path (the `play.*` tests, [Playtests](../testing.md#playtests)). Then [the quick check](../testing.md#before-you-push).

## Worked example

Followed for [#333](https://github.com/armaatus/maplethief-port/issues/333) on a Mac, against a `mac-debug` build with the generated code: `SDL_VIDEO_DRIVER=dummy`, `MAPLETHIEF_DATA_DIR` in a scratch folder, `MAPLETHIEF_SYMBOLS=build/mac-debug/maplethief.sym`, `MAPLETHIEF_REPORT=<file>`, `MAPLETHIEF_CRASH_DIALOG_LOG=<file>`, and `--no-audio --seconds 30 --fixed-step 50 --force-crash trap`. The run exited with 5 after 0.8 s. The report read `guest crash: udf, bkpt or svc`, `pc 0x00002849 main+0x3C`, with the backtrace `main+0x3C`, `start+0x28`; `crashes/` held the 64-line file, the dialog log named it, and the unimplemented report read `unimplemented report: none`.
