# Contributing

Thanks for wanting to help! Bug reports, playtesting, code and docs are all welcome.

## Without writing code

- **Report a bug**: the [bug report](https://github.com/armaatus/maplethief-port/issues/new?template=bug.yml) asks for the version, your system, what happened, what you expected, and the log ([how to get it](docs/play/troubleshooting.md#seeing-what-went-wrong)). A screenshot of a glitch helps too, but keep it to the glitch. Don't attach the game's files.
- **Request a feature**: the [feature request](https://github.com/armaatus/maplethief-port/issues/new?template=feature.yml) is for something concrete; a loose idea fits better in Discussions.
- **Playtest**: play through a part of the game and note anything that differs from the iPhone. [docs/testing/](docs/testing/README.md) has step-by-step runs.
- **Ask a question or suggest an idea** in [Discussions](https://github.com/armaatus/maplethief-port/discussions): Q&A for questions, Ideas for suggestions. Start with the [FAQ](https://github.com/armaatus/maplethief-port/discussions/418).
- **Translate the port's menus** into your language, or fix a translation: [Translating the port's menus](docs/build/how-to/translation.md) is a text file to edit and a test that lists what is missing.
- **Improve the docs**: if something in [the player guide](docs/play/README.md) confused you, it will confuse others. A pull request or an issue is welcome.

## Triage

Every new bug report and feature request gets the `needs-triage` label from its template. The maintainer reads it, asks for what is missing (version, system, the log), and replaces `needs-triage` with a type, an area and a priority, or closes it with a reason: a duplicate, a known issue, a question (moved to Discussions), or not about the port. Small, self-contained issues get `good first issue`.

The project has one maintainer, working in spare time, so answers are best effort: there is no promised response time. Questions in Discussions are answered by whoever knows, the maintainer included.

## Setting up

1. Install the tools in [Build from source](docs/build/README.md#tools) (CMake, Ninja, a C++ compiler; Visual Studio on Windows) and [uv](https://docs.astral.sh/uv/), which runs the Python tests and the linters at pinned versions.
2. Clone, and branch from `dev`: `git switch -c 123-short-slug origin/dev`.
3. **First build, without the game:** `cmake --workflow --preset mac-debug` (`linux-debug`, or `win-debug` from the [x64 developer environment](docs/build/README.md#windows-the-x64-developer-environment)) configures, builds and runs the C/C++ tests. This is what CI builds: the runtime, the frontend and the tools, tested on made-up data. Most changes (tools, parsers, runtime functions, docs) need nothing more.
4. **With the game:** put your own `MapleThief-1.5.ipa` in `original/` and run `just build` ([One-command build](docs/build/README.md#one-command-build-just-build)). Only this builds the playable game, and only with it do the real-asset tests run.
5. Run every test: `scripts/test` (macOS, Linux) or `scripts\test.ps1` (Windows). Tests that need the real game files run only when `work/ipa/` exists; CI skips them ([Testing](docs/build/testing.md)).
6. Read [ARCHITECTURE.md](docs/ARCHITECTURE.md) for the big picture, and skim [the decisions](docs/adr/) for the area you'll work on.

Line endings are LF in every text file on every OS ([`.gitattributes`](.gitattributes)), so the shell scripts run from a Windows checkout too; an editor that follows [`.editorconfig`](.editorconfig) keeps them that way.

## Running one test

```sh
scripts/test --python-only -- tests/test_doc_links.py     # one Python test file
scripts/test --python-only -- -k fat                      # Python tests whose name matches
ctest --preset mac-debug -R golden.                       # C/C++ tests by name, after a build
```

On Windows: `scripts\test.ps1 -PythonOnly -PytestArgs 'tests\test_doc_links.py'`, and `ctest --preset win-debug -R golden.`. `ctest --preset mac-debug -N` lists the C/C++ tests. A test marked `real_assets` is reported as skipped until you have run `scripts/bootstrap` with your own IPA.

## Style

`scripts/lint` checks everything CI checks, on tracked files, with pinned tool versions; `scripts/lint --fix` applies the formatters in place. `scripts/lint --no-tidy` skips clang-tidy, the slow part, for a quick pass.

- **C, C++, Objective-C:** [clang-format](.clang-format) (LLVM style, 4-space indent, 100 columns) and [clang-tidy](.clang-tidy) on `runtime/` and `platform/`. The code builds without warnings on clang and MSVC, and with `CI` set warnings are errors.
- **Python:** [ruff](https://docs.astral.sh/ruff/) (lint and format, 100 columns, Python 3.10 and newer; settings in `pyproject.toml`). Text I/O names its encoding (`encoding="utf-8"`), because Windows doesn't default to UTF-8.
- **Shell:** bash scripts pass [ShellCheck](https://www.shellcheck.net/) and start with `set -euo pipefail`.
- **Docs:** plain Markdown, sentence-case headings, relative links that resolve (`tests/test_doc_links.py` checks them, anchors included). Compendium prose follows [its style guide](docs/compendium/style.md), checked by `scripts/lint`.
- **Comments and names** match the code around them: say why, not what.

## Common changes

Step-by-step guides for the changes most contributions are made of, each followed once end to end ([all how-tos](docs/build/how-to/README.md)):

- [Adding an HLE function](docs/build/how-to/hle-function.md): an iOS library function the game calls (`memcpy`, `CFStringGetLength`)
- [Adding a selector on a host class](docs/build/how-to/host-selector.md): a method of a framework class (`-[UIView setFrame:]`)
- [Adding a setting](docs/build/how-to/setting.md): a `settings.ini` key with its flag, presets, menu row and tests
- [Translating the port's menus](docs/build/how-to/translation.md): fix a text or add a language to `platform/lang/`
- [Debugging a trap](docs/build/how-to/trap.md): from a crash report to the cause, with symbols, traces and the unimplemented report

For reverse engineering: [the notes](docs/re/README.md) say what is known, the [Ghidra tools](tools/ghidra/README.md) build a project of the game, and their [naming contract](tools/ghidra/README.md#naming-contract) is how a name you find gets into it and survives a re-import. `scripts/re-analyze` rebuilds every names file, the Ghidra project and the generated notes after a change. `scripts/difftest` checks the recompiler's lifted functions against an emulator (local only). Every `MAPLETHIEF_*` environment variable is in [Running and options](docs/build/running.md#environment-variables).

## Ground rules

What never goes into git, and what never goes into the repository in any form:

1. **Never commit anything from the game.** Not the IPA, not extracted or converted files, not the generated C (`work/gen/`), not built binaries, not screenshots of game art. Everything derived from the IPA lives in the git-ignored `work/`, `assets-out/` and `build/`, and is regenerated by scripts. Check `git status` before every commit. CI fails if an IPA ever shows up.
2. **Don't paste the game's code into the repository**, not even decompiled. Describe behaviour in words, with addresses and names (`CMSoundPlayer::Play` `0x1D3B9`), as the [reverse-engineering notes](docs/re/README.md) do. Test fixtures are made-up data.
3. **Treat the game's files as untrusted data**: parse them, never execute them.
4. **No instructions for decrypting or obtaining the IPA**, in code, docs, issues or pull requests.

These rules keep the repository shareable. The reasoning is in [ADR 0100](docs/adr/0100-public-repository-and-player-releases.md).

## How work is organised

- **Issues** have a goal, tasks and acceptance criteria. Pick one, or open one first for anything bigger than a small fix, so we can agree on the approach.
- **Branches** are named `<issue>-short-slug`, e.g. `142-gapless-music`.
- **Pull requests** fill the short top part of the template: they say `Closes #N` and show, for each acceptance criterion, how it was checked (test name, command, result). "Looks right" isn't evidence; a test or a golden image is.
- **Design decisions** become an ADR in `docs/adr/` (format in [ADR 0001](docs/adr/0001-record-architecture-decisions.md)), with `docs/ARCHITECTURE.md` updated in the same pull request.
- **New file formats** get a spec in `docs/formats/` plus a parser with tests on synthetic fixtures.
- **Guest addresses** are written as armv7 VM addresses in hex. Function pointers carry the Thumb bit (`0x2A3C1`), code ranges don't (`0x2A3C0`).
- **Pull requests target `dev`.** `main` moves only through a release pull request ([Releases](docs/build/releases.md)).
- **Run the quick check before you push:** `scripts/lint` and `scripts/test`. CI is the gate: a pull request into `dev` runs the linters, the Python tests, the Linux build with its sanitizer run, the Switch NRO and the Android APK, and the release pull request runs every OS ([Testing and CI](docs/build/testing.md#github-ci)). Branch rulesets enforce it: a pull request merges only when CI is green, and always with a merge commit ([Branch rules](docs/build/releases.md#branch-rules)).

## How a pull request is reviewed

1. **Open it against `dev`** and fill the top of the [template](.github/pull_request_template.md): what, why, how you tested it, and which real-asset tests ran (none is a fine answer). Leave the collapsed epic section as it is; it is for the maintainer's own epic pull requests. A draft pull request is welcome for early feedback.
2. **CI runs** on the pull request: the linters, the Python tests on the oldest and newest supported Python, the Linux build with its tests and sanitizers, the Switch and Android builds. A docs-only change skips the builds. A first-time contributor's run waits for the maintainer's approval, GitHub's rule for forks.
3. **The maintainer reads it**: that the change does what it says, that the evidence backs each claim, that nothing from the game is in it, and that it matches the code around it. Real-asset checks the change touches are rerun locally, since CI can't. Expect questions or requested changes; push fixes as new commits on the same branch rather than force-pushing, so the review can see what changed.
4. **It merges** when CI is green and the review is approved, with a merge commit (squash and rebase are off). The issue it closes closes with it.

Reviews are best effort, like triage: there is no promised response time. A small, focused pull request with a test is the quickest to review.

The maintainer works with an AI coding agent. Its rules are in [CLAUDE.md](CLAUDE.md) (also `AGENTS.md`), and they double as a detailed description of the workflow.

## Repository layout

| Path | What |
|---|---|
| `recomp/` | The static recompiler: ARM → C |
| `runtime/` | The runtime that stands in for iOS (libc, Objective-C, Foundation, UIKit, OpenGL ES, audio) |
| `platform/` | The game's frontend per system (SDL3): window, input, settings, menus |
| `tools/` | Python tools: bootstrap, file-format parsers, asset converters, viewers, Ghidra scripts, containers |
| `tests/` | Python tests (pytest) and C/C++ tests (doctest), golden images |
| `scripts/` | Build, test, CI and release helpers |
| `docs/` | Player guide, build docs, design and reverse-engineering notes ([index](docs/README.md)) |
| `original/` | Where your own IPA goes (git-ignored) |
| `work/`, `assets-out/`, `build/` | Generated from your IPA, git-ignored |

## Licence

By contributing you agree that your contribution is licensed under the [MIT licence](LICENSE).
