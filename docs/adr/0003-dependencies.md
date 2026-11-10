# 3. Third-party dependencies and how they are fetched

- **Status:** Accepted
- **Date:** 2026-10-24
- **Issue:** #5
- **Amended:** 2026-10-25: the reason for stb's `_thread` variants corrected after #21 (`CLoaderThread` is not a thread). The decision is unchanged.

## Context

[ADR 0002](0002-language-and-toolchain.md) fixed the languages (C++20 runtime and platform, C17 generated code, Python 3.10+ tools and recompiler) and left two things to this ADR: which third-party libraries we use, and the C++ test framework. Python code may use a third-party package only if this ADR (or a later one) lists it.

What the game needs, from [ANALYSIS.md](../ANALYSIS.md) and local checks against the IPA (2026-10-24):

- **Window, input, gamepads, audio device, file paths** on macOS, Windows, Linux and iOS. [ARCHITECTURE.md](../ARCHITECTURE.md) already assumes SDL3 for all of this.
- **OpenAL: exactly 19 imports.** `alGenBuffers alDeleteBuffers alBufferData alGenSources alDeleteSources alSourcei alSourcePlay alListenerf alGetError` plus 10 `alc*` context/device calls, including `alcGetProcAddress`. The only extension name in the binary is `alBufferDataStatic`. There is **no** `alSourceStop/Pause`, no `alSourcef`, no `alGetSource*`, no `alSource3f/alListener3f` and no streaming queue (`alSourceQueueBuffers`). The game plays whole buffers, optionally looped, with a master gain. It doesn't use 3D positioning, and it never asks a source for its state.
- **SFX: 26 WAV files**, all 16-bit PCM (13 mono at 8 kHz, 11 mono at 22.05 kHz, 2 stereo at 44.1 kHz). They're read through `ExtAudioFile*`.
- **BGM: 18 MP3 files** played through `AVAudioPlayer`. 11 are MPEG-1 Layer III, 44.1 kHz stereo, 128 kbps; 7 (`35`–`41.mp3`) are MPEG-2 Layer III, 22.05 kHz stereo, 80 kbps. They run 30–62 s and have no Xing/LAME info header, so they carry no encoder-delay/padding data.
- **PNG decoding at runtime.** The binary contains no zlib/libpng code (no `inflate`/`IHDR` or zlib error strings) and imports `CGImage*`/`UIImage`, so images are decoded by the OS image stack, which the runtime must replace. There are 1,362 PNGs: 1,322 `.png` and 40 `png/**/*.dat`. **113 of them are Apple CgBI** (BGRA, premultiplied, raw deflate). They're the shell UI images, loaded through `UIImage`.
- **PNG encoding** for `UIImagePNGRepresentation` (imported), and for the diff images that the golden-image helper writes ([#6](https://github.com/armaatus/maplethief-port/issues/6)).
- **A disassembler for tooling**: listings, instruction-mix counts ([#34](https://github.com/armaatus/maplethief-port/issues/34)), and an independent oracle for the recompiler's own Thumb-2/VFP decoder ([#30](https://github.com/armaatus/maplethief-port/issues/30)).
- **Test frameworks**: C/C++ tests through CTest, and Python tests through pytest ([#6](https://github.com/armaatus/maplethief-port/issues/6)).

Constraints:

- **No GPL** in anything we compile, link or ship. LGPL only if it can be linked dynamically on every target. GPL developer tools that run as separate programs are fine (shellcheck from ADR 0002 is GPL-3.0).
- **iOS links everything statically.** The build is sideloaded, not distributed through the App Store, but the licenses must allow it anyway.
- **Reproducible**: exact versions, verified by hash, the same on every machine and in CI.
- **The build writes only under `build/`** ([#2](https://github.com/armaatus/maplethief-port/issues/2) acceptance criterion).
- **CI** on GitHub-hosted macOS/Windows/Linux runners must be able to cache the downloads ([#3](https://github.com/armaatus/maplethief-port/issues/3)), and CI never sees the IPA.
- **The repo holds only our own work** (CLAUDE.md), so third-party source trees are not committed unless there is a reason to.
- Prefer header-only or single-file libraries where quality is equal (issue #5).

## Options considered

All versions below were checked against upstream on 2026-10-24 (GitHub releases and tags, `git ls-remote`, PyPI).

### Platform layer

1. **SDL3** (zlib). Latest release 3.4.18 (2026-10-02). It covers windowing, input, gamepads (through GameController.framework on Apple platforms), audio devices with built-in mixing of bound streams, `SDL_GetPrefPath`, iOS UIKit integration, and the GPU API that [#55](https://github.com/armaatus/maplethief-port/issues/55) evaluates. It builds statically with CMake on every target. Already assumed by ADR 0002 and ARCHITECTURE.md.
2. **SDL2**: in maintenance mode, and its audio API lacks per-stream gain and stream binding. Rejected.
3. **Native frontends per OS**: rejected in ADR 0002.

### OpenAL

1. **OpenAL Soft** 1.25.2 (2026-05-12, LGPL-2.0-or-later: `COPYING` plus the "or any later version" source headers). It is a complete OpenAL 1.1 + EFX + HRTF implementation with its own SDL3 backend, but:
   - The LGPL requires that users can relink the program against a modified library. With static linking, which iOS needs, that means shipping our object files or full source with every build. Dynamic linking on iOS means an embedded framework, signing it, and a second link model for one platform.
   - It is a large C++ library (with its own threads, mixer and backends) for a game that uses 9 `al*` calls with no 3D audio.
   - It would save us only the mixing. Every AL call still needs an HLE thunk that translates guest handles and pointers (including the guest-memory buffer that `alBufferDataStatic` keeps; OpenAL Soft does implement that extension as `AL_EXT_STATIC_BUFFER`), and SDL3 already mixes audio streams.
2. **The platform's OpenAL** (`OpenAL.framework`): deprecated by Apple since macOS 10.15, and absent on Windows and Linux. Rejected.
3. **Our own OpenAL → SDL3 translation layer in `runtime/`.** Each AL buffer holds PCM; for `alBufferDataStatic` it keeps a pointer into guest memory, which suits `SDL_PutAudioStreamDataNoCopy`. Each AL source is one `SDL_AudioStream` bound to the device that `alcOpenDevice` opened, and SDL mixes all bound streams. `AL_LOOPING` re-queues the buffer from a stream callback. `alListenerf(AL_GAIN)` maps to `SDL_SetAudioDeviceGain`. `alcSuspendContext`/`alcProcessContext` pause and resume the device. All of these SDL functions exist in 3.4.18. The layer has no new dependency or license, it is the same on every platform, and it's easy to test with synthetic PCM. It would have to grow if the game turned out to need features it doesn't import, but the import table rules that out.
4. **SDL3_mixer** 3.2.4 (zlib, 2026-06-03): a mixer with its own decoders. It would replace a small amount of our code with another library to build and pin, and OpenAL semantics would still have to be mapped onto it. Rejected.

### MP3 decoder

1. **dr_mp3** 0.7.3 (tag `mp3-0.7.3`, 2026-01-17; public domain (Unlicense) or MIT-0). It's a single header based on minimp3 with a maintained higher-level API: open from a file, memory or callbacks; seek to a PCM frame; get the total frame count; and parse Xing/LAME delay and padding (`delayInPCMFrames`/`paddingInPCMFrames`). It is actively maintained (dr_libs commits in October 2026). **It decodes all 18 shipped MP3s locally** (see Evidence).
2. **minimp3** (CC0): the same decoder core, but it has no tagged releases, and seeking and frame counting live in the separate `minimp3_ex.h`. It's slower-moving upstream. Rejected in favour of its maintained derivative.
3. **SDL3_mixer**: see above.

### WAV decoder

1. **`SDL_LoadWAV`** (part of SDL3): it handles PCM, IEEE float, A-law, µ-law, IMA ADPCM and MS ADPCM WAVs. All 26 shipped WAVs are 16-bit PCM, and **all 26 load with it** locally. No extra library is needed.
2. dr_wav: not needed for these files. Rejected.

### PNG decoder and encoder

1. **stb_image** 2.30 + **stb_image_write** 1.16 (public domain (Unlicense) or MIT; `nothings/stb` has no tags, so we pin a commit). It's a single header with **built-in CgBI support** (`stbi_convert_iphone_png_to_rgb`, `stbi_set_unpremultiply_on_load`, and per-thread `_thread` variants). **It decoded all 1,362 shipped PNGs locally**, and its output for all 113 CgBI files is **bit-identical** to macOS ImageIO (`sips`) in every channel of every pixel. stb_image_write covers `UIImagePNGRepresentation` and the test diff images. It isn't hardened against hostile files, but our inputs are the files of a SHA-256-verified IPA, which all decode cleanly, plus images the game renders itself.
2. **libspng** 0.7.4 (BSD-2-Clause, last release 2023-05-08): more robust against malformed input, and it decodes and encodes, but it needs zlib or miniz as a second dependency and has no CgBI support, so we'd have to inflate the raw deflate and swizzle ourselves. Rejected.
3. **lodepng** (zlib license, no releases): it decodes and encodes in one file, but it has no CgBI support, so headerless deflate would go through its custom-zlib hook, plus our own swizzle and un-premultiply. Rejected.
4. **Python tools** don't need a PNG library: the CgBI un-crusher ([#24](https://github.com/armaatus/maplethief-port/issues/24)) and other converters use the standard library's `zlib`, `struct` and `binascii`, which matches ADR 0002's stdlib-first rule. Pillow is not listed.

### Disassembler for tooling

1. **Capstone** 5.0.9 via its **PyPI wheel** (2026-05-28; BSD-3-Clause, with LLVM-derived tables under the NCSA license, both permissive). The wheels are `py3-none` builds that bundle libcapstone for macOS (arm64, x86_64, universal2), manylinux/musllinux (x86_64, aarch64) and Windows x64. Nothing needs compiling, and they install through `uv` like pytest. Checked locally on CPython 3.10.20 and 3.14.7: it decodes Thumb-2 (`push.w`), VFP (`vpush {d8-d12}`) and VFPv3-D32 registers (`vldr d16, [r0]`). Note that the 5.0.9 wheel's `capstone.__version__` still reports `5.0.7`; use `importlib.metadata.version("capstone")` to check the installed version.
2. **Capstone 6.0** (`6.0.0-Alpha11`, 2026-09-21): its ARM module is regenerated from LLVM ("auto-sync"), which may be more accurate, but it is still an alpha with an API in flux. We revisit it when 6.0.0 final ships; the move is a version bump (see Decision).
3. **Only our own decoder**: the recompiler needs a full decoder anyway, but with no second implementation to compare against, a decoding bug would be invisible until the game misbehaves. Rejected as the only option.
4. **Only Ghidra**: Ghidra stays the analysis workbench ([#7](https://github.com/armaatus/maplethief-port/issues/7) pins its version). But it needs a JDK, starts slowly, and can't be called from a pytest unit test. Rejected as the tooling disassembler.

### C/C++ test framework

1. **doctest** 2.5.3 (MIT, 2026-07-06). It's a single header (no library to build) and compiles fast. `doctest_discover_tests()` registers each test case with CTest, and it supports C++20 on clang, MSVC and GCC. Upstream is active again: 2.5.0 through 2.5.3 shipped between March and July 2026. A configure-build-`ctest` run worked locally on Apple clang 21 (macOS universal) and on GCC 13 with CMake 3.28 (Ubuntu 24.04).
2. **Catch2** 3.16.1 (BSL-1.0, 2026-10-08): the most actively developed option, but v3 is a compiled library (or a large amalgamated `.cpp`), which adds build time to every configuration. Its API is close to doctest's (`TEST_CASE`/`CHECK`/`REQUIRE`, with `SECTION` instead of `SUBCASE`). It is the fallback if doctest maintenance stalls.
3. **GoogleTest** 1.18.0 (BSD-3-Clause, 2026-08-10): it is a compiled library plus gmock, its fixture-class style is heavier than we need, and it has no header-only mode. Rejected.

### Python test runner

1. **pytest** 9.1.1 (MIT, 2026-06-19; `requires-python >= 3.10`, the same floor as ADR 0002). It was already named in ADR 0002. It's run through `uv` at a pinned version and never installed into the system Python.
2. `unittest` (stdlib): workable, but it has no markers or skip reporting, and its fixtures are more verbose. pytest's `real_assets` marker is exactly what [#6](https://github.com/armaatus/maplethief-port/issues/6) needs. Rejected.

### How dependencies are obtained

1. **CMake `FetchContent` from immutable, hash-pinned URLs**, through a shared download cache under `build/`. The URLs are release assets for archives, and `raw.githubusercontent.com/<owner>/<repo>/<commit>/<file>` for single headers, because both are byte-stable. This needs only CMake. It works with every generator and with iOS/universal cross-builds, because each dependency builds with the same toolchain and flags as our code. The cache is one directory that CI can cache, and `FETCHCONTENT_SOURCE_DIR_<NAME>` stays available for local overrides.
2. **GitHub-generated source archives** (`/archive/<sha>.tar.gz`): convenient, but GitHub doesn't guarantee that their bytes (and so their SHA-256) never change. Rejected in favour of release assets and raw files.
3. **`FetchContent` with `GIT_TAG <commit>`**: a pinned commit is reproducible, but it needs git at configure time and clones history (SDL's repository is large), and there is no file-level hash to cache on. Rejected.
4. **Git submodules**: they pin a commit, but every clone needs `--recursive`, and they pull the full upstream repositories, so `git status` noise and repo weight grow. The header-only libraries would also have to be whole-repo submodules. Rejected.
5. **vcpkg / Conan**: binary caching is attractive, but they add a third tool that every user and CI runner must bootstrap. Their versions lag upstream, iOS triplets are community-maintained, and the macOS universal binary needs extra triplet work. That's too much for four C/C++ libraries. Rejected.
6. **System packages (Homebrew, apt)**: not pinned, differ per machine, and don't exist for iOS. Rejected, although a developer can still point `FETCHCONTENT_SOURCE_DIR_SDL3` at a local checkout.
7. **Vendoring sources into the repo**: it works offline, but it contradicts "only our own work" in git, and every bump becomes a large diff. Rejected.

## Decision

### Dependencies

| Dependency | Version | Upstream tag → commit (date) | License | Mechanism | Used for |
|---|---|---|---|---|---|
| **SDL3** | 3.4.18 | `release-3.4.18` → `829a65d769d935c4852f8159e964312c0957260a` (2026-10-02) | Zlib | FetchContent, release archive `SDL3-3.4.18.tar.gz`, SHA-256 `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3`; built static | `platform/`: window, input, gamepads, audio device, paths; the backend for our OpenAL layer; `SDL_LoadWAV` for `ExtAudioFile` |
| **dr_mp3** (dr_libs) | 0.7.3 | `mp3-0.7.3` → `5690d4671d7ad07ae6021756d7222eb159745f06` (2026-01-17) | Unlicense OR MIT-0 | single header `dr_mp3.h` from raw URL at that commit, SHA-256 `d80e6800ddde29d50988ce894affe601858802d1bb8f047f98b84b33ae66c11a` | `runtime/`: `AVAudioPlayer` MP3 BGM ([#63](https://github.com/armaatus/maplethief-port/issues/63)) |
| **stb_image** (stb) | 2.30 | no tags; commit `013ac3beddff3dbffafd5177e7972067cd2b5083` (2024-05-31) | MIT OR Unlicense | single header `stb_image.h` from raw URL at that commit, SHA-256 `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3` | `runtime/`: PNG decoding incl. CgBI for `UIImage`/`CGImage`; test helpers |
| **stb_image_write** (stb) | 1.16 | same commit as stb_image | MIT OR Unlicense | single header `stb_image_write.h` from raw URL at that commit, SHA-256 `cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05` | `runtime/`: `UIImagePNGRepresentation`; golden-image diff PNGs ([#6](https://github.com/armaatus/maplethief-port/issues/6)) |
| **doctest** | 2.5.3 | `v2.5.3` → `2d0a9359a60c51affe2a9bebb1be1dca47868151` (2026-07-06) | MIT | FetchContent, release archive `doctest-v2.5.3.tar.gz`, SHA-256 `eb4a39729d091e43d4d593888bf4d145ee9a0f0e13f7404484b6f2fb60d5d50f` | C/C++ unit tests, registered with CTest (tests only, never shipped) |
| **pytest** | 9.1.1 | `9.1.1` → `cf470ec0bf7eb89cd97dd56df4859eae5db46447` (2026-06-19) | MIT | PyPI, `pytest==9.1.1`, run through `uv` | Python tests for `tools/` and `recomp/` (never shipped) |
| **Capstone** (Python binding + bundled libcapstone) | 5.0.9 | `5.0.9` → `022575848782a4801fd150fdbc927effcbca0864` (2026-05-28) | BSD-3-Clause (LLVM-derived parts: NCSA) | PyPI, `capstone==5.0.9`, run through `uv` | Python tooling: disassembly listings, instruction counts, decoder oracle in recompiler tests (never shipped) |

The OpenAL implementation is **our own translation layer onto SDL3 audio streams** in `runtime/`, not OpenAL Soft. Ghidra is a developer tool whose version [#7](https://github.com/armaatus/maplethief-port/issues/7) pins. The formatters and linters are pinned in `scripts/lint` per ADR 0002. None of these ships in a binary, and `THIRD_PARTY.md` lists them separately as developer tools.

### How they are fetched

- **C/C++ dependencies** are declared in [`cmake/Dependencies.cmake`](../../cmake/Dependencies.cmake). Including that file downloads nothing. `maplethief_use_sdl3()`, `maplethief_use_doctest()`, `maplethief_use_dr_mp3()` and `maplethief_use_stb()` each fetch their files on first use, verify the SHA-256, and provide the targets `SDL3::SDL3`, `doctest::doctest` (plus `doctest_discover_tests()`), `maplethief::dr_mp3` and `maplethief::stb`. The top-level `CMakeLists.txt` ([#2](https://github.com/armaatus/maplethief-port/issues/2)) includes the file and calls only what each target needs.
- **Downloads go to `MAPLETHIEF_DOWNLOAD_DIR`**: by default `<build dir>/_downloads`, and the presets set it to `${sourceDir}/build/_downloads` so that every preset shares one copy. The file is downloaded to a `.part` name with `EXPECTED_HASH` and `TLS_VERIFY`, then renamed. A later configure re-hashes the cached file and doesn't touch the network if it matches. FetchContent then extracts the cached archive (checking the hash again) into its default `<build dir>/_deps`. Nothing is written outside the build tree. This works the same whether FetchContent uses the sub-build (CMake 3.28–3.29, or policy CMP0168 OLD) or populates directly (CMP0168 NEW).
- **SDL3 is always built static** (`SDL_STATIC=ON`, `SDL_SHARED=OFF`, no tests, examples or install rules), on every platform including iOS, so there is one link model everywhere. Third-party targets are `SYSTEM` and `EXCLUDE_FROM_ALL`, which keeps ADR 0002's warnings-as-errors limited to our own code and builds only what we link.
- **Header-only libraries** are compiled in exactly one translation unit per binary, which defines `DR_MP3_IMPLEMENTATION` / `STB_IMAGE_IMPLEMENTATION` / `STB_IMAGE_WRITE_IMPLEMENTATION`. stb's CgBI flags are global unless you use the `_thread` variants, so the runtime uses the `_thread` variants. The game itself decodes on one thread: `CLoaderThread` runs synchronously on the main thread, and all guest code runs on one host thread ([#21](https://github.com/armaatus/maplethief-port/issues/21), [docs/re/timing-threading.md](../re/timing-threading.md)), so the global flags would work as well. The `_thread` variants cost nothing and keep each decode's CgBI settings to its own thread if host code (asset tools, tests, a later prefetch) ever decodes on another one.
- **Python packages** (pytest, Capstone) are declared with `==` pins in `pyproject.toml` under `[dependency-groups]`, and a committed `uv.lock` fixes the transitive versions and hashes. [#6](https://github.com/armaatus/maplethief-port/issues/6) adds both and runs the tests with `uv run --group <group>` with `UV_PROJECT_ENVIRONMENT` pointing into `build/`, so nothing is installed globally. uv supports a `pyproject.toml` with dependency groups and no `[project]` table (checked with uv 0.11.7). The bootstrap path (`scripts/bootstrap`, IPA verification and extraction, slice splitting) stays standard-library only (ADR 0002). [ADR 0007](0007-re-tooling-ghidra.md) adds a third group, `ghidra` (PyGhidra 3.2.0 and its JPype1), used only by `tools/ghidra/` in its own environment.
- **CI** ([#3](https://github.com/armaatus/maplethief-port/issues/3)) caches `build/_downloads` (about 19 MB) with the key `hashFiles('cmake/Dependencies.cmake')`, and caches uv's cache keyed on `uv.lock`.
- **Linux** builds need SDL3's development packages. On Ubuntu 24.04 this set gives X11, Wayland, ALSA, PulseAudio, PipeWire, OpenGL and Vulkan support (checked locally): `build-essential cmake ninja-build pkg-config libasound2-dev libpulse-dev libpipewire-0.3-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev libdbus-1-dev libudev-dev`.

### Rules for changing dependencies

- [`THIRD_PARTY.md`](../../THIRD_PARTY.md) lists every dependency with the same version, commit, SHA-256 and license as `cmake/Dependencies.cmake` (and `pyproject.toml`/`uv.lock` for Python). All of them change in the same commit.
- **Version bumps** of a listed dependency don't need an ADR. Bump one dependency per commit, re-verify the hash from the upstream release, and run the full test suite.
- **Adding or replacing a dependency** needs a new ADR. Examples: a font rasterizer for CoreGraphics text ([#51](https://github.com/armaatus/maplethief-port/issues/51)), a GL loader if [#55](https://github.com/armaatus/maplethief-port/issues/55) picks OpenGL, Capstone in the recompiler's own decode path if [#30](https://github.com/armaatus/maplethief-port/issues/30) wants it, or any LGPL code. That ADR states the license and the mechanism, and updates both files.
- Never GPL in anything compiled, linked or shipped. Never a dependency that can't be built static for iOS.

## Evidence

Local checks on 2026-10-24 (macOS 26 arm64, Apple clang 21, CMake 4.4.3, Ninja 1.13; Docker `ubuntu:24.04` arm64 with CMake 3.28.3 and GCC 13.3). They used a throwaway CMake project, not committed, that includes `cmake/Dependencies.cmake` and calls all four `maplethief_use_*()` functions:

- **macOS universal**: `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0` configured and built in about 7 s. It produced `_deps/sdl3-build/libSDL3.a` and no `.dylib`. `lipo -info` reports `x86_64 arm64`, `minos 13.0`, and the test program prints `SDL 3.4.18 revision SDL-release-3.4.18-0-g829a65d76`. That's the pinned commit.
- **Linux, CMake 3.28** (the ADR 0002 minimum): a fresh configure downloaded the five files and built. A second fresh build directory sharing the download cache configured and built with **`docker run --network none`**, which proves cache reuse without network.
- **CMP0168 NEW** (direct population): a fresh build directory configured from the cache with no download.
- **Hash enforcement**: a cached `dr_mp3.h` with one appended line was detected on the next configure, downloaded again and restored to the pinned SHA-256.
- **Nothing outside the build dir**: `find -newer` over the proof source directory, this worktree and `~/.cmake` after configure and build found no new files. On Linux the source directories were mounted read-only.
- **doctest + CTest**: `doctest_discover_tests()` registered two C++20 test cases, and `ctest` reported `100% tests passed` on macOS and Linux.
- **Real assets** (local only, needs `work/ipa/`): a C program linking `SDL3::SDL3`, `maplethief::stb` and `maplethief::dr_mp3` decoded **1,362/1,362 PNGs** (113 CgBI) and re-encoded each one with stb_image_write. It decoded **18/18 MP3s** to the end and loaded **26/26 WAVs** with `SDL_LoadWAV`, in both the arm64 and x86_64 (Rosetta) slices. All 113 CgBI decodes match `sips` exactly (maximum channel difference 0).
- **Capstone wheel**: imported and disassembled Thumb-2/VFPv3-D32 code on CPython 3.10.20 and 3.14.7 via `uv run`.
- Not checked locally: Windows (MSVC/clang-cl) and iOS (no iOS SDK on this machine). #3 covers Windows in CI, and iOS is covered when the iOS build lands.

## Consequences

- #2 includes `cmake/Dependencies.cmake`, calls `maplethief_use_sdl3()` for the `MapleThief` target, and sets `MAPLETHIEF_DOWNLOAD_DIR` in a hidden base preset. Its "fetches SDL3 reproducibly at a pinned version" check rests on the declarations above.
- #6 calls `maplethief_use_doctest()` and uses `doctest_discover_tests()`. It adds the pytest/Capstone dependency groups and `uv.lock`.
- #3 caches `build/_downloads` and installs the Linux package list above.
- #61 implements OpenAL as a small layer over SDL3 audio streams, with no OpenAL library involved. If a future need (3D sound, EFX) appears, a new ADR can bring in OpenAL Soft as a dynamically linked library on desktop only.
- #63 decodes with dr_mp3. Because the files carry no LAME/Xing delay information, gapless looping has to trim a fixed encoder delay or rely on measured loop points; the decoder can't supply them.
- #24 and the other Python converters stay standard-library only. #51 and #57 decode images with stb_image, using the `_thread` CgBI flags.
- Shipped binaries contain only zlib-licensed code (SDL3) and code we take under its public-domain option (stb under the Unlicense, dr_mp3 under the Unlicense or MIT-0). None of these needs a notice in the binary; doctest, pytest and Capstone never ship. Release packaging still copies `THIRD_PARTY.md` and the license texts into the bundle.
- The first configure needs network access (about 19 MB). After that, builds are offline as long as `build/_downloads` is kept. Deleting `build/` deletes the cache.
- How we'd know this was wrong: an upstream URL disappears or its bytes change (configure fails on the hash; then re-host the file or move to a new release in a bump commit), doctest stops supporting a minimum compiler (switch to Catch2 in a new ADR), or the game turns out to use OpenAL features beyond its import table (revisit OpenAL Soft).

## Addendum: the loop points (#63, 2026-10-29)

#63 measured instead of trimming: Apple's decoder drops its 529-frame decoder delay and keeps every frame's samples, padding included, so the port loops that timeline whole rather than trimming an encoder delay ([ADR 0046](0046-avaudioplayer-mp3-streams-and-loop-points.md)). dr_mp3 is built with `DR_MP3_NO_SIMD` and without contraction there, so its samples are the same on every host.

## Addendum: the Switch-only dependencies (#234, 2026-11-10)

The Nintendo Switch target ([ADR 0138](0138-nintendo-switch-homebrew-target.md)) builds against its own SDL3 and toolchain, listed in `THIRD_PARTY.md` as Switch only: SDL3 from `nx-mod/sdl3-nx`, commit `07ea26a3ca7b60bac414823f0f45e8738c61c217` (SDL 3.4.18), archive SHA-256 `d6511751694fb868c123d0da142ad0417420bb1fe650bca25fb2ad6c8be81801`, and the toolchain image `devkitpro/devkita64` at index digest `sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282`. The NRO also links libnx (ISC), newlib, libstdc++ and libgcc (GPL with the GCC Runtime Library Exception, which allows this), Mesa's `libEGL`, `libglapi` and `libdrm_nouveau` (MIT) and deko3d (Zlib). Mesa's NVK Vulkan driver is not linked, which keeps this ADR's rule against GPL in anything shipped.
