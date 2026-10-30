# maplethief-port

[![CI](https://github.com/armaatus/maplethief-port/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/armaatus/maplethief-port/actions/workflows/ci.yml)

A native port of **MapleStory Thief Edition** (Nexon Mobile, iOS, 2010; version 1.5) built from my own legally owned, decrypted IPA. It's a personal preservation project.

**Status: M0–M6 done (the M5 device-screenshot comparison waits on reference screenshots, #60; the M6 listening check is #124); next is M7 (input and controllers).** `scripts/bootstrap` verifies and unpacks the IPA, every file format has a spec and a tested parser, and `scripts/pipeline` converts the assets for the map, sprite and table viewers. The static recompiler turns the armv7 code into C (M3), and the HLE runtime boots it through `UIApplicationMain` (M4). A deterministic software renderer draws its OpenGL ES 1.1 frames, and the frontend shows them with UIKit's views in a scaled, letterboxed window: the main menu, the loading screen and the first map (M5). The effects and the looping map music play through the runtime's own deterministic mixer and one SDL3 audio stream, with volume settings in `settings.ini` (M6). Input is limited to test taps until M7.

## Targets

| Platform | Notes |
|---|---|
| macOS 13+ | Universal binary (Apple Silicon + Intel), ad-hoc signed `.app` |
| Windows 10/11 x64 | Portable zip |
| iOS 16+ arm64 | Sideloaded via Xcode |
| Linux / Steam Deck | x86_64, glibc 2.39+; a game folder you build from your IPA, also as a non-Steam game on the Deck ([docs/linux-and-steam-deck.md](docs/linux-and-steam-deck.md)) |

Planned features: full **controller support** (direct action mapping, remapping, glyphs), keyboard/mouse play, scaling modes, and opt-in quality-of-life and display enhancements. The original game logic is kept intact.

## How it works

**Hybrid static recompilation.** The armv7 code from the original binary (2,655 functions, mostly Thumb-2) is translated ahead of time into C. A high-level runtime then implements the 333 iOS imports on the host: libc/C++, CoreFoundation/Foundation, the Objective-C runtime, GCD, CoreGraphics, OpenGL ES 1.1 → a modern renderer, OpenAL/AVAudioPlayer, UIKit input, and offline stubs for ads/IAP/Game Center. SDL3 provides windowing, input, gamepads and audio on every platform. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build from your own copy

The repo contains exactly one game input: [`original/MapleThief-1.5.ipa`](original/README.md) (Git LFS), plus checksums. **Everything else is generated from it locally by scripts and is never committed**: extracted files, converted assets, recompiled code and binaries. The game's code and assets are Nexon's property. This repo stays private, and builds are never shared publicly.

You need Git LFS and **Python 3.10+** (standard library only). On macOS the Command Line Tools `python3` is 3.9, which is too old: install one with Homebrew (`brew install python`), from python.org, or with `uv python install 3.12`, and set `PYTHON=/path/to/python3` if it isn't first on `PATH`. Nothing else: the checks and the extraction are done in Python, so no `unzip` or `shasum` is needed.

```sh
git clone git@github.com:armaatus/maplethief-port.git
cd maplethief-port
git lfs install && git lfs pull
scripts/bootstrap               # macOS / Linux
```

On Windows (PowerShell 7+, or Windows PowerShell 5.1):

```powershell
git lfs install; git lfs pull
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
```

`scripts/bootstrap` is safe to re-run and writes byte-identical outputs every time. The scripts only find a suitable Python; the work is done by [`tools/mtport/bootstrap.py`](tools/mtport/bootstrap.py). It:

1. checks that the IPA is the real file (not an LFS pointer), has the right size and matches `original/SHA256SUMS`, then extracts it to `work/ipa/`. The IPA is treated as untrusted data: every entry name is checked before anything is written (no absolute paths, `..` or symbolic links), and nothing extracted is executed. [`scripts/verify-original.sh`](scripts/verify-original.sh) runs just this step (`--no-extract`: only the checks);
2. splits the fat main binary into `work/slices/MapleThief.armv6` and `work/slices/MapleThief.armv7` with a portable splitter ([`tools/mtport/fat.py`](tools/mtport/fat.py), no `lipo` needed) and checks both slices against `original/SHA256SUMS`. On macOS it also cross-checks them against `lipo -thin`;
3. writes `work/slices/SHA256SUMS` and `work/bootstrap.json` (paths, sizes, hashes, fat offsets and the tool version; the schema is documented in [`tools/mtport/bootstrap.py`](tools/mtport/bootstrap.py)). Other tools read `work/bootstrap.json` instead of hard-coding paths.

Any failure (LFS pointer, wrong size or hash, missing or too-old tool) stops with an `error:` line and a non-zero exit.

### Convert the assets

```sh
scripts/pipeline            # macOS / Linux; Windows: scripts\pipeline.ps1
scripts/pipeline check      # dry run: exit 1 if assets-out/ is out of date
```

This converts `work/ipa/Payload/MapleThief.app` into the git-ignored `assets-out/` (standard PNGs, JSON for maps, sprites, tables and strings, script disassembly, the `png/*.dat` packs as single images) in a few seconds, and writes `assets-out/manifest.json`, which lists every bundle file as converted, copied or ignored. Re-running only redoes what changed. Each failed file is printed with its reason, and the exit status is non-zero ([ADR 0011](docs/adr/0011-asset-pipeline-manifest-and-incremental-runs.md)).

### Recompile the code

```sh
python3 -m recomp all       # load, discover and lift the armv7 slice (a few seconds); Windows: py -m recomp all
```

This writes the generated C into the ignored `work/gen/`: `mt_gen.h`, about 60 translation units `fn_NNN.c` (`python3 -m recomp lift --units N` picks another count), `imports.c`, `dispatch.c` and `gen.cmake`. It also writes its reports into `work/recomp/`. A file is rewritten only when its content changes, so the next build recompiles only what changed. When `work/gen/gen.cmake` exists, the build below compiles the generated C as the static library `maplethief_gen` (about 7 s in Release on an Apple M4). Without it, the build skips that target. `-DMAPLETHIEF_GEN_DIR=<dir>` builds another output directory, and `-DMAPLETHIEF_GEN_DIR=` leaves the generated code out ([ADR 0014](docs/adr/0014-recompiler-design.md), [ADR 0021](docs/adr/0021-generated-code-partition-and-build.md)).

### Build

You need **CMake 3.28+**, **Ninja 1.11+**, **Python 3.10+** and a C/C++ compiler: Apple clang 15+ (Xcode 15 or its Command Line Tools) on macOS, Visual Studio 2022 17.8+ with the "Desktop development with C++" workload on Windows, GCC 13+ or Clang 17+ on Linux ([ADR 0002](docs/adr/0002-language-and-toolchain.md)). On Linux, SDL3 also needs its development packages; on Ubuntu 24.04:

```sh
sudo apt install build-essential cmake ninja-build pkg-config python3 libasound2-dev libpulse-dev \
  libpipewire-0.3-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
  libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
  libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev libdbus-1-dev libudev-dev
```

One command configures, builds and runs the tests:

```sh
cmake --workflow --preset mac-debug      # macOS
cmake --workflow --preset linux-debug    # Linux
cmake --workflow --preset win-debug      # Windows, from an x64 Visual Studio developer prompt
```

On Windows the presets use Ninja with MSVC, so they need the Visual Studio **x64** developer environment: start "x64 Native Tools Command Prompt for VS 2022" (or run `Enter-VsDevShell -Arch amd64` in PowerShell; the plain "Developer PowerShell" shortcut targets x86, which configure rejects), or open the folder in Visual Studio or VS Code, whose CMake integration sets it up from the presets. That environment also puts the CMake and Ninja that ship with Visual Studio on `PATH`.

Or step by step (same preset name for each step):

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

#### Package

The three release workflows also **package** the kit with CPack ([ADR 0023](docs/adr/0023-local-ci-and-packaging.md)): `cmake --workflow --preset <preset>` configures, builds, runs CTest and then writes the package into `build/<preset>/`. `cpack --preset <preset>` repackages an existing build.

| Workflow preset | Package |
|---|---|
| `mac-release-universal` | `MapleThief-kit-<version>-macos-universal.zip` (zipped with `ditto -c -k --norsrc --keepParent`; the `.app` is ad-hoc signed) |
| `win-release` | `MapleThief-kit-<version>-windows-x64.zip` |
| `linux-release` | `MapleThief-kit-<version>-linux-x86_64.tar.gz` (built in the Linux container, below) |

The kit holds only our own work and the libraries it links: the platform shell (`MapleThief.app` or `bin/MapleThief`), `lib/` (runtime and static SDL3), `include/`, the recompiler, the Python tools and the user scripts, the docs (Markdown), `THIRD_PARTY.md` and the licence texts. Nothing generated from the IPA: `maplethief_gen` and `work/` are never installed. These kits are what a release carries ([Releases](#releases), [ADR 0024](docs/adr/0024-release-contents-and-flow.md)).

The version is `MAPLETHIEF_VERSION` (`X.Y.Z` or `X.Y.Z-rc.N`, default `0.0.1`), read from the environment variable of the same name on the first configure, or set with `-DMAPLETHIEF_VERSION=...`. It names the kit, and `MapleThief --version` prints it.

Everything the build writes stays under `build/`: one directory per preset, plus `build/_downloads/`, the hash-checked cache of pinned third-party downloads that all presets share ([ADR 0003](docs/adr/0003-dependencies.md)). The first configure needs network access to fill it; later configures don't. Deleting `build/` resets everything.

Run the program from the output path (on macOS also `open build/mac-debug/platform/MapleThief.app`). It opens a 960×640 window titled `MapleThief` and exits with status 0 when you close it.

- **With the generated code** (`work/gen/` from [Recompile the code](#recompile-the-code), then a reconfigure), the program links `maplethief_gen` and **runs the game**: it boots the recompiled armv7 slice of the extracted bundle (`work/ipa/Payload/MapleThief.app` by default; `--bundle <dir>` or `MAPLETHIEF_BUNDLE_DIR` name another) and shows its frames. Saves go to the per-user data directory (`~/Library/Application Support/MapleThief`, `%APPDATA%\MapleThief`, `~/.local/share/MapleThief`), or to `MAPLETHIEF_DATA_DIR`. `--render-scale N` renders at N× (1–8); `--frames N` quits after N frames and checks that the window showed the last one; `--capture <dir>` writes read-backs of the window as PNGs; `--tap T,X,Y` clicks screen point (X, Y) at T seconds, and `--swipe T,X1,Y1,X2,Y2` drags from (X1, Y1) to (X2, Y2) in 0.3 s (test aids, through the same path as the mouse). `MAPLETHIEF_LANGUAGE=ko` plays the Korean build, with its terms screen ([ADR 0049](docs/adr/0049-menus-online-buttons-language-and-menu-flows.md)). Such a build is for your own machine only: a release kit never contains generated code ([ADR 0024](docs/adr/0024-release-contents-and-flow.md), [ADR 0042](docs/adr/0042-frontend-game-loop-and-presentation.md)).
- **Mouse and touch** ([ADR 0047](docs/adr/0047-touch-input-mouse-and-fingers.md)). The left mouse button is your finger: press, drag and release on the game's screen. On a touch screen every finger is a touch, so you can hold the d-pad and attack at once. The right mouse button can be a second finger held at a fixed point: `right_touch = 440,290` in `settings.ini` (or `--right-touch 440,290`; `off` by default) puts it on the attack key. A press on the bars around the image does nothing; a drag that leaves the image stays on its edge. When the window loses focus, every touch is lifted.
- **Gamepads** ([ADR 0048](docs/adr/0048-gamepad-actions-and-delivery.md)). Plug in an Xbox, PlayStation or other SDL3 gamepad, before or while the game runs; the first pad you press is the one that plays. In the field: the d-pad or left stick moves (diagonals too), A / Cross jumps, X / Square or the right trigger attacks, Y / Triangle and B / Circle are quick slots 1 and 2 (a new character's Red and Blue Potions), LB and RB quick slots 3 and 4, the left trigger opens the cash shop, Start the popup menu and Back / View the map. In menus and popups: A / Cross is OK, B / Circle and Start go back, LB / RB are minus / plus (tabs), X / Square and Y / Triangle revive on the death popup. On the help pages left and right turn the page. On the main menu, the terms, the character screens and the cash shop a yellow frame shows the focused button ([ADR 0051](docs/adr/0051-controller-focus-navigation.md)): the d-pad or stick moves it (and scrolls About's text), A / Cross presses it, B / Circle presses the screen's close, Back or No. In the popup menu A / Cross on a tab opens it. Buttons sit by position, so a PlayStation pad's Cross is where an Xbox pad's A is. Unplugging a pad or leaving the window lets go of everything it held.
- **Keyboard** ([docs/controls/keyboard.md](docs/controls/keyboard.md), every key and why). In the field: the arrow keys move, X (or Space, Alt) jumps, Z (or Ctrl) attacks, A S D F (or 1 2 3 4) are the four quick slots (a new character's Red and Blue Potions on A and S), Enter is OK, Esc opens the popup menu, M the map and C the cash shop. In menus and popups: Enter, Z or Space is OK, Esc, X or Backspace goes back, the arrows move, Q / E are minus / plus (tabs), R / G revive on the death popup. On the main menu, the terms and the character screens the arrows move the focused button and Enter presses it (Play first), Esc presses the screen's close, Back or No ([ADR 0051](docs/adr/0051-controller-focus-navigation.md)). On the character select, N is New; Delete is its button (the Delete key does nothing there). A held key is one press until you let go; leaving the window lets go of every key.
- **The on-screen key pad** ([ADR 0052](docs/adr/0052-hiding-the-on-screen-key-pad.md)) fades out while you play with a gamepad or the keyboard and comes back when you touch or click the screen; that first touch already counts. `hide_keypad = always` in `settings.ini` (or `--hide-keypad always`) keeps it hidden (touches on it still work), and `never` always shows it. The default is `auto`.
- **Without it** (CI, a clean clone, the release kits), it shows a solid placeholder colour. `--gfx-prototype` shows the rendering backend's prototype scene instead.
- **Scaling and settings** ([ADR 0043](docs/adr/0043-scaling-modes-mouse-mapping-and-settings-file.md)). `--scale-mode` picks how the 480 × 320 screen fills the window: `integer` (the default: the largest whole multiple, crisp even pixels), `fit` (fills the window, nearest), `smooth` (fills it, bilinear) or `sharp` (fills it, sharp bilinear). `--border-color #RRGGBB` colours the bars around it (black by default), and `--window WxH` sets the window's size in points (960x640 by default). On a HiDPI display the window is drawn at its full pixel resolution. The game also reads `settings.ini` from its data directory; the command line overrides it:

  ```ini
  # settings.ini: one key = value per line; lines starting with # are comments
  scale_mode = sharp
  border_color = #102030
  render_scale = 1
  # volume in percent, 0 to 100
  master_volume = 100
  music_volume = 100
  effects_volume = 100
  # silence while the window is not focused: on | off
  mute_unfocused = on
  # loop the music without the silence at the start and end of its files: on | off (off: as the device)
  gapless_music = off
  # the right mouse button: a touch at screen point X,Y, or off
  right_touch = off
  # hide the on-screen key pad: auto (while a gamepad or the keyboard is used) | always | never
  hide_keypad = auto
  ```

  Unknown keys and bad values are logged and ignored. The audio settings also have flags in every mode (`--master-volume N`, `--music-volume N`, `--effects-volume N`, `--mute-unfocused on|off`, `--gapless-music on|off`), and `--save-settings` writes the settings in effect back to `settings.ini`, keeping your comments ([ADR 0043's addendum](docs/adr/0043-scaling-modes-mouse-mapping-and-settings-file.md#addendum-the-audio-keys-and-writing-the-file-64-2026-10-29)). Most of the game's music files end in seconds of silence, so the music pauses at each loop as it did on the iPhone (3.3 s on the main menu); `gapless_music = on` skips that silence ([ADR 0062](docs/adr/0062-optional-gapless-music-loops.md)).

**Linux and Steam Deck** ([docs/linux-and-steam-deck.md](docs/linux-and-steam-deck.md), [ADR 0065](docs/adr/0065-linux-and-steam-deck-build.md)): `scripts/linux-game build` builds the game in a pinned Ubuntu 24.04 container (on a Mac too) into `build/linux-game/MapleThief-game-<version>-linux-x86_64/` and its `.tar.gz`: the launcher `MapleThief.sh`, the executable (static libstdc++, needs only glibc 2.39+) and a copy of your bundle. `scripts/linux-game check` runs it in a stock Ubuntu 24.04 image. On the Deck it runs as a non-Steam game, at 1280×800 under gamescope. It contains the game: keep it to your own machines.

`--version` prints `MapleThief <version>` and exits. `--smoke-frames N` runs the placeholder (never the game) and quits by itself after N frames; with `SDL_VIDEO_DRIVER=dummy` it runs without a display, which is how the `platform.smoke` test and CI run it. Warnings in our own code are errors when `CI` is set in the environment, or with `-DMAPLETHIEF_WARNINGS_AS_ERRORS=ON`. Presets, flags and the Windows toolchain are explained in [ADR 0004](docs/adr/0004-build-presets-and-generators.md).

#### The macOS app

The `mac-app` preset builds the game as a double-clickable, universal `MapleThief.app` from your own IPA ([ADR 0064](docs/adr/0064-macos-game-app.md)). It needs `work/` and the generated code from the steps above:

```sh
scripts/bootstrap && python3 -m recomp all
cmake --workflow --preset mac-app          # build/mac-app/platform/MapleThief.app
open build/mac-app/platform/MapleThief.app
```

- **Universal:** Release, arm64 and x86_64 (`lipo -info build/mac-app/platform/MapleThief.app/Contents/MacOS/MapleThief`), for macOS 13 and later. To run the Intel slice on Apple silicon (Rosetta), tick Finder › Get Info › *Open using Rosetta*, or run `open --arch x86_64 MapleThief.app`.
- **Self-contained:** the build copies the extracted bundle into `Contents/Resources/Game/`. It also makes the icon from the IPA's own artwork (`iTunesArtwork`, else the largest icon `Info.plist` names) with `python3 -m mtport.macicon`, which uses `sips`. You can move the app anywhere, `/Applications` included. Saves stay in `~/Library/Application Support/MapleThief`.
- **Signed ad hoc:** after every build the whole bundle is signed with `codesign --force --sign -`, without the hardened runtime. There's no Developer ID and no notarisation (ADR 0024). `codesign --verify --deep --strict` passes. `spctl --assess` rejects it, as it rejects every ad-hoc app.
- **For your own machine only:** it holds Nexon's code and files. Such a build refuses `cmake --install` and `cpack`; the kit comes from `mac-release-universal`.

**Gatekeeper.** An app you built yourself has no quarantine attribute, so it opens without asking. A copy that arrived through a browser, AirDrop, Mail or Messages is quarantined: the release kit's zip, for example, or your app copied to another Mac. macOS then refuses to open it the first time ("Apple could not verify 'MapleThief' is free of malware…"), because it isn't notarised. To allow it:

- **macOS 15 Sequoia and later:** open the app once and close the alert with *Done*. In System Settings › Privacy & Security, scroll to *Security*, click **Open Anyway** next to "'MapleThief' was blocked…", then confirm with *Open* and your password. Since Sequoia, Control-click › Open no longer gets past Gatekeeper.
- **macOS 13 Ventura and 14 Sonoma:** Control-click (or right-click) the app, choose **Open**, then *Open* in the dialog. The Privacy & Security route works too.
- **Terminal, any version:** remove the attribute with `xattr -dr com.apple.quarantine MapleThief.app`. For a kit, run it on the unzipped `MapleThief-kit-…` folder, which covers its scripts too. Do this only for a copy you trust.

### Tests

You also need [uv](https://docs.astral.sh/uv/) (for the Python tests and the linters). One command runs every test, C/C++ and Python, and exits non-zero if anything fails:

```sh
scripts/test        # macOS / Linux
```

On Windows, from the x64 developer prompt (see above):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\test.ps1
```

It runs `cmake --workflow --preset <os>-debug` (configure, build, CTest) and then `uv run --locked --group test pytest`. uv installs the pinned pytest and Capstone from `uv.lock` into `build/python-env/`, so nothing is installed globally. On a Mac (outside CI) pytest runs twice, on the newest installed Python and on 3.10, the minimum; elsewhere once, on uv's choice. Useful options: `--release` (the release workflow with package instead, plus the `lipo -archs` check of the universal binary on macOS), `--python LIST` (e.g. `--python 3.10,latest`), `--preset NAME`, `--cpp-only`, `--python-only`, and `-- <pytest args>` (e.g. `scripts/test --python-only -- -k fat`); on Windows `-Release`, `-Preset`, `-CppOnly`, `-PythonOnly`, `-PytestArgs`. `UV_PYTHON=3.10 scripts/test` runs the Python tests on that Python only (uv downloads it if needed).

- Tests that need the real game files are marked `real_assets`. They are reported as **skipped** until `scripts/bootstrap` has created `work/ipa/`, which is always the case in CI. Mark such a test with `@pytest.mark.real_assets`.
- C/C++ tests are doctest test cases in `tests/cpp/` and `tests/golden/`, added with `maplethief_add_cpp_test()` in `tests/CMakeLists.txt`. Run a subset with `ctest --preset mac-debug -R golden.`.
- **Golden images**: `maplethief::golden::check_golden()` (`tests/golden/golden.h`) compares a rendered image with a golden PNG, with a per-channel tolerance and a maximum ratio of differing pixels. On failure it writes `<name>.actual.png` and `<name>.diff.png` (differing pixels in red) to `build/test-output/<preset>/`. Only synthetic goldens are committed (`tests/golden/data/`); goldens of real game frames belong in `work/golden/`. To create or update goldens from the current output, run the test with `MAPLETHIEF_UPDATE_GOLDEN=1`, e.g. `MAPLETHIEF_UPDATE_GOLDEN=1 ctest --preset mac-debug -R golden.`, then review the new PNGs before committing them.

- **The title for a minute** ([#74](https://github.com/armaatus/maplethief-port/issues/74)): with the generated code, `platform.title_60s` runs `MapleThief --fixed-step 16.667 --seconds 60` headless from a fresh data directory (`python3 -m mtport.titlerun soak --binary <MapleThief>`). It fails on a crash, a trap, a GL misuse, a non-empty unimplemented report, a frame loop that stalls, a slide-in that does not move, or a title that changes or goes black after it. Its log, report and shots go to `build/test-output/<preset>/title_60s/`.
- **Coverage of the path to the title**: `PYTHONPATH=tools python3 -m mtport.titlerun coverage` configures `build/title-coverage/` from the `<os>-debug` preset with `-DMAPLETHIEF_TRACE_ENTRY=ON`, builds MapleThief and its symbol table, runs the same minute with `MAPLETHIEF_COVERAGE`, and writes `build/title-coverage/report/title.coverage` (every guest function entered: pointer, entries, name) and `title-coverage.md` (the count, by class, the most entered). The fixed-step run is deterministic, so the same code gives the same report; it is derived from the IPA and never committed.

The details are in [ADR 0005](docs/adr/0005-test-harness.md).

### Local CI

Every check CI runs also runs locally, and runs there first ([ADR 0023](docs/adr/0023-local-ci-and-packaging.md)). On the Mac one command does CI's macOS and Linux jobs, the linters and both Pythons:

```sh
scripts/local-ci                 # HEAD; or scripts/local-ci <rev>
scripts/local-ci --release       # the release workflows instead: build, test, package
scripts/local-ci --only lint,py-min
```

It checks out the commit as a shared clone in `build/local-ci/<rev>/src` (no `work/`, the IPA left as its Git LFS pointer and checked by the same guard as CI), sets `CI=true` so warnings are errors, and runs the jobs `lint` (`scripts/lint`), `py-min` (Python tests on 3.10), `mac` (`scripts/test` on the latest Python, or `--release`) and `linux` (below, in parallel). It prints a summary per job with its duration and test counts, keeps the logs in `build/local-ci/<rev>/logs/` (`<rev>-release/` for `--release`), and exits non-zero if any job failed.

**Linux** runs in a container, x86_64 like `ubuntu-latest` (Rosetta under [OrbStack](https://orbstack.dev) or Docker Desktop on Apple silicon). The image, [`tools/containers/linux/Dockerfile`](tools/containers/linux/Dockerfile), is Ubuntu 26.04 pinned by digest, with its packages from an Ubuntu archive snapshot and CMake, PowerShell and uv at pinned versions:

```sh
scripts/test-linux               # Debug workflow + pytest on a clean export of HEAD
scripts/test-linux --release     # linux-release workflow; the kit lands in build/linux-release/
scripts/test-linux --worktree    # uncommitted changes to tracked files included
scripts/test-linux --arch arm64  # native on Apple silicon: faster, but not what CI runs
scripts/test-linux --real-assets # mount work/ read-only, so the real_assets tests run too
```

The first run builds the image. While `snapshot.ubuntu.com` is unavailable, add `--build-arg UBUNTU_SNAPSHOT=` (also to `scripts/local-ci`) to build it from the live archive.

**Windows** runs natively: `scripts\test.ps1` (Debug + pytest) and `scripts\test.ps1 -Release` (the `win-release` workflow with package), from the x64 developer prompt.

### CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on pull requests and on manual runs (Actions → CI → Run workflow), not on pushes, on GitHub-hosted `ubuntu-latest`, `macos-latest` and `windows-latest` runners ([ADR 0006](docs/adr/0006-continuous-integration.md)). It confirms what the local runs above already showed:

- **build** (one job per OS): the same `scripts/test` / `scripts/test.ps1` as above, so the Debug build, CTest (including the headless `platform.smoke`) and pytest. Warnings are errors because `CI` is set. A manual run adds Release jobs for `linux-release`, `win-release` and `mac-release-universal` (input `release`, on by default), which also package the kit; the universal binary must contain both `arm64` and `x86_64` (`lipo -verify_arch`).
- **lint + Python min/latest** (Linux): `scripts/lint`, then `scripts/test --python-only` on Python 3.10 and on the latest Python release.

CI never sees the IPA: checkout skips Git LFS, and a guard step in every job fails if any `*.ipa` is real content instead of an LFS pointer. `real_assets` tests show up as skipped. A job uploads an artifact only when it fails, and then only `build/test-output/` and CTest's logs.

#### Self-hosted runners

A manual run with input `runners: self-hosted` sends the same jobs to self-hosted runners instead, which cost no Actions minutes. Pull requests always use the hosted runners, so code from a pull request never runs on these machines. The jobs ask for these labels:

| Job | Labels | Machine |
|---|---|---|
| macOS | `self-hosted`, `macOS`, `ARM64` | the Mac |
| Windows | `self-hosted`, `Windows`, `X64` | the Windows PC, with Visual Studio 2022+ and its C++ workload |
| Linux, lint + Python | `self-hosted`, `Linux`, `X64` | a Linux x86_64 runner, e.g. the [actions/runner image](https://github.com/actions/runner/blob/main/images/Dockerfile) in OrbStack with `--platform linux/amd64` |

To set one up: Settings → Actions → Runners → New self-hosted runner gives the download and a registration token; run `./config.sh --url https://github.com/armaatus/maplethief-port --token <token> --labels <labels>` and install it as a service (`./svc.sh install && ./svc.sh start` on macOS/Linux, `config.cmd --runasservice` on Windows). Keep the runner's directory away from the checkout, so its jobs never see `work/` or the real IPA. Settings → Actions → General must keep "Run workflows from fork pull requests" off. The Linux runner needs `sudo` for the apt step (the actions/runner image has it).

## Releases

A release is a GitHub Release in this private repository with one **port kit** per platform, `SHA256SUMS` and generated release notes ([ADR 0024](docs/adr/0024-release-contents-and-flow.md)). A kit holds only our own code, built from the tag: the platform shell, the runtime and SDL3 libraries and headers, the recompiler, the Python tools and scripts, the docs and the licences. It never holds the game build (recompiled code, converted assets, anything from `work/`): each user builds that from their own IPA. A content guard (`python3 -m mtport.release guard`) rejects any kit with generated-code files or symbols, files from `work/`, `assets-out/` or `build/`, files identical to IPA files, or images and audio.

| Kit | Built on | By |
|---|---|---|
| `MapleThief-kit-<version>-macos-universal.zip` | the Mac | `scripts/release build TAG` |
| `MapleThief-kit-<version>-linux-x86_64.tar.gz` | the Mac, in the Linux container | `scripts/release build TAG` |
| `MapleThief-kit-<version>-windows-x64.zip` | the Windows PC | `scripts\release.ps1 build TAG` |

Tags are annotated SemVer, `vX.Y.Z`, and `vX.Y.Z-rc.N` for pre-releases. The flow, with no GitHub Actions involved:

```sh
git tag -a TAG -m "MapleThief TAG" && git push origin TAG   # on main
scripts/release start TAG      # Mac: checks, scripts/local-ci TAG, draft release with notes
scripts/release build TAG      # Mac: macOS + Linux kits, guard, .sha256, upload to the draft
#   Windows PC: scripts\release.ps1 build TAG
scripts/release publish TAG    # Mac: all kits present, SHA256SUMS, publish
```

The macOS kit's `MapleThief.app` is signed ad hoc, not notarised: see [Gatekeeper](#the-macos-app) to open it after a download. `--dry-run` runs every local step and prints the GitHub commands instead, so the flow can be rehearsed on a local tag. The full checklist, with what each step checks and what to do when one fails, is [docs/release-checklist.md](docs/release-checklist.md). To check a download: `shasum -a 256 -c SHA256SUMS` (or `sha256sum -c`).

## Repository layout

| Path | Purpose |
|---|---|
| `original/` | The IPA (LFS) + `SHA256SUMS`, the single source of truth |
| `docs/` | [Analysis](docs/ANALYSIS.md), [architecture](docs/ARCHITECTURE.md), [roadmap](docs/ROADMAP.md) |
| `docs/formats/` | [File format specs](docs/formats/README.md) |
| `docs/adr/` | Architecture decision records |
| `docs/re/` | Reverse-engineering notes |
| `tools/` | Extractor, format parsers, converters, viewers, Ghidra scripts, the Linux CI container (`containers/linux/`) and the portable Linux game build's (`containers/linux-game/`) |
| `recomp/` | Static recompiler |
| `runtime/` | HLE runtime |
| `platform/` | SDL3 frontends per OS |
| `tests/` | Python tests (pytest), C/C++ tests (`cpp/`, doctest), golden-image helper (`golden/`) |
| `scripts/` | Bootstrap / verification / build helpers, `scripts/pipeline`, `scripts/test`, `scripts/lint`, `scripts/local-ci`, `scripts/test-linux`, `scripts/release`, `scripts/linux-game` |
| `work/`, `assets-out/`, `build/` | Generated, git-ignored |

## Planning

- **Roadmap:** [docs/ROADMAP.md](docs/ROADMAP.md), milestones M0–M11 from foundation to v1.0
- **Project board:** [MapleThief Port](https://github.com/users/armaatus/projects/2) (Backlog → Ready → In progress → In review → Done)
- **Issues:** one epic per milestone, plus 94 task issues with goals, tasks, acceptance criteria and blockers

## How the AI agent works through this repo

The port is built by an AI agent working **one epic at a time** in milestone order. It plans the epic's child issues in dependency waves, hands each child to a subagent working in its own git worktree on an `N-slug` branch, merges the finished children into an `epic-N-slug` branch, and opens one PR per epic whose body closes the epic and its children with the evidence for each acceptance criterion. Design decisions become ADRs, file formats get specs plus tested parsers, and anything that needs me (device testing, signing, playthroughs) is labelled `needs-human`. The full rules are in [CLAUDE.md](CLAUDE.md) (also available as `AGENTS.md`).
