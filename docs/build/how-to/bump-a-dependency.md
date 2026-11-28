# Bumping a dependency

Everything the project uses but didn't write is pinned to an exact version and checked by hash ([ADR 0003](../../adr/0003-dependencies.md), [THIRD_PARTY.md](../../../THIRD_PARTY.md)). Two things notice a newer version:

- **Dependabot** ([`.github/dependabot.yml`](../../../.github/dependabot.yml)) opens one grouped pull request into `dev` each Monday for the GitHub Actions (pinned by commit, with the tag in a comment) and one for the Python packages (`pyproject.toml`'s `[dependency-groups]` and `uv.lock`). Review it like any other pull request: CI runs on it, and the section on what to run below says what else.
- **`scripts/deps check`** lists every other pin against upstream, one row each: the C and C++ dependencies of `cmake/Dependencies.cmake`, the container images by digest and the tools in the `ARG` lines of `tools/containers/*/Dockerfile`, uv in CI, Ghidra and its JDK, the linters in `scripts/lint` and the SchemaStore commit their schemas come from. It also lists the Python packages, for the whole picture. Run it by hand, say once a month or before a release; it never runs on a pull request, where upstream would decide whether CI passes.

```sh
scripts/deps check              # every pin; exit 1 when something is newer
scripts/deps check --only sdl   # the rows whose name contains "sdl"
scripts/deps check --offline    # the pins alone, no network
```

A row is `current`, `newer` (with what upstream has), `follows` (it moves with another pin: the Switch's licence texts with the devkitA64 image, Gradle with Ghidra), `fixed` (an apt snapshot date: any later date is newer, so it moves with the image it belongs to) or `unknown` (the lookup failed, with the reason). GitHub's API allows 60 requests an hour without a token; with `gh` logged in, or `GH_TOKEN` set, the check uses that.

Not every `newer` needs a bump. Bump for a fix or a feature we need, a security fix, or to stay close enough that the next bump is small; a bump is a change like any other and can break something.

## 1. One dependency per commit

Change the pin and its row in [THIRD_PARTY.md](../../../THIRD_PARTY.md) in the same commit, one dependency per commit, and say in the message why. `tests/test_deps.py` fails when a SHA-256 of `cmake/Dependencies.cmake` or an image digest of a Dockerfile is not in THIRD_PARTY.md. A new major version, a new licence or a change of mechanism is an ADR 0003 addendum.

## 2. The new hash

**A release archive or a header** (`cmake/Dependencies.cmake`: SDL3, doctest, dr_libs, stb, the fonts). Set the new URL (or commit), download the file and hash it:

```sh
curl -fsSL -o /tmp/SDL3.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
shasum -a 256 /tmp/SDL3.tar.gz
```

Put the hash in the `set(MAPLETHIEF_*_SHA256 ...)` line and in THIRD_PARTY.md's row with the version, tag, commit and date (`gh api repos/libsdl-org/SDL/git/ref/tags/release-3.4.20`). A configure with a wrong hash stops with CMake's `HASH mismatch` and both hashes. The cached copy of the old pin stays in `build/_downloads/`; a new file name means nothing stale is reused.

**sdl3-nx** (the Switch's SDL) is a GitHub-generated archive of a commit of branch `switch-sdl3-deko3d`: the commit is the pin. A new pin also needs `scripts/build-shaders` (SDL's renderer shaders for the Switch are compiled from it; `shaders.stamps` checks the hash) and a Switch build (`scripts/build-switch`).

**A container image** (`tools/containers/*/Dockerfile`, `ARG <NAME>_IMAGE=repo:tag@sha256:...`). The digest is the multi-arch index's:

```sh
docker buildx imagetools inspect ubuntu:26.04 --format '{{json .Manifest.Digest}}'
```

Update every Dockerfile that names the same image (`scripts/deps check` lists them in the row) and THIRD_PARTY.md. With a new Ubuntu image, move `UBUNTU_SNAPSHOT` to a date after the image too. **devkitA64** carries the Switch toolchain: after a new digest, read the versions of libnx, deko3d, newlib, GCC, switch-mesa and libdrm_nouveau from the image (`docker run --rm <image> pacman -Q`), move `MAPLETHIEF_SWITCH_LICENSE_PINS` to the source of each new version (their hashes change with them), and update the Switch section of THIRD_PARTY.md and `UAM_SHA256` in `tools/containers/shaders/Dockerfile`.

**A tool in a Dockerfile** (CMake, PowerShell, DXC, the AppImage runtime): the version ARG and the hash of each architecture's archive, from the release's own checksum file where there is one. CMake is in four Dockerfiles; keep them on one version.

**uv** is pinned twice: `UV_VERSION` in `.github/workflows/ci.yml` and the uv image of `tools/containers/linux/Dockerfile` (tag and digest). Dependabot moves `astral-sh/setup-uv`, not the version it installs.

**A linter** (`scripts/lint`): the `<NAME>_VERSION` line. A newer ruff or clang-format can change what it accepts; run `scripts/lint` and commit any reformatting it asks for as its own commit.

**Ghidra and its JDK** (`tools/mtport/ghidra_setup.py`): URL, SHA-256 and size of each archive, then `scripts/re-analyze` against your IPA and a check that `tools/ghidra/` still names the same functions ([ADR 0007](../../adr/0007-re-tooling-ghidra.md)).

**A Python package** comes from Dependabot. By hand: change the `==` pin in `pyproject.toml`, then `uv lock` and commit both.

## 3. What to run

Always [the quick check](../testing.md#before-you-push): `scripts/lint` and `scripts/test`. Then, by what moved:

| Bumped | Also run |
|---|---|
| SDL3 | `scripts/regress` (the recorded title-to-combat run, on your game files), `platform.gpu_compare` (`ctest --preset mac-debug -R platform.gpu_compare`: the GPU backend against the software rasterizer, needs a GPU and your game files), and `gh workflow run ci.yml --ref <branch>` for every OS |
| sdl3-nx, devkitA64 | `scripts/build-shaders --check`, `scripts/build-switch`, and on a console `scripts/switch-test` (needs-human) |
| dr_mp3, dr_flac | `ctest --preset mac-debug -R audio` with `work/ipa/` present: `audio.real_mp3` decodes every shipped MP3 |
| stb | `ctest --preset mac-debug -R golden` (the test pattern's golden and `platform.device_golden`, every shipped PNG through the texture loader with `work/ipa/`) and `scripts/regress` |
| the fonts | `platform.device_golden` and the `menu languages:` cases (Korean, and the text packs' fallback) |
| doctest | nothing else: the C/C++ suite is its test |
| a Dockerfile | the image's own job: `scripts/test-linux`, `scripts/linux-game build`, `scripts/build-android`, `scripts/build-switch`; CI on all three OSes |
| Capstone | `scripts/test` (the decoder oracle of `recomp/`'s tests) and `python3 -m recomp all` on your game files, with the logged function count unchanged |
| Ghidra, JDK | `scripts/re-analyze` |

The real-asset runs (`scripts/regress`, `platform.gpu_compare`, the decode of the shipped files) are local only: name them in the pull request, as for any change ([Testing and CI](../testing.md)).

## Worked example

Followed for [#340](https://github.com/armaatus/maplethief-port/issues/340) without committing a bump: `scripts/deps check` on 2026-11-28 listed 39 pins, 17 of them newer, among them SDL3 3.4.20 (pinned 3.4.18), dr_mp3 0.7.4 and dr_flac 0.13.4, CMake 4.5.1, a new `ubuntu:26.04` digest and uv 0.13.10, and found the Ubuntu 22.04 image of the portable Linux build missing from THIRD_PARTY.md (now listed). Capstone 5.0.9 was still PyPI's latest release, 6.0.0 being at alpha 11 (see the [ADR 0003 addendum](../../adr/0003-dependencies.md#addendum-2026-11-28-noticing-new-versions-and-capstone-6)).
