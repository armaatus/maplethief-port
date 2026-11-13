# 157. ASan and UBSan: one build option, two presets, a Linux CI job

- **Status:** Accepted (delegated by Joris, 2026-11-13)
- **Date:** 2026-11-13
- **Issue:** [#321](https://github.com/armaatus/maplethief-port/issues/321) (epic [#287](https://github.com/armaatus/maplethief-port/issues/287))
- **Builds on:** [ADR 0002](0002-language-and-toolchain.md) (toolchain, warnings), [ADR 0006](0006-continuous-integration.md) (CI and `scripts/ci-scope`), [ADR 0020](0020-sjlj-exceptions.md) (SjLj with the host's `setjmp`), [ADR 0025](0025-guest-memory-and-address-space.md) (the two memory models), [ADR 0126](0126-dev-branch-release-pull-requests-and-ci-by-target.md) (CI by target), [ADR 0154](0154-threat-model-for-player-supplied-files.md) (the threat model the sanitizers serve)

## Context

Nothing in the build used `-fsanitize`. The runtime and the frontend parse files a player can supply (ADR 0154), so a memory or undefined-behaviour error in our own C++ is a security bug as well as a crash. CI runs the C/C++ tests without the generated code; the game itself runs only locally, from each user's own IPA.

Three things needed care:

- **The guest memory.** ADR 0025's reserved model maps 4 GiB without checks and turns a bad guest access into a `SIGSEGV` that the runtime handles; the checked model bounds-checks every access against the 512 MiB arena in software and traps with the guest PC. ASan installs its own `SIGSEGV` handler.
- **The SjLj unwinder** (ADR 0020) uses the host's `setjmp` and `longjmp` across our frames.
- **The fuzz targets** of #322 and #323 need libFuzzer, which is clang's.

## Decision

1. **One option, `MAPLETHIEF_SANITIZE`** (top-level `CMakeLists.txt`). When on, every target added after SDL gets `-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer` and links the runtimes. That is the runtime, the frontend, the generated code and every test program. SDL is third-party and keeps its own flags; ASan's allocator still covers its allocations. MSVC is refused: it has no UBSan to pair with ASan.
2. **Two presets, `mac-asan` and `linux-asan`**: Debug, `MAPLETHIEF_MEM_MODEL=checked`, the option on, with configure, build, test and workflow presets. Linux uses clang (`clang`, `clang++`), so that #322 and #323 add libFuzzer targets with the same compiler. The checked model keeps guest faults in software, out of ASan's signal handler, and it is the model iOS and the Switch ship.
3. **No recovery.** `-fno-sanitize-recover=all` at compile time, and the test presets' `asan-base` sets `halt_on_error=1:abort_on_error=1` (plus `detect_stack_use_after_return`, `strict_string_checks`, `check_initialization_order`) and UBSan's `halt_on_error=1:print_stacktrace=1`. Any finding fails its test.
4. **Timeouts scale by four** in a sanitized build (`MAPLETHIEF_TEST_TIMEOUT_SCALE`): `maplethief_doctest_discover()` scales the doctest cases' `TIMEOUT`, and `maplethief_scale_test_timeouts()` every other CTest's.
5. **A CI job, `asan`** (`.github/workflows/ci.yml`), on Linux: it runs whenever the build matrix does (`scripts/ci-scope`'s `native`), on pull requests into `dev` and `main` and on manual runs. It installs SDL's build dependencies and clang's runtimes (`libclang-rt-dev`) and runs `cmake --workflow --preset linux-asan`. Making it a required check belongs to #329's rulesets. The fuzz smoke of #322 and #323 becomes further steps of this job, after the tests, so one clang build environment serves both.
6. **LeakSanitizer is off for now** (`detect_leaks=0`; macOS has none anyway). With it on, about 130 Linux tests failed on leaks alone, all from one cause: `mt_objc_destroy` frees each object's host state block without running the state type's `destroy` (which may send messages, so cannot run at teardown), and the host allocations inside the blocks of objects still alive leak with it. That is a teardown lifetime issue, not a memory-safety error, and in the game a runtime lives for the process. [#414](https://github.com/armaatus/maplethief-port/issues/414) adds a host-only release at teardown and turns LeakSanitizer on.
7. **The Linux portability check** (`platform.linux_portable`, ADR 0065) is not registered in a sanitized build: the sanitizer runtimes need `libresolv` and newer glibc symbols, and a sanitized build is never shipped.
8. **Suppressions only with a reason.** None are needed today. A suppression goes in `tests/sanitizers/<tool>.supp`, with a comment above each entry giving its reason, and the preset's options name the file; `tests/test_sanitizer_suppressions.py` fails on an entry without one. UBSan cannot suppress a check that does not recover, so a UBSan finding is always fixed.

What the first runs found, all fixed:

- **UBSan, invalid-enum-load** in `mt_audio_set_gain` and `mt_saves_verdict_name`: both range-check values a C caller may pass, but in C++ a value past an enum's enumerators (with no fixed underlying type) is undefined. `mt_audio_bus` and `mt_saves_verdict` gain a last enumerator at `0x7FFFFFFF`, which widens their range to every `int`.
- **UBSan, nonnull-attribute** in `mt_stubs_pump`: with nothing due and no queue yet, it passed a null pointer to `memmove` (Linux only: glibc declares the argument non-null). It now moves only when something ran.
- **Two of #324's new tests**: ASan, global-buffer-overflow (a 19-byte path with a NUL read as 21 bytes from its literal), and UBSan, invalid-shift-exponent (the test's zip writer shifted a `uint32_t` by 32 for the zero bytes past the fourth).
- **clang with libstdc++** (the Linux job's combination, new to the project) rejects a `std::vector<std::pair<K, T>>` member inside `T` where `T`'s default constructor is implicit. `PlistValue` defines its constructor after the class; the plist test's writer keeps its entries in a struct of its own.

## Consequences

- A pull request that adds a memory or undefined-behaviour error in a path the C/C++ tests reach fails CI.
- The SjLj unwinder needs nothing: ASan intercepts `longjmp` and clears the shadow of the frames it skips, and the runtime's SjLj tests and the real-asset runs pass with `detect_stack_use_after_return` on.
- ASan sees the guest arena as one mapping, so it does not see a guest overflow from one guest object into the next. That is the game's memory; the checked model still traps an access outside the arena.
- The real game runs under ASan only locally (`ctest --preset mac-asan -R '^play\.regress$'` with your own `work/gen/`). On an Apple M4 on 2026-11-13, `play.regress` took 80.5 s and `play.chapter_1_a` 360.9 s, both clean.
- The `asan` job builds everything a second time; it runs in parallel with the `build` jobs, so the wait for a pull request grows only by its own length.

## Addendum: LeakSanitizer on (#414, 2026-11-13)

Accepted (delegated by Joris, 2026-11-13). This replaces point 6: the `asan-base` test preset no longer sets `detect_leaks=0`, so `linux-asan` and CI's `asan` job run LeakSanitizer (Linux's default; macOS has none, so `mac-asan` is unchanged).

1. **A host state type has a `drop`** (`mt_objc_state_type`, `maplethief/objc/host.h`): it frees the host memory a state owns without calling into the guest, no messages and no guest memory. `destroy` still runs at `dealloc`; `drop` runs instead of it, never after it, in two places: `mt_objc_destroy` (`free_services`) for the states of objects still alive, and a save state's restore for the live states it replaces and for a state whose load failed half-way.
2. **It is the snapshot's old `drop`, moved.** `mt_snap::StateOps` had the same function for restores (ADR 0105) but only for the types with a serializer, and teardown never called it. It now lives on the state type and `StateOps` lost the field, so one function serves both. The types that own host memory have one: CFString, CFURL, NSURL, NSArray, NSSet, NSDictionary, UIView, UIBezierPath, the seven CG boxes (CGContext's is new) and NSFileHandle (new: its open file; unsaved writes go with it, as when a process exits without closing the handle). Every other type holds only values and guest addresses, and has none.
3. **What else LeakSanitizer found, all fixed:**
   - **GL renderbuffer targets**: mt_gles frees its `mt_gfx` targets when the runtime changes, and the last runtime's were never freed. They now go at exit, from an `atexit` handler that mt_gles registers after mt_gfx makes its first target, so it runs before mt_gfx's own registry of targets is destroyed.
   - **`__assert_rtn`'s message**: a `std::string` alive when `guest_abort`, which does not return, was called. Its scope now ends before the call.
   - **A test thread's SDL error text**: `thread_priority_test` raises a priority on a `std::thread`, which SDL does not know, so the thread-local error buffer stayed. The test calls `SDL_CleanupTLS` at the end of the thread.
4. **No suppressions.** `tests/test_sanitizer_suppressions.py` also fails if `detect_leaks=0` comes back into `ASAN_OPTIONS` or `LSAN_OPTIONS`.

Consequence: destroying a runtime (each test's own, or the frontend's when it starts another) frees every live state's host memory, and a new state type that owns host memory needs a `drop` or the `asan` job fails on its first test that tears a runtime down with one alive.
