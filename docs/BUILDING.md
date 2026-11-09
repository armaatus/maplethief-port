# Building MapleThief from source

This page takes you from a fresh clone to a running game on your own computer, one step at a time. If you only want to play, you don't need it: the [downloads](play/README.md#download-and-play) need no tools at all.

You bring your **own copy of the game**, and everything else is made from it on your machine. What you build holds the game, so keep it to your own computers and never share or upload it.

For every step by hand, the presets and all the options, see the deep reference: [Build from source](build/README.md).

| System | State |
|---|---|
| macOS 13 or later | builds a universal app (Apple silicon and Intel) |
| Windows 10/11, x64 | builds a portable folder and a zip |
| Linux x86_64 | builds a folder and a tarball, in Docker |
| Steam Deck | [built on another computer](#steam-deck), then copied over |
| iOS | [not yet](#ios) |

## 1. Install the tools

Do the part for your system. All of it is free. You also need about 3 GB of free disk space, and internet access for the first build, which downloads libraries such as SDL3.

### macOS

1. Install the Command Line Tools, which bring Git and the Apple clang compiler (15 or later): `xcode-select --install`
2. Install [Homebrew](https://brew.sh) if you don't have it, then:

   ```sh
   brew install python cmake ninja just
   ```

3. Check that `python3 --version` says 3.10 or later. The `python3` that comes with the Command Line Tools is 3.9, which is too old. If yours is the old one, put `PYTHON=/opt/homebrew/bin/python3` (or wherever Homebrew's is) in front of the build commands below.

### Windows

1. [Git for Windows](https://git-scm.com/download/win).
2. Python 3.10 or later, from [python.org](https://www.python.org/downloads/) or the Microsoft Store.
3. Visual Studio 2022 (17.8 or later) with the **Desktop development with C++** workload. CMake and Ninja come with it. You can run the build from any terminal: it finds Visual Studio by itself.
4. Optional: [just](https://just.systems), with `winget install Casey.Just`.

Use PowerShell for the commands below.

### Linux

The game is built in a container, so you need Docker but no compiler. On Ubuntu 24.04:

```sh
sudo apt install git python3 just docker.io
sudo usermod -aG docker "$USER"     # then log out and in again, so that `docker ps` works
```

On another distribution, install the same things with its own package manager. `just` is optional.

## 2. Get the repository

```sh
git clone https://github.com/armaatus/maplethief-port.git
cd maplethief-port
```

Run all the commands below from this folder.

## 3. Your own game file

You need `MapleThief-1.5.ipa`, the decrypted iOS game, version 1.5. [Your game file](play/your-ipa.md) says exactly which file it must be and how to check it. We can't provide it or say how to get one.

You have two choices:

- **Put it in the `original/` folder**, named exactly `MapleThief-1.5.ipa`. Git ignores it there. Or:
- **Leave it where it is** and name it in the build command (next step, `--ipa`). The build checks the file and copies it to `original/` for you.

## 4. Check your tools

`doctor` lists what it finds, and the fix for what's missing:

| With just | Without just |
|---|---|
| `just doctor` | macOS, Linux: `scripts/build doctor` |
| | Windows: `powershell -ExecutionPolicy Bypass -File scripts\build.ps1 doctor` |

If a line says `FAIL`, see [When doctor complains](#when-doctor-complains). If you'll use `--ipa`, the `The IPA` line fails until the build has copied your file; that's expected.

## 5. Build

Pick the line for your system. It builds for the system you're on, as a Release build.

| System | With just | Without just |
|---|---|---|
| macOS | `just build` | `scripts/build` |
| Linux | `just build` | `scripts/build` |
| Windows | `just build` | `powershell -ExecutionPolicy Bypass -File scripts\build.ps1` |

With your file somewhere else, add `--ipa`:

```sh
scripts/build --ipa ~/Games/MapleThief-1.5.ipa        # macOS, Linux
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 --ipa "C:\Games\MapleThief-1.5.ipa"
```

Quote a path that holds a space. With `just` that takes quotes inside quotes, so use the script directly instead ([why](build/README.md#build-from-your-own-copy-wherever-it-is)). The same page explains every message you can get about a wrong file.

The first build takes a while. It checks your file, extracts it, recompiles the game into C, and compiles everything. Afterwards, a build with nothing changed takes seconds, because each step is skipped when it's up to date.

`just build linux` also works from a Mac or a Windows PC with Docker. The Mac app only builds on a Mac and the Windows game only on Windows.

## 6. Start the game

The build's last line says what it made and how to start it. The same line is in `BUILD.txt` next to the result.

| System | Result | Start it |
|---|---|---|
| macOS | `dist/mac/MapleThief.app` | `just run`, `open dist/mac/MapleThief.app`, or double-click it |
| Windows | `dist\win\MapleThief-game-<version>-windows-x64\` (the folder), and the same as a `.zip` | `just run`, or double-click `MapleThief.exe` in the folder |
| Linux | `dist/linux/MapleThief-game-<version>-linux-x86_64/` (the folder), and its `.tar.gz` | `just run`, or `./MapleThief.sh` in the folder |

Without just, `scripts/build run` does the same as `just run` (Windows: `powershell -ExecutionPolicy Bypass -File scripts\build.ps1 run`).

Your build already holds the game, so there's no import screen. The first start asks how you want to play ([The first start](play/README.md#the-first-start)), and then you're in. [Controls](play/controls.md) has the keys and buttons.

Saves and settings go in a `data` folder next to what you built: `dist/mac/data`, or inside the Windows or Linux folder. A rebuild and `just clean` both leave it alone. [Saves](play/saves.md#where-your-saves-are) has the rest.

To update later, `git pull` and build again.

## Steam Deck

Build on another computer with `just build linux` (it needs Docker), then copy the `.tar.gz` from `dist/linux/` to the Deck and follow [Steam Deck](play/linux-and-steam-deck.md#steam-deck). That's a guide written from the Linux build; the checks on a real Deck are still open ([#84](https://github.com/armaatus/maplethief-port/issues/84)).

## iOS

Not yet. `just build ios` stops and points at [#83](https://github.com/armaatus/maplethief-port/issues/83), which tracks the Xcode project and the sideload steps.

## When doctor complains

| The line | What to do |
|---|---|
| `Python` | Install 3.10 or later. If the right one isn't first on your `PATH`, set `PYTHON=/path/to/python3` |
| `CMake`, `Ninja` | macOS: `brew install cmake ninja`. Windows: they come with Visual Studio's C++ workload. **Linux:** these two show as a `note`, not a failure: `just build` builds in a container and doesn't need them |
| `Apple clang` | `xcode-select --install` |
| `Visual Studio` | Install Visual Studio 2022 with **Desktop development with C++** |
| `Docker` | Needed for a Linux build on any system, and for `just shaders`. On a Mac it's OrbStack or Docker Desktop. It's only a note unless you're on Linux |
| `The IPA` | Check the file against [Your game file](play/your-ipa.md), or use `--ipa` |
| `Disk space` | Free up room (it wants 3 GB) |
| `Compiler (only for a build by hand)`, `uv` | Notes only. You don't need them for `just build` |

If the build itself stops, its message says why and what to do. Wrong-file messages are listed in [Build from your own copy](build/README.md#build-from-your-own-copy-wherever-it-is). If the game builds but won't start, see [Troubleshooting](play/troubleshooting.md). Things we know are open are in [Known issues](KNOWN_ISSUES.md).

## Going further

- [Build from source](build/README.md): every step by hand, the presets, debug builds, the macOS app in detail.
- [Running and options](build/running.md): every command-line option and setting.
- [Testing and CI](build/testing.md): running the tests.
