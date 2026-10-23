# Architecture (proposal)

> Status: **proposal**. Each major choice below is confirmed, refined or replaced by an ADR in `docs/adr/` (the issues that own each decision are named inline). Once an ADR is accepted, update this document to match it and link the ADR.

Facts referenced here come from [ANALYSIS.md](ANALYSIS.md).

## Goals and constraints

- Run MapleStory Thief Edition 1.5 natively on **macOS** (universal arm64 + x86_64), **Windows 10/11 x64** and **iOS 16+ arm64**, with **Linux / Steam Deck** where it's cheap.
- Keep game logic **faithful**: same timing, same RNG, same save semantics. Enhancements are opt-in and layered on top (M10).
- **Controller support** as a first-class input path, not as fake touches where avoidable.
- **Build from your own copy.** The repo contains no derived game content. The build reads `original/*.ipa` and produces everything else locally.

## Approach: hybrid static recompilation

```
original/MapleThief-1.5.ipa
        │  scripts/bootstrap (verify + unzip + lipo -thin)
        ▼
work/ipa/…/MapleThief.app ─────────────► tools/ asset pipeline ──► assets-out/ (PNG, JSON, disasm)
        │ armv7 slice
        ▼
recomp/  (Mach-O loader → function discovery → lifter → C emitter)
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
build/<platform>/MapleThief (.app / .exe / iOS app)
```

### 1. Static recompiler (`recomp/`)

Ahead-of-time translation of the **armv7** slice (Thumb-2 + the single ARM entry stub) into C, in the style of N64Recomp / XenonRecomp / ReXGlue.

- **Input:** the thin armv7 Mach-O. The loader parses segments, sections, `LC_DYLD_INFO_ONLY` binds/rebases/lazy binds, `LC_FUNCTION_STARTS`, `__mod_init_func` and ObjC sections (`__objc_classlist`, `__objc_const`, `__objc_selrefs`, …).
- **Function discovery:** seeded by `LC_FUNCTION_STARTS` (2,655 entries, Thumb bit = LSB). It's cross-checked against ObjC method IMPs, vtable slots, the Ghidra export (M1-01) and the parallel armv6 slice.
- **Lifting:** each guest function becomes `void f_0002A3C1(CPUState *cpu)` (or similar). The covered instruction classes are ARMv7 integer, load/store, branches, IT blocks, TBB/TBH, VFPv3-D32 (f32 and f64, d0–d31) and the handful of NEON register moves. Flags are computed lazily where possible.
- **CPU state:** `r0–r15`, `cpsr` (NZCV + Thumb bit), VFP `d0–d31`, `fpscr`. Register values that stay inside a function are kept in C locals so the host compiler can optimize them.
- **Memory model:** the guest has a 32-bit address space. The default proposal is a host-reserved **4 GiB region** with `guest_addr + base` addressing, falling back to a bounds-checked offset scheme on platforms where reservation is impractical (iOS). This is decided in the M3-01/M4-01 ADRs.
- **Calls:** direct `bl`/`blx imm` become direct C calls. Indirect calls (`blx reg`, `bx reg`, vtables, ObjC IMPs, function pointers, blocks) go through a **guest-address → host-function table**. A miss traps with a diagnostic, and optionally falls back to an interpreter (see M3-06).
- **Imports:** `__symbolstub1` / lazy pointers are resolved at load time to **HLE thunks** that marshal AAPCS arguments from `CPUState` into host functions.
- **Exceptions:** SjLj (`_Unwind_SjLj_Register` builds a linked list of function contexts with a `setjmp`-like resume point). Recompiled code models the landing pads as explicit dispatch (M3-08).
- **Output:** generated C split across many translation units with a size-balanced partitioner. It's never committed and lives in `work/gen/`.

**Alternative to evaluate in M3-01:** lifting the **armv6** slice instead (pure ARM, no IT blocks), or using it as a differential oracle.

### 2. High-level runtime / HLE layer (`runtime/`)

The runtime provides the 333 imported symbols, plus the framework classes reached through `objc_msgSend`, on the host:

| Area | Plan |
|---|---|
| Memory & loader | Map segments, apply rebases/binds, set up stack + guest heap (`malloc` family in guest space), run the 2 static constructors, then `UIApplicationMain` |
| libc / compiler-rt | `mem*/str*`, printf family (guest varargs), `floor/pow/trunc`, `gettimeofday`, `sysctl(byname)` stubs, integer-division helpers |
| C++ ABI | `new/delete`, `__cxa_atexit`, `__cxa_pure_virtual`, `type_info` vtables as guest data, SjLj unwinder |
| Objective-C | **ADR M4-05**: (a) a minimal ObjC 2 runtime with `objc_msgSend` over guest metadata and host-implemented UIKit/Foundation classes, or (b) a native rewrite of the 18 shell classes calling into recompiled C++ engine code |
| CoreFoundation / Foundation | Subsets: CFString/CFData/CFBundle (resource lookup into the extracted `.app`), CFRunLoop timers, NSString/NSArray/NSDictionary/NSNumber/NSData, NSUserDefaults → host config file, NSFileManager/NSFileHandle, NSKeyedArchiver as needed, NSTimer/NSThread |
| GCD + blocks | `dispatch_*` on host threads with a main-queue pump; `_NSConcreteStackBlock/GlobalBlock`, `_Block_object_*` |
| CoreGraphics / UIKit drawing | Bitmap contexts, CGImage, gradients, clip paths, text drawing with a bundled font; PNG decode via a host library |
| OpenGL ES 1.1 | Fixed-function emulation (matrix stacks, fixed-point conversion, client arrays, texturing, blending) onto the backend chosen in **ADR M5-01** |
| Audio | OpenAL → OpenAL Soft or a thin translation to SDL3 audio (ADR M0-05/M6-01); `ExtAudioFile` → WAV decode; `AVAudioPlayer` → dr_mp3/minimp3 streaming |
| Input | UIKit touch events synthesized from mouse/touch; a **controller layer** that drives keypad actions directly (M7) |
| Stubs | Chartboost (no-op), StoreKit (offline; ADR in M4-11), GameKit (local achievements file), SystemConfiguration (always offline), sockets (fail fast), `UIWebView` (native text or skip), `MPMoviePlayerController` (skip) |

### 3. Platform layer (`platform/`)

- **SDL3** for windowing, input, gamepads, audio device, file paths (`SDL_GetPrefPath`) and the main loop on every OS.
- **Graphics backend (ADR M5-01):** SDL3 GPU (Metal/D3D12/Vulkan) vs. OpenGL 3.3 core / GLES 3 with a Metal path on Apple. GLES1 emulation targets an internal command list so the backend is swappable.
- iOS: SDL3 UIKit integration, safe-area aware layout, MFi/Xbox/PS controllers via GameController.framework (through SDL).

## Later, optional: decompilation track

Once the game is fully playable via recompilation, hot or interesting engine systems (e.g. `CMMap` rendering, `CHero` physics, the script VM) can be **progressively replaced with hand-written C++** that is verified against the recompiled version (same inputs → same outputs). That enables deeper mods such as widescreen and real HD rendering. It isn't required for v1.

## Build from your copy

1. `scripts/verify-original.sh` / `scripts/bootstrap` verify `original/*.ipa` against `original/SHA256SUMS`, extract to `work/ipa/` and split slices into `work/slices/`.
2. The asset pipeline (`tools/`) converts game data into `assets-out/` where conversion helps (CgBI PNGs, tables → JSON, script disassembly). The runtime can also read the original files directly.
3. The recompiler emits `work/gen/`.
4. CMake builds `runtime/` + `platform/` + `work/gen/` into `build/`.

**CI never sees the IPA.** It builds tools, recompiler and runtime and runs unit tests on synthetic fixtures. Real-asset builds and golden-image tests run locally.

## Repository layout

| Path | Purpose |
|---|---|
| `original/` | The IPA (LFS) + checksums. The only game input. |
| `tools/` | Python/Rust tooling: extractor, format parsers, converters, viewers, Ghidra scripts |
| `recomp/` | The static recompiler |
| `runtime/` | HLE layer (C/C++) |
| `platform/` | SDL3 frontends per OS (`macos/`, `windows/`, `ios/`, `linux/`) |
| `tests/` | Unit tests, recompiler snippet tests, golden-image harness |
| `scripts/` | Bootstrap/verification/build helper scripts |
| `docs/` | Analysis, architecture, roadmap |
| `docs/formats/` | One spec per reverse-engineered file format |
| `docs/adr/` | Architecture decision records |
| `docs/re/` | Reverse-engineering notes (ObjC map, imports, program flow, …) |
| `work/`, `assets-out/`, `build/` | **Generated, git-ignored** |
