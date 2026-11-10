# Testing and CI

The tests, the checks to run before you push, and what GitHub CI runs. Building comes first: [Build from source](README.md).

## Running the tests

You need [uv](https://docs.astral.sh/uv/) for the Python tests and the linters. One command runs every test, C/C++ and Python, and exits non-zero if anything fails:

```sh
scripts/test        # macOS, Linux
```

On Windows, from the [x64 developer environment](README.md#windows-the-x64-developer-environment):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\test.ps1
```

It runs `cmake --workflow --preset <os>-debug` (configure, build, CTest) and then `uv run --locked --group test pytest`. uv installs the pinned pytest and Capstone from `uv.lock` into `build/python-env/`, so nothing is installed globally. On a Mac (outside CI) pytest runs twice, on the newest installed Python and on 3.10, the minimum; elsewhere once, on uv's choice.

| Option (`scripts/test`) | On Windows | What it does |
|---|---|---|
| `--release` | `-Release` | the release workflow with its package instead, plus the `lipo` check of the universal binary on macOS; implies C/C++ only |
| `--preset NAME` | `-Preset NAME` | another workflow preset |
| `--python LIST` | | the Pythons to run on, e.g. `3.10,latest` |
| `--cpp-only` | `-CppOnly` | skip the Python tests |
| `--python-only` | `-PythonOnly` | skip the C/C++ tests (needs only uv) |
| `-- ARGS` | `-PytestArgs` | passed to pytest, e.g. `scripts/test --python-only -- -k fat` |

`UV_PYTHON=3.10 scripts/test` runs the Python tests on that Python only (uv downloads it if needed).

- **Real game files.** Tests that need them are marked `real_assets` (`@pytest.mark.real_assets`) and are reported as **skipped** until `scripts/bootstrap` has created `work/ipa/`. CI never has it, so they run only on your machine.
- **C/C++ tests** are doctest cases in `tests/cpp/` and `tests/golden/`, added with `maplethief_add_cpp_test()` in `tests/CMakeLists.txt`. Run a subset with `ctest --preset mac-debug -R golden.`.
- **File-format parsers** get synthetic fixtures in CI and a "parse every shipped file" test that runs only with `work/ipa/`.
- **Doc links.** `tests/test_doc_links.py` checks that every relative Markdown link in `README.md`, `CONTRIBUTING.md` and `docs/` resolves, anchors included.

The design is in [ADR 0005](../adr/0005-test-harness.md).

### Golden images

`maplethief::golden::check_golden()` (`tests/golden/golden.h`) compares a rendered image with a golden PNG, with a per-channel tolerance and a maximum ratio of differing pixels. On failure it writes `<name>.actual.png` and `<name>.diff.png` (differing pixels in red) to `build/test-output/<preset>/`.

Only synthetic goldens are committed (`tests/golden/data/`). Goldens of real game frames belong in the ignored `work/golden/` ([tests/golden/README.md](../../tests/golden/README.md)). To create or update goldens from the current output, run the test with `MAPLETHIEF_UPDATE_GOLDEN=1`, for example `MAPLETHIEF_UPDATE_GOLDEN=1 ctest --preset mac-debug -R golden.`, then review the new PNGs before committing them.

### The title for a minute

With the generated code, `platform.title_60s` runs `MapleThief --fixed-step 16.667 --seconds 60` headless from a fresh data folder (`python3 -m mtport.titlerun soak --binary <MapleThief>`). It fails on a crash, a trap, a GL misuse, a non-empty unimplemented report, a frame loop that stalls, a slide-in that doesn't move, or a title that changes or goes black after it. Its log, report and shots go to `build/test-output/<preset>/title_60s/`.

`PYTHONPATH=tools python3 -m mtport.titlerun coverage` configures `build/title-coverage/` with `-DMAPLETHIEF_TRACE_ENTRY=ON`, runs the same minute with `MAPLETHIEF_COVERAGE`, and writes `build/title-coverage/report/title.coverage` and `title-coverage.md`: every guest function entered on the way to the title. The fixed-step run is deterministic, so the same code gives the same report. It is derived from the IPA and never committed.

### Playtests

Step-by-step runs for checking the game by hand are in [docs/testing/](../testing/).

## Before you push

Run the quick local check ([ADR 0126](../adr/0126-dev-branch-release-pull-requests-and-ci-by-target.md)):

```sh
scripts/lint                            # formatters and linters, on tracked files
cmake --workflow --preset mac-debug     # or scripts/test, which adds the Python tests
```

Add the real-asset tests your change touches, since CI can't run them. The Mac build has clang warnings that match MSVC's (`-Wshadow` and the int and float conversions), so a clean Mac build is the best local stand-in for Windows.

## Local CI (optional)

`scripts/local-ci` runs CI's macOS and Linux jobs, the linters and both Pythons on one clean checkout of a commit. It is no longer required before a push, but it's useful when you want the Linux container or a clean tree.

```sh
scripts/local-ci                 # HEAD; or scripts/local-ci <rev>
scripts/local-ci --release       # the release workflows instead: build, test, package
scripts/local-ci --only lint,py-min
```

It checks out the commit as a shared clone in `build/local-ci/<rev>/src`, with no `work/` and no IPA. It sets `CI=true` so warnings are errors, and runs the jobs `lint` (`scripts/lint`), `py-min` (Python tests on 3.10), `mac` (`scripts/test` on the latest Python, or `--release`) `linux` (the container below, in parallel) and `switch` (`scripts/build-switch` with the `switch-release` workflow in the devkitA64 container, also in parallel; it leaves `build/switch-release/MapleThief.nro`, [Nintendo Switch](README.md#nintendo-switch)). When `scripts/ci-scope` finds only documentation or compendium changes it runs `lint` and `py-min` alone, as CI does, and it drops `switch` when the changes only touch another platform's own files; `--full` runs everything, and `--since REF` sets what counts as changed. It prints a summary per job with its duration and test counts, keeps the logs in `build/local-ci/<rev>/logs/`, and exits non-zero if any job failed ([ADR 0023](../adr/0023-local-ci-and-packaging.md)).

### The Linux container

Linux runs in a container, x86_64 like `ubuntu-latest` (Rosetta under [OrbStack](https://orbstack.dev) or Docker Desktop on Apple silicon). The image, [`tools/containers/linux/Dockerfile`](../../tools/containers/linux/Dockerfile), is Ubuntu 26.04 pinned by digest, with its packages from an Ubuntu archive snapshot and CMake, PowerShell and uv at pinned versions.

```sh
scripts/test-linux               # Debug workflow + pytest on a clean export of HEAD
scripts/test-linux --release     # linux-release workflow; the kit lands in build/linux-release/
scripts/test-linux --worktree    # uncommitted changes to tracked files included
scripts/test-linux --arch arm64  # native on Apple silicon: faster, but not what CI runs
scripts/test-linux --real-assets # mount work/ read-only, so the real_assets tests run too
```

The first run builds the image. While `snapshot.ubuntu.com` is unavailable, add `--build-arg UBUNTU_SNAPSHOT=` (also to `scripts/local-ci`) to build it from the live archive.

Windows has no container: run `scripts\test.ps1` (Debug and pytest) and `scripts\test.ps1 -Release` (the `win-release` workflow with its package) natively, from the x64 developer environment.

## GitHub CI

`dev` is the integration branch and the default. Every feature, fix or epic pull request targets it. `main` moves only through a release pull request from `dev` ([Releases](releases.md)). [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) runs on pull requests and manual runs, not on pushes, and the pull request's target decides what runs ([ADR 0126](../adr/0126-dev-branch-release-pull-requests-and-ci-by-target.md)):

| Pull request into | Runs |
|---|---|
| `dev` | lint and Python 3.10 and latest (Linux), the Linux Debug build with the C/C++ tests, and the Switch NRO |
| `main` | lint and Python, and Debug **and** Release builds with the C/C++ tests and packaged kits on macOS, Windows and Linux |
| manual run | the full set, Release unless its `release` input is off |

GitHub's hosted runners are free for this public repository, so use them. To run all three OSes on any branch:

```sh
gh workflow run ci.yml --ref <branch>
gh run watch
```

A pull request is merged only when its checks are green (`gh pr checks --watch`).

- **The `build` jobs** run the same `scripts/test` and `scripts/test.ps1` as above: the Debug build, CTest (including the headless `platform.smoke`) and pytest, with warnings as errors. The Release jobs run the release workflows and package the kits; the universal binary must contain both `arm64` and `x86_64`.
- **The `lint + Python min/latest` job** (Linux) runs `scripts/lint`, then `scripts/test --python-only` on Python 3.10 and the latest Python, and the guard that no compendium output is tracked ([ADR 0121](../adr/0121-public-compendium-site.md)).
- **The `ios` job** (macOS, on the runs that build every OS: the release pull request and manual runs) runs the `ios-ci` workflow: our own code for iOS arm64, Debug and Release, no IPA and no signing. It links the frontend and runtime into the placeholder shell and checks the app is arm64 ([ADR 0136](../adr/0136-ios-app-through-xcode.md)). The game itself and a device are local.
- **The `switch` job** (Linux, on every pull request and manual run) runs `scripts/build-switch HEAD --preset switch-release --arch amd64`: `MapleThief.nro` from our own code in the devkitA64 image pinned by digest, no IPA. Its CTest checks read the NRO's NACP and icon (`platform.switch_nro`) and run the release guard on it (`platform.switch_guard`). A console is never involved: hardware runs are local, with `scripts/switch-run` ([ADR 0138](../adr/0138-nintendo-switch-homebrew-target.md)).
- **Docs-only changes.** A pull request that touches only documentation or compendium work skips the build jobs, and one whose native changes only touch another platform's own files (`platform/macos/`, `scripts/*.ps1`, the Linux containers, …) skips the `switch` job. `scripts/ci-scope` decides, from an allow-list ([ADR 0006](../adr/0006-continuous-integration.md)'s addendum).

CI never sees the IPA. A guard step in every job fails if a `*.ipa` ever shows up in the checkout, and `real_assets` tests show up as skipped. A job uploads an artifact only when it fails, and then only `build/test-output/` and CTest's logs.

### Self-hosted runners

A manual run with input `runners: self-hosted` sends the same jobs to self-hosted runners instead. Pull requests always use the hosted runners, so code from a pull request never runs on these machines. The jobs ask for these labels:

| Job | Labels | Machine |
|---|---|---|
| macOS | `self-hosted`, `macOS`, `ARM64` | a Mac |
| Windows | `self-hosted`, `Windows`, `X64` | a Windows PC with Visual Studio 2022+ and its C++ workload |
| Linux, lint and Python | `self-hosted`, `Linux`, `X64` | a Linux x86_64 runner, for example the [actions/runner image](https://github.com/actions/runner/blob/main/images/Dockerfile) in OrbStack with `--platform linux/amd64` |

To set one up, Settings → Actions → Runners → New self-hosted runner gives the download and a registration token. Run `./config.sh --url https://github.com/armaatus/maplethief-port --token <token> --labels <labels>` and install it as a service (`./svc.sh install && ./svc.sh start` on macOS and Linux, `config.cmd --runasservice` on Windows). Keep the runner's directory away from your checkout, so its jobs never see `work/` or your IPA. Settings → Actions → General must keep "Run workflows from fork pull requests" off. The Linux runner needs `sudo` for the apt step.
