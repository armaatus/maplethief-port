# 0213. A guest program of our own runs end to end in CI

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** #385 (research: #346; epic #290)

## Context

CI never has the IPA ([ADR 0100](0100-public-repository-and-player-releases.md)), so the path the game takes through the port (load, discover, lift, compile, link with the runtime and the platform shell, boot, draw, play sound) runs only on a machine with the user's own copy. CI tested each layer on its own, with synthetic fixtures. Nothing tested the layers together, so a change could pass every unit test and still break the boot.

The research in #346 tried two ways to give CI a program to run:

- **Route A.** Compile our own Objective-C, C and C++ with clang for `armv7-apple-ios5.0` and link the objects with Apple's `ld` against `.tbd` stubs of our own. The result is a thin armv7 `MH_EXECUTE` with the same load commands, binds and ObjC metadata as the game's image. Building it takes well under a second. Only Apple's `ld` links 32-bit ARM Mach-O: LLVM's `ld64.lld` has no armv7, and neither `lld` nor cctools is on the Linux or Windows runners.
- **Route B.** Hand-assembled Thumb code in `tests/machofixtures.py`'s `ImageBuilder`. It works on every OS, but an ObjC app with a GL frame and sound written in assembly is days of fragile work. It would also only test code we wrote by hand, never the code shapes a compiler produces.

What the work turned up (#385):

- A modern `ld` writes `__symbol_stub4` stubs (`ldr ip, [pc]; ldr pc, [ip]`), or `__picsymbolstub4` stubs with PIE. The game has `__symbolstub1` (`ldr pc, [pc, #n]`). With `-no_pie` the image has no rebase opcodes, so it runs at its link addresses ([ADR 0025](0025-guest-memory-and-address-space.md)).
- Current clang emits SjLj code in shapes the game's LLVM-GCC never did. With NEON it saves `d8`–`d15` with `vst1` into a realigned stack, which the lifter does not support. Its landing-pad dispatch pads a one-entry `tbb` table with a zero byte, and the dispatch's bound check (`cmp rI, #1; bls`) admits that byte too. Compiling with `-mfpu=vfpv3` (VFP, no NEON) gives the game's shapes (`vpush`), and discovery treats the padding byte as no case.
- The game never throws, so the runtime had no `__cxa_throw` or typed-catch helpers. The game's window comes from its nib, so the host UIKit had no `-makeKeyAndVisible` and always loaded a nib. The game imports neither `glClear` nor `NSLog`.

## Options considered

1. **Route A, with the image built on a macOS runner and handed to the other jobs as a workflow artifact.** This is real compiler output through every layer. It needs one small macOS job (no CMake build).
2. **Route A, with the image committed.** It needs no macOS job, but the image is a built binary, and CLAUDE.md keeps every built binary out of git.
3. **Route A, with a linker of our own for armv7 Mach-O.** This would work on every OS, but it is a large tool to write and maintain for one test.
4. **Route B.** No macOS job is needed, but it costs days of work and covers less (above).

## Decision

We take option 1.

- `tests/guest/` holds the program: Objective-C, C and C++ of our own, minimal headers of our own (no SDK headers), `.tbd` stubs under the game's install names (libobjc.A, libSystem.B, libstdc++.6, libgcc_s.1, CoreFoundation, Foundation, UIKit, QuartzCore, OpenGLES, OpenAL), an ARM `start` with crt1's instructions (so the boot finds `main` as it does in the game), an `Info.plist`, and `make_sound.py`, which writes the synthetic `tone.wav`. The program uses only APIs the game imports, plus the four C++ exception helpers. It covers:
  - an app delegate with no nib
  - its own class hierarchy: `super`, a category and a class method
  - a `UIWindow` with a `UIView` subclass on a `CAEAGLLayer` and an `EAGLContext`
  - one GLES 1.1 frame: a full-screen quad as the clear (the game imports no `glClear`), a coloured triangle, then `presentRenderbuffer:`
  - a repeating `CFRunLoopTimer` as the frame loop. This is the game's own loop (`RunLoopTimer_Arm`); the game uses no `NSTimer` or `CADisplayLink`, and the runtime has neither.
  - the bundle's WAV read through CFBundle (the game's file path) and played by one OpenAL source
  - a C++ static constructor
  - a C++ `throw` of a derived class, caught as its base through SjLj
  - `puts`/`printf` lines (the game's `NSLog` is Chartboost-only and not implemented)
- `scripts/guest-image` compiles (`-mthumb -mfpu=vfpv3 -O1 -fno-jump-tables`) and links (`ld -arch armv7 -platform_version ios 5.0 5.0 -no_pie -e start`) the program into `build/guest/`. Off macOS it fails with a clear message.
- The recompiler accepts the modern stub shapes (`mtport.imports.stub_pointer`). Its `recomp all` takes `--work DIR` and `--no-hooks`. `--no-hooks` turns off the game's hooks, observers and may-return calls ([ADR 0069](0069-guest-function-hooks.md)), which are keyed by the game's addresses.
- `cmake/GuestProgram.cmake` works from `MAPLETHIEF_GUEST_DIR` (default `build/guest`). When that directory holds the image, the configure step recompiles it into `<build dir>/guest/` and adds `maplethief_guest_gen` and `MapleThiefGuest`. `MapleThiefGuest` is the same `platform/src/main.cpp`, linked with the guest's generated code instead of the game's, so CI builds no second runtime. The test `platform.guest_program` (`tests/guest/guest_program_test.cmake`) runs it headless on the virtual clock (`--fixed-step 33.333 --frames 10`). It checks:
  - every layer's log line, with the values the program computes
  - that the log has no trap, no unrecognized selector and no unimplemented report
  - the last window frame against the synthetic golden `tests/golden/data/guest_program.png`, exactly
  - that the tone is in the captured mix (`--audio-capture`)

  Without the image the test reports SKIPPED.
- CI's `guest-image` job runs `scripts/guest-image` on `macos-latest` and uploads `build/guest/` as an artifact kept for one day. Every `build` job (Linux on a pull request into `dev`, the full matrix otherwise) downloads it before its tests.

## Consequences

- One test now catches a break in any layer: a wrong flag in a lifter form, a removed HLE import, a renderer change (each shown in #385's pull request). A pull request that breaks the boot path fails CI without the IPA.
- The guest program's imports are a small part of the game's. A layer the game uses and the program does not is still covered only by its unit tests and by local real-asset runs. Widening the program is cheap: add code, then a log line or a golden pixel.
- The `build` jobs now wait for a macOS runner, typically under a minute. If `macos-latest` ever drops armv7 from Apple's `ld`, the job fails at its toolchain step. The fallback is option 3 (a linker of our own) or a pinned older Xcode.
- The runtime gains what only the guest program uses: `__cxa_allocate_exception`, `__cxa_throw`, `__cxa_begin_catch` and `__cxa_end_catch` over the SjLj unwinder ([ADR 0020](0020-sjlj-exceptions.md)); a launch with no main nib when the `Info.plist` names none and the bundle has no `MainWindow.nib`; and `-[UIWindow makeKeyAndVisible]`. The game's paths are unchanged.
- The new job name is a required check in `.github/rulesets/main.json`. Applying it to GitHub (`scripts/rulesets`) is a repository setting, done once this is on `dev`.
