# Linux and Steam Deck hand-offs

The hand-offs that need a Steam Deck (or a Linux PC for #264). One player tarball, built on the Mac from `dev`, covers all of them. Release v0.1.0 predates the data folder, so it is only used to make older saves for #264. [How to report](README.md#how-to-report) says what to post.

## Setup

1. On the Mac: Docker (OrbStack) running, your copy at `original/MapleThief-1.5.ipa`, then `scripts/bootstrap && python3 -m recomp all`.
2. `scripts/linux-game player`: it builds `build/linux-game/MapleThief-0.0.1-linux-x86_64.tar.gz` in the container (Ubuntu 24.04, about 5 minutes). It holds no game; the first start imports your IPA. Optional: `scripts/linux-game build` for the tarball with the game inside, and `scripts/linux-game check` to start it on a plain Ubuntu.
3. A way to copy files to the Deck (a USB stick or the network), Desktop Mode and Konsole on the Deck.
4. For #264's older saves: `gh release download v0.1.0 -p "MapleThief-0.1.0-linux-x86_64.tar.gz"`.

[Linux and Steam Deck](../../play/linux-and-steam-deck.md) is the player page for the same steps.

## #264: Data folder sign-off (Linux part)

Covers the Linux part of [#264](https://github.com/armaatus/maplethief-port/issues/264); [Mac](mac.md#264-data-folder-sign-off-mac-part) and [Windows](windows.md#264-data-folder-sign-off-windows-part) have the others. The comment's `epic-259-data-folder` is merged: the tarball above is from `dev`. On the Deck in Desktop Mode (or a Linux PC), run `./MapleThief.sh --print-data-dir` before and after each step.

1. Older saves: move `~/.local/share/MapleThief` aside if it exists, unpack the v0.1.0 tarball, import, save a hero, quit. That leaves saves in `~/.local/share/MapleThief`.
2. Unpack the new tarball into `~/Games/` and start `./MapleThief.sh`: the import, then the older saves are copied into `data/` with a notice for 20 s; the old folder is left as it was.
3. Move `~/.local/share/MapleThief` aside again, unpack a fresh copy, import, play, save, change a setting and a binding: only `data/` and `game/` in the folder change, and `~/.local/share/MapleThief` isn't created.
4. Copy the whole folder elsewhere (a USB stick, another machine) and start it: the hero, settings and bindings come along.
5. Delete `data/saves/`: empty slots, settings kept. Delete `data/`: a first start.
6. On the title, Settings > Saves > **Data location** > User folder: after the restart the hero is there and `~/.local/share/MapleThief` is used. Switch back: the same.
7. Optional (the Windows Program Files step has no Linux twin): unpacked in a folder you can't write, `reason: fallback-not-writable`.

**Post back:** one Linux comment with each step's result, the `--print-data-dir` outputs, and the Saves tab's text.

## #84: The Steam Deck build

Closes [#84](https://github.com/armaatus/maplethief-port/issues/84) (its Deck check; the build merged in #135). Uses the folder from #264 step 3, which already has the game imported. The comment's save path `~/.local/share/MapleThief/` is out of date: saves are in the game folder's `data/` now.

1. In Desktop Mode: `./MapleThief.sh --install-desktop`, then in Steam *Add a Non-Steam Game* > MapleThief. Properties > Compatibility: leave the tool **off**.
2. In Game Mode: it starts, with no `GLIBC_2.xx not found`.
3. The picture is integer x2 with bars, not blurred by gamescope. Also try the launch option `--scale-mode sharp`.
4. With Steam's Gamepad layout: A jumps, X attacks, the menu button opens the popup menu, the view button the map.
5. The touchscreen taps the game; the key pad shows on the first touch.
6. Sound plays.
7. A save survives a restart (in the folder's `data/saves/`).
8. If the window stays 960 x 640: set the launch option `env > ~/env.txt; %command%` and send the gamescope variables from that file.

**Post back:** pass or fail for steps 2 to 7, the SteamOS version, and step 8's variables if needed.

## #185: Gamepad pointer on the Deck's trackpad

Covers the Deck half of [#185](https://github.com/armaatus/maplethief-port/issues/185); the DualSense touchpad half needs a DualSense on the Mac or PC (any time, same steps with the pad's touchpad). In Game Mode with the Gamepad layout:

1. Launch options: `SDL_LOGGING=input=debug %command% --pad-pointer both --fast-forward x2 --language ko` (Korean gives Help two pages).
2. Does the right trackpad arrive as an SDL touchpad? If not, note what the Gamepad layout maps it to, and whether "Gamepad with Mouse Trackpad" or a custom "As touchpad" changes that.
3. Tap About's spot on the main menu with the right trackpad: About opens. On Help, a right to left drag turns the page.
4. Move the right stick: an arrow shows. Hold R3 for 0.3 s: it taps at the arrow. A short R3 fast-forwards.

**Post back:** the result per step and the `input: pad pointer tap at (x, y)` lines from the newest log in the folder's `data/logs/`.

## #142: Performance (Steam Deck part)

Covers the Deck part of [#142](https://github.com/armaatus/maplethief-port/issues/142); see the [Mac section](mac.md#142-performance-under-rosetta-mac-part) for the criterion (at least 19.5 ticks a second, p99 under 75 ms).

1. Launch option `%command% --perf-session`, at default settings.
2. Load a hero, stay on a map for 5 minutes or more, then Quit from the settings menu.
3. In the newest log in the folder's `data/logs/`, read the `game: ticks:` and `game: clock:` lines.

**Post back:** those two lines.
