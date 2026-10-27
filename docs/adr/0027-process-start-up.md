# 27. Process start-up: the runtime is the kernel, dyld and crt1, in one sequence with a boot trace

- **Status:** Accepted
- **Date:** 2026-10-27
- **Issue:** #43

## Context

On the device, three parties start the game before its first line of app code runs. The kernel maps the executable and dyld, and puts `argc`, `argv`, `envp` and the `apple` strings on the main thread's stack. dyld maps the dylibs, binds the image's pointers, lets libobjc realize the classes, runs the static constructors and jumps to the entry point. crt1's `start` then calls `main`, and `main` calls `UIApplicationMain`, which never returns. The runtime has to do all of that for the recompiled program, in an order that the parts built so far already depend on: the import table ([ADR 0019](0019-import-thunks-and-hle-abi.md)), guest memory ([ADR 0025](0025-guest-memory-and-address-space.md)) and the Objective-C runtime ([ADR 0026](0026-objective-c-strategy.md)).

Facts from the armv7 slice (`work/recomp/loader.json`, the bytes of the slice):

- **Entry.** `LC_UNIXTHREAD` sets `pc = 0x27E0` with the T bit clear. There is no `LC_MAIN`. The code there is crt1's `start` for iOS, 11 ARM instructions: `ldr r0, [sp]` (argc), `add r1, sp, #4` (argv), a loop that skips `envp` to find `apple`, `bic sp, sp, #7`, `blx 0x280C` (`main`, Thumb, so `0x280D`) and `b exit`. It never touches `NXArgc`, `NXArgv`, `environ` or `__progname`.
- **`__program_vars`** (`0xF1370`, 20 bytes) is dyld's `ProgramVars`: `{0x1000 (the Mach header), &NXArgc 0x11A400, &NXArgv 0x11A404, &environ 0x11A408, &__progname 0x11A40C}`, all in `__common`. dyld fills those four variables.
- **`__mod_init_func`** (`0xF14CC`, type `S_MOD_INIT_FUNC_POINTERS`) holds two constructors, `0xA925` and `0x9E041` ([re/program-flow.md](../re/program-flow.md#static-constructors)). Each registers a destructor with `__cxa_atexit`: `0x6639` (`TextCache_StaticFini`) and `0x9DBB1` (`TaggedString_StaticFini`). dyld calls initializers as `func(argc, argv, envp, apple, &programVars)`.
- **No rebasing.** The rebase stream is empty, and the image runs at its link-time addresses (ADR 0025).
- **Imports before `UIApplicationMain`.** The constructors call `operator new` (`__Znwm`), `operator new[]` (`__Znam`), `strcpy` and `__cxa_atexit`. `main` sends `+[NSAutoreleasePool alloc]` and `-init`, then calls `UIApplicationMain(argc, argv, nil, nil)`.

## Options considered

**Where the boot reads the image's layout.**

1. **`work/recomp/loader.json`.** The runtime would need a JSON reader, and the shipped port would need the manifest next to the slice.
2. **Generated tables**, like the import tables of ADR 0019. This means one more recompiler output and one more generated file to keep in sync.
3. **The Mach-O itself.** The runtime already reads the slice to map its segments. Its sections and `LC_UNIXTHREAD` are in the same load commands, and dyld reads them from there too.

**How `main` is reached.**

1. **Run `start` as it is.** This is the most literal choice. But `start` calls `main` with a direct C call in the generated code, so the boot cannot see `main` start. The trace could only show `main` afterwards, from a return address.
2. **Do what `start` does in the host, then call `main`.** The boot recognises crt1's stub word by word, takes `main` from its `blx`, and calls `main` with `start`'s registers. `lr` is the address of `start`'s `b exit`, so backtraces look the same. Any other entry runs as it is.

**How a run ends.** `UIApplicationMain` never returns on a device, and the app lifecycle that will run inside it (#54) does not exist yet. The boot test, a trap and `exit` must all leave the guest without killing the host process.

1. **Return codes through every frame.** Generated code has no way to return early.
2. **A host `longjmp` back to the run call.** The SjLj unwinder already uses this pattern (ADR 0020). Generated code keeps no state in C locals across calls, and the runtime's C++ uses no exceptions and no destructors that would be skipped.

## Decision

Option 3 for the image, option 2 for `main`, and `longjmp` to end a run. The interface is `runtime/include/maplethief/boot.h`. The implementation is `runtime/src/boot/` (C17):

- `boot.c`: loading and the sequence
- `image.c`: the Mach-O's sections, its entry and the crt1 check
- `objc_boot.c`: the Objective-C steps
- `process.c`: what guest code reaches while it runs
- `trap.c`: the runtime's `mt_trap`

**The sequence.** `mt_process_load` does steps 1 to 10. Each step is one function and one milestone of the boot trace.

1. `image`: read the sections, the entry and the rebase size, and refuse an image with rebases. Reserve guest memory, map the segments, attach the main thread's `mt_cpu` and set FPSCR to `MT_FPSCR_IOS` ([ADR 0022](0022-guest-floating-point-mode.md)).
2. `link`: fill the import table by name. A list of implementations from the configuration comes first, then the runtime's registry.
3. `objc`: create the Objective-C runtime, so that the `init` hooks of the imported class objects in step 4 can register host classes with it (`mt_process_objc(mt_process_current())`).
4. `place`: give every data import a zeroed object on the guest heap, then run the `init` hooks.
5. `binds`: write the 1,268 bound pointers, and give the SjLj unwinder its personality addresses.
6. `objc-image`: the ObjC start-up of ADR 0029: record the placed class objects, add the host classes and placeholders, then `mt_objc_load_image` over `__objc_classlist`, `__objc_catlist` and `__objc_selrefs`.
7. `objc-overrides`: install the host overrides of ADR 0026 / ADR 0029 (`mt_objc_install_overrides`), then `mt_objc_set_current`.
8. `seal`: make `__TEXT` read-only.
9. `stack`: create the 1 MiB main stack. At its top goes the kernel's layout: `argc`, `argv[]`, 0, `envp[]`, 0, `apple[]`, 0, then the strings. `sp` points at `argc` and is 16-byte aligned.
10. `program-vars`: write `argc`, `argv`, `envp` and the basename of `argv[0]` through the pointers in `__program_vars`. Its Mach header word must be the image's.

`mt_process_run` does the rest:

11. `constructor` (one per entry): every `S_MOD_INIT_FUNC_POINTERS` entry, called through `mt_dispatch` with `(argc, argv, envp, apple, &programVars)`. The fifth argument goes at `[sp]`. While the constructors run, every `__cxa_atexit` adds an `atexit` milestone.
12. `main`: if the entry is crt1's `start`, the boot calls `main` with `start`'s registers: r0–r3 = `argc`, `argv`, `envp`, `apple`, `sp` at `argc` aligned to 8, and `lr` = `start + 40`. When `main` returns, the boot calls the guest's own `_exit` import, as `start`'s `b exit` does. Any other entry runs as the kernel would start it (`entry`), with `sp` at `argc`.

**Arguments and environment.** `argv[0]` and `apple[0]` are the executable's guest path. By default that is `/var/mobile/Applications/00000000-0000-0000-0000-000000000000/MapleThief.app/MapleThief`, a device sandbox with a fixed placeholder UUID. The default environment is what an iOS app saw: `PATH`, `TMPDIR`, `SHELL`, `HOME`, `USER`, `LOGNAME` and `CFFIXED_USER_HOME`, under that sandbox. The game reads none of them (it imports no `getenv`), but `environ` is valid. The configuration can replace the path, add arguments or replace the environment.

**The boot trace.** Every milestone is one log line, `boot: <name>: <detail>`, through the import configuration's log sink. It is also an entry `{name, value}` that tests and tools read with `mt_process_milestones`. Other modules add theirs with `mt_process_milestone`: `UIApplicationMain` here, and later the app delegate and the first GL frame (#54, #117).

**`UIApplicationMain`** is an HLE import. It records its milestone, with the caller in `lr`, and passes the guest's arguments to the app lifecycle hook `mt_boot_config.app_main`, which #54 and #117 provide. With no hook installed, it stops the guest.

**How a run ends.** `mt_process_run` takes a host `setjmp`, and every end is a `longjmp` back to it. The process and its guest memory stay readable until `mt_process_destroy`.

- **exit:** `_exit`, or `main` returning when `_exit` is unimplemented and the policy lets it return. The `__cxa_atexit` handlers run first, last registered first, each through `mt_dispatch` with its object. The outcome is `MT_PROCESS_EXITED` with the status.
- **stop:** `mt_process_stop(cpu, reason)`. No handler runs, and the outcome is `MT_PROCESS_STOPPED`.
- **trap:** the runtime's `mt_trap` (`trap.c`) logs the trap, records it as a milestone and returns `MT_PROCESS_TRAPPED` with its kind, PC and detail. Without a running process, it aborts the host. It is alone in its file, so a test that defines its own `mt_trap` links the runtime library without it.

**`__cxa_atexit` and `exit`** belong to this module, because they are the process's own bookkeeping: `mt_process_finalize(cpu, dso)` is `__cxa_finalize`. The rest of the C++ ABI, including `operator new` and `delete`, stays with #45.

**One process at a time.** The guest is single-threaded (ADR 0014). `mt_process_current` is the loaded process, and a second `mt_process_load` fails.

**Linking.** `cmake/GeneratedCode.cmake` compiles `runtime/src/recomp/dispatch.c` into `maplethief_gen`, and links `maplethief_gen` to the runtime, so a program that links both is complete. `process.c` needs only the dispatch table and the import code, so the recompiler's C test harness compiles it together with `registry.cpp`.

## Consequences

- **#47** fills in the Objective-C steps in `objc_boot.c`: the host classes (registered from the `init` hooks, through `mt_process_objc`), stricter checks and `+initialize`. Until then, step 6 logs one problem for each image class whose superclass is a framework class.
- **#54 and #117** provide `app_main`: `UIApplication`, `MainWindow.nib`, the delegate and the run loop, with more milestones. #117's boot test builds on `tests/cpp/boot_real_test.cpp`.
- **#53** replaces the one-line trap report in `trap.c` with the crash report (registers and a symbolized backtrace). The milestones are a natural part of that report.
- **#44** implements `exit`'s stdio side, if any, on top of `mt_process_exit`. `_exit` is registered here.
- **Tests.**
  - `tests/cpp/boot_test.cpp` (doctest, `boot.*`, in CI) builds a synthetic Mach-O with crt1's stub, `__program_vars`, two constructors and a bind. It checks the milestone order, the stack layout, the program variables, the constructors' and `main`'s arguments, the atexit order, exit, stop, trap, the configuration's implementations and arguments, a non-crt1 entry and malformed images.
  - `tests/cpp/boot_real_test.cpp` (`runtime.boot_real`, label `real_assets`, local only) links `maplethief_gen`. It boots the slice with the policy `stop` and checks this trace: `image 0x27E0 → link → objc → place 111 → binds 1268 → objc-image 75 → objc-overrides 2 → seal → stack → program-vars 0xF1370 → constructor 0xA925 → atexit 0x6639 → constructor 0x9E041 → atexit 0x9DBB1 → main 0x280D → UIApplicationMain → stop`. It also checks that `UIApplicationMain(1, argv, nil, nil)` is called from inside `main`, that no import reached the default handler, the program variables, the text cache's records on the heap, and the ObjC runtime's state (75 image classes realized with 0 problems, 2 overrides, main's `NSAutoreleasePool` in place, no undeliverable message; ADR 0029). Imports that other M4 issues own get strict stand-ins in that test, and the test fails once the runtime implements one of them.
- **How we'd know this was wrong:**
  - Guest code reads something from the start-up that we don't set up: an `apple` string other than the path, the kernel's `mach_header` slot below `argc`, or environment variables. Each is a small addition to step 9.
  - A `longjmp` skips host state that should have been cleaned up, for example a host-side lock or object that is live while the guest runs. Then the stop path needs explicit cleanup hooks.
  - Another binary's entry is not crt1's stub. It then runs as the entry, which is correct, but the trace has no `main` milestone.
