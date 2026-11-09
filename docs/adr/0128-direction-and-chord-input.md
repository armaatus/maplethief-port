# 128. Direction resolution: a 4-way stick, the last-pressed direction, one move for a diagonal

- **Status:** Accepted
- **Date:** 2026-11-09
- **Issue:** #138 (epic #267)
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
