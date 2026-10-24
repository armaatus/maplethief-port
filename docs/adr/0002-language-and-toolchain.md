# 2. Language and toolchain for tools, recompiler and runtime

- **Status:** Accepted
- **Date:** 2026-10-24
- **Issue:** #1

## Context

The repo has five kinds of code, and each one has different pressures:

| Part | What it does | Pressure |
|---|---|---|
| `runtime/` | HLE layer: guest memory, loader, 333 imports, an Objective-C runtime and Foundation/UIKit classes, GLES 1.1 emulation, OpenAL, GCD | Lives for the whole project and is linked into every shipped binary. Hard to change later. |
| `platform/` | SDL3 frontends for macOS, Windows, iOS and Linux | Small. Must build for every target, including iOS arm64 and the macOS universal binary. |
| `recomp/` | Mach-O loader, function discovery, Thumb-2/VFP lifter, C emitter | Runs on the build machine, never on the target. Iterated constantly during M3. Must run for a user who only has the IPA, because generated code is never committed. |
| Generated code (`work/gen/`) | One C function per guest function, about 2,655 functions | Must compile with clang and MSVC/clang-cl ([ROADMAP M3](../ROADMAP.md#m3--static-recompiler)). Large, machine-written, never hand-edited. |
| `tools/`, `scripts/` | Bootstrap, fat Mach-O splitter, format parsers, converters, viewers, Ghidra scripts | Lots of small parsers with tests. Should stay easy to change. |

Facts that bear on the choice (from [ANALYSIS.md](../ANALYSIS.md)):

- The armv7 `__text` is 880,956 bytes (0xD713C), which is roughly 300k–450k Thumb-2 instructions in 2,655 functions. This is a small program for a static recompiler.
- The runtime has to emulate Objective-C classes (UIKit, Foundation, CoreFoundation) and C++ ABI pieces (`type_info` vtables, `__cxa_atexit`, SjLj unwinding). That is object-shaped code with tables, strings and reference counting.
- Every import needs a thunk that reads AAPCS arguments out of the guest CPU state and calls a host function. There are 333 imports, so how cheaply thunks can be written matters.
- iOS is a cross-compile target: anything that must *run* during the build (the recompiler, asset tools) runs on the Mac, not on the device.

Local measurement (2026-10-24, Apple silicon, CPython 3.14): a pure-Python pass that decodes 880,956 bytes as Thumb halfwords, classifies 402,896 instruction units with bit masks, builds a small IR tuple per instruction and emits one line of C per instruction took **0.18 s (0.45 µs per instruction)**. A real lifter does much more work per instruction, but a 100× slower lifter still finishes the whole binary in under 20 s.

Platform targets (CLAUDE.md): macOS 13+ universal (arm64 + x86_64), Windows 10/11 x64, iOS 16+ arm64, Linux x86_64 / Steam Deck where it's cheap.

## Options considered

### `runtime/`

1. **C++20**: classes with RAII map naturally onto emulated ObjC/Foundation objects (`NSString`, `NSArray`, `NSDictionary`, CF types). It has standard containers for selector and class tables, `std::string`/`std::u16string` for the UTF-16 `__ustring` literals, and templates that can generate AAPCS import thunks from a host function's C++ signature instead of writing 333 of them by hand. `std::bit_cast`, `<bit>` and `std::span` cover the low-level guest-memory work. Downsides: more language surface, so the subset has to be held to what every minimum compiler supports.
2. **C17**: one language for runtime and generated code, with the simplest possible ABI. But every container, string type and thunk would be written by hand or with macros, and the ObjC class emulation becomes manual vtables. That cost is paid in M4–M6, which is most of the project.
3. **Objective-C / Swift**: would reuse Apple's real runtime on macOS/iOS, but they don't exist on Windows and Linux. Rejected.

### `platform/`

1. **C++20 with SDL3**, the same language as the runtime, so the frontend links the runtime without a language boundary. Objective-C++ is used only for the few things SDL3 doesn't expose on Apple platforms.
2. **Native per-OS frontends** (Swift/AppKit, Win32, …): more code for no gain, since SDL3 already covers window, input, gamepad, audio and paths. Rejected.

### Generated code

1. **C17** (portable subset): it compiles fast, has no name mangling, matches the existing static recompilers (N64Recomp, XenonRecomp) and works with MSVC `cl` (`/std:c17`), clang-cl, Apple clang, clang and GCC.
2. **C++20**: no benefit for straight-line machine-written code, and it compiles noticeably slower for many megabytes of source. Rejected.

### `recomp/`

1. **Python 3**: fastest to iterate and to test (pytest snippet tests that emit C, compile it with the host compiler and run it). It's the same language as `tools/`, so parsers (Mach-O, ObjC metadata) are shared rather than duplicated. It runs on the build machine with no host-tool build step, which matters for iOS cross-builds: a C++ recompiler would need a second, host-targeted CMake build when the main build targets iOS. Speed is enough for this binary (measurement above). Downsides: no static typing by default, and a slow interpreter if the design turns out to need many repeated whole-program passes.
2. **C++20**: fastest, can share the CPU-state header with the runtime, and is used by N64Recomp/XenonRecomp. But it needs a host-tool build for cross-compiles, it duplicates every parser already written in Python for `tools/`, and the edit-compile-test loop is slower.
3. **Rust**: fast and type-safe, but it adds a third toolchain (rustup/cargo) that every user and every CI runner must install, and it has no code shared with either the Python tools or the C++ runtime. Rejected for this project size.

### `tools/` and `scripts/`

1. **Python 3 (stdlib first)**: good binary parsing (`struct`, `plistlib`, `zipfile`, `hashlib`), pytest for tests, available on every desktop OS. The macOS Command Line Tools ship Python 3.9, which is below the minimum chosen here, so macOS users install a newer one (Homebrew, python.org or `uv python install`).
2. **Rust**: single static binaries are nice for distribution, but the same third-toolchain cost as above applies. Rejected.
3. **Shell only**: fine for glue, but not for binary parsers with tests. Shell stays a thin layer.

### Formatting and linting

1. **clang-format** for C/C++/Objective-C(++), **ruff** (lint + black-compatible formatter, replacing black/isort/flake8), **shellcheck** for shell scripts, all pinned and run through `uvx` so no global install is needed.
2. black + isort + flake8: three tools for what ruff does in one. Rejected.
3. clang-tidy now: there is no runtime code yet, and its checks need a compile database. Deferred until the runtime has real code (a later ADR can add it).

## Decision

### Languages

| Part | Language | Notes |
|---|---|---|
| `runtime/` | **C++20** | Firm. The interface that generated code calls is a C header (`extern "C"`). |
| `platform/` | **C++20**, on SDL3 | Objective-C++ (`.mm`) is allowed only under `platform/macos/` and `platform/ios/`, and only for what SDL3 doesn't provide. No Swift. |
| Generated code (`work/gen/`) | **C17**, portable subset | Compiled as C, never as C++. |
| `recomp/` | **Python 3.10+** | Runs on the build machine only. |
| `tools/` | **Python 3.10+** | Reversible: a later ADR can move a single tool elsewhere. |
| `scripts/` | **bash** entry points (+ **PowerShell** for Windows-native entry points) | Shell does orchestration only; any logic beyond a few lines goes into Python. |
| Ghidra scripts | Python 3 via PyGhidra where the Ghidra version chosen in #7 supports it, otherwise Java | Developer-only; end users never run Ghidra. |
| Python tests | **pytest** | Wired up in #6. |
| C/C++ tests | Framework chosen in ADR 0003 (#5), run through **CTest** | Wired up in #6. |
| Build system | **CMake** (generator: **Ninja** preferred) | Presets in #2. |

### Rules for each language

**C++20 (`runtime/`, `platform/`)**

- Core language features of C++20 are fine (concepts, designated initializers, `<=>`, `consteval`/`constinit`, `[[likely]]`).
- Library features are used only if they exist in libc++ (Xcode 15), the MSVC STL (VS 2022 17.8) and libstdc++ 13 alike. In particular, don't depend on `std::format`, `std::jthread`/`std::stop_token`, or `std::chrono` calendars/time zones unless CI on every minimum compiler proves they work. `std::span`, `std::bit_cast`, `<bit>` and `std::string_view` are fine.
- No C++ exception may propagate into generated code. Every function that generated code calls is `noexcept`. Guest C++ exceptions are SjLj and are modelled explicitly ([#37](https://github.com/armaatus/maplethief-port/issues/37)), not with host exceptions.
- Warnings: `-Wall -Wextra` (clang/GCC) and `/W4` (MSVC), treated as errors in CI for our own code. Third-party code is exempt.

**C17 (generated code)**

- Compiled with `-std=c17` / `/std:c17`. Only `<stdint.h>`/`<stddef.h>`/`<string.h>`-level headers, fixed-width integer types, and helpers from the recompiler's support header.
- No VLAs, no GNU statement expressions, no computed `goto`, no `__int128`, no `_Atomic`.
- Compiler-specific features such as `__attribute__((musttail))` for guest tail calls or `__builtin_*` overflow/bit intrinsics go only through macros in the support header (designed in [#30](https://github.com/armaatus/maplethief-port/issues/30)), each with a portable fallback.
- **Clang compiles the generated code in shipping builds on every platform**: Apple clang on macOS/iOS, clang-cl on Windows, clang on Linux. That gives one optimizer and one set of builtins, and guaranteed tail calls through `musttail`. MSVC `cl` must still compile all generated code without errors (ROADMAP M3 exit criterion), so that configuration is built in CI as a portability check.

**Python (`tools/`, `recomp/`, `scripts/` helpers, tests)**

- Minimum **Python 3.10** (for `match` statements, which suit instruction decoding, and `X | Y` type unions). Python 3.10 is the system Python on Ubuntu 22.04. CI tests the minimum and the latest release.
- **Standard library only** for everything on the bootstrap path (`scripts/bootstrap`, IPA verification and extraction, fat Mach-O splitting), so a fresh machine needs nothing beyond Python itself.
- Other code may use third-party packages only when ADR 0003 (#5) or a later ADR lists them, and they are then declared in `pyproject.toml`. Dev tools (pytest, ruff) are run through `uv`/`uvx` at pinned versions and are never installed into the system Python.
- Public functions in `recomp/` have type hints.
- **Recompiler performance budget:** a full recompile of the armv7 slice takes **at most 60 s** on an Apple-silicon dev machine. If profiling and caching can't hold that, or if #30 picks a lifter whose API only exists in C/C++ (e.g. remill/LLVM), a new ADR moves `recomp/` to C++20. The contract between recompiler and runtime (the generated C17 and its support header) does not depend on the recompiler's language, so that move stays contained in `recomp/`.

### Minimum toolchain versions

| Toolchain | Minimum | Why this floor |
|---|---|---|
| CMake | **3.28** | It's the apt version on Ubuntu 24.04 and bundled with VS 2022 17.10+. It supports presets schema v8, `FetchContent` with `SYSTEM`/`EXCLUDE_FROM_ALL`, and `CMAKE_COMPILE_WARNING_AS_ERROR`. `cmake_minimum_required(VERSION 3.28)`. |
| Ninja | **1.11** | Generator for every preset where #2 can use it. |
| Apple clang (macOS, iOS) | **Apple clang 15.0.0, Xcode 15.0** | Covers the C++20 core language and the libc++ pieces allowed above, and deploys to macOS 13 and iOS 16. A full Xcode is needed only for iOS builds and signing; the Command Line Tools are enough for macOS. |
| MSVC (Windows) | **Visual Studio 2022 17.8, MSVC 19.38 (toolset v143)** | Stable C++20 and `/std:c17`. |
| clang-cl (Windows) | **Clang 17** (the VS 2022 "C++ Clang tools" component or upstream LLVM) | The shipping compiler for Windows. |
| GCC (Linux) | **GCC 13** | Ubuntu 24.04 default. Runtime/platform only. |
| Clang (Linux) | **Clang 17** | Ubuntu 24.04 ships 17 and 18. Shipping compiler for Linux. |
| Python | **3.10** | See above. |

### Formatting and linting

- **C, C++, Objective-C(++):** clang-format **23.1.3** (the PyPI `clang-format` wheel, run through `uvx`), configured in `.clang-format` (LLVM style, 4-space indent, 100 columns). Generated code in `work/gen/` is never formatted or linted. It isn't tracked, and the linters only look at git-tracked files.
- **Python:** ruff **0.17.2**, for both `ruff check` and `ruff format`, configured in the root `pyproject.toml` (`target-version = "py310"`, 100 columns, rule sets E, W, F, I, UP, B). Ruff is limited to Python files, so it leaves code blocks in the Markdown docs alone.
- **Shell:** shellcheck **0.11.0** (the PyPI `shellcheck-py` wheel, through `uvx`).
- **Everything else:** `.editorconfig` (UTF-8, LF, final newline, 4 spaces; 2 spaces for Markdown/YAML/JSON).
- `scripts/lint` runs all of the above over git-tracked files and exits non-zero on any finding. `scripts/lint --fix` applies the formatters. The tool versions are pinned in that script; bumping one is its own commit, because formatter upgrades reformat code. CI (#3) runs `scripts/lint` on the Linux runner.

## Consequences

- Contributors and users need **two toolchains**: a C/C++ compiler with CMake, and Python 3.10+. `uv` is needed only to run the linters and dev tools. There's no Rust.
- macOS users on the system Python 3.9 must install a newer Python. `scripts/bootstrap` (#4) should check the version and print that hint.
- The runtime can use classes, containers and templates for the ObjC/Foundation emulation and the import thunks. This decision is firm, and changing it means rewriting M4–M6.
- `tools/` and `recomp/` share one parser codebase (Mach-O, ObjC metadata, plists), so `recomp/` imports from `tools/` instead of re-implementing those parsers.
- iOS builds don't need a host-tool build of the recompiler: `python -m recomp` writes `work/gen/` once, and every target build consumes the same generated C.
- Shipping builds compile generated code with clang everywhere, and MSVC `cl` stays a CI portability check. If MSVC can't handle the generated code's size or the support-header fallbacks prove too slow, that configuration can be dropped by a later ADR without affecting shipping builds.
- How we'd know this was wrong: the recompiler breaks its 60 s budget after profiling (then move `recomp/` to C++20), or a C++20 library feature we need is missing on a minimum compiler (then raise that minimum in a new ADR rather than working around it).
- Follow-ups: CMake presets and compiler flags in #2; dependencies and the C++ test framework in ADR 0003 (#5); CI running `scripts/lint` and the minimum/latest Python in #3; the pytest/CTest harness in #6.
