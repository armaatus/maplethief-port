# Architecture (proposal)

> Status: **proposal**. Each major choice below is confirmed, refined or replaced by an ADR in `docs/adr/` (the issues that own each decision are named inline). Once an ADR is accepted, update this document to match it and link the ADR.

Facts referenced here come from [ANALYSIS.md](ANALYSIS.md).

Accepted decisions so far:

- [ADR 0002](adr/0002-language-and-toolchain.md): languages, minimum toolchain versions, formatting and linting.
- [ADR 0003](adr/0003-dependencies.md): third-party libraries, their pinned versions, and how the build fetches them ([THIRD_PARTY.md](../THIRD_PARTY.md)).
- [ADR 0004](adr/0004-build-presets-and-generators.md): CMake presets, generators, compiler flags and the headless smoke test.
- [ADR 0005](adr/0005-test-harness.md): test layout, `scripts/test`, the `real_assets` marker and the golden-image helper.
- [ADR 0006](adr/0006-continuous-integration.md): GitHub Actions CI, what runs on which OS and when, and how CI is kept away from the IPA.
- [ADR 0007](adr/0007-re-tooling-ghidra.md): RE tooling: pinned headless Ghidra 12.2 + JDK 25 driven by PyGhidra scripts, the deterministic export schema and the `work/re/names/*.json` naming contract ([tools/ghidra/README.md](../tools/ghidra/README.md)).
- [ADR 0008](adr/0008-cpp-rtti-and-virtual-function-names.md): C++ classes and vtables are recovered from the Mach-O's RTTI binds by `tools/mtport/rtti.py`, with `Class::vfN` / `~Class` / `@thunk` / key-derived method names ([docs/re/cpp-classes.md](re/cpp-classes.md)).

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

OpenAL is our own translation layer onto SDL3 audio streams, not OpenAL Soft. C/C++ dependencies come from hash-pinned URLs through CMake `FetchContent` ([`cmake/Dependencies.cmake`](../cmake/Dependencies.cmake)), into a download cache under `build/`. Nothing third-party is committed.

## Approach: hybrid static recompilation

```
original/MapleThief-1.5.ipa
        │  scripts/bootstrap (verify + extract + split fat binary)
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
build/<preset>/platform/MapleThief (.app / .exe / iOS app)
```

### 1. Static recompiler (`recomp/`)

Ahead-of-time translation of the **armv7** slice (Thumb-2 + the single ARM entry stub) into C, in the style of N64Recomp / XenonRecomp / ReXGlue. The recompiler is written in Python and runs only on the build machine ([ADR 0002](adr/0002-language-and-toolchain.md)).

- **Input:** the thin armv7 Mach-O. The loader parses segments, sections, `LC_DYLD_INFO_ONLY` binds/rebases/lazy binds, `LC_FUNCTION_STARTS`, `__mod_init_func` and ObjC sections (`__objc_classlist`, `__objc_const`, `__objc_selrefs`, …).
- **Function discovery:** seeded by `LC_FUNCTION_STARTS` (2,655 entries, Thumb bit = LSB). It's cross-checked against ObjC method IMPs, vtable slots, the Ghidra export ([#7](https://github.com/armaatus/maplethief-port/issues/7)) and the parallel armv6 slice.
- **Lifting:** each guest function becomes `void f_0002A3C1(CPUState *cpu)` (or similar). The covered instruction classes are ARMv7 integer, load/store, branches, IT blocks, TBB/TBH, VFPv3-D32 (f32 and f64, d0–d31) and the handful of NEON register moves. Flags are computed lazily where possible.
- **CPU state:** `r0–r15`, `cpsr` (NZCV + Thumb bit), VFP `d0–d31`, `fpscr`. Register values that stay inside a function are kept in C locals so the host compiler can optimize them.
- **Memory model:** the guest has a 32-bit address space. The default proposal is a host-reserved **4 GiB region** with `guest_addr + base` addressing, falling back to a bounds-checked offset scheme on platforms where reservation is impractical (iOS). This is decided in the [#30](https://github.com/armaatus/maplethief-port/issues/30)/M4-01 ADRs.
- **Calls:** direct `bl`/`blx imm` become direct C calls. Indirect calls (`blx reg`, `bx reg`, vtables, ObjC IMPs, function pointers, blocks) go through a **guest-address → host-function table**. A miss traps with a diagnostic, and optionally falls back to an interpreter (see [#35](https://github.com/armaatus/maplethief-port/issues/35)).
- **Imports:** `__symbolstub1` / lazy pointers are resolved at load time to **HLE thunks** that marshal AAPCS arguments from `CPUState` into host functions.
- **Exceptions:** SjLj (`_Unwind_SjLj_Register` builds a linked list of function contexts with a `setjmp`-like resume point). Recompiled code models the landing pads as explicit dispatch ([#37](https://github.com/armaatus/maplethief-port/issues/37)).
- **Output:** generated C17 split across many translation units with a size-balanced partitioner. It's never committed and lives in `work/gen/`.

**Alternative to evaluate in [#30](https://github.com/armaatus/maplethief-port/issues/30):** lifting the **armv6** slice instead (pure ARM, no IT blocks), or using it as a differential oracle.

### 2. High-level runtime / HLE layer (`runtime/`)

The runtime is C++20. It provides the 333 imported symbols, plus the framework classes reached through `objc_msgSend`, on the host:

| Area | Plan |
|---|---|
| Memory & loader | Map segments, apply rebases/binds, set up stack + guest heap (`malloc` family in guest space), run the 2 static constructors, then `UIApplicationMain` |
| libc / compiler-rt | `mem*/str*`, printf family (guest varargs), `floor/pow/trunc`, `gettimeofday`, `sysctl(byname)` stubs, integer-division helpers |
| C++ ABI | `new/delete`, `__cxa_atexit`, `__cxa_pure_virtual`, `type_info` vtables as guest data, SjLj unwinder |
| Objective-C | **ADR [#46](https://github.com/armaatus/maplethief-port/issues/46)**: (a) a minimal ObjC 2 runtime with `objc_msgSend` over guest metadata and host-implemented UIKit/Foundation classes, or (b) a native rewrite of the 18 shell classes calling into recompiled C++ engine code |
| CoreFoundation / Foundation | Subsets: CFString/CFData/CFBundle (resource lookup into the extracted `.app`), CFRunLoop timers, NSString/NSArray/NSDictionary/NSNumber/NSData, NSUserDefaults → host config file, NSFileManager/NSFileHandle, NSKeyedArchiver as needed, NSTimer/NSThread |
| GCD + blocks | `dispatch_*` on host threads with a main-queue pump; `_NSConcreteStackBlock/GlobalBlock`, `_Block_object_*` |
| CoreGraphics / UIKit drawing | Bitmap contexts, CGImage, gradients, clip paths, text drawing with a bundled font (font library: later ADR, [#51](https://github.com/armaatus/maplethief-port/issues/51)); PNG decode, including CgBI, with stb_image, and `UIImagePNGRepresentation` with stb_image_write ([ADR 0003](adr/0003-dependencies.md)) |
| OpenGL ES 1.1 | Fixed-function emulation (matrix stacks, fixed-point conversion, client arrays, texturing, blending) onto the backend chosen in **ADR [#55](https://github.com/armaatus/maplethief-port/issues/55)** |
| Audio | OpenAL → our own thin translation layer onto SDL3 audio streams (one stream per source, no OpenAL Soft; [ADR 0003](adr/0003-dependencies.md), [#61](https://github.com/armaatus/maplethief-port/issues/61)); `ExtAudioFile` → `SDL_LoadWAV` (all 26 WAVs are 16-bit PCM); `AVAudioPlayer` → dr_mp3 streaming ([#63](https://github.com/armaatus/maplethief-port/issues/63)) |
| Input | UIKit touch events synthesized from mouse/touch; a **controller layer** that drives keypad actions directly (M7) |
| Stubs | Chartboost (no-op), StoreKit (offline; ADR in [#52](https://github.com/armaatus/maplethief-port/issues/52)), GameKit (local achievements file), SystemConfiguration (always offline), sockets (fail fast), `UIWebView` (native text or skip), `MPMoviePlayerController` (skip) |

### 3. Platform layer (`platform/`)

- C++20 on **SDL3** (3.4.x, linked statically on every platform; [ADR 0003](adr/0003-dependencies.md)) for windowing, input, gamepads, audio device, file paths (`SDL_GetPrefPath`) and the main loop on every OS.
- **Graphics backend (ADR [#55](https://github.com/armaatus/maplethief-port/issues/55)):** SDL3 GPU (Metal/D3D12/Vulkan) vs. OpenGL 3.3 core / GLES 3 with a Metal path on Apple. GLES1 emulation targets an internal command list so the backend is swappable.
- iOS: SDL3 UIKit integration, safe-area aware layout, MFi/Xbox/PS controllers via GameController.framework (through SDL).

## Later, optional: decompilation track

Once the game is fully playable via recompilation, hot or interesting engine systems (e.g. `CMMap` rendering, `CHero` physics, the script VM) can be **progressively replaced with hand-written C++** that is verified against the recompiled version (same inputs → same outputs). That enables deeper mods such as widescreen and real HD rendering. It isn't required for v1.

## Build from your copy

1. `scripts/bootstrap` (`scripts/bootstrap.ps1` on Windows) finds Python 3.10+ and runs `tools/mtport/bootstrap.py`, which verifies `original/*.ipa` against `original/SHA256SUMS`, extracts it to `work/ipa/` with `zipfile` (checking every entry name first, no `unzip` needed), splits the slices into `work/slices/` with a portable fat Mach-O splitter (`tools/mtport/fat.py`, no `lipo`) and writes `work/bootstrap.json`, which later tools read for paths and hashes.
2. The asset pipeline (`tools/`) converts game data into `assets-out/` where conversion helps (CgBI PNGs, tables → JSON, script disassembly). The runtime can also read the original files directly.
3. The recompiler emits `work/gen/`.
4. CMake (≥ 3.28) fetches the pinned dependencies into `build/_downloads/` (hash-checked, cached; [ADR 0003](adr/0003-dependencies.md)) and builds `runtime/` + `platform/` + `work/gen/` into `build/<preset>/`. One command per OS, e.g. `cmake --workflow --preset mac-debug`, configures, builds and tests ([ADR 0004](adr/0004-build-presets-and-generators.md)). The top-level `CMakeLists.txt` has the targets `runtime` (static library), `MapleThief` (the executable; a `.app` bundle on macOS), and `tools` / `recomp` (byte-compile the Python packages).

**CI never sees the IPA.** It builds tools, recompiler and runtime and runs unit tests on synthetic fixtures. Real-asset builds and golden-image tests run locally. GitHub Actions (`.github/workflows/ci.yml`, [ADR 0006](adr/0006-continuous-integration.md)) runs `scripts/test` on macOS, Windows (MSVC) and Linux for every push and pull request, adds the Release presets (including the macOS universal binary) on `main`, and runs `scripts/lint` and the Python tests on the minimum and latest Python. Checkout skips Git LFS, and a guard step fails any job in which an `*.ipa` is real content instead of an LFS pointer.

## Testing

Settled in [ADR 0005](adr/0005-test-harness.md). `scripts/test` (`scripts/test.ps1` on Windows) runs everything and fails if anything fails:

- **C/C++**: doctest executables, one per area (`runtime_test`, `golden_test`, …), each test case registered with CTest as `<area>.<test case>`, next to the `platform.smoke` test. They run through `cmake --workflow --preset <os>-debug`.
- **Python**: pytest 9.1.1 from the `test` dependency group, run with `uv run --locked` in `build/python-env/<os>-<cpu>`. Tests marked `real_assets` need `work/ipa/` and are reported as skipped without it (always in CI).
- **Golden images**: the C++ library `maplethief_golden` (`tests/golden/golden.h`) checks a rendered RGBA image against a golden PNG with a per-channel tolerance and a maximum ratio of differing pixels. On failure it writes `<name>.actual.png` and `<name>.diff.png` to `build/test-output/<preset>/`. Synthetic goldens are committed in `tests/golden/data/`. Goldens of real game frames live in `work/golden/` and are never committed. `MAPLETHIEF_UPDATE_GOLDEN=1` (re)writes goldens from the current output.

## Repository layout

| Path | Purpose |
|---|---|
| `original/` | The IPA (LFS) + checksums. The only game input. |
| `tools/` | Python tooling: extractor, format parsers, converters, viewers, Ghidra scripts |
| `recomp/` | The static recompiler |
| `runtime/` | HLE layer (C/C++): `include/maplethief/` holds the C interface, `src/` the implementation |
| `platform/` | SDL3 frontend: shared code in `src/`, per-OS files in `macos/` (e.g. the `Info.plist` template), `windows/`, `ios/`, `linux/` |
| `tests/` | Python tests (`test_*.py`, pytest), C/C++ unit tests (`cpp/`, doctest + CTest), the golden-image helper and its synthetic goldens (`golden/`); see [ADR 0005](adr/0005-test-harness.md) |
| `scripts/` | Bootstrap/verification/build helper scripts; `scripts/lint` runs the formatters and linters, `scripts/test` all tests |
| `cmake/` | CMake modules; `Dependencies.cmake` pins every C/C++ dependency ([ADR 0003](adr/0003-dependencies.md)) |
| `CMakeLists.txt`, `CMakePresets.json` | Top-level build and the per-OS presets ([ADR 0004](adr/0004-build-presets-and-generators.md)) |
| `pyproject.toml`, `uv.lock` | Python tool configuration and the pinned Python dependencies ([ADR 0003](adr/0003-dependencies.md), [ADR 0005](adr/0005-test-harness.md)) |
| `THIRD_PARTY.md` | Every third-party dependency with version, commit, hash and license |
| `docs/` | Analysis, architecture, roadmap |
| `docs/formats/` | One spec per reverse-engineered file format |
| `docs/adr/` | Architecture decision records |
| `docs/re/` | Reverse-engineering notes (ObjC map, imports, program flow, …) |
| `work/`, `assets-out/`, `build/` | **Generated, git-ignored** |
