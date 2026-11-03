# 94. Controller extras: auto-run and hold-or-toggle as a latched walk

- **Status:** Accepted
- **Date:** 2026-11-02
- **Issue:** #172
- **Builds on:** [ADR 0092](0092-quality-of-life-assists-and-ui-animations.md) (Assists as frontend presses), [ADR 0048](0048-gamepad-actions-and-delivery.md) (actions and their delivery), [ADR 0087](0087-game-speed-and-fast-forward.md) (the port's own actions, fast-forward), [ADR 0070](0070-settings-registry-and-presets.md) (the registry, Assists)

## Context

#90's verdicts ([docs/re/qol.md](../re/qol.md)) leave two controller extras for a follow-up: **auto-run** (the hero keeps walking without a held direction) and **hold-or-toggle** (a press starts a held action and the next press stops it). They fit ADR 0092's layer, the frontend pressing the game's own actions. But they need rules that turbo did not: which actions may latch, and what ends a latch.

The game holds only one thing: walking. `CStateGame::DoHeroKeyAction` `0x8163D` runs every frame and keeps only left and right. Attack, jump, the skills and the quick slots fire once per press, and OK and back are presses too. While the walk key is held, every other press waits until it is released (docs/ANALYSIS.md, Input). A d-pad + jump chord is the exception: SkyCanvas makes it a jump to that side. The port has one held action of its own, `fast_forward` (ADR 0087).

## Options considered

**What may latch.**

1. Any action. A latched attack would only repeat what turbo does already. A latched OK or back would answer popups, including the death popup's "Return to town". A latched quick slot would drink every potion. Rejected.
2. **Walking (left and right) only**, plus the port's `fast_forward`, which the frontend holds anyway. **Chosen.** Up and down stay presses: up enters a portal or talks, and down climbs. Neither is a walk the game keeps.

**How the walk is kept.**

1. A guest hook on `DoHeroKeyAction` that walks as if the key were held. This skips the game's pad state, the key pad's d-pad picture and the other presses' rules. It also needs generated code.
2. **The frontend presses the direction itself and keeps it pressed** (`mt_input_press`), as a second finger on the d-pad would. **Chosen**, as in ADR 0092. The runtime counts holds per action, so the player's own press and release of that direction stay theirs: holding right while it is latched, then letting go, leaves it latched. Ending the latch is one `mt_input_release`.

**What starts one.**

- `auto_run = on`: the port's new action `auto_run` (`MT_ACTION_AUTO_RUN`, after `MT_ACTION_FAST_FORWARD`; a port action, never delivered). A press latches the direction held now (if both are held, the one pressed last). With no direction held, it latches the last direction pressed in the field. If there is none, it does nothing. A second press ends the latch.
- `hold_toggle = walk | all`: a press of left or right latches it, and a second press of the latched direction ends it.
- `hold_toggle = fast_forward | all`: a press of `fast_forward` turns it on and the next turns it off. `set_fast_forward` ORs the toggle with the held key.

Presses are counted by `Actions::presses(action, context)`, so a tap that goes down and up between two run-loop turns is still seen. Two presses of one action within one turn count as one.

**What ends a walk latch.** Every case is tested in `platform_test`'s `auto_run:` cases.

| Cancel | Why |
|---|---|
| auto_run pressed again, or (`hold_toggle` walk) the latched direction pressed again | the toggle |
| the other direction pressed | the player turned round. With `hold_toggle` walk, that direction latches instead |
| the field left: the key pad's panel not 0, game mode not 0 (popup menu, scenes, loading), a popup open | in menus the directions are a cursor. The latch is released at the first turn the Assists see the menu |
| the hero dying or dead (`CHero +0x10` 0 or 1, or HP 0) | the death popup follows |
| a new map: `CStateGame +0x84` / `+0x88` or the hero pointer changed | a portal is a new start |
| both settings off for walking | nothing would end it otherwise |
| a port overlay opening (settings menu, remap overlay) | the overlay lets go of every key; the latches go with them (`Assists::drop_latches`) |

A latch never *starts* outside the field: not with a panel other than 0, a popup, game mode other than 0, a dead hero or no hero. This is the same "field" as turbo and auto-potion use.

**The default controls of `auto_run`.** On a gamepad, L3 (the left stick's click), which is free in both contexts. On the keyboard, R, which is free in the field and stays Revive in menus. Both are bound in the field only. L3 + R3 still opens the settings menu, and that overlay ends any latch.

**Fast-forward's toggle and menus.** Fast-forward is the port's clock, not a press to the game, so the toggle works in menus as the held key does. Only an overlay opening ends it.

## Decision

- `runtime`: `MT_ACTION_AUTO_RUN` ("auto_run", "Auto-run") in `maplethief/input.h` and the action table.
- `platform/src/actions.{h,cpp}`: `Actions::presses(action, context)`. Default rows: L3 → `auto_run` (field), and R → `auto_run` in the field (Revive in menus as before).
- `platform/src/assists.{h,cpp}`: the walk latch (`walk_latch()`, `LatchEnd`, `Stats::latches` / `latch_ends` / `last_end`), the fast-forward toggle (`fast_forward_latched()`) and `drop_latches()`. `AssistView` gains the map (`area`, `map`). `auto_potion` treats a latched walk as a held one and waits.
- `game.cpp`: `drop_latches` where an overlay opens. `set_fast_forward` reads the toggle.
- **Rows** (appended after `auto_potion`, both Assists, `kAtOnce` and read every turn, `off` in every preset column):
  - `auto_run = off | on` (`--auto-run`)
  - `hold_toggle = off | walk | fast_forward | all` (`--hold-toggle`)

  The help text says that a latched walk makes attacks and potions wait.
- The remap overlay names the port's actions from the runtime table's titles. "Fast-forward" used to show as "?".

## Consequences

- With `auto_run` on, one press keeps the hero walking across a map. Jump still works: the d-pad + jump chord jumps that way, so steps are cleared. Attacks, skills and potions wait while it walks, as with a held d-pad. Opening a menu, dying, a portal, or a tap of the other direction stops it.
- With both settings off, `Assists` presses nothing new. L3 and R in the field are bound to a port action that never reaches the game, and they did nothing before either. `Actions` only counts presses. A run is unchanged (the real-asset suite with the defaults).
- A bindings file that gives L3 or R to another action takes it from `auto_run` without a warning, as for `fast_forward`. The bindings tests that used L3 as a free control now list it first, because the gamepad rows come before the keyboard's.
- No guest code changes and nothing is generated.

## Evidence

- `platform_test` `auto_run:` and `hold_toggle:` cases (`tests/cpp/auto_run_test.cpp`): latch and end for every cancel above, nothing in menus, popups, scenes or dead, both off pressing nothing, the runtime's hold counts, the fast-forward toggle, auto-potion waiting, `Actions::presses` and the default controls. `presets_test` covers the two rows' columns, and `keyboard_test` the doc table.
- Real assets, Release arm64: `play.auto_run`. With auto_run off, L3 latches nothing. With it on, the latched walk goes to map 2.8's left edge with nothing held; a tap of right ends it; L3 alone crosses the map from x 0 to 479 (A over the tutorial's steps); the popup menu ends the latch and L3 there latches nothing. Then `hold_toggle = walk` walks from one tap of left to the next.
