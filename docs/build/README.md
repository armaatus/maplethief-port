# Build from source

You only need this page to change the port, or to play on a system without a download. Players don't need to build anything: see [Playing MapleThief](../play/README.md). This page is the deep reference. For a walk from a fresh clone to a running game on one system, follow the [build guide](../BUILDING.md).

The repository holds only our own work. **You bring your own copy of the game**, `MapleThief-1.5.ipa`, and everything else is made from it locally by scripts: extracted files, converted assets, recompiled code and binaries. All of it lands in the git-ignored `work/`, `assets-out/`, `build/` and `dist/`, and none of it is ever committed ([ADR 0100](../adr/0100-public-repository-and-player-releases.md)). What you build holds the game: keep it to your own machines.

Below: the tools for each system, then the [one-command build](#one-command-build-just-build), then every step by hand. The other pages are [Running and options](running.md), [Testing and CI](testing.md) and [Releases](releases.md). `just doctor` checks what you have and says the fix for what's missing.

## Tools

Install these first. Python is only used by our scripts and needs nothing beyond its standard library.

| | macOS | Windows | Linux |
|---|---|---|---|
| Python 3.10+ | Homebrew, python.org, or `uv python install 3.12` | python.org (`py -3`) or the Microsoft Store | your distribution's `python3` |
| [just](https://just.systems) | `brew install just` | `winget install Casey.Just` or `scoop install just` | `apt install just` |
| Git | Xcode's Command Line Tools | [Git for Windows](https://git-scm.com/download/win) | your distribution's `git` |
| Compiler | Apple clang 15+ (Xcode 15 or its Command Line Tools) | Visual Studio 2022 17.8+ with the "Desktop development with C++" workload | GCC 13+ or Clang 17+ (only for a build by hand) |
| CMake 3.28+, Ninja 1.11+ | `brew install cmake ninja` | they come with Visual Studio | packages below (only for a build by hand) |
| Docker | OrbStack or Docker Desktop, for `just build linux` and `just build switch` | Docker Desktop, for `just build linux` | for `just build` and `just build switch` |
| [uv](https://docs.astral.sh/uv/) | for the tests and linters | for the tests and linters | for the tests and linters |

macOS's own `python3` from the Command Line Tools is 3.9, which is too old. Set `PYTHON=/path/to/python3` if the right one isn't first on your `PATH`; every script honours it.

On Linux, `just build` works in a container, so Docker is all it needs. A build by hand, or `scripts/linux-game build --native`, also needs SDL3's development packages. On Ubuntu 24.04:

```sh
sudo apt install build-essential cmake ninja-build pkg-config python3 libasound2-dev libpulse-dev \
  libpipewire-0.3-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
  libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
  libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev libdbus-1-dev libudev-dev
```

The versions are set in [ADR 0002](../adr/0002-language-and-toolchain.md).

## Get the code and your game file

```sh
git clone https://github.com/armaatus/maplethief-port.git
cd maplethief-port
```

Put your copy in `original/`, named exactly `MapleThief-1.5.ipa`, or leave it where it is and [point the build at it](#build-from-your-own-copy-wherever-it-is). [original/README.md](../../original/README.md) says which file it has to be and how to check it, and [Your game file](../play/your-ipa.md) says the same for players. `*.ipa` is git-ignored, and CI fails if one ever shows up.

## One-command build: `just build`

```sh
just build mac       # macOS: dist/mac/MapleThief.app
just build win       # Windows: dist\win\MapleThief-game-…-windows-x64\ and its .zip
just build linux     # Linux / Steam Deck, in Docker, from any OS: dist/linux/…
just build switch    # Nintendo Switch homebrew, in Docker, from macOS or Linux: dist/switch/MapleThief.nro
just run             # start this OS's build
```

Without `just`, the same words work on `scripts/build` (macOS, Linux) and `scripts\build.ps1` (Windows): `scripts/build mac`. `just build` alone builds for this OS. `macos`, `windows` and a leading dash (`-linux`) also work. Windows sets up the Visual Studio x64 environment itself, from any terminal. The Windows exe and the macOS app only build on their own OS; `linux` builds from any OS with Docker, on Windows through Git for Windows' bash. `just build ios` needs a Mac with Xcode: it builds `dist/ios/MapleThief.app` ([ADR 0136](../adr/0136-ios-app-through-xcode.md)) and stops with what is missing without it. [Playing on your iPhone](../play/iphone.md) has the steps to sign and install it. `just build switch` builds `MapleThief.nro` in Docker from macOS or Linux, without the IPA for now ([Nintendo Switch](#nintendo-switch)).

| Recipe | What it does |
|---|---|
| `just build [mac\|win\|linux\|ios\|switch] [debug\|release] [--ipa PATH]` | runs every step below, leaves the game in `dist/`. Default: this OS, Release. `--ipa` takes [your copy from anywhere](#build-from-your-own-copy-wherever-it-is) |
| `just run [debug] [-- ARGS]` | starts the `dist/` build for this OS, and says what to run if there is none |
| `just bootstrap` | checks the IPA, extracts it into `work/` |
| `just assets` | converts the assets (`scripts/pipeline`; the game doesn't need this step) |
| `just recomp` | writes the generated C into `work/gen/` |
| `just shaders` | recompiles the GPU shaders, in Docker, when a source changed |
| `just test [ARGS]` | every unit test: `scripts/test` (Windows: `scripts\test.ps1`) |
| `just ci [ARGS]` | what CI runs: `scripts/local-ci` (Mac or Linux) |
| `just package [mac\|linux]` | the own-code kit, from a clean checkout, into `build/local-ci/<rev>/` (it holds none of the game) |
| `just clean [work]` | removes `dist/` and the builds. With the word `work`, also `work/` and `assets-out/`. Your saves stay, and `original/` is never touched |
| `just doctor` | checks Python, CMake, Ninja, the compiler, Docker, the IPA and disk space, with the fix for each |

Each step is skipped when its stamp is up to date ([ADR 0133](../adr/0133-one-command-builds.md)), so a second run with nothing changed takes seconds. A changed IPA is noticed. What you get, with one line saying how to start it, also in `BUILD.txt` next to it:

| `just build` | Result | |
|---|---|---|
| `mac` | `dist/mac/MapleThief.app` | universal (arm64, x86_64), ad-hoc signed, the bundle inside ([The macOS app](#the-macos-app)) |
| `win` | `dist/win/MapleThief-game-<version>-windows-x64/` and the same as a `.zip` | a portable folder: `MapleThief.exe` and `game/`, static runtime, runs on another PC |
| `linux` | `dist/linux/MapleThief-game-<version>-linux-x86_64/` and its `.tar.gz` | built in Docker ([Linux and Steam Deck](#linux-and-steam-deck)) |
| `ios` | `dist/ios/MapleThief.app` | Mac with Xcode; signed with `MAPLETHIEF_IOS_TEAM`, else unsigned ([Playing on your iPhone](../play/iphone.md)) |
| `switch` | `dist/switch/MapleThief.nro` | built in Docker from macOS or Linux; for now our own code only, it shows its version ([Nintendo Switch](#nintendo-switch)) |
| `… debug` | the same below `dist/<platform>/debug/` | **for debugging only**, see below |

Then start it from that path: `just run`, `open dist/mac/MapleThief.app`, the exe in the Windows folder, or `./MapleThief.sh` in the Linux folder. There's no import screen: your build already has the game, unlike the [player downloads](releases.md#what-a-release-holds), which import it on first launch ([ADR 0125](../adr/0125-player-builds-icon-guard-modes-and-release-flow.md)).

**To play, use a Release build:** `just build`, or a Release preset by hand. **A Debug build is for debugging.** It keeps asserts and memory checks on and can fall behind the game's real speed, most of all at a higher render scale, so don't judge the game's feel or timing by it ([ADR 0134](../adr/0134-debug-builds-optimise-the-hot-code.md)). `just build mac debug` also needs `work/` to stay where it is.

**Your data** goes in a `data` folder next to what you built, as in the player downloads: `dist/mac/data`, the `data` folder in the Windows or Linux folder ([ADR 0127](../adr/0127-data-folder-next-to-the-game.md), [Where your saves are](../play/saves.md#where-your-saves-are)). A rebuild keeps it, and so does `just clean`. Delete it by hand to reset. A build made by hand with a preset keeps its data in `build/<preset>/platform/data`, and saves from an older build stay there until you move them. To keep the data apart from the build, set `MAPLETHIEF_DATA_DIR` or start with `--data-location user`. [Running and options](running.md#the-data-folder) has the details.

### Build from your own copy, wherever it is

If your copy isn't in `original/` yet, name it on the command line. It is the same build as above:

```sh
scripts/build mac --ipa ~/Games/MapleThief-1.5.ipa
scripts/build-from-ipa ~/Games/MapleThief-1.5.ipa        # the same, path first
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 win --ipa "C:\My Games\MapleThief-1.5.ipa"
powershell -ExecutionPolicy Bypass -File scripts\build-from-ipa.ps1 "C:\My Games\MapleThief-1.5.ipa"
```

`scripts/build-from-ipa PATH [platform] [debug|release]` only calls `scripts/build`, so everything on this page holds for both ([ADR 0135](../adr/0135-accept-an-ipa-by-its-main-binary.md)). `--ipa` also goes with `scripts/build bootstrap`, which stops after the check and the extraction.

The file is checked before anything is copied or built. **A copy passes when its main binary, `Payload/MapleThief.app/MapleThief`, matches `original/SHA256SUMS`**, so a copy that was only repacked into a new zip is fine. A re-signed copy is not: signing changes that binary. The checked file is then copied to `original/MapleThief-1.5.ipa`, and the rest of the build reads it there. A file that's already in that spot is never replaced: if it's the same game, the build uses it and leaves it as it is, and if it's anything else, the build stops and tells you to move it away.

The tools are checked first too, with the same fixes as `just doctor`, for what that platform's build needs. These are the messages you can get:

| The message says | What it means |
|---|---|
| there is no file at this path | check the path, and quote it if it holds a space |
| not a zip file, not an iPhone app, another app, or no main binary | this file isn't the game's IPA |
| encrypted (`cryptid 1`), or encrypted with a zip password | this copy is encrypted and can't be used ([Your game file](../play/your-ipa.md)) |
| this is MapleThief 1.3; the port needs 1.5, or no readable version | the port only supports 1.5 |
| MapleThief 1.5 but not the known binary | the file was changed or damaged |
| already there and is not this game | `original/MapleThief-1.5.ipa` holds something else: move it away or delete it |
| tools that are missing or too old | each line names the tool and its fix |

**A path with a space** works with `scripts/build`, `scripts/build-from-ipa` and the `.ps1` scripts when you quote it as usual. `just` splits its arguments on spaces, so it needs quotes inside the quotes: `just build mac --ipa "'/my games/MapleThief-1.5.ipa'"`, or use the script directly. A relative path is read from the folder you ran the script in, and with `just` from the repository's root.

## Every step by hand

These are the steps `just build` runs, and the ones it leaves out. Run them from the repository's root. On Windows use PowerShell, from the [x64 developer environment](#windows-the-x64-developer-environment) for anything that compiles.

### 1. Bootstrap

```sh
scripts/bootstrap               # macOS, Linux
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
```

It is safe to re-run and writes byte-identical outputs every time. The scripts only find a suitable Python; the work is done by [`tools/mtport/bootstrap.py`](../../tools/mtport/bootstrap.py). It:

1. checks the IPA against `original/SHA256SUMS` (the exact file, or a [repacked copy](#build-from-your-own-copy-wherever-it-is) with the right main binary), then extracts it to `work/ipa/`. The IPA is treated as untrusted data: every entry name is checked before anything is written (no absolute paths, `..` or symbolic links), and nothing extracted is executed. `scripts/verify-original.sh` runs just this step (`--no-extract`: only the checks);
2. splits the fat main binary into `work/slices/MapleThief.armv6` and `work/slices/MapleThief.armv7` with a portable splitter ([`tools/mtport/fat.py`](../../tools/mtport/fat.py), no `lipo` needed) and checks both slices. On macOS it also cross-checks them against `lipo -thin`;
3. writes `work/slices/SHA256SUMS` and `work/bootstrap.json` (paths, sizes, hashes, fat offsets and the tool version). Other tools read `work/bootstrap.json` instead of hard-coding paths.

Any failure (missing file, the wrong game or version, an encrypted copy, a tool that's too old) stops with an `error:` line and a non-zero exit.

### 2. Convert the assets (optional)

The game doesn't need this step, and `just build` skips it. It is for the map, sprite and table viewers and the other tools that read standard files.

```sh
scripts/pipeline            # macOS, Linux; Windows: scripts\pipeline.ps1
scripts/pipeline check      # dry run: exit 1 if assets-out/ is out of date
```

This converts `work/ipa/Payload/MapleThief.app` into the git-ignored `assets-out/`: standard PNGs, JSON for maps, sprites, tables and strings, script disassembly, and the `png/*.dat` packs as single images. It writes `assets-out/manifest.json`, which lists every bundle file as converted, copied or ignored. Re-running only redoes what changed. Each failed file is printed with its reason, and the exit status is non-zero ([ADR 0011](../adr/0011-asset-pipeline-manifest-and-incremental-runs.md)).

### 3. Recompile the code

```sh
python3 -m recomp all       # Windows: py -m recomp all
```

This loads, discovers and lifts the armv7 slice in a few seconds. It writes the generated C into the ignored `work/gen/` (`mt_gen.h`, the translation units `fn_NNN.c`, `imports.c`, `dispatch.c` and `gen.cmake`) and its reports into `work/recomp/`. A file is rewritten only when its content changes, so the next build recompiles only what changed. `python3 -m recomp lift --units N` picks another number of translation units.

When `work/gen/gen.cmake` exists, the build compiles it as the static library `maplethief_gen`, and the program runs the game. Without it, the build skips that target and the program shows a placeholder. `-DMAPLETHIEF_GEN_DIR=<dir>` builds another output directory, and `-DMAPLETHIEF_GEN_DIR=` leaves the generated code out ([ADR 0014](../adr/0014-recompiler-design.md), [ADR 0021](../adr/0021-generated-code-partition-and-build.md)).

### 4. Configure, build and test

One command configures, builds and runs the tests:

```sh
cmake --workflow --preset mac-debug      # macOS
cmake --workflow --preset linux-debug    # Linux
cmake --workflow --preset win-debug      # Windows, from the x64 developer environment
```

Or step by step, with the same preset name for each step:

```sh
cmake --preset mac-debug             # configure into build/mac-debug
cmake --build --preset mac-debug     # build
ctest --preset mac-debug             # test
cmake --build --preset mac-debug --target run     # start the program
cmake --build --preset mac-debug --target smoke   # its headless smoke test, outside CTest
```

| Preset | OS | Build type | Output |
|---|---|---|---|
| `mac-debug` | macOS | Debug, host architecture | `build/mac-debug/platform/MapleThief.app` |
| `mac-release-universal` | macOS | Release, arm64 + x86_64, macOS 13+ | `build/mac-release-universal/platform/MapleThief.app` |
| `mac-app` | macOS | Release, arm64 + x86_64, macOS 13+, the game app from your IPA ([below](#the-macos-app)) | `build/mac-app/platform/MapleThief.app` |
| `win-debug` / `win-release` | Windows | Debug / Release, x64, MSVC | `build/win-*/platform/MapleThief.exe` |
| `linux-debug` / `linux-release` | Linux | Debug / Release | `build/linux-*/platform/MapleThief` |
| `mac-player` / `win-player` / `linux-player` | each OS | Release, the release's player build | see [Releases](releases.md) |
| `switch-debug` / `switch-release` | the devkitA64 container (`DEVKITPRO` set) | Debug / Release, AArch64 `-mcpu=cortex-a57+crypto`, `MAPLETHIEF_MEM_MODEL=checked` | `build/switch-*/MapleThief.nro`; the tests as `build/switch-*/tests/<program>.nro` ([Nintendo Switch](#nintendo-switch)) |

Run the program from the output path. With the generated code it runs the game from the extracted bundle in `work/ipa/` (`--bundle` or `MAPLETHIEF_BUNDLE_DIR` name another), and it needs that `work/` to stay where it is. Without the generated code (CI, a clean clone, the kits) it shows a solid placeholder colour.

Everything the build writes stays under `build/`: one directory per preset, plus `build/_downloads/`, the hash-checked cache of pinned third-party downloads that all presets share ([ADR 0003](../adr/0003-dependencies.md)). The first configure needs network access to fill it; later ones don't. Deleting `build/` resets everything, your saves in it included.

Warnings in our own code are errors when `CI` is set in the environment, or with `-DMAPLETHIEF_WARNINGS_AS_ERRORS=ON`. Presets, flags and the Windows toolchain are explained in [ADR 0004](../adr/0004-build-presets-and-generators.md).

### 5. Package a kit (optional)

The three release workflows also **package** a kit with CPack ([ADR 0023](../adr/0023-local-ci-and-packaging.md)): `cmake --workflow --preset <preset>` configures, builds, runs CTest and writes the package into `build/<preset>/`. `cpack --preset <preset>` repackages an existing build.

| Workflow preset | Package |
|---|---|
| `mac-release-universal` | `MapleThief-kit-<version>-macos-universal.zip` (the `.app` is ad-hoc signed) |
| `win-release` | `MapleThief-kit-<version>-windows-x64.zip` |
| `linux-release` | `MapleThief-kit-<version>-linux-x86_64.tar.gz` (built in the Linux container, [Testing and CI](testing.md#the-linux-container)) |
| `switch-release` | `MapleThief-kit-<version>-switch.zip` (`MapleThief.nro` and the docs; built in the Switch container) |

A kit holds only our own work and the libraries it links: the platform shell, `lib/` (the runtime and static SDL3), `include/`, the recompiler, the Python tools and the user scripts, the docs (Markdown), `THIRD_PARTY.md` and the licence texts. Nothing generated from the IPA: `maplethief_gen` and `work/` are never installed ([Releases](releases.md), [ADR 0024](../adr/0024-release-contents-and-flow.md)).

The version is `MAPLETHIEF_VERSION` (`X.Y.Z` or `X.Y.Z-rc.N`, default `0.0.1`), read from the environment variable of the same name on the first configure, or set with `-DMAPLETHIEF_VERSION=...`. It names the kit, and `MapleThief --version` prints it.

## Windows: the x64 developer environment

`just build` sets this up by itself. For anything else that compiles (the `win-*` presets, `scripts\test.ps1`) the presets use Ninja with MSVC, so they need the Visual Studio **x64** developer environment. Start "x64 Native Tools Command Prompt for VS 2022", or run `Enter-VsDevShell -Arch amd64` in PowerShell (the plain "Developer PowerShell" shortcut targets x86, which configure rejects), or open the folder in Visual Studio or VS Code, whose CMake integration sets it up from the presets. That environment also puts the CMake and Ninja that ship with Visual Studio on `PATH`.

## The macOS app

The `mac-app` preset builds the game as a double-clickable, universal `MapleThief.app` from your own IPA ([ADR 0064](../adr/0064-macos-game-app.md)). `just build` is the short way. By hand it needs `work/` and the generated code from the steps above:

```sh
scripts/bootstrap && python3 -m recomp all
cmake --workflow --preset mac-app          # build/mac-app/platform/MapleThief.app
open build/mac-app/platform/MapleThief.app
```

- **Universal:** Release, arm64 and x86_64, for macOS 13 and later. To run the Intel slice on Apple silicon, use `open --arch x86_64 MapleThief.app`.
- **Self-contained:** the build copies the extracted bundle into `Contents/Resources/Game/`, and makes the icon from the IPA's own artwork with `python3 -m mtport.macicon`, which uses `sips`. You can move the app anywhere. Its data goes in `data` next to it, except when it sits in an Applications folder, where it uses `~/Library/Application Support/MapleThief` ([ADR 0127](../adr/0127-data-folder-next-to-the-game.md)).
- **Signed ad hoc:** after every build the whole bundle is signed with `codesign --force --sign -`, without the hardened runtime. There's no Developer ID and no notarisation ([ADR 0024](../adr/0024-release-contents-and-flow.md)).
- **For your own machine only:** it holds Nexon's code and files. Such a build refuses `cmake --install` and `cpack`.

An app you built yourself has no quarantine attribute, so it opens without asking. A copy that arrived through a browser, AirDrop, Mail or Messages is quarantined, and macOS blocks it the first time and runs it from a read-only copy. The steps to allow it are the same as for the player download: [Mac](../play/README.md#mac) in the player guide.

## Linux and Steam Deck

`just build linux` (into `dist/linux/`), or `scripts/linux-game build` by hand (into `build/linux-game/`), builds the game in a pinned Ubuntu 24.04 container, on a Mac too, as `MapleThief-game-<version>-linux-x86_64/` and its `.tar.gz` ([ADR 0065](../adr/0065-linux-and-steam-deck-build.md)). It holds the launcher `MapleThief.sh`, the executable (static libstdc++, needs only glibc 2.39+) and a copy of your bundle. `scripts/linux-game check` runs it in a stock Ubuntu 24.04 image, and `--native` builds on your own Linux machine with your compiler. [Linux and Steam Deck](../play/linux-and-steam-deck.md) has the Steam Deck steps.

## Nintendo Switch

`MapleThief.nro` is a homebrew app for the Homebrew Menu ([ADR 0138](../adr/0138-nintendo-switch-homebrew-target.md), epic [#233](https://github.com/armaatus/maplethief-port/issues/233)). It is built with devkitA64 and libnx in a container: [`tools/containers/switch/Dockerfile`](../../tools/containers/switch/Dockerfile) starts from the `devkitpro/devkita64` image pinned by digest and adds CMake 4.4.4. SDL3 is the Switch fork pinned in `cmake/Dependencies.cmake`. Docker is all the host needs; on Apple silicon the image runs natively as arm64.

```sh
scripts/build-switch                          # HEAD, switch-release: build/switch-release/MapleThief.nro
scripts/build-switch --worktree               # your uncommitted tree instead of a commit
scripts/build-switch REV --preset switch-debug
scripts/build-switch --gen work/gen           # with your generated code (local only; the game on Horizon is #238)
just build switch                             # the same as scripts/build-switch, copied to dist/switch/
```

`scripts/build-switch` builds the image if it is missing (tagged `maplethief-switch:<hash of the Dockerfile>-<arch>`), copies the commit into the container with `git archive`, and runs `cmake --workflow --preset switch-release` there (configure, build, CTest and the kit). It copies `MapleThief.nro`, the test NROs, the kit and the test logs into `build/<preset>/`. By hand inside the image, `cmake --workflow --preset switch-debug` works the same; the `switch-*` presets need `DEVKITPRO` (set in the image) and use `$DEVKITPRO/cmake/Switch.cmake`.

What the build checks, on the host side of the build (CTest):

- `platform.switch_nro`: `python3 -m mtport.nro check` reads the NRO's NACP (name `MapleThief`, author `armaatus`, the project version) and requires a 256 x 256 baseline JPEG icon ([NRO format](../formats/nro.md)). The icon is our own drawing, made at build time by `python3 -m mtport.appicon switch-icon`.
- `platform.switch_guard`: the release guard on the NRO, split into its code, icon, NACP and RomFS.

The unit tests compile into one NRO per doctest program, `build/switch-*/tests/<program>.nro`; CTest does not run them, since they only run on a console. For now the NRO is our own code without the game: started from the Homebrew Menu, it shows `MapleThief <version>` and returns to the menu on +. The runtime on Horizon is [#236](https://github.com/armaatus/maplethief-port/issues/236); what Horizon lacks so far is stubbed in `__SWITCH__` branches that say so at run time (`runtime/include/maplethief/horizon.h`).

**On a console.** Copy `MapleThief.nro` to `sdmc:/switch/maplethief/` on the SD card and start it from the Homebrew Menu. Or send it over the network: press Y in the Homebrew Menu (NetLoader), which shows the console's IP address, then

```sh
scripts/switch-run --ip 192.168.1.23 build/switch-release/MapleThief.nro
MAPLETHIEF_SWITCH_IP=192.168.1.23 scripts/switch-run build/switch-release/tests/runtime_test.nro -- --list-test-cases
```

`scripts/switch-run` sends the NRO with `nxlink` (devkitPro's, or the one in the image) and stays connected, so what the program writes to stderr shows in your terminal. The console's IP address is the one the NetLoader shows, or Settings → Internet → Connection Status. CI never runs it.

## Shaders

The GPU renderer's shaders are HLSL in `runtime/src/gfx/gpu/shaders/`. After changing one, `scripts/build-shaders` (Docker) recompiles them into the committed `runtime/src/gfx/gpu/shaders_compiled.h`, and `scripts/build-shaders --check` (also the `shaders.stamps` test) says whether it is up to date.
