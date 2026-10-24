# 5. Test harness: test layout, golden-image comparison and the Python test environment

- **Status:** Accepted
- **Date:** 2026-10-24
- **Issue:** #6

## Context

[ADR 0003](0003-dependencies.md) picked the test frameworks: doctest through CTest for C/C++, and pytest 9.1.1 through `uv` for Python, pinned in `pyproject.toml` dependency groups with a committed `uv.lock`. [ADR 0004](0004-build-presets-and-generators.md) set up the presets, so `cmake --workflow --preset <os>-debug` configures, builds and runs CTest, and test presets fail when they find no tests. #6 asks for:

- one command that runs the C/C++ and the Python tests and fails if either fails;
- a `real_assets` marker that skips tests when `work/ipa/` is absent, reported as skipped. CI never has the IPA, and the format parsers (#13…#19) each get a "parse every shipped file" test;
- a golden-image helper: compare two RGBA images with a per-channel tolerance and a maximum ratio of differing pixels, and write a diff PNG into `build/test-output/` on failure. Synthetic goldens are committed under `tests/golden/`. Goldens of real game frames are derived from the IPA, so they stay local in `work/golden/`.

Other constraints: nothing is written outside `build/` (ADR 0004), and the bootstrap path stays standard-library only (ADR 0002).

Something learned while doing this: ADR 0003 says uv accepts a `pyproject.toml` with dependency groups and no `[project]` table. That holds for `uv run --group`, but **`uv lock` refuses it** (uv 0.11.7: "No `project` table found"). Adding `[tool.uv.workspace]` makes the file a virtual workspace root that `uv lock` accepts. But there is then nowhere to declare `requires-python`, and uv defaults it to the interpreter it happens to find (`>=3.12` on the machine where this was tried). A lockfile that depends on the machine can't be checked with `--locked`.

## Options considered

### Where the golden-image helper lives

1. **C++ library linked into the tests**: the frames to check come from C++ (the GLES 1.1 emulation, the platform layer), so a test can check a frame where it renders it, with no files in between. PNG input and output use stb_image and stb_image_write, which ADR 0003 already pins for this.
2. **Python helper**: the C++ tests would first have to dump frames to disk, then a second step would compare them. It also needs an image library. Pillow isn't in ADR 0003, and a PNG codec written in the standard library is code to maintain. Rejected.

### What "matches" means

1. **Per-channel absolute difference plus a ratio of differing pixels**: a pixel differs when any of R, G, B, A is off by more than `channel`, and the images match when at most `max_differing_ratio` of the pixels differ. Both numbers are easy to reason about and to pick per test: exact for the software paths, a little slack for GPU backends ([#55](https://github.com/armaatus/maplethief-port/issues/55)) that round differently.
2. **Perceptual metrics (PSNR, SSIM)**: they hide where the difference is, and a threshold on them is hard to explain in a review. They can be added later next to option 1 if a backend needs them.
3. **Exact match only**: too brittle once frames come from different GPU backends.

### Where test output goes

1. **`build/test-output/<preset>/`** (the presets set the CMake cache variable `MAPLETHIEF_TEST_OUTPUT_DIR`): one directory to look in or to upload from CI, as #6 asks, and presets don't overwrite each other.
2. `build/<preset>/test-output/`: scattered across preset directories. Used only as the fallback when CMake is configured without a preset.

### Telling uv the Python floor

1. **A minimal `[project]` table** (`name`, `version = "0.0.0"`, `requires-python = ">=3.10"`, no dependencies) with `[tool.uv] package = false`. uv treats the repo as a virtual project: it never builds or installs it, and `uv.lock` records `requires-python = ">=3.10"` on every machine.
2. **Virtual workspace root** (`[tool.uv.workspace]`, no `[project]`): the lockfile's Python floor depends on the machine that ran `uv lock` (see Context). Rejected.
3. **`uv lock --python 3.10` by convention**: easy to forget, and `uv run --locked` on another interpreter would then report the lock as stale. Rejected.

### Python environment location

1. **`build/python-env/<os>-<cpu>`** through `UV_PROJECT_ENVIRONMENT`: inside `build/`, and a Linux container that shares the checkout with a macOS host doesn't keep recreating the host's environment.
2. `.venv/` in the repo root (uv's default): writes outside `build/`. Rejected.

## Decision

- **Layout.** `tests/` holds both suites. Python tests are `tests/test_*.py` and run with pytest; `tests/conftest.py` holds the shared fixtures. C/C++ tests are `*_test.cpp` files, and the names never collide with pytest's patterns:
  - `tests/CMakeLists.txt` (added from the top-level `CMakeLists.txt`) defines `maplethief_add_cpp_test(<target> AREA <area> SOURCES … [LIBRARIES …])`. It builds one doctest executable per area with a shared `main()` (`tests/cpp/doctest_main.cpp`) and `maplethief_target_defaults()`, and registers each test case with CTest as `<area>.<test case name>` (label `cpp`, 60 s timeout) through `doctest_discover_tests()`.
  - `tests/cpp/` holds unit tests for `runtime/` (area `runtime`).
  - `tests/golden/` holds the golden-image helper library `maplethief_golden`, its tests (area `golden`), and `data/` with the committed synthetic goldens.
- **Golden helper** (`tests/golden/golden.h`): `load_png`/`write_png` (RGBA8, file access through `std::filesystem`, so non-ASCII Windows paths work), `compare(actual, expected, {channel, max_differing_ratio})`, and `check_golden(actual, golden_png, name, tolerance)`, which returns `passed` plus a one-line `message` for `CHECK_MESSAGE`. The comparison counts differing pixels over the union of both sizes, so a size mismatch always fails.
  - On failure it writes `<name>.actual.png` and `<name>.diff.png` under `test_output_dir()`. In the diff, matching pixels are the expected image dimmed to grey (64–127), pixels within the channel tolerance are yellow, differing pixels are red, and pixels outside the other image are magenta. A later passing run deletes both files. A missing golden fails and writes the actual image.
  - `test_output_dir()` is the `MAPLETHIEF_TEST_OUTPUT_DIR` environment variable if set, else the CMake cache variable of the same name. The presets set that to `build/test-output/<preset>`.
  - `MAPLETHIEF_UPDATE_GOLDEN=1` makes `check_golden` write the actual image as the golden when the golden is missing or different (`Update::Never` opts a call out). That is how the committed synthetic golden is made: `test_pattern(64, 64)`, a deterministic gradient, checkerboard and alpha ramp, generated in code and stored as `tests/golden/data/test_pattern_64x64.png` (326 bytes). Real-game goldens use the same helper with paths under `work/golden/` and are never committed.
  - The helper compiles its own static copy of stb_image (PNG only) and stb_image_write, so it can't clash with the runtime's copy when a test links both.
- **Python.** `pyproject.toml` gets the minimal `[project]` table with `[tool.uv] package = false` and no default groups, plus two dependency groups: `tools = ["capstone==5.0.9"]` and `test = ["pytest==9.1.1", {include-group = "tools"}]`. `uv.lock` is committed (`.gitignore` excepts it from the Ghidra `*.lock` rule). pytest is configured in `[tool.pytest]`: `testpaths = ["tests"]`, `pythonpath = ["tools"]` (replacing the old `sys.path` edit in `conftest.py`), `strict = true` and `-ra`. The `real_assets` marker is registered there, and `conftest.py` adds a skip with the reason "real assets: work/ipa/ absent (run scripts/bootstrap)" to every marked test when `work/ipa/` is missing. `tests/test_dependencies.py` checks that the installed pytest and Capstone versions match THIRD_PARTY.md and that Capstone decodes Thumb-2 and VFP.
- **One command.** `scripts/test` (bash) and `scripts/test.ps1` (Windows PowerShell 5.1 / PowerShell 7) first run `cmake --workflow --preset <os>-debug`, with the preset detected from the OS and overridable with `--preset`/`-Preset` or `MAPLETHIEF_PRESET`. Then they run `uv run --locked --group test pytest -p no:cacheprovider` with `UV_PROJECT_ENVIRONMENT=build/python-env/<os>-<cpu>` and `PYTHONPYCACHEPREFIX=build/pycache`. Both suites always run. A summary names each suite's result, and the exit status is 1 if either failed. `UV_PYTHON` selects the Python version (CI runs the minimum, 3.10, and the latest).

## Consequences

- Any test that renders something checks it with one `check_golden` call. A failing CI run can upload `build/test-output/` as an artifact and show the diff PNG.
- Format parsers add `@pytest.mark.real_assets` (or `pytestmark = pytest.mark.real_assets`) to their "parse every shipped file" tests. In CI those tests show up as SKIPPED with the reason, not as passing.
- `cmake --workflow --preset <os>-debug` alone still runs every C/C++ test. The Python tests need `uv`, which ADR 0002 already requires for the linters.
- `uv run --locked` fails when `pyproject.toml` and `uv.lock` disagree, so a pin changed without `uv lock` can't slip through CI.
- doctest discovers test cases by running each test executable right after it is built. That needs a host-runnable binary. When iOS presets arrive (M9), they must skip `tests/` (or build without running discovery).
- This ADR refines ADR 0003's "no `[project]` table" note (see Context). ADR 0003's decision itself (dependency groups, `==` pins, committed `uv.lock`, `uv run --group`) is unchanged.
- How we'd know this was wrong: GPU-backend frames that only match with tolerances so loose that they hide real bugs (add a perceptual metric next to the per-channel one), or doctest discovery getting in the way of cross-compiling (switch registration to `add_test` per executable).
