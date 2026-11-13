# Nintendo Switch hand-offs

The Switch epic (#233) is merged into `dev`. Its hardware sign-off is one session on the console, written up in the [Switch sign-off checklist](../switch-checklist.md); this page does not repeat it. It says which rows sign off which issue, and adds what the checklist doesn't cover: the hello NRO (#235), the runtime's test NRO (#236), the GPU comparison (#242), a desktop comparison (#238), the emulator decision (#237) and the tuning issue (#279). [How to report](README.md#how-to-report) says what to post.

#234 (ADR 0138) is done: accepted on 2026-11-10 and closed.

## Setup

Everything under *Before you start* in the [checklist](../switch-checklist.md#before-you-start): the console with the Homebrew Menu, the dock and TV, both Joy-Con and a Pro Controller, the Mac on the same network with Docker running and TCP 28771 open, your IPA, a desktop save zip for G4. Also a PNG of your own (for G1), and stock clocks (no overclocking tool).

The builds, on the Mac from `dev`, in this order (the hello build first, as the last line overwrites its NRO):

```sh
scripts/bootstrap && python3 -m recomp all
scripts/build-switch HEAD --preset switch-release
#   -> build/switch-release/MapleThief.nro: the hello NRO (#235), and build/switch-release/tests/*.nro (#236)
MAPLETHIEF_VERSION=0.9.245-rc.1 scripts/build-switch --preset switch-player --gen work/gen
#   -> build/switch-player/MapleThief-0.9.245-rc.1-switch.zip and build/switch-player/MapleThief.nro (the game)
# after #235 and #236, the game in the same place (row F1 and #242):
scripts/build-switch HEAD --preset switch-release --gen work/gen
```

`just build switch` builds the same game NRO into `dist/switch/` when your IPA is in `original/`, and the hello NRO without it. Below, `<IP>` is the address the Homebrew Menu's NetLoader (Y) shows; `export MAPLETHIEF_SWITCH_IP=<IP>` saves typing it.

## #235: The hello NRO on the console

Closes [#235](https://github.com/armaatus/maplethief-port/issues/235)'s "Hello NRO on hardware". Not covered by the checklist (I3 and F1 run the game, not the hello screen). Before the checklist's I2, which renames the folder:

1. `PYTHONPATH=tools python3 -B -m mtport.nro info build/switch-release/MapleThief.nro`
2. Copy it to `sdmc:/switch/maplethief/MapleThief.nro`. The Homebrew Menu shows the icon, the name MapleThief, the author armaatus and version 0.0.1.
3. Start it: a text screen with `MapleThief <version>` and "Press + to return to the Homebrew Menu". + returns with no error.
4. Y (NetLoader), then on the Mac `scripts/switch-run --ip <IP> build/switch-release/MapleThief.nro`: the terminal prints `MapleThief <version>`.

**Post back:** what each step showed, the system version and the Homebrew Menu version.

## #236: The runtime's test NRO

Closes [#236](https://github.com/armaatus/maplethief-port/issues/236) (its three hardware criteria). Listed under the checklist's [extra runs](../switch-checklist.md#extra-runs-from-the-children-optional-same-session).

1. Homebrew Menu from the Album (applet mode), Y, then `scripts/switch-test --ip <IP>`: `== MapleThiefTests: PASS (... applet mode ...)`.
2. Title takeover (hold R while starting a game), Y, the same command: `title takeover (Application)`.
3. Y, then `scripts/switch-test --ip <IP> --crash-test`: `== crash test: PASS`, a backtrace with `CGame::Update+0x42`, and back to the Homebrew Menu.

**Post back:** the three logs from `build/test-output/switch/`, plus `sdmc:/switch/maplethief-tests/results.txt` and `sdmc:/switch/maplethief/crash.txt`, attached.

## #243: The player build and the import

Closes [#243](https://github.com/armaatus/maplethief-port/issues/243): checklist rows [I1 to I6](../switch-checklist.md#i-installing-from-the-zip-and-the-import). The comment's version 0.9.243-rc.1 or the checklist's 0.9.245-rc.1 both work.

**Post back:** on #245, with the checklist.

## #240: Lifecycle, audio and memory

Closes [#240](https://github.com/armaatus/maplethief-port/issues/240): rows [M1 and M2](../switch-checklist.md#m-applet-mode-and-title-takeover), [L1 to L5](../switch-checklist.md#l-home-sleep-and-a-quit-and-restart-that-keeps-the-save) and [P1](../switch-checklist.md#p-thirty-minutes-of-play). Set `autosave = on_quit` before L1.

**Post back:** on #245, and copy M1's `memory: the run's peak (applet mode): ...` line to #240 as well (its criterion asks for it there; it sets the applet margin).

## #239: Controllers and touch

Closes [#239](https://github.com/armaatus/maplethief-port/issues/239): rows [C1 to C8](../switch-checklist.md#c-controllers-and-touch). Ignore the comment's `just build switch`.

**Post back:** on #245, with the checklist.

## #238: The game to the first map

Closes [#238](https://github.com/armaatus/maplethief-port/issues/238): rows I5, [D1 to D3](../switch-checklist.md#d-handheld-and-docked), M1 and M2. Its comment's expected lines are out of date: the `gfx:` line now says the GPU (deko3d) renders, and a clean run prints no `unimplemented report` line (the checklist has the current text). Its emulator criterion waits on #237. One check isn't in the checklist:

1. Take console screenshots (the Capture button) of the title, the main menu, character creation and the first map.
2. On the Mac, compare them with the desktop build's frames (F12, or the `--shot` runs of [the first map run](../first-map-run.md)).

**Post back:** one line on #238 with the comparison's verdict. No pictures.

## #241: The settings menu

Closes [#241](https://github.com/armaatus/maplethief-port/issues/241): rows [G1 to G5](../switch-checklist.md#g-the-settings-pages). The comment opens the menu with L3+R3; `+` works too now. Optional, for the exact "export, then import on a fresh data folder" criterion: `scripts/switch-run --ip <IP> build/switch-release/tests/platform_test.nro -- -tc="*export then import on a fresh data folder*"` (any of the builds above has the test NROs).

**Post back:** on #245, with the checklist.

## #242: The GPU renderer

Closes [#242](https://github.com/armaatus/maplethief-port/issues/242). Row D3 and F1's `gfx: GPU time per frame` line cover part of it; sections 1 to 3 of its 2026-11-11 comment are extra. Do them before F1. Use `NRO=build/switch-release/MapleThief.nro` from the last build of the setup: the comment's `scripts/build-switch --preset switch-release` has no `--gen`, so it builds the hello NRO. "Software stays the default" is superseded: the GPU stayed the default on #244 (2026-11-11), so its section 3 checks setting software instead.

1. Prototype scene: `scripts/switch-run "$NRO" -- --gfx-prototype --gfx-backend gpu --smoke-frames 120`, then the same without `--gfx-backend gpu`: no `MISMATCH`.
2. The first map, software against GPU, as the comment's section 2 says (move only `sdmc:/switch/maplethief/data/saves/` aside before each run, keep `settings.ini`; copy the shots to `build/shots/` for the compare).
3. Filters handheld and docked: `--filter xbr`, `--filter crt`, `--filter lcd`, `--colorblind-filter protanopia`.
4. GPU time: `--gfx-gpu-timing --frames 3600`, at render scale 1 and with `--render-scale 4`.

**Post back:** the probe lines, the compare output, the filter results and the two GPU time lines.

## #244: Performance

Closes [#244](https://github.com/armaatus/maplethief-port/issues/244): row [F1](../switch-checklist.md#f1-the-logged-session-handheld-and-docked) (F2 optional), after the last build of the setup. [Switch performance](../switch-performance.md) has the details. The renderer decision is made (the GPU stays).

**Post back:** on #245, with the checklist (`perfsession: verdict: PASS` handheld and docked).

## #245: The sign-off

Closes [#245](https://github.com/armaatus/maplethief-port/issues/245): the whole [checklist](../switch-checklist.md), reported as its [How to report](../switch-checklist.md#how-to-report) says. It is also the Switch's row of #92.

**Post back:** one comment with the tick sheet, the logs named by row, the console model and versions, the zip version and the renderer.

## #279: Profile and tune

[#279](https://github.com/armaatus/maplethief-port/issues/279) waits on F1. If both screens pass with headroom (the busiest second under about 70 %), the agent closes it; otherwise the agent prepares builds with other optimisation flags and you run `scripts/switch-perf` on each.

**Post back:** nothing now; later, the `perfsession:` lines of each build the agent asks for.

## #237: The emulator decision

[#237](https://github.com/armaatus/maplethief-port/issues/237) needs a decision on the Mac, no console. On 2026-11-11 you chose the installed Astris only, and the agent then waited for its Gatekeeper approval. Since then an Astris (1.0.32) run reached the first map (PR #280's notes), but nothing is recorded on #237.

1. Confirm that Astris is approved and runs, and its version.
2. Choose: (a) the Astris-only comparison, and the agent records the 2026-11-11 run and decides on a smoke script around it; (b) allow the official downloads of the other emulators the issue names; or (c) close it, with an ADR 0138 addendum saying the console is the only check.

**Post back:** one comment with the confirmation and the choice. Nothing about keys or firmware.
