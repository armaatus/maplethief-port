# Switch performance: the logged session

[#244](https://github.com/armaatus/maplethief-port/issues/244), [ADR 0145](../adr/0145-switch-performance-session-and-defaults.md). Numbers only: no screenshots.

The criterion: at default settings on a stock-clocked Switch, on a map, the game must hold **20.00 ticks a second** with **at least 99 % of frames held 3 vblanks** over five minutes, both handheld and docked. Fast-forward and the busiest scene must be logged too.

## How to run it

```sh
scripts/build-switch HEAD --preset switch-release   # needs work/gen (scripts/bootstrap)
export MAPLETHIEF_SWITCH_IP=<the address the Homebrew Menu's NetLoader (Y) shows>
scripts/switch-perf
```

The game starts with `--perf-session --fast-forward x2 --gfx-gpu-timing` and logs a `perf:` line every second. When it quits, `python3 -m mtport.perfsession judge` reads the log and prints the `perfsession:` lines and the verdict. To judge a kept log again, run `scripts/switch-perf --judge build/test-output/switch/MapleThief-perf-<time>.log`. A desktop run takes `--perf-session` as well, and is judged with `--displays desktop`.

Each `perf:` line covers one second of play. It never mixes two screens, two scenes or two game speeds, and a pause such as HOME or sleep ends it. The format is in `platform/src/perf_session.h`.

## Measured on the Mac

Release arm64, Apple M4, a 60 Hz Metal window, `--no-audio`, real time. Every run here shared the machine with a sibling epic's builds (load average 2 to 11), so the times are noisy.

**A session on map 7.4.** The save is a copy of `play.autosave_map_7_4`'s, and the hero walks with the key pad. Fast-forward x2 is held for 12 s. These are the judge's figures for the 56 s on the map:

| Backend | Ticks a second | Held 3 vblanks | Per tick: guest | mt_gfx | compositor | present | other | CPU busy (mean) | Busiest second |
|---|---|---|---|---|---|---|---|---|---|
| GPU (Metal) | 20.00 (1119) | 99.91 % | 0.77 ms | 0.05 ms | 0.88 ms | 0.23 ms | 0.20 ms | 3.9 % (≈ 1.95 ms a tick) | 5.2 % |
| software | 20.00 (1121) | 99.91 % | 0.38 ms | 11.29 ms | 0.55 ms | 0.24 ms | 0.23 ms | 25.8 % (≈ 12.9 ms a tick) | 35.8 % (mt_gfx 16.2 ms) |

Fast-forward x2 ran at 39.97 ticks a second on the GPU and 40.02 on software, against a target of 40.

**`play.mechanics`' save, walking through a scene, 30 s.** The work counts the CPU's time without the waits:

| Run | Work a tick, mean | p99 | mt_gfx a tick |
|---|---|---|---|
| GPU, Metal window | 1.29 ms | 2.91 ms | 0.04 ms |
| software, Metal window | 15.50 ms | 22.49 ms | 14.24 ms |
| software, dummy video, P-cores (two runs) | 14.09 / 14.67 ms | 19.22 / 19.22 ms | 12.28 / 12.81 ms |
| software, dummy video, E-cores (`taskpolicy -b`) | 28.05 ms | 32.02 ms | 26.22 ms |

The E-core run fell to 14.6 ticks a second, partly because background QoS delays the waits.

**Scaled to the console.** This is an assumption, and the console decides. By Geekbench 6 single core, an Apple M4 scores about 3,750 and a Tegra X1 about 317 at 2.0 GHz, so at the Switch's 1020 MHz it would score about 162. That makes the console about **23 times** slower ([ADR 0145](../adr/0145-switch-performance-session-and-defaults.md#evidence) has the sources and caveats).

| Path | Mac, a tick | × 23 | Against the 50 ms tick |
|---|---|---|---|
| software, map 7.4, mean | 12.9 ms | ≈ 300 ms | six ticks' worth: ruled out |
| GPU, map 7.4, mean | 1.95 ms | ≈ 45 ms | fits if the factor is about 17 or less |
| GPU, map 7.4, busiest second | 2.6 ms | ≈ 60 ms | fits if the factor is about 13 or less |

Hence the Switch defaults: `gfx_backend = gpu`, `render_scale = 1`, with `filter = none` and `frame_rate = original` (ADR 0145).

**Compiler flags.** The Switch's Release keeps `-O3` from CMake, with `-mcpu=cortex-a57+crypto` and no LTO. Their speed needs the console, so they are left to [#279](https://github.com/armaatus/maplethief-port/issues/279).

## To be measured on the console

Joris's run (`needs-human` on #244). Paste the `perfsession:` lines here.

| Screen | Seconds on a map | Ticks a second | Held 3 vblanks | CPU busy | Busiest second | Verdict |
|---|---|---|---|---|---|---|
| handheld | | | | | | |
| docked | | | | | | |

| | |
|---|---|
| fast-forward x2 (ticks a second, % of 40) | |
| the GPU's time a frame (`--gfx-gpu-timing`, the exit log) | |
| the software backend, handheld (`-- --gfx-backend software`, optional) | |
