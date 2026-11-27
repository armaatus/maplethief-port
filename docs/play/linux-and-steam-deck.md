# Linux and Steam Deck

How to build the game for Linux x86_64 (or aarch64) from your own IPA and play it on a Linux PC or a Steam Deck as a non-Steam game. The decisions behind it are in [ADR 0065](../adr/0065-linux-and-steam-deck-build.md) (#84). If you only want to play, the release's player download is easier: [Download and play](README.md#linux-and-steam-deck).

Three different things are called "the Linux build":

| | What | Where it comes from | Shared? |
|---|---|---|---|
| **Player download** | `MapleThief-<version>-linux-x86_64.tar.gz` (on ARM64: `-linux-aarch64`): the game's program without any of its files; it imports your IPA on first launch ([ADR 0125](../adr/0125-player-builds-icon-guard-modes-and-release-flow.md)) | `scripts/linux-game player`, released by `scripts/release` | Yes, it is a release asset |
| **Port kit** | `MapleThief-kit-<version>-linux-x86_64.tar.gz` (on ARM64: `-linux-aarch64`): the platform shell (no game code), the runtime, the tools ([ADR 0024](../adr/0024-release-contents-and-flow.md)) | `cmake --workflow --preset linux-release`, released by `scripts/release` | Yes, it is a release asset |
| **Game folder** | `MapleThief-game-<version>-linux-x86_64/` and its `.tar.gz`: the game, built from your IPA | `scripts/linux-game build`, into `build/linux-game/` | **No.** It contains the game. Keep it to your own machines |

The player download and the game folder both have `MapleThief.sh`, `--install-desktop` and `--uninstall-desktop`; the launcher uses the folder's `game/` copy when there is one. The [Steam Deck](#steam-deck) steps below use the player download. The build sections are about the game folder.

## Build it

You need the extracted bundle and the generated code ([Build from source](../build/README.md)):

```sh
scripts/bootstrap          # work/ipa/
python3 -m recomp all      # work/gen/
scripts/linux-game build   # build/linux-game/MapleThief-game-0.0.1-linux-x86_64{/,.tar.gz}
```

`scripts/linux-game build` runs in Docker by default, on a Mac (OrbStack or Docker Desktop) or on Linux. It uses the image of [`tools/containers/linux-game/`](../../tools/containers/linux-game/Dockerfile): Ubuntu 24.04 with GCC 14, pinned like the CI image, so the result runs on **glibc 2.39 or newer**. On Apple silicon it runs x86_64 under Rosetta and takes about five minutes. It exports a clean copy of `HEAD` (`--worktree` includes your uncommitted changes to tracked files) and mounts `work/` read-only. Then it configures `linux-release` with the generated code, builds `MapleThief`, and runs `platform.linux_portable` (NEEDED is only glibc, no symbol newer than `GLIBC_2.39`), `platform.version` and `platform.smoke` before it stages the folder.

On a Linux machine, `scripts/linux-game build --native` uses your own compiler and checkout instead (GCC 13+ or Clang 17+, the packages in [Build from source](../build/README.md)). The result then needs your machine's glibc or newer.

`MAPLETHIEF_VERSION=1.2.3 scripts/linux-game build` names the build. The default is `0.0.1`.

### What is in the folder

```
MapleThief-game-<version>-linux-x86_64/
  MapleThief.sh          the launcher: run this
  bin/MapleThief         the game (libstdc++ and SDL3 linked in; needs only glibc)
  game/MapleThief.app/   a copy of your extracted bundle
  icon.png               the game's icon, for the menu entry and Steam
  README.txt
```

The tarball has the same folder, with fixed timestamps and owner, so building twice from the same inputs gives the same file.

### Check it on a plain distribution

```sh
scripts/linux-game check                       # the newest build/linux-game/*.tar.gz
scripts/linux-game check --image debian:13     # any other image
```

This unpacks the tarball and runs it in a stock `ubuntu:24.04` image (pinned by digest) with no package installed. First `MapleThief.sh --version`, then the game headless with SDL's dummy video and audio drivers: a tap on Play at 3 s and 150 window frames into the first map. It must exit 0. Its read-backs land in `build/linux-game/check/capture/` (git-ignored, game art: don't commit them).

## Play it on Linux

```sh
tar -xzf MapleThief-game-0.0.1-linux-x86_64.tar.gz
cd MapleThief-game-0.0.1-linux-x86_64
./MapleThief.sh                     # every MapleThief option works, e.g. --scale-mode sharp
./MapleThief.sh --install-desktop   # adds MapleThief to your application menu
./MapleThief.sh --uninstall-desktop # removes it from the menu again
```

Saves and `settings.ini` live in `data/` in the game folder, next to `MapleThief.sh` ([Saves](saves.md#where-your-saves-are)), or in `~/.local/share/MapleThief/` if you chose the user folder or the folder can't be written. The keyboard, mouse and gamepad controls are the usual ones ([Controls](controls.md)). SDL3 picks Wayland or X11, and PipeWire, PulseAudio or ALSA, by itself. `SDL_VIDEO_DRIVER=x11 ./MapleThief.sh` forces X11 if Wayland gives you trouble.

## Steam Deck

The Deck runs MapleThief natively, as a non-Steam game. No Proton is involved. These steps use the player download; a game folder you built yourself works the same way, with its own folder name.

1. **Download** `MapleThief-<version>-linux-x86_64.tar.gz` in Desktop Mode (Steam button → Power → Switch to Desktop), from the [Releases page](https://github.com/armaatus/maplethief-port/releases) or the [itch.io page](https://armaatus.itch.io/maplethief). Copy your `MapleThief-1.5.ipa` to the Deck too, for example into `~/Downloads`, over the network or on a USB stick.
2. **Unpack it**, for example to `~/Games/`: right-click → Extract in Dolphin, or in Konsole `mkdir -p ~/Games && tar -xzf ~/Downloads/MapleThief-<version>-linux-x86_64.tar.gz -C ~/Games`. You get the folder `~/Games/MapleThief-<version>-linux-x86_64/`.
3. **Import your game file**, still in Desktop Mode: run `MapleThief.sh` in that folder (double-click it in Dolphin, or `~/Games/MapleThief-<version>-linux-x86_64/MapleThief.sh` in Konsole), choose your `MapleThief-1.5.ipa` in the file dialog, and close the game once it reaches the title screen (or run it once with `--import-ipa ~/Downloads/MapleThief-1.5.ipa`). The game is now in the folder's `game/`. Game Mode has no file dialog, so do this step here.
4. **Menu entry**: in Konsole, `~/Games/MapleThief-<version>-linux-x86_64/MapleThief.sh --install-desktop`.
5. **Add it to Steam**: Steam → Games → *Add a Non-Steam Game to My Library…* → tick **MapleThief**. If it isn't listed, use *Browse…* and pick `MapleThief.sh`. The shortcut then shows the game's icon.
6. In the shortcut's **Properties → Compatibility**, leave *Force the use of a specific Steam Play compatibility tool* **off**. It's a native Linux program.
7. Back in **Game Mode**, start MapleThief from the library.

Your saves and settings are in `data/` in that folder. To update, unpack the new version next to the old one, move `data/` and `game/` into it, and point the Steam shortcut at the new `MapleThief.sh` (or run `--install-desktop` again from the new folder and add it again). Removing it: [Uninstall and reset](uninstall-and-reset.md#linux-and-steam-deck).

### Screen

The Deck's screen is 1280 × 800. In Game Mode Steam runs the game under gamescope, and `MapleThief.sh` then opens a 1280 × 800 window. That way the game's own scaling fills the screen and gamescope doesn't scale it a second time. Pick the scaling in the settings menu or in `data/settings.ini` in the game's folder, or as a launch option (Properties → Launch Options, e.g. `--scale-mode sharp`):

| `scale_mode` | On 1280 × 800 |
|---|---|
| `integer` (default) | ×2: 960 × 640, crisp pixels, bars of 160 px left and right and 80 px top and bottom |
| `sharp` | ×2.5: 1200 × 800, sharp bilinear, bars of 40 px left and right |
| `fit` / `smooth` | ×2.5 like `sharp`, nearest / bilinear |

`MAPLETHIEF_GAMESCOPE_WINDOW=WxH` sets another window size, for an external screen, and an explicit `--window` always wins. On a desktop (Desktop Mode or any Linux PC) none of this applies: the window is 960 × 640 as usual.

### Controls

Steam Input hands the Deck's controls to non-Steam games as an Xbox-style gamepad, which SDL3 opens like any other pad. The game's gamepad bindings ([ADR 0048](../adr/0048-gamepad-actions-and-delivery.md), [Controls](controls.md)) apply as they are with Steam's **Gamepad** layout:

| Deck | In the field | In menus and popups |
|---|---|---|
| D-pad, left stick | move (diagonals too) | move the focus |
| A | jump | OK |
| B | quick slot 2 (Blue Potion) | back |
| X, R2 | attack | revive (death popup) |
| Y | quick slot 1 (Red Potion) | revive (death popup) |
| L1 / R1 | quick slots 3 / 4 | minus / plus (tabs) |
| L2 | cash shop | |
| ☰ (Menu) | popup menu | back |
| ⧉ (View) | map | |

The back grips (L4, L5, R4, R5) have no binding by default. In the controller settings you can give them the face buttons, for example R4 = Y and R5 = B for the potions, so that your thumb stays on the stick. The **touchscreen** is the iPhone's screen: touches reach the game as fingers ([ADR 0047](../adr/0047-touch-input-mouse-and-fingers.md)), so the on-screen key pad works as on the iPhone and shows again on the first touch (`hide_keypad = auto`, [ADR 0052](../adr/0052-hiding-the-on-screen-key-pad.md)). With a mouse-trackpad layout, the right trackpad's click is the left mouse button, which is a touch too.

### If it doesn't start

- Start it from Konsole in Desktop Mode (`./MapleThief.sh`) to see its log.
- ``version `GLIBC_2.xx' not found``: the system's glibc is older than 2.39. Update SteamOS (or the distribution).
- No sound or a black window: try `SDL_AUDIO_DRIVER=pulseaudio` or `SDL_VIDEO_DRIVER=x11` as launch options, written as `SDL_VIDEO_DRIVER=x11 %command%`.

## Not done yet

- **The Deck itself** hasn't been checked yet (#84, `needs-human`). That covers Game Mode's window, the default controller layout, the touchscreen and the glibc of current SteamOS.
- No AppImage and no Flatpak ([ADR 0065](../adr/0065-linux-and-steam-deck-build.md)).
- aarch64 (ARM64): the release has a player download and a kit (`-linux-aarch64`, [ADR 0194](../adr/0194-windows-arm64-and-linux-aarch64-builds.md)), and `--arch arm64` builds an aarch64 game folder in the same way. The test suite runs in the arm64 container and on GitHub's ARM64 runner; the build hasn't been played on ARM64 hardware yet (an ARM64 Linux laptop or board). The Steam Deck is x86_64.
