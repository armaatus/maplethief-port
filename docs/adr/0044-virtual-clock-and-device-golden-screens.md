# 44. A virtual clock for scripted runs, and golden screens against the device

- **Status:** Accepted
- **Date:** 2026-10-29
- **Issue:** #60
- **Builds on:** [ADR 0005](0005-test-harness.md) (golden images, `MAPLETHIEF_UPDATE_GOLDEN`, `work/golden/`), [ADR 0010](0010-asset-pipeline-and-viewers.md) (the standard-library PNG codec), [ADR 0033](0033-gcd-worker-threads-and-main-queue.md) (GCD's clock and sleep hooks), [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (the screen, `screen_from_window`), [ADR 0042](0042-frontend-game-loop-and-presentation.md) (the frontend, `--tap`, `--capture`)

## Context

#60 compares our frames of six screens (title, main menu, character select, first map, a popup, an NPC dialog) with screenshots Joris takes on his device, and the epic's exit criterion asks for the title, the main menu and the first map to match at 480 × 320 within a tolerance. Two things stand in the way:

- **Reaching a screen reproducibly.** `--tap T,X,Y` (ADR 0042) taps at T seconds of the run loop's clock, which is the host's wall clock. Two runs reach a screen after different numbers of ticks, so they show different frames. The game's state follows its clock closely ([re/timing-threading.md](../re/timing-threading.md)): every timer reads `GetTimeMs` (`gettimeofday`), the tick fires when 50 ms of it have passed, and **the random seed is the clock at boot**. UIKit's animations (the main menu's slide-in) follow `CFAbsoluteTimeGetCurrent`, GCD's timers its own monotonic clock.
- **The screenshots themselves.** They are copyrighted (they live in `work/golden/`, never committed), they do not exist yet, and they come in whatever form the device saves: landscape or portrait, 1x or Retina 2x, maybe letterboxed on a 4-inch device. Text is drawn with another font here (NanumGothic for MarkerFelt-Thin and Helvetica, #118), and animated parts of a screen are caught at another moment.

Where the guest reads the time, all through runtime seams that already exist: `mt_libc_config.clock` (Unix microseconds) is behind `gettimeofday` and `mt_cf_absolute_time` (so `CFAbsoluteTimeGetCurrent`, the run loop and UIKit's default clock); `mt_gcd_config.clock` and `sleep_until` are GCD's. Nothing else in the runtime reads a clock the guest can see (the GCD wall-clock branch of `dispatch_time` is for `DISPATCH_WALLTIME`, which the game does not import; `mt_gfx`'s and the frontend's own timings are statistics).

## Options considered

**Reproducible runs.**

1. **Save states**: snapshot guest memory and the runtime's host state at a screen. Pro: any state, instantly. Con: the runtime keeps host-side state (objects, run loop, GCD, UIKit views, textures) that a snapshot would have to capture; a large piece of work, and the snapshot would still have to be reached once.
2. **A virtual clock, advanced by the host (fixed step).** One counter behind every guest clock; the turn hook advances it by a fixed step per run-loop turn instead of waiting, and taps and shots are scheduled in its time. Pro: small (the seams exist), exact, faster than real time with a Release build; the analysis of #21 recommends it for replays and goldens. Con: the run does not reproduce the device's timing inside a turn: time stands still while the guest computes (a load takes no time, a slow tick does not delay the next).
3. **Input replay on the real clock** with waits for screen conditions. Con: ticks still drift with the host's speed, so frames differ; and the seed differs per run.

**Where the comparison lives.**

1. **Python, on the standard-library PNG codec** (`mtport.png`, ADR 0010), driving `MapleThief` as a subprocess. Pro: the normalisation and masks are easy to test with synthetic images in CI; the harness drives the real program as a user would, with no new executable linking the generated code. Con: a second implementation of golden.h's rule (kept to the same rule and diff colours).
2. **C++ on golden.h**, a new real-asset test executable linking `maplethief_gen` and the frontend. Con: another large link of the generated code, and the normalisation code would live in a test library.

## Decision

- **The fixed-step mode: `MapleThief --fixed-step MS`** (`platform/src/clock.{h,cpp}`, `VirtualClock`).
  - `install()` points `mt_libc_config.clock` and `mt_gcd_config.clock`/`sleep_until` at one atomic counter of virtual nanoseconds, before the process loads. Unix time is 2025-01-01T00:00:00Z plus it; GCD time is 10⁹ plus it.
  - The start puts the game's 32-bit `GetTimeMs` at 522,812,416: positive, in [0, 2³⁰), 18.8 days before its 2³¹ jump (which would stop the ticks). The random seed is the same on every run.
  - The turn hook advances the clock by MS after every run-loop turn and never waits. Inside a turn the time stands still. GCD's `sleep_until` (a wait on the main thread) jumps the clock to the deadline.
  - The step is 1 ms in the screen list: the game's `CFRunLoopTimer` is re-armed every 1 ms, so the tick gate sees every millisecond, and a 50 ms tick comes every 50 turns.
  - What makes a present in this mode depends only on virtual time: SDL window events do not, and a running UIKit animation presents at most once per 1/60 virtual second (else every 1 ms turn of the main menu's slide-in would composite).
  - **`--shot T,NAME`** writes the composited 480 × 320 screen (`Screen::compose`, what the device's screen shows: no window, no scaling) at the first turn at or after T to `<--capture DIR>/NAME.png`. The run ends after the last shot; an unwritten shot is exit status 3.
  - `--tap` is unchanged: its times are run-loop times, so in this mode they are virtual.
- **The screen list `tests/golden/device_screens.json`** (our own data): per screen its name, its run and shot time, what it shows, the steps on the device, and its masks; per run its taps and where its data directory comes from (`empty`, or the data directory an earlier run left: the character select is a second launch on the fresh run's save); three tolerances. `tests/golden/README.md` documents the workflow and the capture steps.
- **The harness `python3 -m mtport.devicegolden`** (`tools/mtport/devicegolden.py`), registered as CTest `platform.device_golden` (`real_assets`, only with generated code, skipped with 77 when the game cannot start).
  - It runs each run headless with a data directory of its own, then compares each shot with **our recorded render** `work/golden/ours/<name>.png` (exact: channel 0, ratio 0) and with **the device screenshot** `work/golden/device/<name>.png`.
  - A missing golden skips its case with a message ("not captured yet"). `MAPLETHIEF_UPDATE_GOLDEN=1` (or `--update`) records our renders, never device screenshots.
  - Output goes to `build/test-output/<preset>/device_golden/`: `<name>.actual.png`, the normalised `<name>.device.png`, `<name>.device.diff.png` (always) and `<name>.ours.diff.png` (on a mismatch), and `runs/<run>/` with the log, shots and data directory. `--repeat N` fails unless N runs give identical shots; `--arch x86_64` runs the universal binary under Rosetta.
- **The comparison `mtport.goldencmp`** (also a CLI for one pair) keeps golden.h's rule and diff colours (masked pixels dark blue) and adds:
  - **Normalisation** to the 480 × 320 screen: 480 × 320 as is; 320 × 480 turned with ADR 0037's mapping (screen pixel (x, y) = window pixel (y, 479 − x), a quarter turn clockwise); 960 × 640 averaged per 2 × 2 block (Retina devices show the non-Retina game at 2×); 640 × 960 both; 1136 × 640 and 640 × 1136 (4-inch devices) cropped to the middle after checking that the 88-pixel bars are black. Anything else is rejected with the list of accepted sizes. Alpha becomes 255.
  - **Masks**: screen rectangles left out; the differing ratio is taken over the compared (unmasked) pixels.
- **Starting tolerances**, to be set once real screenshots are compared: `device_1x` channel 16, ratio 1 %; `device_2x` channel 32, ratio 5 %. On a Retina device iOS scales the 480 × 320 GL layer up with filtering and draws UIKit text at 2×, so averaging back blurs every edge: a looser tolerance there. Masks cover text (all engine and label text, #118) and the two animated portals of the first map.
- **"Title" is the main menu with its promo banner** after the 2 s slide-in: the game has no title screen of its own ([ANALYSIS.md → Program flow](../ANALYSIS.md#program-flow)).

## Evidence

- **Determinism** (local, real assets, 2026-10-29, rechecked after the merges of #57 and #59):
  - `python3 -m mtport.devicegolden --repeat 2` with the universal Release binary: all six screens identical over two runs.
  - Our renders recorded from that Release arm64 run; the same screens from the Release binary under `arch -x86_64` and from the Debug build (`platform.device_golden`, mac-debug, the bounds-checked memory model) are identical to them: 0 of 153,600 pixels differ on every screen.
  - Speed: the two runs (59 + 5 virtual seconds) take about 25 s in Release and 108 s in Debug on an Apple M4.
- **CI** (synthetic): `tests/test_goldencmp.py` (both portrait directions on an asymmetric image, the ADR 0037 corners, 2 × 2 averaging and its rounding, Retina portrait, the 4-inch crop and its bar check, rejected sizes, the channel and ratio rule, masks and the ratio base, size mismatch, the CLI's exit statuses); `tests/test_devicegolden.py` (the committed list and 13 broken variants, the command lines, and the whole harness against a stand-in program: data carried to the second run, skips, recording, a changed render, a portrait 2× device screenshot with a mask, a bad size, `--repeat`, a game that cannot start, a crash); `platform.clock: …` (doctest: the libc and GCD clocks follow the virtual clock, only advancing moves it, uninstalling restores the host clock, the start's `GetTimeMs`).

## Consequences

- Any screen the scripts reach can get a golden of our own today, so renderer regressions in these screens fail `platform.device_golden` before the device screenshots exist. A deliberate rendering change (e.g. `GL_LINES`, #121, which draws the NPC dialog's frame) is followed by `MAPLETHIEF_UPDATE_GOLDEN=1` and a look at the new renders.
- The device comparisons wait for Joris's screenshots (#60 stays `needs-human`); the tolerances and masks are then set from the first real diffs.
- The scripts depend on the game's own timing in virtual time (the intro's lines appear about 22 s after Play). A change in how the runtime runs the guest that shifts ticks moves the screens; the ours goldens then show it.
- The fixed-step mode is not paced: in a real window it runs as fast as the host can. It is a test mode.
- How we'd know this was wrong: a run that is not identical twice (`--repeat`), between Debug and Release, or between arm64 and x86_64 — then something outside the virtual clock reaches the guest (a host pointer order, a thread race, an uninitialised read); or a screen the device shows that time standing still inside a turn cannot reach (then the clock advances per guest clock read instead).
