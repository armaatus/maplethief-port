# 87. Game speed and fast-forward: the speed table patched, a rate on the real-time guest clock

- **Status:** Accepted
- **Date:** 2026-10-31
- **Issue:** #151
- **Builds on:** [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (the clocks the guest reads), [ADR 0048](0048-gamepad-actions-and-delivery.md) and [ADR 0054](0054-bindings-file-remap-overlay-and-button-glyphs.md) (actions and bindings), [ADR 0070](0070-settings-registry-and-presets.md) (Assists), [ADR 0071](0071-frame-telemetry-and-frame-time-overlay.md) (the tick telemetry), [ADR 0074](0074-save-files-atomic-writes-sanitising-and-ios-documents.md) (the config clamp), [ADR 0075](0075-display-locked-frame-pacing.md) (the paced clock)

## Context

Grinding is slow at the game's speeds. The game ticks when CMTimer's interval (the i64 at `0x11A478`) has passed on `GetTimeMs`. `CDataMgr::ApplyGameSpeed` `0x37C41` sets that interval from `g_gameSpeedIntervals[config[1]]`: the i32 table `{65, 50, 40, −1}` at `0x108C58` (`__DATA`), indexed by the Speed option's byte of the game's config ([timing-threading.md](../re/timing-threading.md#interval-and-game-speed)). It runs from `LoadConfig` at boot and from the options menu. The logic is fixed-step, so the interval scales the whole game. Only the real-time sub-timers keep their wall-clock length.

#151 asks for two opt-in Assists: the game's three speeds plus two faster ones, and a key that runs the game faster while it is held. Neither may change a run with both off, and neither may write a port-only value into the guest's config file.

Three real-time clocks exist: `VirtualClock` (fixed-step, tests), `PacedClock` (`pacing = display`, ADR 0075) and `LaunchClock` (`pacing = wall`, #155's launch shift on the host's wall clock).

## Options considered

**Game speed.**

1. **Write CMTimer's interval every turn.** The options menu's choice would be overwritten at once, and the boot's `ApplyGameSpeed` would still set the game's own value first.
2. **Hook `ApplyGameSpeed`.** That needs a new hook and regenerated code for what one table write does.
3. **Patch the table.** Write the chosen length into all three entries of `0x108C58` after the process loads and before it runs, so `LoadConfig`'s `ApplyGameSpeed` sets it as it sets the game's own. A live change (the settings menu) patches the table again and writes CMTimer's interval directly, as `SetFrameInterval` `0x21DA1` does (a plain store). **Chosen.** The recompiled `ApplyGameSpeed` reads the table through guest memory (`MT_LD32` at `0x37C60`), and nothing else reads it (`tests/test_timing_real.py`).

The game's own Speed option still works with `game_speed = game`, the neutral value: the table is not touched and the run is the original's, byte for byte. With any other value the option changes nothing, because each of its indexes reads the same length. Its index stays in the config (0–2). So the config file never holds a port-only value, #78's clamp of a bad index is untouched, and `game` brings the option's own speed back. 33 ms and 25 ms are the port-only lengths (30 and 40 ticks a second).

**Fast-forward: where the multiplier goes.** A host-side rate on the guest's clock, so that everything the game times runs faster: the tick gate, the sub-timers and the run loop's timers.

- `PacedClock` (`pacing = display`): the plan gets a `rate`. The wall plan maps guest time = guest0 + rate × (host − host0); `host_when` divides. `set_plan`'s rebase already keeps the guest time continuous at every change, so pressing and releasing the key are two plan changes.
- `LaunchClock` (`pacing = wall`): `set_rate` rebases the same way (guest = base + rate × (source − base source)). The launch shift stays, because it is in the base. GCD keeps the host's clocks there, as before: only Chartboost's request timeout uses them.
- `VirtualClock` (fixed-step): no effect. Its time is not paced, so "faster" means nothing (logged).

**Fast-forward and the display lock.** A tick of interval / rate is rarely a whole number of refreshes (25 ms at 60 Hz is 1.5). Two choices: fall back to the wall clock while the key is held, or run 2 ticks per present on the lock. **Fallback chosen:** `plan_pacing(…, rate > 1)` gives the wall plan × rate (`the wall clock x2 (fast-forward)`), and releasing the key re-plans to the lock. Judder does not matter while fast-forwarding. One catch: the locked plan's correction was against the wall clock since the start. After a hold the guest clock is ahead of it by what the hold gained, so the lock would have held the clock still until the wall clock caught up. The correction is now against a *reference*: the wall clock plus the time fast-forward gained (`Mapping::ref0`, `gained_ns()`). Without fast-forward the reference is the wall clock, and the arithmetic is the old one.

**Fast-forward and presents.** At ×3 the game presents up to 120 GL frames a second (3 × 40 at 25 ms). Each one presented with vsync would hold the game to the refresh rate (measured: ×3 at 25 ms ran at 60 ticks a second on the dummy driver's emulated vsync). While the key is held, a window present comes at most once a refresh (`ff_skip`; 60 Hz if the rate is unknown). A GL frame in between is taken and replaced by the next one, and the turn hook presents the newest at the next refresh. FrameStats still counts every GL present as a tick.

**Fast-forward and the turn hook's wait.** On the wall plan the hook sleeps until the run loop's next timer. That is a guest time, so the host sleep is divided by the rate (the game's 1 ms poll stays 1 guest ms). A sleep's overshoot then counts rate times over on the guest's clock, which is a late poll of the game's gate and a slower tick. While the key is held, sub-2 ms waits use `SDL_DelayPrecise`, which spins its last stretch, instead of `SDL_DelayNS`. Without fast-forward the wait is unchanged.

**Audio.** The mixer runs on the host's clock and reads no guest time (`runtime/src/audio/`). At ×2 the music keeps its pitch and tempo, and effects come as often as the game plays them. **Pitch kept, not muted:** muting would hide useful cues (hits, level-ups), and resampling the music to the rate has no point while grinding.

**The key.** Keys are bindings (ADR 0048/0054), so the key is an action: `MT_ACTION_FAST_FORWARD` in the runtime's table (`fast_forward`, "Fast-forward"), the first of the port's own actions (`MT_ACTION_FIRST_PORT`). `mt_input_press` ignores the port's actions, and `Actions` never sends them to its sink. It holds them, and the frontend asks `holding()` every turn. That gives the bindings file, the remap overlay, the glyphs and `--save-bindings` for free. The defaults are Tab and the right stick's click (R3), in the field and in menus. R3 is also half of L3 + R3 (the overlay) and R3 + Back (a screenshot), both of which still work. An unbound `fast_forward` is not warned about, since the port's actions are optional.

**Telemetry.** FrameStats' overrun threshold follows the tick on the host's clock: 1.1 × interval / rate (`overrun_ns_for`; 55 ms at 50 ms, as before). The exit `game: clock:` line compares the ticks a second with what the run's fast-forward makes of the interval: the interval's rate × (host + gained) / host. It also gives the time held and the game time gained, and the guest clock's distance from the wall clock less that gain.

## Decision

- **Settings** (Assists, ADR 0070; `Applies::kAtOnce`; in every preset column with the neutral value):
  - `game_speed = game | 65 | 50 | 40 | 33 | 25` (`--game-speed`; neutral `game`, not ADR 0070's planned "50 ms", because 50 would override a player's own Speed option);
  - `fast_forward = off | x2 | x3` (`--fast-forward`; neutral `off`).
  - #89's menu shows both under Assists from the registry.
- **`platform/src/game_speed.{h,cpp}`**:
  - `patch_game_speed(cpu, speed)` (the boot), `apply_game_speed(cpu, speed)` (live: table + interval once the config pointer at `0x11A98C` is set; `game` → the option's own length, a bad byte read as 2 like `mt_saves_sanitise`);
  - `fast_forward_rate`, `overrun_ns_for`.
  - The table is checked first (`{65, 50, 40, −1}` or one length three times, then −1); nothing is written otherwise.
- **`run_game`**: the table after `mt_process_load` (only when not `game`); every turn the live `game_speed`, then `set_fast_forward`. That is the rate from `Actions::holding(MT_ACTION_FAST_FORWARD)` or `--hold-fast-forward T,S` (a test aid, run-loop time), passed to the paced clock's plan or the launch clock, and the overrun threshold. It also makes the present gate and the wall wait above.
- **The exit line** as above. The log names each change (`game: fast-forward x2 at 4.000 s`, `pacing: the wall clock x2 (fast-forward), at 4.000 s`, `game: game_speed = 25 at …`).

## Evidence

- **CI** (synthetic, `tests/cpp/game_speed_test.cpp`):
  - The rows, parse and serialise; the presets set them neutral.
  - The table patched before a synthetic guest's `LoadConfig` for every speed and Speed byte, with the config byte left alone; `game` restores the table; a foreign table and an unmapped guest are refused.
  - A live change sets the interval, and before the config only the table.
  - The action is held through Tab and R3 in both contexts and never sent to the sink or delivered by the runtime; a bindings file moves it.
  - The paced clock's ×2 wall plan: twice the host time, `gained_ns`, `host_when` against `guest_at` for 2,000 random times, continuity and monotony over 5,000 steps of random toggles between locked, ×2, ×3 and wall, and within a refresh plus half a step of the reference throughout. After a 10 s ×3 hold, a lock moves 1 s in 1 s (no stall).
  - The launch clock's rate with #155's shift.
  - The game's tick gate (`GetTimeMs`, `lastFrame = now`, the 1 ms re-arm) on both clocks, driven by the wall wait divided by the rate: every speed at ×1, ×2 and ×3 within 1 %.
- **Real assets, fixed-step** (`play.game_speed`, Release arm64, Apple M4): the boot with 33 ms patched in, then live changes through 65, 50, 40, 25 and `game`. Ticks a second over 6 s of the main menu from CMTimer's `lastFrame`: **30.303, 15.385, 20.000, 25.000, 40.000, 20.000**, each +0.00 % from 1000 / its length. The config's Speed byte stays 1 in memory and on disk.
  - The test's virtual clock steps 1.001 ms a turn. At exactly 1 ms the game's re-armed fire date (callout + 0.001 s) ties with the next turn's time, and CFAbsoluteTime's rounding makes the timer miss about every other turn. That costs about 0.75 ms a tick for every speed, the game's own included: at 1 ms the same run measured −0.48 % to −2.01 %, and the game's own 50 ms −1.51 %. That is a property of the 1 ms fixed step. A device's timer fires about every millisecond, and GetTimeMs' whole milliseconds absorb a slightly late poll.
- **Both off → byte-identical**: `platform.device_golden`'s seven fixed-step screens match the renders recorded before #151 with 0 differing pixels. `play.first_map`, `platform.audio_capture` and `platform.perf_telemetry` pass.
- **Real time** (Release, dummy drivers, `--vsync off`, `--preset original`, the main menu from 6 s by `--perf-csv`; the machine shared with other builds, load average 5 to 18):

  | Option | Ticks a second | Against |
  |---|---|---|
  | ×2 at 50 ms | 39.692 | 40 (−0.77 %) |
  | ×3 at 50 ms | 59.533 | 60 (−0.78 %) |
  | ×3 at 25 ms | 118.397 | 120 (−1.34 %) |
  | 65 ms | 15.356 | 15.385 (−0.19 %) |
  | 50 ms / `game` | 19.958 / 19.957 | 20 (−0.21 %) |
  | 40 ms | 24.591 | 25 (−1.64 %) |
  | 33 ms | 29.599 | 30.303 (−2.32 %) |
  | 25 ms | 39.258 | 40 (−1.86 %) |

  The exit lines of the ×2 and ×3 runs: −0.60 % and −0.82 % over the whole run. On the wall clock every speed carries the original gate's poll slack (a tick starts at the first 1 ms poll after its interval, and a poll is late by the host's wake-up). That is about 0.5–0.8 ms a tick, the same for the game's own lengths (ADR 0075 measured −1.1 % to −1.6 % at 50 ms on the wall clock). Relative to a 25–40 ms tick it is about 2–3 %: `platform.game_speed`'s 24 s run at 25 ms, boot included, ends at −3.18 % (×2 −0.62 %, ×3 −1.15 %, ×2 with `pacing = wall` −0.68 %). A display lock removes it where the display allows (50 ms at 60/120 Hz, 25 ms at 120 Hz); #165 would lock the clock to a 1 ms grid elsewhere. `platform.game_speed` checks these runs at 3 % (fast-forward) and 5 % (25 ms).

## Consequences

- A player can run the game at 30 or 40 ticks a second, or hold Tab or R3 for ×2 or ×3. Both are Assists, off in every preset, and a run with both off is unchanged.
- With `game_speed` set, the game's own Speed option has no effect until it is `game` again. The menu still shows and saves the option's own choice.
- While fast-forwarding, the clock is not locked to the display, window presents are at most one a refresh, and the wait spins its last stretch (a core busy while the key is held). Releasing the key locks again at once.
- The paced clock's lock corrects against the wall clock plus the fast-forward's gain. The exit line's "from the wall clock" is reported less that gain.
- Shorter ticks on the wall clock show the original gate's slack more (about −2 % to −3 % at 25–40 ms). That is not new, only more visible.
- `fast_forward` is the first port action in the runtime's table. Later ones (#90's turbo) go after it, before `MT_ACTION_COUNT`.
