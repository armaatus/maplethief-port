# Architecture (proposal)

> Status: **proposal**. Each major choice below is confirmed, refined or replaced by an ADR in `docs/adr/` (the issues that own each decision are named inline). Once an ADR is accepted, update this document to match it and link the ADR.

Facts referenced here come from [ANALYSIS.md](ANALYSIS.md).

Accepted decisions so far:

- [ADR 0002](adr/0002-language-and-toolchain.md): languages, minimum toolchain versions, formatting and linting.
- [ADR 0003](adr/0003-dependencies.md): third-party libraries, their pinned versions, and how the build fetches them ([THIRD_PARTY.md](../THIRD_PARTY.md)).
- [ADR 0004](adr/0004-build-presets-and-generators.md): CMake presets, generators, compiler flags and the headless smoke test.
- [ADR 0005](adr/0005-test-harness.md): test layout, `scripts/test`, the `real_assets` marker and the golden-image helper.
- [ADR 0006](adr/0006-continuous-integration.md): GitHub Actions CI, what runs on which OS and when, and how CI is kept away from the IPA; since #113 it runs on pull requests and manual runs only (Release jobs on manual runs), optionally on self-hosted runners.
- [ADR 0007](adr/0007-re-tooling-ghidra.md): RE tooling: pinned headless Ghidra 12.2 + JDK 25 driven by PyGhidra scripts, the deterministic export schema and the `work/re/names/*.json` naming contract ([tools/ghidra/README.md](../tools/ghidra/README.md)).
- [ADR 0008](adr/0008-cpp-rtti-and-virtual-function-names.md): C++ classes and vtables are recovered from the Mach-O's RTTI binds by `tools/mtport/rtti.py`, with `Class::vfN` / `~Class` / `@thunk` / key-derived method names ([docs/re/cpp-classes.md](re/cpp-classes.md)).
- [ADR 0009](adr/0009-re-analysis-outputs.md): RE analysis outputs: Capstone `armscan` as a second xref source cross-checked with Ghidra, `libtags`' ordered first-claim-wins tag rules, and generated docs in `docs/re/` (new docs: generated blocks inside hand-written Markdown) built from the canonical export of `scripts/re-analyze`.
- [ADR 0010](adr/0010-asset-pipeline-and-viewers.md): asset pipeline and viewers: one standard-library Python renderer (`tools/mtport/png.py`) produces every pixel, and the map, sprite and table viewers are generated static HTML pages under `assets-out/viewers/`, each with a batch mode.
- [ADR 0011](adr/0011-asset-pipeline-manifest-and-incremental-runs.md): asset pipeline manifest: every bundle file gets one rule (converted / copied / ignored with a reason) from `mtport.formats`, `assets-out/manifest.json` records content and output hashes, and rule keys hash the parser source so a code change invalidates its outputs.
- [ADR 0012](adr/0012-re-module-layout-and-shared-helpers.md): RE module layout: a large `tools/mtport/<name>.py` is the public face (docstring, CLI, re-exports) of `<name>_analysis` / `<name>_doc` parts, and each low-level helper (Thumb-2 decoding, function owner, data-object window, export loading, address parsing, names files, generated doc blocks) has one implementation in `armscan`, `ghidra_common`, `names` or `macho`.
- [ADR 0013](adr/0013-ghidra-analysis-without-time-limits.md): Ghidra auto-analysis has no time limits (decompiler timeouts 0, demangler one day), so the export doesn't depend on the host's speed. The non-default options are `mtport.ghidra_common.ANALYSIS_OPTIONS`.
- [ADR 0014](adr/0014-recompiler-design.md): the static recompiler: our own standard-library Thumb-2/VFP decoder with Capstone as its test oracle, the armv7 slice at its link-time addresses, generated C17 functions `void f_XXXXXXXX(mt_cpu *)` over a `restrict`-qualified CPU state with flags in C locals, the guest memory API for both memory models, native returns, the dispatch / import / SjLj interfaces of `runtime/include/maplethief/recomp/`, and the `recomp/` module layout.
- [ADR 0015](adr/0015-recompiler-snippet-suite.md): the recompiler snippet suite: one `tests/recomp/snippets/<group>/<name>.snippet` file per hand-written Thumb-2 snippet (bytes next to the assembly, start state, expected end state from the ARM ARM), stored bytes checked by Capstone everywhere and clang where available, so CI needs no ARM assembler; registers and flags not named in `[out]` must be unchanged.
- [ADR 0016](adr/0016-function-discovery.md): function discovery is recursive descent with the recompiler's decoder. Jump tables are bounded by their guarding `cmp` through a value-numbered walk back, noreturn functions come from a fixpoint, and SjLj resume addresses are internal entries. Ghidra is only a cross-check in `work/recomp/discovery-report.md`.
- [ADR 0017](adr/0017-unicorn-differential-testing.md): the differential test of real leaf functions, run as machine code in Unicorn and as recompiled C on seeded random inputs (registers, flags, VFP registers and every changed byte compared). Unicorn is GPL-2.0, so it sits in its own `difftest` dependency group that is local only, outside `test` and CI, and never ships.
- [ADR 0019](adr/0019-import-thunks-and-hle-abi.md): import ids in `nlist` order, function/data classification with a size table for data symbols, the generated descriptor / bind / thunk tables of `imports.c`, the runtime filling `mt_import_table` by name from its HLE registry, the unimplemented-import policy (`MAPLETHIEF_UNIMPLEMENTED_IMPORT`), and the iOS armv7 calling convention of `maplethief/hle/abi.h`.
- [ADR 0020](adr/0020-sjlj-exceptions.md): SjLj exceptions: the lifter finds each resume address's jump-buffer store and emits a host `setjmp` hook and the resume label there; `runtime/src/sjlj.c` implements GCC's SjLj unwinder and personality over the guest's contexts and LSDAs (`_setjmp`/`_longjmp` on Apple), registered as HLE imports; uncaught exceptions, `throw()` violations and `std::terminate` trap. The game never throws in normal play.
- [ADR 0021](adr/0021-generated-code-partition-and-build.md): the generated C is split into translation units by fixed address cells of `__text` (64 by default), so a change to one function rewrites and rebuilds one unit. Every file is written only if it changed, and `work/gen/gen.cmake` feeds the target `maplethief_gen` through `cmake/GeneratedCode.cmake` when `MAPLETHIEF_GEN_DIR` (default `work/gen`) holds it.
- [ADR 0022](adr/0022-guest-floating-point-mode.md): the guest floating-point mode is the iOS thread default FPSCR = DN | FZ (round to nearest, flush-to-zero, default NaN), fixed at compile time for VFP and NEON alike; it supersedes ADR 0014's FPSCR = 0 assumption.
- [ADR 0024](adr/0024-release-contents-and-flow.md): a release carries one own-code port kit per platform (never the game build), built locally from an annotated `vX.Y.Z[-rc.N]` tag by `scripts/release` / `scripts/release.ps1` (tag → draft → upload → publish, generated notes grouped by label, `SHA256SUMS`); the version comes from the tag through `MAPLETHIEF_VERSION` (`MapleThief --version`), and a content guard (`tools/mtport/release.py`) rejects generated code, derived files and recompiled symbols.
- [ADR 0023](adr/0023-local-ci-and-packaging.md): every CI check runs locally first: `scripts/local-ci` on the Mac (lint, both Pythons, macOS, and Linux x86_64 in the pinned container of `tools/containers/linux/`, run by `scripts/test-linux`), `scripts\test.ps1` on Windows; the release workflows package a kit with CPack (`cmake/Packaging.cmake`: ZIP via `ditto` on macOS, ZIP on Windows, TGZ on Linux) that never contains generated code or anything from `work/`.
- [ADR 0025](adr/0025-guest-memory-and-address-space.md): guest memory. One fixed layout on every host: page zero, the image at its link-time addresses, a 480 MiB guest heap and 16 MiB of guarded guest stacks, all inside a 512 MiB arena at guest 0. It is backed by a 4 GiB + guard reservation (unchecked Release code) or by the arena alone (checked code; the iOS fallback). `MAPLETHIEF_MEM_MODEL` picks `MT_MEM_CHECKED` for the generated code. The heap's bookkeeping lives on the host, and an out-of-range access traps as `MT_TRAP_MEMORY` with the guest PC.
- [ADR 0026](adr/0026-objective-c-strategy.md): Objective-C runs on a runtime over the guest's own metadata (`runtime/src/objc/`): `objc_msgSend` looks methods up in the image's classes, recompiled IMPs run unchanged, and framework classes are host classes bound to the imported class objects. The shell is not rewritten. Host overrides replace single methods, such as `+[Chartboost sharedChartboost]` → nil.

## Goals and constraints

- Run MapleStory Thief Edition 1.5 natively on **macOS** (universal arm64 + x86_64), **Windows 10/11 x64** and **iOS 16+ arm64**, with **Linux / Steam Deck** where it's cheap.
- Keep game logic **faithful**: same timing, same RNG, same save semantics. Enhancements are opt-in and layered on top (M10).
- **Controller support** as a first-class input path, not as fake touches where avoidable.
- **Build from your own copy.** The repo contains no derived game content. The build reads `original/*.ipa` and produces everything else locally.

## Languages and toolchain

Settled in [ADR 0002](adr/0002-language-and-toolchain.md), which also has the rules for each language:

| Part | Language |
|---|---|
| `runtime/` | C++20, with a C (`extern "C"`) interface for generated code |
| `platform/` | C++20 on SDL3; Objective-C++ only in `platform/macos/` and `platform/ios/` where SDL3 falls short |
| Generated code (`work/gen/`) | C17, portable subset. Shipping builds compile it with clang everywhere (Apple clang, clang-cl, clang); MSVC `cl` must also compile it |
| `recomp/`, `tools/` | Python 3.10+ (stdlib only on the bootstrap path) |
| `scripts/` | bash entry points (+ PowerShell on Windows); logic lives in Python |
| Build | CMake ≥ 3.28, Ninja preferred |

Minimum compilers: Apple clang 15 (Xcode 15.0), MSVC 19.38 (VS 2022 17.8), clang-cl 17, GCC 13, Clang 17. Formatting and linting: clang-format, ruff and shellcheck at pinned versions, all run by `scripts/lint`.

## Third-party dependencies

Settled in [ADR 0003](adr/0003-dependencies.md) and listed with versions, commits, hashes and licenses in [THIRD_PARTY.md](../THIRD_PARTY.md):

| Dependency | Role |
|---|---|
| SDL3 (static) | Platform layer, audio device, `SDL_LoadWAV` |
| dr_mp3 | MP3 BGM decoding behind `AVAudioPlayer` |
| stb_image + stb_image_write | PNG decode (incl. CgBI) and encode behind `UIImage`/`CGImage` |
| doctest | C/C++ unit tests through CTest |
| pytest, Capstone (PyPI, via `uv`) | Python tests; disassembler for tooling and as a recompiler test oracle |
| Unicorn (PyPI, group `difftest`, GPL-2.0, local only) | Execution oracle of the recompiler's differential test ([ADR 0017](adr/0017-unicorn-differential-testing.md)); never linked or shipped |

OpenAL is our own translation layer onto SDL3 audio streams, not OpenAL Soft. C/C++ dependencies come from hash-pinned URLs through CMake `FetchContent` ([`cmake/Dependencies.cmake`](../cmake/Dependencies.cmake)), into a download cache under `build/`. Nothing third-party is committed.

## Approach: hybrid static recompilation

```
original/MapleThief-1.5.ipa
        │  scripts/bootstrap (verify + extract + split fat binary)
        ▼
work/ipa/…/MapleThief.app ─────────────► tools/ asset pipeline ──► assets-out/ (PNG, JSON, disasm)
        │ armv7 slice
        ▼
recomp/  (loader → decoder → function discovery → lifter → C output; ADR 0014)
        │
        ▼
work/gen/*.c  (one C function per guest function, ~2,655)
        │
        ▼
runtime/  (HLE: memory, loader, libc/C++/CF/Foundation, ObjC, GCD, GLES1, OpenAL, …)
        │
        ▼
platform/  (SDL3 frontend per OS: window, input, gamepad, audio device, paths)
        │
        ▼
build/<preset>/platform/MapleThief (.app / .exe / iOS app)
```

### 1. Static recompiler (`recomp/`)

Ahead-of-time translation of the **armv7** slice (Thumb-2 + the single ARM entry stub) into C, in the style of N64Recomp / XenonRecomp / ReXGlue. The recompiler is standard-library Python and runs only on the build machine ([ADR 0002](adr/0002-language-and-toolchain.md)). The design is settled in [ADR 0014](adr/0014-recompiler-design.md):

- **Input:** the thin armv7 Mach-O, mapped at its link-time addresses (slide 0), so PC-relative addresses and literal-pool loads from the immutable `__TEXT` fold into constants. The armv6 slice is not lifted: it has as much conditional execution, none of the RE work applies to it, and it stays available as a second oracle (#41). `recomp/loader.py` builds on `mtport.macho` ([#31](https://github.com/armaatus/maplethief-port/issues/31)): segments and sections with their file mapping, `LC_FUNCTION_STARTS`, the entry point, the 2 init functions, rebases and the 1,272 binds, the 333 imports with their stub and pointer addresses, and the ObjC section ranges. `python -m recomp load` writes them to the loader manifest `work/recomp/loader.json` (schema in `recomp/loader.py`), which the runtime's loader reads.
- **Decoding:** our own Thumb-2/VFP decoder from the ARM ARM's tables (`recomp/decode/`), with explicit IT state, plus an A32 decoder for the entry stub's instruction classes (`decode/arm.py`, [#33](https://github.com/armaatus/maplethief-port/issues/33)). Capstone is its oracle in tests: identical text on all 330,052 instructions of `__text`, on every 16-bit encoding, on 60,000 random 32-bit ones per CI run and on random words of every A32 class. `decode/neon.py` decodes the Advanced SIMD classes the binary uses (381 instructions, [#34](https://github.com/armaatus/maplethief-port/issues/34)).
- **Function discovery:** recursive descent with our own decoder from the 2,655 `LC_FUNCTION_STARTS` seeds plus ObjC IMPs, vtable slots, init functions and call targets (`recomp/discover.py`, [ADR 0016](adr/0016-function-discovery.md), [#32](https://github.com/armaatus/maplethief-port/issues/32)). It resolves jump tables from their guarding `cmp` (184 `tbb`/`tbh` and 16 SjLj branch tables), stops at noreturn calls (imports plus a whole-program fixpoint), marks literal pools and tables as data, and enters the SjLj landing code at its resume address. `python -m recomp discover` writes `work/recomp/functions.json` (blocks, edges, mode, tables, calls) and `work/recomp/discovery-report.md`. The report cross-checks the result with the Ghidra export, which is never an input: 0 unexplained differences, and every table target agrees.
- **Lifting:** each guest function becomes `void f_XXXXXXXX(mt_cpu *MT_RESTRICT cpu)`, named after its function pointer (Thumb bit set). Each instruction becomes a few C statements, IT and other conditional instructions an `if`, and branch targets labels. One lifter module per instruction class lives in `recomp/lift/`; a form with no lifter becomes a trap at its address and a coverage entry. `python -m recomp census` ([#33](https://github.com/armaatus/maplethief-port/issues/33)) counts every instruction form and its trapped instances in `work/recomp/census.json`. No instruction of the binary traps (all 2,655 functions lift cleanly, [#34](https://github.com/armaatus/maplethief-port/issues/34)), and each of its 189 integer and 55 VFP/NEON forms has a snippet test.
- **CPU state:** `mt_cpu` (`runtime/include/maplethief/recomp/cpu.h`) holds `r[16]`, `cpsr`, `fpscr`, `d[32]` (s registers are the halves of d0–d15), the guest memory base and size. Generated code accesses it directly through a `restrict` pointer, so the state is exact at every call, return, trap and SjLj resume while the host compiler still keeps registers in host registers between calls. N, Z, C and V are C locals computed only if the function reads them; the recompiler proves per function that no flag is live into a function or across a call.
- **Floating point:** one fixed guest mode, the FPSCR iOS gives every thread and the binary never changes: round to nearest, flush-to-zero, default NaN (`0x03000000`, [ADR 0022](adr/0022-guest-floating-point-mode.md)). That is also NEON's standard mode, so VFP and NEON f32 lanes share the `vfp.h` helpers. They compute with host IEEE arithmetic (contraction off) and apply the ARM rules explicitly: input and output flushing decided on the exact value, the default NaN, and saturating float→integer conversions with NaN → 0, all without C undefined behaviour, so every host gives the same bits.
- **Memory model:** guest address + `cpu->mem`, through `memcpy`-based `MT_LD*`/`MT_ST*` macros that carry the guest PC. One API, two models: a 4 GiB reservation with no checks, or bounds checks against the 512 MiB arena that trap with the guest PC (Debug builds, tests, iOS). The runtime side is `maplethief/memory.h` ([ADR 0025](adr/0025-guest-memory-and-address-space.md), [#42](https://github.com/armaatus/maplethief-port/issues/42); see section 2).
- **Calls:** `bl`/`blx imm` set `lr` and call the target's C function; returns are C returns (checked against `lr` at entry in debug builds); `b` to another function is a guaranteed tail call under clang (`MT_TAILCALL`). Indirect calls and jumps (`blx reg`, `bx reg`, vtables, ObjC IMPs, function pointers, blocks) go through `mt_dispatch`, the **guest function pointer → host function table** (`work/gen/dispatch.c`: every function, keyed by pointer with the Thumb bit, plus a 16-byte bucket index; then the import thunks of `imports.c`; the lookup is `runtime/src/recomp/dispatch.c`). A miss traps with `MT_TRAP_NO_FUNCTION` and is reported with the site, the function it is in, `lr`, the reason (wrong instruction set, misaligned, mid-function, null) and the r7 backtrace; there is no interpreter fallback ([#35](https://github.com/armaatus/maplethief-port/issues/35), ADR 0014 addendum).
- **Imports** ([ADR 0019](adr/0019-import-thunks-and-hle-abi.md), [#36](https://github.com/armaatus/maplethief-port/issues/36)):
  - **Ids and classification.** All 333 imports have an id, their `nlist` index. 222 are functions: called through a stub, or reached only through a pointer. 111 are data: ObjC classes, constants and block isas.
  - **Calls.** A call through a `__symbolstub1` stub is `MT_CALL_IMPORT(id)`, which goes through `mt_import_table`.
  - **The generated tables.** `work/gen/imports.c` holds a descriptor per import (flags, guest code address, data size and alignment), every bind (slot, id, addend), and one thunk per function import, so that a guest pointer to an import is callable through the dispatch table.
  - **Start-up.** The runtime (`runtime/src/hle/`) fills the table by name from its HLE registry. Every other slot gets the default handler, which logs the name and the caller (`lr`), then stops or returns 0, per `MAPLETHIEF_UNIMPLEMENTED_IMPORT=stop|continue|trace`. It also places zeroed data objects through the memory model's allocator, then writes the binds.
  - **Calling convention.** HLE functions read their arguments with the iOS armv7 rules in `maplethief/hle/abi.h`: soft-float, 64-bit values 4-byte aligned and allowed to split across r3 and the stack, structures larger than 4 bytes returned through r0.
- **Exceptions:** SjLj (205 functions, [ADR 0020](adr/0020-sjlj-exceptions.md)). Where the guest stores a resume address into a function context's jump buffer, the generated code takes a host `setjmp` (`MT_SJLJ_SETJMP`, found by `recomp/lift/sjlj.py`) and the runtime's unwinder (`runtime/src/sjlj.c`: `_Unwind_SjLj_*`, the personality reading the guest's LSDAs, the ObjC catch helpers) `longjmp`s back to the resume label with the guest state in `mt_cpu`. `python -m recomp exceptions` writes the context census, `work/recomp/exceptions.json`. No guest code starts an exception, so in normal play the landing pads never run; an uncaught exception or `std::terminate` traps.
- **Output:** C17 in `work/gen/` (`mt_gen.h`, `fn_NNN.c`, `imports.c`, `dispatch.c`, `gen.cmake`), never committed. A function goes into the unit of the fixed `__text` address cell that holds it (64 cells by default), and files are written only when they change, so a change to one function rebuilds one unit ([ADR 0021](adr/0021-generated-code-partition-and-build.md), [#38](https://github.com/armaatus/maplethief-port/issues/38)). The full Release build of the 42 MB of C takes about 7 s on an Apple M4 with 10 cores, bound by the four largest functions (0.8–1.3 MB of C each). Reports go to `work/recomp/`. `python -m recomp` is the command line (`all` runs load, discover and lift, then writes the coverage report); `recomp.snippet.lift_snippet` is the interface of the snippet tests ([#39](https://github.com/armaatus/maplethief-port/issues/39)).
- **Coverage:** `python -m recomp coverage` ([#40](https://github.com/armaatus/maplethief-port/issues/40), ADR 0014 addendum) compiles every generated unit with the host compiler (the build's flags plus warnings as errors) and charges each error to its function. It writes `work/recomp/coverage.md` and `coverage.json`: per function and per #10 tag, discovered / lifted cleanly / compiled, and every function that is not clean with its stage, address and reason. `lift` and `all` write the same report without compiling (`--compile` to measure it), and every run ends with its summary line. All 2,655 functions are discovered, lifted cleanly and compiled (target 99%).

The ADR's prototype lifts the whole binary in 2.3 s; 89.5% of the functions lift with no problem before any of the later M3 children, and the 34 MB of C passes clang `-Wall -Wextra -Werror -Wpedantic` and compiles at `-O2` in 17 s on one core.

### 2. High-level runtime / HLE layer (`runtime/`)

The runtime is C++20. It provides the 333 imported symbols, plus the framework classes reached through `objc_msgSend`, on the host:

| Area | Plan |
|---|---|
| Memory & loader | Guest memory ([ADR 0025](adr/0025-guest-memory-and-address-space.md), `maplethief/memory.h`, `runtime/src/mem/`). The layout is fixed on every host: page zero 0x0–0xFFF, the image 0x1000–0xFFFFFF, the heap 0x1000000–0x1EFFFFFF, stacks 0x1F000000–0x1FFFFFFF, and `cpu->mem_size` = 512 MiB. The reservation is 4 GiB + guard, or the arena alone on iOS, with pages committed on demand. The image's `LC_SEGMENT`s are mapped at their vmaddr and then sealed to their `initprot`. The guest heap (`mt_heap_*`) has Darwin semantics, 16-byte alignment and host-side bookkeeping. Guest stacks (`mt_stack_*`) have no-access guards. The HLE pointer helpers are `mt_g2h` / `mt_g2h_str` / `mt_h2g`, and `mt_mem_describe` names any guest address. Then the loader ([#43](https://github.com/armaatus/maplethief-port/issues/43)) applies binds, runs the 2 static constructors and reaches `UIApplicationMain` |
| libc / compiler-rt | `mem*/str*`, printf family (guest varargs), `floor/pow/trunc`, `gettimeofday`, `sysctl(byname)` stubs, integer-division helpers |
| C++ ABI | `new/delete`, `__cxa_atexit`, `__cxa_pure_virtual`, `type_info` vtables as guest data, SjLj unwinder |
| Objective-C | [ADR 0026](adr/0026-objective-c-strategy.md): an ObjC 2 runtime over the guest's metadata (`runtime/src/objc/`, `maplethief/objc/objc.h`). The 75 image classes are realized from `__objc_classlist` at start-up. Framework classes are host classes whose class objects are the imported `_OBJC_CLASS_$_*` data objects. `objc_msgSend`/`_stret`/`Super2` enter recompiled IMPs through `mt_dispatch`, or host methods with the same signature. The 18 shell classes run recompiled and unchanged on host UIKit/Foundation classes. Host state lives in side tables keyed by guest object; guest instance sizes are the SDK's (`NSObject` 4, `UIView` 46, `UIViewController` 152). Host overrides replace single methods. An undeliverable message is logged and follows the unimplemented-import policy (`MT_TRAP_OBJC`) |
| CoreFoundation / Foundation | Subsets: CFString/CFData/CFBundle (resource lookup into the extracted `.app`), CFRunLoop timers, NSString/NSArray/NSDictionary/NSNumber/NSData, NSUserDefaults → host config file, NSFileManager/NSFileHandle, NSKeyedArchiver as needed, NSTimer/NSThread |
| GCD + blocks | `dispatch_*` with a main-queue pump; `_NSConcreteStackBlock/GlobalBlock`, `_Block_object_*`. Only Chartboost uses non-main queues, so running those blocks inline on the main thread is enough ([re/timing-threading.md](re/timing-threading.md#for-the-runtime)) |
| CoreGraphics / UIKit drawing | Bitmap contexts, CGImage, gradients, clip paths, text drawing with a bundled font (font library: later ADR, [#51](https://github.com/armaatus/maplethief-port/issues/51)); PNG decode, including CgBI, with stb_image, and `UIImagePNGRepresentation` with stb_image_write ([ADR 0003](adr/0003-dependencies.md)) |
| OpenGL ES 1.1 | Fixed-function emulation (matrix stacks, fixed-point conversion, client arrays, texturing, blending) onto the backend chosen in **ADR [#55](https://github.com/armaatus/maplethief-port/issues/55)** |
| Audio | OpenAL → our own thin translation layer onto SDL3 audio streams (one stream per source, no OpenAL Soft; [ADR 0003](adr/0003-dependencies.md), [#61](https://github.com/armaatus/maplethief-port/issues/61)); `ExtAudioFile` → `SDL_LoadWAV` (all 26 WAVs are 16-bit PCM); `AVAudioPlayer` → dr_mp3 streaming ([#63](https://github.com/armaatus/maplethief-port/issues/63)) |
| Input | UIKit touch events synthesized from mouse/touch; a **controller layer** that drives keypad actions directly (M7) |
| Stubs | Chartboost: the host override `+[Chartboost sharedChartboost]` → nil, so none of its code runs (ADR 0026). StoreKit: `canMakePayments` NO (offline; ADR in [#52](https://github.com/armaatus/maplethief-port/issues/52)). GameKit: `IsGameCenterAvailable` NO, or emulated with a local achievements file (#52). Sockets fail fast. `UIWebView`, SystemConfiguration and `MPMoviePlayerController` are unused once Chartboost and the dead movie code don't run |

### 3. Platform layer (`platform/`)

- C++20 on **SDL3** (3.4.x, linked statically on every platform; [ADR 0003](adr/0003-dependencies.md)) for windowing, input, gamepads, audio device, file paths (`SDL_GetPrefPath`) and the main loop on every OS.
- **Graphics backend (ADR [#55](https://github.com/armaatus/maplethief-port/issues/55)):** SDL3 GPU (Metal/D3D12/Vulkan) vs. OpenGL 3.3 core / GLES 3 with a Metal path on Apple. GLES1 emulation targets an internal command list so the backend is swappable.
- iOS: SDL3 UIKit integration, safe-area aware layout, MFi/Xbox/PS controllers via GameController.framework (through SDL).

## Later, optional: decompilation track

Once the game is fully playable via recompilation, hot or interesting engine systems (e.g. `CMMap` rendering, `CHero` physics, the script VM) can be **progressively replaced with hand-written C++** that is verified against the recompiled version (same inputs → same outputs). That enables deeper mods such as widescreen and real HD rendering. It isn't required for v1.

## Build from your copy

1. `scripts/bootstrap` (`scripts/bootstrap.ps1` on Windows) finds Python 3.10+ and runs `tools/mtport/bootstrap.py`, which verifies `original/*.ipa` against `original/SHA256SUMS`, extracts it to `work/ipa/` with `zipfile` (checking every entry name first, no `unzip` needed), splits the slices into `work/slices/` with a portable fat Mach-O splitter (`tools/mtport/fat.py`, no `lipo`) and writes `work/bootstrap.json`, which later tools read for paths and hashes.
2. The asset pipeline, `scripts/pipeline [run|check]` (`scripts/pipeline.ps1` on Windows; the work is `tools/mtport/pipeline.py`), converts the bundle into `assets-out/`: CgBI PNGs become standard PNGs, maps, sprite projects, tables, strings and the nib become JSON, scripts become `.dis` listings, the `png/**/*.dat` packs become single images, and standard files are copied. Every bundle file gets one outcome (converted, copied or ignored with a reason) in `assets-out/manifest.json`, with content hashes; runs are incremental (a second run writes nothing) and outputs are invalidated by a hash of the parser code ([ADR 0011](adr/0011-asset-pipeline-manifest-and-incremental-runs.md)). The runtime can also read the original files directly. The viewers render from the same parsers into `assets-out/viewers/` ([ADR 0010](adr/0010-asset-pipeline-and-viewers.md)). Their page scaffolding (escaping, page shell, sortable tables, owned output directory) is `tools/mtport/htmlpage.py`; the data-table browser is `python3 -m mtport.tablebrowser build|check` (`assets-out/viewers/tables/`); the map viewer is `python3 -m mtport.mapview build|check|export|batch` (`assets-out/viewers/maps/`); the sprite/animation viewer is `python3 -m mtport.spriteview build|strip|batch` (`assets-out/viewers/sprites/`).
3. The recompiler, `python -m recomp all` (standard library only, [ADR 0014](adr/0014-recompiler-design.md)), writes the generated C to `work/gen/` and its reports to `work/recomp/`.
4. CMake (≥ 3.28) fetches the pinned dependencies into `build/_downloads/` (hash-checked, cached; [ADR 0003](adr/0003-dependencies.md)) and builds `runtime/` + `platform/` + `work/gen/` into `build/<preset>/`. One command per OS, e.g. `cmake --workflow --preset mac-debug`, configures, builds and tests ([ADR 0004](adr/0004-build-presets-and-generators.md)); the `<os>-release` workflows also package the kit with CPack ([ADR 0023](adr/0023-local-ci-and-packaging.md)), the own-code port kit a GitHub Release carries; `scripts/release` builds the release's kits locally from a tag and guards them against generated or derived content ([ADR 0024](adr/0024-release-contents-and-flow.md)). The top-level `CMakeLists.txt` has the targets `runtime` (static library), `MapleThief` (the executable; a `.app` bundle on macOS), `tools` / `recomp` (byte-compile the Python packages), and `maplethief_gen`, the static library of the generated C. `cmake/GeneratedCode.cmake` adds that target only when `MAPLETHIEF_GEN_DIR` (default `work/gen`) holds a `gen.cmake`, so a checkout without the IPA configures and builds without it ([ADR 0021](adr/0021-generated-code-partition-and-build.md)).

**CI never sees the IPA.** It builds tools, recompiler and runtime and runs unit tests on synthetic fixtures. Real-asset builds and golden-image tests run locally. Every CI job runs locally first ([ADR 0023](adr/0023-local-ci-and-packaging.md)): `scripts/local-ci` on the Mac runs the linters, the Python tests on 3.10, `scripts/test` on macOS and the Linux job in a pinned Ubuntu 26.04 x86_64 container on a clean checkout, and `scripts\test.ps1` runs on Windows. GitHub Actions (`.github/workflows/ci.yml`, [ADR 0006](adr/0006-continuous-integration.md)) is the confirmation: it runs `scripts/test` on macOS, Windows (MSVC) and Linux for every pull request and manual run, adds the Release presets (including the macOS universal binary and the packaged kits) on manual runs, and runs `scripts/lint` and the Python tests on the minimum and latest Python. Checkout skips Git LFS, and a guard step fails any job in which an `*.ipa` is real content instead of an LFS pointer.

## Testing

Settled in [ADR 0005](adr/0005-test-harness.md). `scripts/test` (`scripts/test.ps1` on Windows) runs everything and fails if anything fails:

- **C/C++**: doctest executables, one per area (`runtime_test`, `golden_test`, …), each test case registered with CTest as `<area>.<test case>`, next to the `platform.smoke` test. They run through `cmake --workflow --preset <os>-debug`.
- **Python**: pytest 9.1.1 from the `test` dependency group, run with `uv run --locked` in `build/python-env/<os>-<cpu>`. Tests marked `real_assets` need `work/ipa/` and are reported as skipped without it (always in CI).
- **Golden images**: the C++ library `maplethief_golden` (`tests/golden/golden.h`) checks a rendered RGBA image against a golden PNG with a per-channel tolerance and a maximum ratio of differing pixels. On failure it writes `<name>.actual.png` and `<name>.diff.png` to `build/test-output/<preset>/`. Synthetic goldens are committed in `tests/golden/data/`. Goldens of real game frames live in `work/golden/` and are never committed. `MAPLETHIEF_UPDATE_GOLDEN=1` (re)writes goldens from the current output.
- **Recompiler snippets** ([ADR 0015](adr/0015-recompiler-snippet-suite.md)): every `tests/recomp/snippets/**/*.snippet` file is one pytest test that lifts the snippet, compiles it with the host C compiler (MSVC on Windows) and compares the final CPU state and memory with the file. The format and how to add a snippet are in `tests/recomp/README.md`.
- **Differential test** ([ADR 0017](adr/0017-unicorn-differential-testing.md), local only): `scripts/difftest` (`python -m recomp difftest`, `recomp/difftest/`) selects the binary's leaf functions, runs each on 1,000 seeded random inputs in Unicorn and as recompiled C (production and flags-in/out lifts, one driver executable), and compares the end states and every changed byte. A mismatch is reported with its seed in `work/recomp/difftest.md`. 775 of the 786 leaves pass. The synthetic parts (selection, inputs, the C driver on every compiler) run in CI without Unicorn.

## Repository layout

| Path | Purpose |
|---|---|
| `original/` | The IPA (LFS) + checksums. The only game input. |
| `tools/` | Python tooling: extractor, format parsers, converters, viewers, Ghidra scripts |
| `recomp/` | The static recompiler |
| `runtime/` | HLE layer (C/C++): `include/maplethief/` holds the C interface (`include/maplethief/recomp/` the contract with generated code, ADR 0014), `src/` the implementation |
| `platform/` | SDL3 frontend: shared code in `src/`, per-OS files in `macos/` (e.g. the `Info.plist` template), `windows/`, `ios/`, `linux/` |
| `tests/` | Python tests (`test_*.py`, pytest), C/C++ unit tests (`cpp/`, doctest + CTest), the golden-image helper and its synthetic goldens (`golden/`); see [ADR 0005](adr/0005-test-harness.md) |
| `scripts/` | Bootstrap/verification/build helper scripts; `scripts/pipeline` converts the bundle into `assets-out/`, `scripts/lint` runs the formatters and linters, `scripts/test` all tests |
| `cmake/` | CMake modules; `Dependencies.cmake` pins every C/C++ dependency ([ADR 0003](adr/0003-dependencies.md)) |
| `CMakeLists.txt`, `CMakePresets.json` | Top-level build and the per-OS presets ([ADR 0004](adr/0004-build-presets-and-generators.md)) |
| `pyproject.toml`, `uv.lock` | Python tool configuration and the pinned Python dependencies ([ADR 0003](adr/0003-dependencies.md), [ADR 0005](adr/0005-test-harness.md)) |
| `THIRD_PARTY.md` | Every third-party dependency with version, commit, hash and license |
| `docs/` | Analysis, architecture, roadmap |
| `docs/formats/` | One spec per reverse-engineered file format |
| `docs/adr/` | Architecture decision records |
| `docs/re/` | Reverse-engineering notes (ObjC map, imports, program flow, …) |
| `work/`, `assets-out/`, `build/` | **Generated, git-ignored** |
