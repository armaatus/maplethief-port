# 6. Continuous integration on GitHub Actions

- **Status:** Accepted
- **Date:** 2026-10-24
- **Issue:** #3

## Context

Every push and pull request must build all targets and run the unit tests on macOS, Windows and Linux, without ever needing the IPA (issue #3). Earlier ADRs already fix most of what CI runs:

- [ADR 0002](0002-language-and-toolchain.md): warnings are errors in CI for our own code; CI runs `scripts/lint` on Linux and tests the minimum (3.10) and the latest Python.
- [ADR 0003](0003-dependencies.md): CI caches `build/_downloads` keyed on `hashFiles('cmake/Dependencies.cmake')`, and uv's cache keyed on `uv.lock`.
- [ADR 0004](0004-build-presets-and-generators.md): CI runs `cmake --workflow --preset <os>-debug`, with a Visual Studio developer environment step first on Windows; `MAPLETHIEF_WARNINGS_AS_ERRORS` defaults to on because Actions sets `CI`; the `platform.smoke` test needs no display.
- [ADR 0005](0005-test-harness.md): `scripts/test` / `scripts/test.ps1` run everything; `real_assets` tests are skipped without `work/ipa/`; a failing run may upload `build/test-output/`.

The repo is private, so runner minutes are limited, and macOS minutes cost ten times Linux minutes (Windows: twice). The IPA is in Git LFS and must never reach CI, its caches or its artifacts. Issue #3 asks for at most 10 minutes of wall-clock per run with a warm cache.

What was left open: the triggers, which configurations run when, how CI proves it never saw the IPA, how Python and the tools are provisioned, how actions are pinned, and what may be uploaded.

## Options considered

### Triggers

1. **`push` on every branch + `pull_request` on any base.** Every pushed commit gets a run (child and epic branches included), and a PR also tests its merge result. A PR from a branch of this repo runs twice (push and pull_request). Accepted: PRs are rare (one per epic, CLAUDE.md), and the push run on the epic branch is what tells the orchestrator a wave is green.
2. **`push` only on `main` + `pull_request`.** Half the runs on PR branches, but branches without a PR (the epic branch between waves) get no CI at all. Rejected.

### Configurations

1. **Debug everywhere on every run, Release added on pushes to `main`.** The issue's note ("Debug only on PRs, Release on main"). The Release jobs are extra matrix entries that run in parallel with the Debug ones, so a run on `main` takes no longer. Accepted.
2. **Debug and Release on every run.** Doubles the minutes for little gain while the code is a placeholder. Rejected.

### Proving that CI never saw the IPA

1. **`lfs: false` on checkout plus a guard step** in every job, right after checkout: it fails if any `*.ipa` in the workspace is larger than 1 KiB or doesn't start with the Git LFS pointer header, or if `.git/lfs/objects` contains anything, and otherwise prints each pointer and `git lfs ls-files` into the log. Accepted: cheap (one second), and the log shows the evidence.
2. **`lfs: false` alone.** Correct today, but nothing would notice if a later change (a checkout option, a `git lfs pull` in a script) fetched the IPA. Rejected.

### Python and tools

1. **`actions/setup-python` for every interpreter, uv pinned to 0.11.7 by `astral-sh/setup-uv`, `UV_PYTHON_DOWNLOADS=never`.** The build jobs use the newest `3.x` in the runner's tool cache (on 2026-10-24: 3.14.8 on Linux and macOS, 3.15.1 on Windows) for CMake's `find_package(Python3)` and pytest. The lint job installs 3.10 and the latest release (`3.x` with `check-latest`) and points `UV_PYTHON` at each. The log then names exactly the interpreter that ran. Accepted.
2. **uv-managed Pythons (`UV_PYTHON=3.10`).** Works, but uv 0.11.7's download list predates the current Python release, so "latest" would silently lag. Rejected for CI; it stays the documented local way.

### Pinning actions

1. **Full commit SHAs with the tag in a comment.** Immune to moved tags. Accepted. Bump one action per commit.
2. **Version tags.** Simpler to read, but a tag can be moved. Rejected.

## Decision

`.github/workflows/ci.yml` runs on every `push`, every `pull_request` and `workflow_dispatch`, with `permissions: contents: read` and a concurrency group that cancels superseded runs except on `main`. It has two jobs:

- **`build`**, a matrix over `ubuntu-latest`, `macos-latest` and `windows-latest` with config `debug`, plus config `release` on pushes to `main`. Each job checks out without LFS and without persisted credentials, runs the IPA guard, sets up Python (and uv for Debug), restores `build/_downloads` (key `downloads-<os>-hashFiles('cmake/Dependencies.cmake')`) and installs the platform prerequisites: the README's apt package list on Linux, Ninja on macOS if the image lacks it, and the Visual Studio x64 developer environment on Windows (`ilammy/msvc-dev-cmd`, `arch: x64`). Debug runs `scripts/test` (`scripts/test.ps1` under pwsh on Windows). Release runs `cmake --workflow --preset` with `linux-release`, `win-release` or `mac-release-universal`; on macOS `lipo -verify_arch arm64 x86_64` then checks the universal binary.
- **`lint-python`** on Linux: the IPA guard, `scripts/lint`, then `scripts/test --python-only` on Python 3.10 and on the latest Python release. Its uv cache key also hashes `scripts/lint`, which pins the linters.

A job uploads an artifact only when it fails, and then only `build/test-output/` and CTest's `LastTest*.log`. CI never uploads `build/` as a whole, binaries, or anything derived from the IPA; there is no IPA in CI, but the rule is written into the workflow so it survives later changes.

## Consequences

- The commands in CI are the documented local ones (`scripts/test`, `scripts/test.ps1`, `cmake --workflow --preset …`), so a red CI job reproduces locally with the same command.
- The first build on Windows and with MSVC happened in CI, and found one bug: MSVC's STL can't print a `std::string_view` in a doctest `CHECK` against doctest's own forward declaration of `std::basic_ostream`, so the doctest targets are now built with `DOCTEST_CONFIG_USE_STD_HEADERS` (`tests/CMakeLists.txt`).
- The `-latest` images move. On 2026-10-24 they had CMake 4.4.4 and Ninja 1.13.2 everywhere, Apple clang 21 on macOS 26.6 (arm64), GCC 15.2 on Ubuntu 26.04, and Visual Studio 2026 (18, MSVC 19.51) on Windows Server 2025. The README's apt package list, written for Ubuntu 24.04, also works on 26.04. CI therefore tests the newest compilers, not the minimums from ADR 0002 (VS 2022 17.8, Xcode 15, GCC 13). A minimum-compiler job (e.g. `ubuntu-24.04` with GCC 13) can be added if a minimum-only failure ever matters.
- A PR from a branch of this repo is tested twice (push and merge result). If minutes get tight, the cheapest cut is skipping the `pull_request` run for same-repo branches.
- The Release jobs run only after a merge to `main`. A Release-only failure (e.g. an MSVC warning that only appears with optimizations) shows up there, not in the PR.
- "Latest Python" floats with `actions/setup-python`'s `3.x`, so a new Python release can turn CI red without a commit. That is the point of testing it, and the fix is either a code change or an explicit pin in the workflow.
- Bumping an action, uv or a runner image is a one-line change; the pins live at the top of the workflow and next to each `uses:`.

## Addendum: triggers and self-hosted runners (#113, 2026-10-27)

The Actions minutes ran out on 2026-10-26. Every check now runs locally first ([ADR 0023](0023-local-ci-and-packaging.md): `scripts/local-ci` on the Mac, `scripts\test.ps1` on the Windows PC), and CI becomes the confirmation. That changes two decisions above:

- **Triggers: `pull_request` + `workflow_dispatch`, no `push`.** The option rejected above ("push only on main + pull_request") assumed that the epic branch between waves needs CI to tell the orchestrator a wave is green; `scripts/local-ci` does that now. Without `push`, `main` gets no run after a merge either: its pull request ran just before.
- **Release jobs on manual runs.** "Release added on pushes to `main`" can't happen without `push`. A manual run adds the Release jobs unless its `release` input is off (default on), and they now also package the kit (ADR 0023). Pull requests run the Debug jobs only, so a Release-only failure shows up locally (`scripts/local-ci --release`) or in a manual run. The matrix and the jobs are otherwise unchanged.
- **Self-hosted runners, optional.** A manual run's `runners` input (`hosted` by default, or `self-hosted`) sends the same jobs to self-hosted runners with the labels `self-hosted` + `macOS` + `ARM64` (the Mac), `self-hosted` + `Windows` + `X64` (the PC) and `self-hosted` + `Linux` + `X64` (a Linux runner, e.g. the actions/runner image in OrbStack). They use no Actions minutes ([billing](https://docs.github.com/en/billing/concepts/product-billing/github-actions)). Pull requests always run on hosted runners, so code from a pull request never runs on our machines; fork pull request workflows stay off, and runner directories stay away from `work/` ([secure use](https://docs.github.com/en/actions/reference/security/secure-use)). Setting them up needs registration tokens from Joris (README, "Self-hosted runners"). GitHub announced a per-minute charge for self-hosted runners in private repositories and then postponed it ([changelog](https://github.blog/changelog/2025-12-16-coming-soon-simpler-pricing-and-a-better-experience-for-github-actions/)); recheck before relying on them.

## Addendum: documentation and compendium pull requests skip the build matrix (epic #210, 2026-11-06)

Joris asked that CI spend no minutes on builds a pull request cannot affect. A `scope` job now runs first on every pull request: it lists the changed files through the pull request files API and passes them to `scripts/ci-scope`. When every path is documentation or compendium work (`docs/`, any `*.md`, `tools/mtport/compendium/`, `tools/mtport/prose_lint.py`, their tests and fixtures, `scripts/compendium`, the compendium workflow), the macOS, Windows and Linux build jobs are skipped and only the lint + Python min/latest job runs. Any other path, an empty list or a list cut at the API's 3,000-file limit runs the full matrix, and manual runs always run everything.

- **An allow-list, not a deny-list.** A path matches a pattern or counts as native, so a new kind of file never skips the build by accident. `tests/test_ci_scope.py` checks both sides.
- **Cost:** the `scope` job is a sparse checkout of one file and an API call (one billed minute); a skipped build matrix saves three jobs.
- **What it gives up:** compendium Python runs on Linux only in CI for such pull requests, not on macOS and Windows. `scripts/local-ci` still runs every job locally.
- **Branch protection:** `main` has no required checks, so skipped jobs block nothing. If required checks are added, skipped jobs count as passed.
