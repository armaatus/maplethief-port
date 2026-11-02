# 76. The settings menu: built from the registry, opened by the remap overlay's controls, applied live

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #89 (epic #105)
- **Builds on:** [ADR 0070](0070-settings-registry-and-presets.md) (the registry, the presets), [ADR 0054](0054-bindings-file-remap-overlay-and-button-glyphs.md) (the port-drawn overlay, its pause, `Canvas`, the glyphs), [ADR 0063](0063-app-lifecycle-background-and-minimise.md) (the lifecycle while an overlay holds the turn), [ADR 0071](0071-frame-telemetry-and-frame-time-overlay.md) (`FrameStats::recent()`)

## Context

The epic's exit criterion is that every enhancement can be switched and reached from the settings menu with a pad, a keyboard, a mouse and a touch screen. About 15 children add settings to the registry in parallel with this menu, so the menu can't list them by hand.

Facts that limit the options:

- **The registry already has what a menu needs** (ADR 0070): a group, label, help and value type per row, `setting_text`, `setting_choices`, `step_setting`, the presets and `matching_preset`.
- **Every default control is bound somewhere** (ADR 0054): Esc and Start open the game's popup menu in the field and go back in menus, and Back / View is the map. Only F1, Guide and the stick clicks are free, and the remap overlay already uses them.
- **The game has its own options**: sound, speed and vibration, in the option tab of its popup menu (`CDataMgr +0xB0` tab 5, [docs/controls/keypad.md](../controls/keypad.md)). That menu opens only in the field.
- **Some settings are read once.** `gfx_backend` picks the device, `hd_text` scales text textures as they're made, and the game opens its music players at start-up (`gapless_music`). A filter needs SDL's GPU renderer, which `main.cpp` asks for only when a filter or the GPU backend is set at start-up. Others already change while the game runs: #152's render scale (`apply_render_scale`, between two run-loop turns) and #145's fullscreen, vsync and `pause_unfocused` (`apply_settings_at_once`, ADR 0077).
- **Scripted runs must not change**: goldens, shots, captures and fixed-step runs, as the perf overlay's `perf_overlay_allowed` and the picker's `wants_preset_picker` already ensure.

What had to be decided:

- What the menu is made of, so a new row appears without touching the menu.
- What opens it on each device, and how that fits with the remap overlay.
- What the game does while the menu is open.
- What applies at once, what waits for the next start, and what gets written.
- How buttons that aren't settings fit in: the bindings, the game's options, and #148's export and import.

## Options considered

**What the menu is made of.**

1. **A hand-written screen per group.** Every child would then edit the menu as well, and the exit criterion would be checked by hand.
2. **A list of all the rows, ungrouped.** Too long for a 480 × 320 screen, and Assists would get no visible separation.
3. **Tabs from the registry's groups, rows from the registry's rows. Chosen.** A tab exists only for a group that has rows, so Accessibility appears with #158's first row. Action rows (`menu_actions()`) sit in the same tabs.

**What opens it.**

1. **Esc or Start, as many games do.** Both already open the game's popup menu in the field, so the first press would reach the game.
2. **New keys (F2) and a new chord.** Every free pad control is already the remap overlay's.
3. **F1, Guide and L3 + R3 open the settings menu, and the remap overlay becomes its Controls tab's "Bindings" row. Chosen.** There is one way into everything the port adds, and nothing bound changes. In a scripted run, where the menu never opens, those controls open the remap overlay directly, as before.

**Touch.**

1. **A two-finger tap.** The game uses two fingers together (the d-pad plus jump or attack), so this would fire during play.
2. **A three-finger tap.** iOS takes it for undo, and a fast player can use three fingers.
3. **A gear button. Chosen.** It sits in the bars beside the image when they are wide enough (a phone's safe-area margins, an iPad's or a desktop's letterbox), which is outside the game. Without bars, it sits over the image's top-right corner, but only once a touch screen has been used (on iOS from the start). It is drawn in the window only, after the read-backs, like the focus highlight.

**Live changes.**

1. **Apply everything at the next start.** That is simple, but the player hears nothing while dragging a volume slider.
2. **Restart subsystems in place** (renderer, device, music players). This is risky in the middle of the game.
3. **Apply at once what the frontend can, and say "next start" for the rest. Chosen.** `Applies::kAtOnce` marks a row the frontend applies at once. `apply_live_setting` is the one place that knows how. A row it can't apply this time, such as a filter without a GPU renderer, is marked as waiting for the next start.

**What gets written.**

1. **The live settings, whole.** A one-run flag (`--music-volume 50`) would then become permanent the first time the player touched any other row.
2. **The menu's changes, replayed over the file. Chosen.** The menu logs the `apply_setting` calls it makes. On close, `save_menu_changes` loads `settings.ini`, replays them and writes the file through `save_settings`, which rewrites the file byte for byte (ADR 0043, 0070). Preset transitions such as "a change makes it custom" replay the same way.

## Decision

### The menu

`platform/src/settings_menu.{h,cpp}`, `SettingsMenu`.

- **Layout (480 × 320 screen points, drawn with `Canvas` over a copy of the composited screen, like the remap overlay):**
  - The title and a close box.
  - The preset line: Original, Original + controller, Enhanced or Custom (or "None yet"), with its summary (`preset_summary`, or "Your own values (as Enhanced)" from `matching_preset`).
  - The tabs: the registry's groups that have rows or actions, in group order. The Assists tab is set apart, drawn in orange, and its tab carries the line "ASSISTS CHANGE HOW THE GAME PLAYS. No preset ever turns one on."
  - Ten rows at a time, each a label and a `< value >`. Rows with `frame_cost` show the last second's frontend time and frame rate next to the label. A `*` marks a value that waits for the next start.
  - Two lines of help for the selected row, a status line, and hints drawn with the glyphs of the pad in use.
- **Rows.**
  - Settings: `setting_text` for the value, `step_setting` for Left and Right.
  - Values offered for colours and custom values: `SettingValue::names`, now also read by `setting_choices`. `border_color` offers black, three greys and white. `right_touch` offers off, the attack key (440,290) and the jump key (370,290). A value of the player's own steps to the first or the last value offered. `check_registry` checks that each offered value reads back as written.
  - The preset line steps through the four presets with `apply_setting("preset", …)`. Changing a row a preset controls makes it Custom, as in ADR 0070.
- **Input.**

  | | Keyboard | Gamepad | Mouse | Touch |
  |---|---|---|---|---|
  | open | F1 | Guide, L3 + R3 | the gear | the gear |
  | move | Up / Down, W / S | d-pad, left stick | hover | – |
  | change | Left / Right, A / D; Enter / Space = next | d-pad, stick; South = next | click `<` / `>` (or the value) | tap `<` / `>` (or the value) |
  | tab | Tab / Shift+Tab, Q / E, Page Up / Down | LB / RB | click a tab | tap a tab |
  | action | Enter / Space | South | click the row | tap the row |
  | scroll | (follows the selection) | (follows) | wheel, click `^` / `v` | tap `^` / `v` |
  | close | Esc, Backspace, F1 | East, Start, Guide | the close box | the close box |

  Routing goes through `menu_route_event`: `handle_event` gives it every event after the remap overlay (while that is open) and before the devices. While the menu is open it takes every key, pad and pointer event, so `Actions`, `Focus` and the game see none. Opening lifts every held key, pad control and touch, as the remap overlay does. A press and release must land on the same target for a click to count.
- **Never in a scripted run.** `scripted_run(options)` (`game.h`) is now the one list of scripted options, and `wants_preset_picker` uses it too: `--frames`, `--seconds`, `--capture`, `--capture-at`, `--shot`, `--tap` / `--swipe`, `--fixed-step`, `--audio-capture`, `--minimise`, `--background` and `--quit`. In such a run `set_allowed(false)`: no hotkey opens the menu, the gear isn't drawn, and F1, Guide and L3 + R3 open the remap overlay as before.

### The pause

`run_remap` becomes `run_overlays` and holds the turn while the remap overlay or the menu is open. This is ADR 0054's pause, with ADR 0063's rules unchanged: a minimise keeps the overlay open and draws nothing, and a quit ends the hold. Opening the menu takes `FrameStats::recent()` first, so the cost shown is the game's last second, not the pause. When the menu closes, or a quit ends it, its changes are written. The guest's timers fold the gap, and `FrameStats::gap` records it as a pause.

### Live changes (`platform/src/live_settings.{h,cpp}`)

`apply_live_setting(targets, row, settings)` applies these rows, which are now `Applies::kAtOnce`:

| Row(s) | Applied to |
|---|---|
| `scale_mode`, `border_color` | `Presenter::set_scaling` |
| `filter` | the present pass, on SDL's GPU renderer only (otherwise it waits for the next start) |
| `master_volume`, `music_volume`, `effects_volume` | the mixer's bus gains |
| `mute_unfocused` | `FocusMute::set_enabled` (off unmutes now; never under `--audio-capture`) |
| `right_touch` | `Input::set_right_touch` |
| `hide_keypad` | `KeypadHider::set_mode` (it fades) |
| `rumble`, `rumble_strength` | `Rumble::configure` |
| `perf_overlay` | `PerfOverlay::set_on` |
| `fullscreen`, `vsync` (#145, ADR 0077) | `Desktop::apply`, then `follow_vsync`: `Game::vsync` and the telemetry follow, so the paced clock re-plans at the next turn (#80, ADR 0075) |
| `pause_unfocused` (#145) | `Lifecycle::set_pause_unfocused` |
| `render_scale` (#152, ADR 0080) | `request_render_scale` in `game.cpp`: a new governor (auto from the scale in effect, or the fixed N) and `pending_scale`, which `apply_render_scale` (`mt_gles_set_render_scale` and the textures that follow) takes at the next run-loop turn. The menu holds the turn, so the new scale shows when it closes. |

This is the one path for every `kAtOnce` row. `game.cpp`'s `apply_settings_at_once` (#145: the start, the fullscreen hotkeys) applies the desktop's rows through `apply_live_setting` too, with the same `LiveTargets` (`live_targets(g)`). A fullscreen hotkey's change is written like the menu's, replayed over the file (`save_menu_changes`), so a one-run flag is never written.

`gfx_backend`, `hd_text`, `gapless_music`, `pacing`, `language`, `texture_pack`, `texture_filter`, `fix_original_bugs`, `cash_shop`, `save_dir` and `save_backups` stay `kNextStart`. `game.cpp` reads every setting from its live `Settings` (a copy of `GameOptions::settings`). The old copies in `GameOptions` are gone, except `render_scale` and `hd_text`, which are read once at start-up. A child that makes a row live adds its case to `apply_live_setting` and sets `.applies`. The test `live settings: each live row reaches its object` fails if a `kAtOnce` row has no case.

### Action rows

`menu_actions()` (in `settings_menu.cpp`) lists each action's key, group, label and help. The frontend registers a handler per key with `SettingsMenu::set_action(key, fn)`; `fn` returns the status line. A row without a handler says it isn't available in this build.

| Key | Tab | What it does (`game.cpp`'s `start_menu`) |
|---|---|---|
| `bindings` | Controls | Opens the remap overlay over the menu. Its Save or Cancel returns to the menu. |
| `game_options` | Gameplay | In the field: closes the menu, and `GameOptionsOpener` presses the field's Menu action, waits for the popup menu (panel 1, at most 3 s), lets it settle (0.3 s), then taps its option tab (`popup_menu_tab_point(5)`, virtual finger `0x1040`), as a finger does. Anywhere else it says the options are in the game's popup menu: no other screen leads to them, and Menu means "back" in menus. |
| `export_saves` | Saves | `export_saves_action` (`live_settings.cpp`) on #148's `export_saves` (ADR 0078): a zip of the save files in `<data dir>/exports/MapleThief-saves-<UTC time>.zip`. |
| `import_saves` | Saves | `import_saves_action` on #148's `import_saves`: the newest `.zip` by name in `<data dir>/import/` (made if missing), else that folder's save files; checked, and the files it replaces backed up first. Only where no character is loaded (the key pad's panel is -1: the title, the main menu, the slot screens), since the game holds the loaded slot in memory and would save over it (ADR 0078). |
| `backups` | Saves | `backups_action` on `list_backups`: how many backups each slot has, and the imports'. |

A file dialog was left out: SDL3's are asynchronous, absent on iOS, and would sit behind a fullscreen window. Fixed folders in the data directory work the same everywhere, and the status line names the file. Restoring a backup needs a list to choose from, which the menu's one status line has no room for; `--import-saves` of a backup's folder restores one (ADR 0078), and a backups screen is a follow-up.

A new action row is one line in `kActions` (`settings_menu.cpp`) plus one `g.menu.set_action("key", fn)` in `start_menu`. #154's Achievements and #159's key pad editor are added that way.

`SettingInfo::frame_cost` marks the rows that show the frame-time cost: `render_scale` and `gfx_backend`.

## Evidence

- **CI (synthetic, `tests/cpp/settings_menu_test.cpp`, `platform_test`):**
  - **The exit criterion.** `settings menu: every registry row and action is reachable by keys, pad, mouse and touch` walks `settings_registry()` and `menu_actions()`. For each of the four devices it opens the menu on a dummy-driver window, reaches the row's tab and the row with that device's events through `menu_route_event`, and changes its value (or runs the action) with the same device. Every registry row is in exactly one tab, the tab of its group.
  - The tabs follow the rows: synthetic Accessibility and Assists rows get their tabs, and Assists gets its warning line.
  - The preset line: Original, Original + controller, Enhanced and Custom in turn. A change to a row the preset controls makes it Custom. `hd_text` and `gapless_music` wait for the next start; `hide_keypad` applies at once. A filter that can't apply waits. The exact list of `apply_setting` calls is the one the file gets.
  - **Open, change, close with each device** on a dummy-driver window, with pushed SDL events through the frontend's routing:
    - F1 and Right: `scale_mode = fit`, and the Presenter's mode follows.
    - Guide, RB, RB and the stick: `hide_keypad = always`, on the `KeypadHider`.
    - A click on the Audio tab and on `<`: master volume 95, and the bus gain is 0.95.
    - A tap on the gear, the Video tab and `>`: border `#202020` on the Presenter.

    `settings.ini` then has those four values and not the one-run `music_volume = 50`, and it reads back equal.
  - `save_menu_changes` on a file with `preset = enhanced`, a comment and an unknown key: the menu's `hd_text = off` makes it custom, and the rest is kept byte for byte.
  - `live settings: each live row reaches its object`: every `kAtOnce` row applies through `apply_live_setting` (the filter only on a GPU renderer), and every other row returns false. vsync off reaches the `Desktop`, `Game::vsync` and the telemetry; `pause_unfocused` the `Lifecycle`; `render_scale` the frontend's callback.
  - `settings menu: the Saves actions …` (`save_tools_test.cpp`): the export writes the zip in `exports/` with every file; the import is refused with a character loaded (panel 0), from an empty `import/` folder and from a broken zip that sorts last (nothing changes), and otherwise imports the newest zip byte for byte; the backups line counts a slot's backups and the imports.
  - **Scripted runs.** `scripted_run` is true for each of the 11 options. A menu that isn't allowed takes no hotkey. F1, Guide, a held L3 + R3 on a virtual Xbox 360 pad (one stick alone doesn't) open it. Start, East, Esc, F1, Backspace and Guide close it.
  - The gear's place at 1080p (the right bar), 4:3 (the top bar), a phone with 47-point insets at 3× (inside the safe area, beside the image) and 960 × 640 (over the image). Scrolling by wheel and arrows; a press on one row and a release on another does nothing.
  - **The drawing, by hash:** Video `936a4ffac54dae67`, Controls `45510865f57dac53`, Assists `90ea01347aead46f`, the gear `a2a99d07b4fa4a0c`. The PNGs are in `build/test-output/<preset>/settings_menu/`.
  - `presets_test.cpp`: `border_color` and `right_touch` step through what they offer, and `check_registry` catches an offered value that doesn't read back (`#ffffff`, `black`).
- **Real assets (local, `ctest -L real_assets`):**
  - `play.game_options` starts from `play.save_quit`'s save. On the main menu the key pad has no panel, so Import saves may run and Game options isn't offered. In the field (Henesys, slot 0), Import saves is refused. `GameOptionsOpener` opens the popup menu on its option tab (game mode 1, `CDataMgr +0xB0` = 5). With the popup open it refuses, and B returns to the field.
  - `platform.device_golden`'s recorded renders match pixel for pixel. The remap overlay's real-asset run `input.remap_play` (F1 in a scripted run, `run_overlays` holding the turn) and every `play.*`, `menus.*` and `input.*` mode pass.
- **Not covered by an automated test** (a needs-human check on #89):
  - The interactive `run_overlays` loop with the settings menu in a real window, with real key, pad, mouse and touch input. Scripted runs never open the menu, by design.
  - The gear on an iPhone.

## Consequences

- A child adds a setting and gets its menu row, its tab (a new group's tab appears by itself), its label, help and values. To apply it live, it adds a case to `apply_live_setting` and sets `.applies = Applies::kAtOnce`.
- A child adds a button (#154's Achievements, #159's key pad editor, a reset) as a `kActions` row plus a `set_action` call in `game.cpp`'s `start_menu`.
- F1, Guide and L3 + R3 now open the settings menu. The remap overlay is its Controls → Bindings row, and it is still opened directly in scripted runs, as `remap_real_test` drives it. `bindings.ini` still can't bind F1 or Guide.
- `GameOptions` lost the copies `scale_mode`, `border_color`, `filter`, `mute_unfocused`, `right_touch`, `hide_keypad`, `rumble`, `rumble_strength` and `perf_overlay`; read `options.settings` instead.
- **How we'd know this was wrong:**
  - Players don't find the menu: then a first-run hint in the picker, or the gear on the desktop too.
  - The gear in the bars gets in the way on some layout: then hide it after a few seconds without a touch.
  - A live change misbehaves in the middle of the game: then that row goes back to `kNextStart`.
  - The game's own options menu doesn't open reliably from the tap: then the row only explains where to find it.

## Addendum (#105 wave 4 integration): the other children's rows and actions

- **Action rows.** `achievements` (Gameplay, #154): the first choice shows `achievement_summary` on the status line, and each further choice shows the next achievement (`achievements_status`: "2/7 Rookie (unlocked): …"), then the summary again. With `local_game_center` off it says so. `keypad_layout_edit` (Controls, #159): `request_keypad_editor`. If the game shows its pad, the menu closes and the editor opens at the same turn; otherwise it opens once the pad is shown. A Leaderboard row was left out. The local leaderboard (ADR 0084) is drawn while the game's GameKit controller waits, and the frontend's tick closes it when no controller is open. Opened from the menu in the field, the game would also run on behind a screen that takes all input.
- **Live rows.** `border` and `border_image` (#157) are now `kAtOnce`: `Presenter::set_border`. `keypad_layout` (#159): `install_keypad_layout` through `LiveTargets::keypad_layout`. `menu_repeat`, `confirm_guard` (#147), `focus_highlight`, `button_prompts` (#146), `game_speed`, `fast_forward` (#151) and `autosave` (#150) are read from `Game::settings` by the turn hook every turn (`LiveTargets::turn_reads_settings`). `game_speed` then writes the game's speed table and CMTimer's interval at the next turn. `hide_online_ui` and `local_game_center` (#154) stay `kNextStart`: their host overrides and hooks are installed at start-up.
- A key pad layout saved in the editor becomes `Game::settings.keypad_layout` too, so the menu shows it.
- `live settings: each live row reaches its object` covers the new cases. With no target, each of them waits.
