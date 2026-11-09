# 128. Direction resolution: a 4-way stick, the last-pressed direction, one move for a diagonal; chord parity and a bounded input queue

- **Status:** Accepted
- **Date:** 2026-11-09
- **Issue:** #138, #139 (epic #267)
- **Builds on:** [ADR 0048](0048-gamepad-actions-and-delivery.md) (`Actions`, the runtime's delivery), [ADR 0070](0070-settings-registry-and-presets.md) (the registry and the presets), [ADR 0051](0051-controller-focus-navigation.md) (`Focus` and `MenuInput` behind `Actions`), [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (`settings.ini`)

## Context

On the device a finger is on one arm of the d-pad, so the game never sees two directions held together. Three things in the port hold two:

- **The stick.** Each of the four directions covers 135 degrees (`stick_directions`, ADR 0048), so past a sector edge a stick holds up and left together. `SkyCanvas::Tick` then resolves the held keys by its fixed order, left and right before up and down ([keypad.md](../controls/keypad.md#hold-and-repeat)), and the hero walks past the ladder instead of climbing.
- **Rolling keys.** Left still held when up goes down is the same held pair.
- **Menus.** A diagonal sends two presses, so two cursor moves. `Focus` and `MenuInput` both sit behind `Actions`, and the settings menu reads the stick's axes on its own.

## Options considered

**Where the priority lives.**

1. **In the runtime (`mt_input_press`, by the `held[]` counts).** The counts are per action, so a release could not tell whether the direction it ends is the one the guest has. No.
2. **In `Actions`, per held control. Chosen.** Each control already resolves its action once and keeps it until release. Adding one bit, "the sink has this control's press", makes "replace" exact: every press the sink gets has exactly one release, and a replaced direction comes back when the newer one lets go, without the runtime knowing.

**A diagonal in a menu.**

1. **Any second direction while one is held waits.** Simple, but it breaks #147's tested behaviour (`menu_input_test`: down held, right pressed 0.2 s later takes over), and it swallows a quick roll from one arm to another.
2. **A time window. Chosen.** Two different directions that go down within 80 ms are one diagonal; pressed later, the second is a deliberate press and moves as before. The window needs a clock, and `Actions` had none: the game and the game driver inject the guest's (`mt_cf_absolute_time`). A bare `Actions` has no clock and no diagonals, as every older test expects.

## Decision

- **`stick_mode = 4way | 8way`** (Controls, `--stick-mode`, `kAtOnce`; `StickConfig::mode`). `4way` is one direction: the dominant axis. The held direction keeps the stick until `hysteresis_degrees` (8) past the diagonal, so a stick on the edge does not flicker; a direction not held takes it at the diagonal itself. `8way` is the old emulation. A bare `Gamepads` stays 8-way (`StickConfig` as it was built, the tests that construct one directly); the game and the game driver set it from the setting each turn.
- **`direction_priority = game | last`** (Controls, `--direction-priority`, `kAtOnce`; `Actions::set_direction_priority`). In the **field** with `last`, only the newest held direction reaches the sink: a press releases the older one and presses the new one, in that order, so the two go to the pad's flags in one tick's batch. Letting the newest go brings back the newest one still held, and a press of the same direction from a second device counts twice, as the runtime's counts expect. An older one let go while replaced is quiet. `game` passes every direction on, as before. The field's rule runs again each `Actions::turn()`, so a changed setting, a context changed under held controls, and a press held back in a menu all settle within a turn.
- **A diagonal in a menu is one move, by either priority** (`Actions::menu_diagonal`, 80 ms, `Actions::kDiagonalSeconds`). A direction control pressed within the window of a *different* direction pressed in the menu is held back: no press reaches `Focus` or `MenuInput`, its release is quiet. A direction held over from the field does not count; the same direction from two devices is not a diagonal; held back and still held when the menu closes, it catches up in the field. The **settings menu** reads the stick itself and gets the same rule from its axes: while one axis is engaged, the other axis's edge does nothing (the axis that got there first made the move).
- **Presets** (#89's table, ADR 0070's recipe). `stick_mode` is `4way` in every column: the original never holds a diagonal, so 4-way is what the original plays like, and `Original + controller` is the same game on a pad. `direction_priority` is `game` in Original and Original + controller (they keep the game's own order) and `last` in Enhanced. **Without a settings file** the defaults are the Original column (ADR 0070): `4way` and `game`. The stick change applies to everyone, because that is the point of the issue, and `scripts/regress --preset original` (a recorded pad on the real game) gives the same digest as `dev`.

## Consequences

- A stick climbs ladders as a finger does, at any angle that is nearer up than sideways, in every preset.
- Rolling keys and the d-pad climb under `last` (Enhanced, or chosen in the menu); `game` keeps the original's order for anyone who wants it and is what the existing `input.*` tests run.
- `Actions` has a clock for one thing. It stays optional, so a test that drives `Actions` by hand stays deterministic.
- 8-way is still there for a player who wants diagonals (`stick_mode = 8way`), but a diagonal in the field still walks by the game's order.

## Evidence

- Unit tests (`tests/cpp/gamepad_test.cpp`, `tests/cpp/settings_menu_test.cpp`, `tests/cpp/presets_test.cpp`): the 4-way sectors and hysteresis; `last` replacing, restoring and releasing, each press with exactly one release, focus loss, the setting changed under held keys; a diagonal in a menu by either priority, the window's edge, the same direction from two devices; the columns in `kExpected`.
- Real-asset tests (`input.gamepad_climb_stick`, `input.gamepad_climb_last`, `input.gamepad_climb_game`, `input.keyboard_climb_last`, `input.keyboard_climb_game`; `tests/cpp/menu_nav_real_test.cpp`, `platform/menu_nav_test.cmake`): at the tutorial's rope on map 2.8, a stick 30 degrees off up climbs (climb state 4, `CHero +0x10`); left held then up climbs with `last` and walks on with `game`, by the d-pad and by the keys.
- The existing `input.*` tests pass unchanged with `stick_mode = 8way` and `direction_priority = game` (`MAPLETHIEF_SETTINGS`, and the test programs that build `Actions` and `Gamepads` directly, which are 8-way and `game` by construction) and with the defaults; `scripts/regress --preset original` gives the `dev` digest.

## Chord parity, the bounded queue and the touch clobber (#139)

### What a chord is proven against

Two fingers on the key pad are the original's input. `input.chords` (real assets) plays each chord three ways on the same turns: two fingers (`Input::touch`), the virtual pad's buttons, the keyboard. The hero's x, y and state at every game tick must match byte for byte (`platform/menu_nav_test.cmake`). The runs use the defaults, the Original column: `stick_mode = 4way`, `direction_priority = game`. A fourth run plays the scenes by the keys with `direction_priority = last`, and it too matches two fingers on every tick: only one scene holds two directions, up then left, and there the newest is left, which is also what the game's order picks. Where `last` differs from two fingers by design is left held and then up at a ladder (the newest climbs; two fingers walk on): that is #138's `input.gamepad_climb_last` / `_game` and `input.keyboard_climb_last` / `_game`. Sequences: jump left, jump right, jump then attack in the air, hold attack (one attack), the double-tap run latch, up + left at the ladder (left wins, no climb), climb the rope (map 2.8), climb the ladder (map 2.9).

**The first comparison failed, and the cause was a port bug.** With right pressed and jump one tick's length later, both edges could land between the same two polls. Two fingers then reach one tick together (`'3'`, a jump to the right); the queue's one step per tick made the second key wait, so the tick saw right alone, walked, and took the two-tick switch: the jump came two ticks late. A later key release went the same way. So the pacing is now by key, not by step: while the last step of a tick went by the pad, a *different* key's BOOL may still change in that tick (`touched_this_tick`), as a second finger's does. One key still changes once a tick (the tick has to see it held), and a key event is still one step of its tick. `scripts/regress --preset original` is unchanged by it.

### The input queue

The queue exists because `SkyCanvas` keeps one pending press and release and the pad's BOOLs are only polled once a tick, so a burst must be paced at one step per tick. That is unbounded: at 20 Hz a key's press and release are two edges per tick and the backlog grew by one edge a tick, so a mash played back ever later, and a press during a load (no `SkyCanvas`, slots stuck busy) played back when the game came back.

**Options.** A hard cap (drop the newest events) loses the guest's flag agreement. Dropping presses alone leaves a release behind it. Merging only a press with its own release (a *tap*, both still queued) keeps every key's final state: per key, queued events alternate, and removing a press + release pair leaves the alternation intact.

**Decision (`bound_queue` in `input.cpp`).**
- Every queued event carries the tick it was first seen at. Each turn, the lateness of a press = the ticks it has waited + the steps ahead of it in the queue (the same step rules as `step()`, batches of different keys on the pad's route) + 1 if a step already went this tick.
- While a press would be more than **2 ticks late**, the oldest queued tap of the late press's key is merged away (the press and its release, or the late press itself if it is part of a tap); if there is none, the oldest tap of another key ahead of it. A press whose release has not arrived is not merged, however late; it is a held key and the guest's flag must end up set. A mash at 20 Hz therefore delivers **one tap every second tick** (the game's two-tick switch rate: 10 taps per second at 50 ms; measured 31 presses in 64 ticks, the worst press 1 tick late, 29 taps merged), each press at most 2 ticks after it was pressed, and the mash's final state (held or released) always reaches the guest (`input.chords_mash`, `input runtime: a 20 Hz mash`). Taps beyond that rate would have been lost to `Tick`'s polling anyway: the game can see at most one press per two ticks.
- A tap also goes the moment nothing could see it: while there is no `SkyCanvas`, or on the turn after the run loop stood still for more than 0.25 s (a blocking load, a pause, the remap overlay). Held keys are kept and land as soon as the game can take them: that press is late only because the game was not there, and playing it is what makes `held[]` true.
- Limits: a press that is late because of held keys ahead of it (four non-pad keys pressed together in a menu: the fourth is 3 ticks late, `held keys cannot be merged` in `gamepad_test.cpp`) cannot be helped, and is not. In the field the pad route changes different keys in one tick, so a chord never queues behind itself.

`Event::at` is not serialised; a restored queue restarts its ages at the restore. The save-state format is unchanged. `mt_input_stats` gains `merged_taps` and `max_press_lag` for the tests.

### The touch clobber

`touchesMoved:` `0xB6BB5` clears every key and both finger records when a touch is unmatched (a finger that began on no key and slides, or moved past the 50-point match), and `touchesEnded:` `0xB5CF1` has the same clear for an unmatched touch (`0xB6304`-`0xB63B8`; `input.chords_mixed` re-asserts once for the move and once for the lift): the game forgets a key the keyboard or a pad holds, and the hero stops while the key is down. A matched finger lifted or slid off clears only its own key (and, lifted, the key under its end point).

**Decision.** Around the delivery of a turn's touches, the runtime compares: `mt_input_before_touches` reads the pad's two finger records (`firstPointType`, `secondPointType`), `mt_input_after_touches` looks at every action with route pad, `held[]` non-zero, nothing queued for it and a cleared BOOL, and calls its setter again, before any tick polls the pad. Not re-asserted: a key a finger was recorded on before the touches (the finger let it go, whichever way), a key with an event queued (its own step is coming), a key whose route is not the pad. A re-assert is not a press: it is counted in `reasserts`, and it can only fire on a turn that delivers touches (only touched turns reach it), so a pure key or pad game and `scripts/regress --preset original` (a recorded pad) are unchanged.

### Consequences

- `input.chords`, `input.chords_mash` and `input.chords_mixed` (a held key survives a finger dragged over the pad; the hero's x grows every tick) run locally on real assets; `input_runtime` cases in `tests/cpp/gamepad_test.cpp` (a 20 Hz mash, different keys in one tick, held keys unmerged, a load with no canvas, slots busy for ticks, a stopped run loop, the clobber and its exclusions) run in CI.
- A finger lifted from a key the keyboard also holds is not re-asserted: the game's flag goes off with the finger while `held[]` still counts the key, until the key is let go and pressed again. That is left as the game does it.
