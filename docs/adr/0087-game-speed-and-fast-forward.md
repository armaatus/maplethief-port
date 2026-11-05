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

## Addendum (#105 wave 4 integration): R3 and the settings menu's L3 + R3

#89 (ADR 0076) opens the settings menu with L3 + R3, and this ADR binds `fast_forward` to R3. The chord wins, and R3 stays `fast_forward`'s pad default (moving it would leave the pad with no free control: every other one is the game's or the remap overlay's).

- **L3 first, then R3.** The menu sees R3 before the pads (`handle_event`: the overlays first) and takes it as the chord, so the game never holds `fast_forward`.
- **R3 first, then L3.** `fast_forward` runs from the R3 press until L3 completes the chord. The menu opens and lets go of every pad control (`Gamepads::release_all`, `Actions::release_source`), `fast_forward` included. They count as held until let go and pressed again, so letting go of R3 in the menu or after it starts nothing.
- **The pause is never fast-forwarded.** Before an overlay holds the turn, the turn hook sets the clock's rate again (`follow_clock_rate`): the rate is back at 1× and the paced clock's plan is locked again. Neither the pause nor the exit line's fast-forward time counts the menu's time at ×2 or ×3. The remap overlay's own L3 + R3 (scripted runs) works the same way.
- Tested by `platform.settings menu: L3 + R3 wins over the fast_forward action R3 in either order` (a virtual Xbox 360 pad, both orders).

## Addendum (fix-ff-pacing): presents before the gate, the wait aimed at it

`platform.game_speed` had started failing: the exit line's deviation was −15 % to −30 % at ×2 and ×3 in Debug (−29.46 % at ×2 with the machine busy, −14.93 % at ×2 and −17.83 % at ×3 with it quiet) and up to −9.5 % in Release. The bug was older than the day it was found: commit 422b81b fails too. The test tolerance was right; the pacing was wrong.

**The cause.** The game's gate takes `lastFrame = now`, with no catch-up, so host work that runs past the moment the gate opens makes that tick late, and the game keeps the loss. Two kinds of host work did that while fast-forwarding:

- **Presents in the wrong place.** The one-present-a-refresh limit (`ff_skip`) held back a tick's GL frame whenever the last present was less than a refresh ago. The turn hook then presented it at the first turn a refresh later, in the middle of the next tick's wait. A turn trace in Debug (×2, 50 ms ticks: 25 ms of host time) showed the cycle. A tick's work took about 3.5 ms. Its frame was held back, and 13.5 ms later a turn spent 20.7 ms presenting it (an 18 ms CPU composite on the dummy driver). The next gate opened during that present, about 10 ms before the next poll. Ticks came every 37.5 ms instead of 25, and each one's frame was again less than a refresh after the last present, so the cycle never broke. In Release the composite is about 4 ms, so such presents ran over the gate only now and then.
- **The poll after the gate.** A tick starts at the first 1 ms poll after its gate opens. While fast-forwarding, each poll's host time (the turn hook's own work, the wait's wake) counts two or three times over on the guest's clock. In Release under load, a poll cycle measured 1.4 ms of host time (3.7 guest ms) against the nominal 0.5. A gate that opened just after a poll waited a whole cycle: tick turns were 51.7 guest ms apart on average, instead of 50.

**The fix** (`game_speed.h`, `game.cpp`), only while fast-forwarding:

- **A present starts only where it ends before the gate opens** (`FastForwardPresents`). The frontend reads CMTimer's `lastFrame` (`0x11A4D8`) and interval (`0x11A478`) and works out the host time to the gate (`gate_in_us`, divided by the rate). A present is allowed if the longest of the last 8 presents' costs, plus 1 ms, fits in that time. A frame that does not fit stays pending, and the next tick's frame replaces it. In practice the tick's own GL present goes through, right after the gate, when the most time is left.
- **A lateness budget for presents that never fit.** Where a present costs more than a tick's slack (Debug's 18 ms composite against 25 or 16.7 ms ticks), the rule above would freeze the window. A budget lets one through now and then instead. It grows by 1 % of the host time, holds at most one present's cost, and pays the overrun each such present is expected to cost, so presents slow the game by at most about 1 %. The one-a-refresh limit stays.
- **The wait ends at the gate** (`ff_gate_wait`). When the gate opens after the run loop's next wake but within 1 ms of it, the turn hook waits for the gate (plus 5 µs) instead. The timer, due by then, fires once and finds the gate open. Only the poll that would find the gate shut is left out; a gate further off keeps the 1 ms polls, and GCD's deadlines move by at most that 1 guest ms.
- The exit log gains `game: fast-forward: N window presents, M of them over the game's gate on the lateness budget (presents cost up to X ms)`.

**Unchanged.** At 1× nothing moves: the tick's frame is presented inside the tick as before, and the gate keeps the original's poll slack. The display lock (ADR 0075) and the fixed-step mode do not fast-forward this way. `skip_intros`' ×8 (ADR 0096) goes through the same gate, so a skipped scene shows fewer frames.

**Consequences.**

- In Release (a present of about 6 ms) the frames still fit, as before. `platform.game_speed` measured 400 presents in 10 s at ×2 (every tick's frame, none on the budget) and 391 in 6.7 s at ×3 (the one-a-refresh limit, 8 on the budget).
- In Debug on the dummy driver the window updates only on the budget: about 10 presents a second at ×2 and 1 at ×3 (`platform.game_speed`: 105 presents in 10 s at ×2, 63 of them on the budget; 6 in 6.7 s at ×3). That is the price of a CPU composite that costs most of a fast tick.
- A backend whose `SDL_RenderPresent` blocks for vsync counts the block as cost, since the gate waits for it too. Such a present fits at ×2 (25 ms ticks) but not at ×3 (16.7 ms), so ×3 would update the window on the budget. Metal returns at once (ADR 0071). The D3D and GL paths are not yet measured on a real display.
- **Load is still felt.** Under heavy load (other builds and real-asset suites, load average 12 to 24 on 10 cores) a wake can still be late, and the main thread competes for a core at the default QoS that ctest's processes run at. An A/B run of Release ×3 at load 12 to 14 measured −1.46 % and −2.59 % as is, and +0.15 % and +0.17 % with the main thread at SDL's time-critical priority. That experiment was not kept. An app opened from Finder gets the interactive QoS anyway, and a priority change is a decision of its own: [ADR 0120](0120-main-thread-priority-in-real-time-runs.md) (#206) took it, SDL's high priority in real-time runs.

**Measured** (`platform.game_speed`, Apple Silicon, load average 3 to 6, each run's ×2, ×3, ×2 with `pacing = wall`, 25 ms):

| Build | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Debug (`mac-debug`) | −0.93, −0.50, −0.92, −3.19 % | −0.75, −0.44, −0.90, −2.53 % | −0.71, −0.19, −0.95, −2.64 % |
| Release (host arch) | −0.31, −0.36, −0.02, −1.59 % | −0.02, −0.11, −0.16, −1.41 % | −0.64, −0.15, +0.01, −1.76 % |

The 25 ms run (1×) is unchanged by this fix. Its −1.4 % to −3.2 % is the original's poll slack, plus, in Debug, ticks whose work nearly fills the 25 ms (Debug's composite of 14 ms included), and it is within its 5 %. Under load average 13 or more, one Debug run measured −5.47 % on it.

**Tests.** `game_speed_test.cpp` covers:

- `gate_in_us` from a synthetic guest's `lastFrame` and interval;
- `ff_gate_wait`'s cases;
- `FastForwardPresents` (fit, the refresh limit, the 1 % budget, no burst after a quiet stretch, restart, the cost window);
- a simulation of the gate driven as the frontend drives it, with presents of 6 ms (Release-like) and 21 ms (Debug-like). With the fix, ×2 and ×3 come within 1.5 % of 40 and 60 ticks a second with presents still shown. Without it, the Debug-like shape at ×2 runs at 26.5 ticks a second.
