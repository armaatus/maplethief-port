# 92. Quality of life: turbo attack and auto-potion as frontend presses, UIKit animations scaled by the host

- **Status:** Accepted
- **Date:** 2026-11-02
- **Issue:** #90
- **Builds on:** [ADR 0048](0048-gamepad-actions-and-delivery.md) (actions and their delivery), [ADR 0070](0070-settings-registry-and-presets.md) (the registry, Assists), [ADR 0087](0087-game-speed-and-fast-forward.md) (the port's actions), [ADR 0089](0089-autosave-on-quit-and-background.md) (save anywhere rejected; the guest reads of the field), [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (UIKit's animations)

## Context

#90 asks for small comfort options that cannot break game logic, each behind a toggle and off by default. [docs/re/qol.md](../re/qol.md) gives every candidate's hook point, risk, effort and verdict. Three are cheap and safe: turbo attack, auto-potion and faster menu animations. The others become follow-up issues; save anywhere stays rejected (ADR 0089).

The game fires attack, skills and jump once per press and treats only walking as a held action (docs/ANALYSIS.md, Input). A fresh character's HP potion is on quick slot 1, and the quick slots are `CHero +0xE0 + 8n` `{kind, id}`, as `CHero::KeyAttack` `0x66DD9` reads them. The title menu and the character select animate with `+[UIView beginAnimations:]` blocks, and nothing in the game waits on them.

## Options considered

**Turbo attack and auto-potion: where the press comes from.**

1. **A guest hook** (ADR 0069) on `CStateGame::DoHeroKeyAction` `0x8163D` or `CHero::KeyAttack`, calling the attack or `CHero::UseItem` `0x66695` from the host. That skips the game's own checks between key and action (a held walk blocks a press, the pending-press slot, the hero's state) or has to copy them, and needs regenerated code.
2. **The frontend presses the game's actions** (`mt_input_press` / `mt_input_release`), as a second finger would. The game's own path runs: SkyCanvas's slots, `KeyAttack`, `UseItem`, the potion's sound and count. **Chosen.** Nothing in the guest changes, and an Assist can do nothing a player could not do with the same keys.

**Turbo's pulse.** A release and a press queued together every 6 game ticks. The runtime's FIFO delivers them a tick apart, so the hold count never drops (the player's own release stays theirs), and a pair is queued only when nothing else waits. 6 ticks because one attack lasts 5: at 4, every other press fell inside an attack and was dropped (8 attacks from 14 repeats in 3 s; at 6, 10 from 9). Only in the field: in popups and menus attack is OK, and a repeat could answer the death popup.

**Auto-potion's choice of slot.** The first quick slot 1–4 holding an item (kind 1) that is a potion (`+0x8C` = 1) with HP to restore (`+0xC4` > 0) and a count, found the way `KeyAttack` finds the item. A slot number in the setting was rejected: the player can drag items between slots, and a wrong slot would drink an MP potion or cast a skill. Time is game ticks (CMTimer's `lastFrame`), so `game_speed` and fast-forward scale the 20-tick cooldown as they scale the game.

**Menu animations: guest or host.** The durations are constants in `MainMenuView` and `ChooseCharacterView`; patching them would mean hooking each method. The host's `+commitAnimations` builds every animation, so a scale there covers all of them. **Chosen:** `mt_uikit_set_animation_scale` multiplies delay and duration at commit; 0 starts none. The model values were always set at once, so the guest sees nothing.

**A test aid for "the regression suite with the feature on".** The play tests have their own driver (`game_driver.h`) and the `platform.*` tests run the game with their own flags. **`MAPLETHIEF_SETTINGS="key = value; …"`** is read by both: the game applies it after `settings.ini` and `--preset`, before the other flags, and the driver applies it over its settings at the boot. `ctest -L real_assets` then runs with a feature on without editing a test.

## Decision

- **Rows** (`platform/src/settings.{h,cpp}`, appended after `autosave`; all `Applies::kAtOnce`, read every turn from the live settings):
  - `ui_animations = normal | fast | instant` (Video, `--ui-animations`; `normal` in every preset column): the UIKit scale 1, 0.25 or 0.
  - `turbo_attack = off | on` (Assists, `--turbo-attack`; `off` in every column).
  - `auto_potion = off | 25 | 50 | 75` (Assists, `--auto-potion`; `off` in every column): the percentage of max HP.
- **`platform/src/assists.{h,cpp}`**: `read_assist_view` (the field, the hero, its HP and the HP potion's slot, through a `GuestRead32`; `guest_reader(cpu)` now comes from `autosave.h`) and `Assists::turn`, called from `game.cpp`'s turn hook after MenuInput and from `game_driver.h`'s turn.
- **`runtime`**: `mt_uikit_set_animation_scale` / `mt_uikit_animation_scale` (`maplethief/uikit.h`). `game.cpp` sets it before `mt_process_run` and whenever the setting changes.
- **`MAPLETHIEF_SETTINGS`** in `main.cpp` and `game_driver.h` through `apply_setting_list`.

## Consequences

- Holding attack keeps attacking with `turbo_attack`, about 10 attacks in 3 s at the game's 50 ms. A held walk still blocks it, as it blocks the player's own presses.
- With `auto_potion`, the hero drinks its HP potion under the threshold in the field, once a second at most while HP stays low and potions last. Not in menus, popups or scenes, not when the walk key is held (the game ignores the press), never an MP potion.
- The title menu settles in a quarter of the time with `fast` and at once with `instant`. Play can be pressed sooner; the early-Play guards (#122, #129) cover that.
- Each Assist is a frontend press: anything a player could do with the same keys and nothing more. Off, nothing is pressed and nothing is read, and a run is unchanged.
- `MAPLETHIEF_SETTINGS` acts like command-line flags: a settings menu that saves writes its values too. It is meant for test runs.
- Auto-run and hold-or-toggle (#172), fast text (#173), cutscene skip (#174), auto-loot (#175) and a minimap (#176) are follow-up issues ([re/qol.md](../re/qol.md)).

## Evidence

- `platform_test` `assists:` cases (synthetic guest), `presets_test` (the three rows' columns), `objc_test` `views: the host's animation scale …`.
- Real assets, Release arm64: `play.auto_potion`, `play.turbo_attack`, `play.ui_animations_normal`, `_fast` and `_instant` ([re/qol.md](../re/qol.md#verification) has the numbers).
- `ctest -L real_assets` with each feature on through `MAPLETHIEF_SETTINGS` (the issue's acceptance; the PR lists the result).
