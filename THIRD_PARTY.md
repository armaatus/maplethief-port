# Third-party dependencies

Everything this project uses but didn't write. The decision and the rules live in [ADR 0003](docs/adr/0003-dependencies.md). C/C++ pins are in [`cmake/Dependencies.cmake`](cmake/Dependencies.cmake), and Python pins are in the `[dependency-groups]` of [`pyproject.toml`](pyproject.toml), with the transitive versions and hashes in [`uv.lock`](uv.lock) ([ADR 0005](docs/adr/0005-test-harness.md)). Every version, commit, SHA-256 and license below must match those files. Change them in the same commit.

No third-party source is committed to this repository. The build downloads the files listed here into `build/_downloads/` and checks each one against its SHA-256.

## Libraries

| Dependency | Version | Upstream tag → commit (date) | License | Mechanism | Used for |
|---|---|---|---|---|---|
| **SDL3** | 3.4.18 | `release-3.4.18` → `829a65d769d935c4852f8159e964312c0957260a` (2026-10-02) | Zlib | FetchContent, release archive `SDL3-3.4.18.tar.gz`, SHA-256 `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3`; built static | `platform/`: window, input, gamepads, audio device, paths; the backend for our OpenAL layer; `SDL_LoadWAV` for `ExtAudioFile` |
| **dr_mp3** (dr_libs) | 0.7.3 | `mp3-0.7.3` → `5690d4671d7ad07ae6021756d7222eb159745f06` (2026-01-17) | Unlicense OR MIT-0 | single header `dr_mp3.h` from raw URL at that commit, SHA-256 `d80e6800ddde29d50988ce894affe601858802d1bb8f047f98b84b33ae66c11a` | `runtime/`: `AVAudioPlayer` MP3 BGM ([#63](https://github.com/armaatus/maplethief-port/issues/63)) |
| **stb_image** (stb) | 2.30 | no tags; commit `013ac3beddff3dbffafd5177e7972067cd2b5083` (2024-05-31) | MIT OR Unlicense | single header `stb_image.h` from raw URL at that commit, SHA-256 `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3` | `runtime/`: PNG decoding incl. CgBI for `UIImage`/`CGImage`; the golden-image test helper |
| **stb_image_write** (stb) | 1.16 | same commit as stb_image | MIT OR Unlicense | single header `stb_image_write.h` from raw URL at that commit, SHA-256 `cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05` | `runtime/`: `UIImagePNGRepresentation`; golden-image diff PNGs (`tests/golden/`, [ADR 0005](docs/adr/0005-test-harness.md)) |
| **doctest** | 2.5.3 | `v2.5.3` → `2d0a9359a60c51affe2a9bebb1be1dca47868151` (2026-07-06) | MIT | FetchContent, release archive `doctest-v2.5.3.tar.gz`, SHA-256 `eb4a39729d091e43d4d593888bf4d145ee9a0f0e13f7404484b6f2fb60d5d50f` | C/C++ unit tests, registered with CTest (tests only, never shipped) |
| **pytest** | 9.1.1 | `9.1.1` → `cf470ec0bf7eb89cd97dd56df4859eae5db46447` (2026-06-19) | MIT | PyPI, `pytest==9.1.1` (group `test`), run through `uv` | Python tests for `tools/` and `recomp/` (never shipped) |
| **Capstone** (Python binding + bundled libcapstone) | 5.0.9 | `5.0.9` → `022575848782a4801fd150fdbc927effcbca0864` (2026-05-28) | BSD-3-Clause (LLVM-derived parts: NCSA) | PyPI, `capstone==5.0.9` (group `tools`, included in `test`), run through `uv` | Python tooling: disassembly listings, instruction counts, decoder oracle in recompiler tests (never shipped) |
| **Unicorn** (Python binding + bundled libunicorn) | 2.1.4 | `2.1.4` → `8028ec436f2d9376525352dd38ed9ed6b9f6be10` (2025-09-09) | GPL-2.0 (libunicorn and its QEMU code; the wheel's metadata says BSD, which covers only the Python binding) | PyPI, `unicorn==2.1.4` (group `difftest`, not in `test` or CI), run through `uv` in `build/python-env/difftest-<os>-<cpu>` | The emulator of the recompiler's differential test, `scripts/difftest` ([ADR 0017](docs/adr/0017-unicorn-differential-testing.md); local only, never linked or shipped) |
| **PyGhidra** | 3.2.0 | `Ghidra_12.2_build` → `8ca9077040cc43cd790babcfccf19475622a3e9a` (2026-10-22) | Apache-2.0 | PyPI, `pyghidra==3.2.0` (group `ghidra`), run through `uv` in `build/ghidra/python-env` | Runs the Python 3 Ghidra scripts in `tools/ghidra/` headlessly ([ADR 0007](docs/adr/0007-re-tooling-ghidra.md); never shipped, never in the tests) |
| **JPype1** | 1.7.1 | `v1.7.1` → `6b63aca75b8273f5aeb7e82b696418679b07b60a` (2026-05-06) | Apache-2.0 | PyPI, dependency of PyGhidra, pinned in `uv.lock` | The Python↔Java bridge under PyGhidra (never shipped) |

Download URLs:

- SDL3: <https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz>
- dr_mp3: <https://raw.githubusercontent.com/mackron/dr_libs/5690d4671d7ad07ae6021756d7222eb159745f06/dr_mp3.h>
- stb_image: <https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h>
- stb_image_write: <https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image_write.h>
- doctest: <https://github.com/doctest/doctest/releases/download/v2.5.3/doctest-v2.5.3.tar.gz>
- pytest: <https://pypi.org/project/pytest/9.1.1/>
- Capstone: <https://pypi.org/project/capstone/5.0.9/>
- Unicorn: <https://pypi.org/project/unicorn/2.1.4/> (source and license: <https://github.com/unicorn-engine/unicorn/tree/2.1.4>)
- PyGhidra: <https://pypi.org/project/pyghidra/3.2.0/>
- JPype1: <https://pypi.org/project/jpype1/1.7.1/>

## Developer tools (never linked or shipped)

| Tool | Version | License | Pinned in |
|---|---|---|---|
| ruff | 0.17.2 | MIT | `scripts/lint` ([ADR 0002](docs/adr/0002-language-and-toolchain.md)) |
| clang-format (PyPI wheel) | 23.1.3 | Apache-2.0 WITH LLVM-exception | `scripts/lint` (ADR 0002) |
| shellcheck (`shellcheck-py` wheel) | 0.11.0 (wheel 0.11.0.1) | GPL-3.0-only (run as a separate program; nothing links it) | `scripts/lint` (ADR 0002) |
| Ghidra | 12.2 | Apache-2.0 (bundled parts: see its `licenses/`) | `tools/mtport/ghidra_setup.py` ([ADR 0007](docs/adr/0007-re-tooling-ghidra.md)): `ghidra_12.2_PUBLIC_20261016.zip`, SHA-256 `2c2cd776db3c1f6a097ba418922378ac3649cadd5e844a6f5f088ae628a7be67` |
| Temurin JDK | 25.0.4.1+1 | GPL-2.0 WITH Classpath-exception-2.0 | `tools/mtport/ghidra_setup.py` (ADR 0007): one archive per host (macOS arm64/x64, Linux x64/arm64, Windows x64), each with its SHA-256 |
| Gradle | 9.8.0 | Apache-2.0 | `tools/mtport/ghidra_setup.py` (ADR 0007): `gradle-9.8.0-bin.zip`, SHA-256 `bafd5ce9cfaea0fbccfdc8439a1ac42fbd4cd9c89dc9a988228d8a2639a58e6c`; only builds Ghidra's native tools on hosts the release zip has none for (macOS, Linux arm64) |

## Shipped binaries

Release builds link only SDL3 (Zlib), stb_image and stb_image_write (used under the Unlicense) and dr_mp3 (used under the Unlicense or MIT-0). None of these requires a notice in the binary. Release packaging still copies this file and the upstream license texts into the app bundle.
