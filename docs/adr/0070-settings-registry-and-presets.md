# 70. A settings registry, presets (Original, Original + controller, Enhanced, Custom) and a first-launch picker

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #144 (epic #105)
- **Builds on:** [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (`settings.ini`, its format and its byte-for-byte rewrite), [ADR 0054](0054-bindings-file-remap-overlay-and-button-glyphs.md) (the port-drawn overlay, `Canvas`, the glyphs), [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (fixed-step runs)

## Context

M10 adds about 15 enhancements, each with a setting (#145–#159, #86, #87, #90, #150), and #89 builds a settings menu over all of them. In 2026-10-30 research on Ship of Harkinian, 2Ship2Harkinian and Zelda64Recomp, two patterns worked:

- **Presets named by audience, shown first.** One choice sets the whole experience.
- **Groups.** Enhancements (presentation and comfort) are kept apart from Assists and cheats (anything that changes gameplay), and a preset never turns an assist on.

Before this ADR, `settings.{h,cpp}` had a hand-written field per setting, a branch per key in `apply_setting`, a key list for `serialise_settings`, a comment block, and a hand-kept list of flags in `main.cpp`. So every new setting meant five edits. A preset needs to know every setting's value per preset, and the menu needs a label, help text and the values for every setting. Neither had anywhere to live.

What has to be decided:

- Where a setting's metadata lives, so a later child adds one row and nothing else.
- What a preset is in the file. Does it record a choice or apply one, and which wins when the file disagrees with it?
- What "no settings file" means. Today's players and every test run have none.
- Which settings a preset controls, and how Assists are kept off.
- When the first-launch picker shows, and how a test run avoids it.

## Options considered

**The metadata.**

1. **Keep the hand-written branches** and add a preset table beside them. That is two lists to keep in step, with no help for #89.
2. **X-macros.** One list, expanded several times. It is compact, but hard to read and to debug, and the value types (enums with names, ranges, colours, a custom pair) don't fit a macro list well.
3. **A constexpr table of rows** (`SettingInfo`, written with C++20 designated initialisers). Each row binds its field through small templates (`choice<&Settings::f>(names)`, `on_off<…>()`, `uint_range<…>(min, max, step)`, `color<…>()`, or `custom_value(parse, print, expect)`). Parsing, writing, the flags, the presets and the menu are loops over the rows.

**The preset in the file.**

1. **A one-shot "apply" that isn't stored** (SoH's buttons). The file then can't say which preset the player is on, so there is no "Custom", and the picker can't know whether anyone chose.
2. **`preset = …` stored, and the keys win.** A disagreeing key makes it `custom`. But hand-editing `preset = enhanced` in a file the port wrote then does nothing: every key still has the old preset's value, so it reads as custom.
3. **`preset = …` stored, and a real preset wins** over the keys it controls. A disagreeing line is reported and ignored, and `custom` keeps every line. Hand-editing the preset works, and the comment above the `preset` line says how to keep a value of your own. Changing a key through the port (the command line, #89's menu) moves the preset to `custom`.

**No settings file.**

1. **No file means Original.** `hide_keypad` would go from `auto` to `never` for every run without a file, including every test and the fixed-step goldens, and keyboard and pad players who never saw a picker would lose #71's auto-hide.
2. **No file means today's defaults (`Settings{}`), and no preset.** The picker is where a new player chooses. Scripted runs keep exactly what they had.

## Decision

### The registry

The registry is `platform/src/settings.{h,cpp}`. `settings_registry()` returns the table `kRegistry` in `settings.cpp`, one `SettingInfo` per setting, in the order a new file lists them:

```cpp
{.key = "hd_text",                       // lower case, digits, '_'
 .group = SettingGroup::kVideo,          // the menu's section
 .comment = "# draw the game's text textures at the render scale: on | off",  // '\n' between lines
 .value = on_off<&Settings::hd_text>(),  // the type, bound to its field
 .presets = {"off", "off", "on"},        // Original, Original + controller, Enhanced; or kNotInPresets
 .flag = "--hd-text",                    // "--" + the key with '-'; nullptr: no flag
 .game_only = true,                      // the flag is a game option
 .label = "HD text",                     // #89's label
 .help = "Draws the game's text at the render scale (needs a render scale of 2 or more)",
 .applies = Applies::kNextStart},        // kAtOnce once #89 applies it live
```

- **Value types (`ValueKind`):**
  - `kChoice`: an enum whose values are 0, 1, … in its name list (`kScaleModeNames` and the like). The `*_name` and `parse_*` functions now read the same lists.
  - `kOnOff`: on/off and their spellings.
  - `kUint`: a range of at most three digits, with a menu step.
  - `kColor`: `#RRGGBB`.
  - `kCustom`: its own parser, printer and "is not …" words. `right_touch` is one.
- **Messages.** A bad value's message is built from the type: `scale_mode: 'huge' is not integer, fit, smooth or sharp`, `… is not on or off`, `… is not an integer from 0 to 100`, `… is not #RRGGBB`. Every existing message is unchanged.
- **Groups (`SettingGroup`):** Video, Audio, Controls, Gameplay, Accessibility, **Assists**, Saves, Other. The Enhancements are the first five. Assists hold whatever changes gameplay (game speed, fast-forward, turbo, auto-run, auto-potion, cheats).
- **The menu's helpers (#89):**
  - `setting_text` (the value as the file writes it), `setting_expects` and `setting_choices` (a choice's names, off/on, or a range's steps).
  - `step_setting(settings, row, ±1)`, the menu's Left and Right. It wraps a choice or a switch and clamps a range.
  - `find_setting(key)` and `find_setting_flag("--hd-text")`.
- **The command line.** `main.cpp` has one branch for every row with a flag: `--music-volume 50` is the line `music_volume = 50`, checked by `apply_setting` and applied in command-line order. `--scale-mode`, `--border-color` and `--render-scale` now take that path too. Their values and effects are unchanged, and their error messages now use the file's wording.
- **Checks.**
  - A `static_assert` in `settings.cpp` (`registry_ok`): no missing fields; keys and flags unique; `preset` is not a row; preset columns all or none; every Assists row in the presets with the same value in each column.
  - `check_registry(rows)` in the tests adds:
    - keys in lower case;
    - comment lines that start with `#`;
    - flags that are `--` plus the key;
    - a label and help on every row;
    - every column a value the row reads back as the same text;
    - every Assists column the value a default `Settings` has (off / neutral).

  The test also runs `check_registry` on bad synthetic rows to show that it catches each mistake.
- **`GameOptions::settings`** carries every setting in effect into `run_game`. New settings are read from there. The older fields (`scale_mode`, `hide_keypad`, …) stay as copies until #89 makes them live.

### Adding a setting (the recipe for #145–#159, #86, #87, #90, #150)

1. Add its field to `Settings`. The default is today's behaviour, which is normally the Original column. If the default differs from Original, add the key to the test `presets: no settings file is today's defaults` and to the list under Defaults below, and say why.
2. Add its row to `kRegistry`. That one edit also gives you the file line, the comment, the flag, the presets and the menu entry.
3. Read it in the frontend from `options.settings` (`GameOptions`), or from `Settings` where it is applied.
4. Extend `presets_test.cpp`'s `kExpected` with the row's columns. That table is #89's, written out independently of the registry. The test fails until both agree.
5. A new enum needs a name list `kFooNames` in `settings.h`, in the enum's order.

What goes in the presets:

- **In the presets:** a value that is part of the experience. That is everything that differs between the columns or defines "original" (presentation, comfort, pacing, Gameplay fixes), and every Assist, which is neutral in every column.
- **Not in the presets (`kNotInPresets`):** personal values that stay the same in every preset. These are the volumes, strengths, dead zones and thresholds, bindings and layouts, Accessibility, language, paths, and Saves actions.

### The presets

- **`preset = original | original_controller | enhanced | custom`**, in `settings.ini`, and `--preset P` (a game option).
  - `apply_preset` sets every row in the presets to the preset's column and records the preset. Rows outside the presets keep their values. `custom` changes nothing but the label.
  - `apply_setting` (the command line, the menu) moves a preset to `custom` when it sets a key the presets control to a value other than the preset's. Setting the same value, or changing a key outside the presets, keeps the preset.
  - `matching_preset` and `preset_differences` tell #89's menu which preset a set of values equals.
- **In the file:**
  - A preset other than `custom` wins over the lines of the keys it controls, wherever the `preset` line is. The last good `preset` line counts. A line that disagrees is reported and ignored, for example `line 7: hd_text = off: preset = enhanced sets on, ignored (preset = custom keeps the file's values)`.
  - `custom` keeps every line.
  - Without a `preset` line, each line is `apply_setting`, as before.
  - The port writes `preset` first in a new file, after a two-line comment saying this, and appends it to an older file only once a preset is set.
  - A `preset` line in an old text while no preset is set becomes a comment, so `parse(serialise(s, t)) == s` still holds for every `t`.
- **On the command line,** `--preset` applies before every other setting flag. The other flags then make the preset `custom` when they change a key it controls.
- **The columns for today's keys** (#89's table; `presets_test.cpp`'s `kExpected`):

  | Group | Key | Original | Original + controller | Enhanced |
  |---|---|---|---|---|
  | Video | `scale_mode` | integer | integer | integer |
  | Video | `render_scale` | 1 | 1 | 1 (`auto` with #152) |
  | Video | `gfx_backend` | software | software | software (`gpu` after #131) |
  | Video | `filter` | none | none | none |
  | Video | `hd_text` | off | off | on |
  | Audio | `mute_unfocused` | on | on | on |
  | Audio | `gapless_music` | off | off | on |
  | Controls | `hide_keypad` | never | auto | auto |
  | Controls | `rumble` | on | on | on (Original and Original + controller: the game's vibration only, once #72's follow-up splits it) |
  | Video | `perf_overlay` (#137, ADR 0071) | off | off | off |
  | Assists | `cash_shop` (#156, ADR 0072; after `perf_overlay`) | off | off | off |
  | Video | `pacing` (#80, ADR 0075; after `cash_shop`) | display | display | display |
  | Video | `fullscreen` (#145, ADR 0077; after `save_backups`) | off | off | desktop |
  | Video | `vsync` (#145, ADR 0077) | on | on | on |
  | Gameplay | `pause_unfocused` (#145, ADR 0077) | off | on | on |
  | Gameplay | `fix_original_bugs` (#155, [re/original-bugs.md](../re/original-bugs.md); after `language`) | off | off | on |
  | Video | `texture_filter` (#153, ADR 0081; after `texture_pack`) | game | game | nearest |
  | Gameplay | `hide_online_ui` (#154, ADR 0084; after `texture_filter`) | off | on | on |
  | Gameplay | `local_game_center` (#154, ADR 0084; after `hide_online_ui`) | off | off | on |
  | Controls | `menu_repeat` (#147, ADR 0051; after `local_game_center`) | off | on | on |
  | Controls | `confirm_guard` (#147, ADR 0051; after `menu_repeat`) | off | on | on |
  | Controls | `focus_highlight` (#146, ADR 0051's addendum; after `confirm_guard`) | on | on | on |
  | Controls | `button_prompts` (#146; after `focus_highlight`) | off | on | on |
  | Video | `border` (#157, ADR 0085; after `keypad_layout`, then `border_image`) | color | color | blur |
  | Assists | `game_speed` (#151, ADR 0087; after `border_image`) | game | game | game |
  | Assists | `fast_forward` (#151, ADR 0087; after `game_speed`) | off | off | off |
  | Gameplay | `autosave` (#150, ADR 0089; after `fast_forward`) | off | off | on_quit |
  | Video | `widescreen` (#86, ADR 0090; after `autosave`, last in a new file) | off | off | auto |

  Not in the presets: `border_color`, `master_volume`, `music_volume`, `effects_volume`, `right_touch`, `rumble_strength`, `save_dir`, `save_backups` (#148), `language` (#145), `texture_pack` (#153), `keypad_layout` (#159, ADR 0086; after `button_prompts`: the player's own layout, `original` by default), `border_image` (#157; after `border`).

### The planned keys (#89's table), for the children that add them

These are proposed names. Each child settles its own key in its row. "–" means not in the presets.

| Group | Key(s) | Original | Original + controller | Enhanced | Issue |
|---|---|---|---|---|---|
| Video | `render_scale` gains `auto` | 1 | 1 | auto | #152 |
| Video | `gfx_backend` | software | software | gpu (after #131) | #120, #131 |
| Video | `texture_filter` | game | game | nearest at N > 1 | #153 |
| Video | `texture_pack` | none | none | none | #153 |
| Video | `border` (+ `border_image`) | color | color | blur | #157 |
| Video | `widescreen` | off | off | auto (ADR 0090) | #86 |
| Video | `frame_rate` | original | original | per #87's verdict | #87 |
| Video | `vsync` (`pacing` is in the table above, #80) | on | on | on | #145 |
| Video | `fullscreen` | off | off | desktop | #145 |
| Controls | `stick_mode` / `direction_priority` | 4way / game | 4way / game | 4way / last | #138 |
| Controls | `focus_highlight` / `button_prompts` | on / off | on / on | on / on | #146 |
| Controls | `menu_repeat` / `confirm_guard` | off / off | on / on | on / on | #147 |
| Controls | `rumble` splits by event (`rumble_events`) | game | game | game + level-up | #72 |
| Controls | `dead_zone` / `trigger_threshold` | – (0.30 / 0.5) | – | – | #67 |
| Controls | `keypad_layout` | original | original | custom (the player's layout itself: –) | #159 |
| Controls | bindings, glyph style | – (bindings.ini) | – | – | #70 |
| Gameplay | `pause_unfocused` | off | on | on | #145 |
| Gameplay | `autosave` | off | off | on_quit | #150 |
| Gameplay | `fix_original_bugs` | off | off | on | #155 |
| Gameplay | `fast_text` / `skip_intros` | off / off | off / off | per #90 / off (#174, ADR 0096) | #90, #174 |
| Accessibility | `colorblind_filter` / `screen_shake` | – (off / 100) | – | – | #158 |
| Accessibility | `flashes` | – (100) | – | – | #186 |
| Assists | `game_speed` / `fast_forward` | game / off | game / off | game / off (`game`: the game's own Speed option, ADR 0087) | #151 |
| Assists | `turbo_attack` / `auto_run` / `auto_potion` | off | off | off | #90 |
| Saves | export, import, backups, `save_dir` | – | – | – | #148 |
| Other | `language` | – (en) | – | – | #145 |

Assists are in the presets with the neutral value in every column, so applying any preset turns them off. They are never on through a preset, and the `static_assert` and `check_registry` enforce that.

### Defaults: what "no settings file" means

- **No settings file, or a file without a `preset` line, means today's defaults (`Settings{}`) and no preset (`Settings::preset` empty).** Every key is as the file or the defaults say, exactly as before #144. Every run without a file behaves byte for byte as before: the tests, the goldens, the fixed-step runs, the Linux game folder's first start.
- **The defaults are the Original column, with one difference: `hide_keypad = auto` (Original: `never`).**
  - `auto` has been the default since #71 (ADR 0052), and keyboard and pad players rely on it.
  - Original keeps the key pad shown because the iPhone always showed it.
  - `auto` only hides the pad after a key or pad button is used, and a touch shows it again. A run with touch and mouse input only shows exactly what Original shows.
  - So the defaults equalled the Original + controller column until #145. Since then they differ from it in the keys that are on there and off by default: `pause_unfocused`, `hide_online_ui`, `menu_repeat`, `confirm_guard` and `button_prompts` (the addendums). The test `presets: no settings file is today's defaults: Original but for hide_keypad` pins this list of differences.
- **`pacing = display` (#80, ADR 0075) is the default and the Original column, but it is not the behaviour from before #80.** A real-time run without a file now steps the guest clock at the display's refreshes. The game keeps its original 20 ticks a second, where before it ran about 1 % slow. Fixed-step runs (the tests, the goldens) never use it and stay byte for byte as before. `pacing = wall` is the old loop.
- **The first-launch picker is where a new player chooses.** It shows the preset lines to anyone who has no preset yet.

### The first-launch picker

`platform/src/preset_picker.{h,cpp}`, `PresetPicker` and `run_preset_picker`.

- **When it shows.** All of these must hold (`wants_preset_picker`):
  - the game runs (not the placeholder, `--gfx-prototype` or `--audio-test-tone`);
  - no preset after the file and the command line;
  - the run is interactive: none of `--frames`, `--seconds`, `--capture`, `--capture-at`, `--shot`, `--tap`/`--swipe`, `--fixed-step`, `--audio-capture`, `--minimise`, `--background` or `--quit`;
  - the video driver is not SDL's `dummy` or `offscreen`.

  So no `--shot`, `--capture`, golden, CTest or headless run ever sees it. `--preset` or a `preset` line skips it.
- **What it shows.** It runs before the game boots, in the window, through a `Presenter` of its own in the player's scale mode and border colour (so the safe area applies). It draws itself with `Canvas` and the glyphs of the pad in use, as the remap overlay does (ADR 0054), on a 480 × 320-point screen at 2× (port art only).
  - One line per preset: its title and a one-line summary (`preset_summary`, at most 64 characters).
  - Without a file: Original, Original + controller, Enhanced, with Original selected.
  - With an older file (no `preset` key): "Keep my settings" (custom) first and selected, so nothing changes by accident.
- **Input:**
  - The keyboard: Up and Down (and W and S) move; Enter, Space or 1–4 choose.
  - The pad: the d-pad or the left stick (an edge with a margin) moves; South or Start chooses. The pad is opened for the picker and closed after it; `Gamepads` enumerates the pads itself when the game starts.
  - The mouse: hovering selects a line, and a press and release on the same line chooses it.
  - A touch screen's finger: the same as the mouse. SDL's touch-made mouse events and a trackpad's fingers are ignored, as in `Input`.
  - Closing the window quits without writing anything.
- **After a choice,** `apply_preset`, then `save_settings` (the byte-for-byte rewrite of an existing file), then the game starts with those settings. If a preset changes what draws (`gfx_backend`, or a filter's need for the GPU renderer), the renderer is made again first. No column does that today; it is for #131's `gpu`.

## Evidence

- **CI (synthetic; `tests/cpp/presets_test.cpp`, `platform_test`, `presets: *` and `preset picker: *`):**
  - **Table-driven:** the registry equals `kExpected` (#89's columns written out). From the defaults and from settings with every key changed, each of the three presets sets every key it controls to exactly its column and leaves every other key unchanged. `apply_setting("preset", …)` gives the same result, and the three columns are pairwise different (`matching_preset`).
  - **One change makes it custom:** for each preset and each of the 16 rows, setting the same value keeps the preset, and another value makes it `custom` (only for rows in the presets) and changes nothing else. A bad value changes nothing. `step_setting` behaves the same, and `--preset` then a disagreeing flag gives `custom`.
  - **Assists:** `check_registry(settings_registry())` is empty, and every Assists row's columns equal the default value. Synthetic rows are caught: an assist that is on in Enhanced, an assist outside the presets, a duplicate key, a wrong flag, an unparsable column, an upper-case key, a comment line without `#`, a partial column and a missing label.
  - **The file:** a preset wins over disagreeing lines, which are reported, wherever its line is. `custom` keeps every line. The last good `preset` line counts, a bad one is reported, and a repeat is noted. A new file starts with `preset`, and each preset's file parses back with no problem. An older CR LF file gets `preset` appended with its own lines kept byte for byte. A rewrite over itself is identical, and a change of preset rewrites the lines in place. An orphan `preset` line becomes a comment.
  - **No file:** `preset_differences(Settings{}, original)` is exactly `hide_keypad`. Against Original + controller it is the exact list in the addendums (since #145), so `matching_preset(Settings{})` is none.
  - **The picker:**
    - `wants_preset_picker` is off for the placeholder, with a preset, under `dummy` and `offscreen`, and for each of 11 scripted options.
    - `PresetPicker` takes keys (moves, repeats, wrapping, Enter, number keys), the pad (d-pad, the stick's edge, South, Start, East ignored), the mouse (hover, press and release on the same line; not across lines, the gap or the bars) and a finger.
    - **The loop itself** (`run_preset_picker` on a dummy-driver 960 × 640 window) takes pushed events: keys (Enhanced), a mouse click (Original + controller), a finger tap (Keep my settings), the pad's South (Original), and `SDL_EVENT_QUIT` (nothing chosen, quit).
    - The drawing is checked by hash: first launch `549f3447437997b9`, and with "Keep my settings" and the third line selected `6fdbd8828b9bb18a`. The PNGs are in the test output directory.
  - The existing `settings: *` tests in `scaling_test.cpp` pass unchanged: the file format, the messages, the round trips, the byte-for-byte rewrite and the key order.
- **Local (real assets): `preset = original` is byte-identical to no settings file.** #91's regression suite doesn't exist yet, so a fixed-step run stands in for it, on this Mac with a Debug build of the recompiled game.
  - **The run:** the device-golden `fresh` run (ADR 0044, `tests/golden/device_screens.json`): `--fixed-step 1` with its 12 taps (the banner, Play, the intro's 5 speech boxes, MacArthur's 4 OKs, the cash shop popup), 60 virtual seconds. It took `--shot` every 0.25 s (240 shots, plus `--capture`'s first-lit read-backs: 242 PNGs, 106 distinct frames) and `--audio-capture`.
  - **Twice:** with an empty `MAPLETHIEF_DATA_DIR` (the log: `settings: no …/settings.ini: the defaults`, `input: hide_keypad auto`), and with a `settings.ini` of the single line `preset = original` (`settings: read … (preset original)`, `input: hide_keypad never`).
  - **Result:** all 242 PNGs are byte-identical (the SHA-256 lists are equal: `1064833330a9…`). The 60 s audio capture is byte-identical (`2fd3b6b25b5b…`). The save files the run wrote (`Documents/config`, `quest0`, `theif0`, `cupon`) are identical (`diff -r`).
  - **The picker in a real window** (Cocoa, Metal), with no settings file and no scripted option:
    - The log shows `presets: no preset chosen yet: the first-launch picker (no settings.ini)` before the boot, and the picker waited there.
    - A SIGTERM (which SDL turns into `SDL_EVENT_QUIT`) ended it with `presets: the window closed in the picker: nothing written`, and the data directory stayed empty.
    - This Mac's session gave no GUI access, so the run sent no real key, click, pad or touch input. The loop's handling of those is the CI test above. A hands-on check on the Mac and on an iPhone's touch screen is left to Joris.

## Consequences

- A later child adds a setting with its field and one row. Its flag, file line, comment, preset columns and menu entry come with it, and the tests fail until its columns match `kExpected`.
- **#89** groups `settings_registry()` by `group`. It shows `label`, `setting_text` and `help`, steps with `step_setting`, and applies a preset with `apply_preset` (or `apply_setting("preset", …)`). It writes the file with `save_settings`. It shows "Custom" when `preset` is custom, and Assists as a visibly separate section. For a live change, a row's `applies` becomes `kAtOnce` once the frontend applies it, and the old `GameOptions` copies move onto `options.settings`.
- Players with an older `settings.ini` see the picker once, with "Keep my settings" first. New players see it before the game starts. Scripted runs never see it.
- `--scale-mode`, `--border-color` and `--render-scale` now report bad values in the file's wording (`--scale-mode: scale_mode: 'x' is not integer, fit, smooth or sharp`). `--render-scale` takes at most three digits, as the file does.
- **How we'd know this was wrong:**
  - Players edit a preset-controlled key by hand and are confused when it is ignored. Then the file would switch to "keys win" (option 2), and the menu would become the place to change presets.
  - The defaults drift from the Original column as children add keys. Then "no file" should become Original outright, with the goldens re-recorded.
  - Assists turn out to need per-preset values (a gentler game speed in Enhanced). Then they are no longer Assists by this ADR's definition: they move to Gameplay.

## Addendum: #145's keys and the defaults (2026-10-30)

#145 added `fullscreen` (off / off / desktop), `vsync` (on / on / on), `pause_unfocused` (off / on / on) and `language` (not in the presets), each defaulting to today's behaviour ([ADR 0077](0077-desktop-conveniences.md)). The defaults are still the Original column but for `hide_keypad`, and no longer equal the Original + controller column, which has `pause_unfocused = on`: `matching_preset(Settings{})` is now none. The test `presets: no settings file is today's defaults` pins both differences.

## Addendum: #154, #147, #146 and #159's keys and the defaults (2026-10-31)

#154 added `hide_online_ui` (off / on / on) and `local_game_center` (off / off / on) ([ADR 0084](0084-offline-polish-hidden-online-ui-local-game-center.md)), #147 `menu_repeat` and `confirm_guard` (off / on / on each) and #146 `focus_highlight` (on / on / on) and `button_prompts` (off / on / on) (both in [ADR 0051](0051-controller-focus-navigation.md)'s addendums), and #159 `keypad_layout` (`original`, not in the presets: [ADR 0086](0086-customisable-touch-key-pad-layout.md)). Each defaults to its Original column. #146 had made `button_prompts` on by default to keep the defaults equal to the Original + controller column, but #145 had already parted them, so the rule above decides: off, and a run without a file shows no prompts, as before #146. The defaults now differ from Original only in `hide_keypad`, and from Original + controller in `pause_unfocused`, `hide_online_ui`, `menu_repeat`, `confirm_guard` and `button_prompts`, each on there and off by default. The test `presets: no settings file is today's defaults: Original but for hide_keypad` pins both lists; the preset-switch test computes the file's change in size from the registry's columns.

## Addendum: #157, #151 and #150's keys, and #89's live rows (2026-11-02)

#157 added `border` (color / color / blur) and `border_image` (not in the presets: [ADR 0085](0085-borders-around-the-picture.md)). #151 added the Assists `game_speed` (game in every column) and `fast_forward` (off in every column, [ADR 0087](0087-game-speed-and-fast-forward.md)). #150 added `autosave` (off / off / on_quit, [ADR 0089](0089-autosave-on-quit-and-background.md)). Each defaults to its Original column, which here is also its Original + controller column. So both pinned lists are unchanged. The defaults differ from Original only in `hide_keypad`, and from Original + controller in `pause_unfocused`, `hide_online_ui`, `menu_repeat`, `confirm_guard` and `button_prompts`. The preset-switch test still computes its delta from the registry. `autosave` is now last in a new file. With #89's menu merged, `border`, `border_image`, `keypad_layout`, `game_speed`, `fast_forward`, `autosave` and #146/#147's four rows are `Applies::kAtOnce` and apply through `apply_live_setting` ([ADR 0076](0076-settings-menu.md)'s addendum).

## Addendum: #86's key (2026-11-02)

#86 added `widescreen` (off / off / auto, Video, `Applies::kNextStart`: the GL layer's size is set before the boot; [ADR 0090](0090-widescreen.md)), last in a new file after `autosave`. It is off in Original and in Original + controller, as by default, so both pinned lists are unchanged (`hide_keypad`; `pause_unfocused`, `hide_online_ui`, `menu_repeat`, `confirm_guard`, `button_prompts`), and the preset-switch test's delta still comes from the registry.

## Addendum: #87's key (2026-11-02)

#87 added `frame_rate` (original / original / original, Video, `Applies::kAtOnce` through the turn hook; [ADR 0091](0091-frame-interpolation.md)'s verdict keeps it out of Enhanced), last in a new file after #86's `widescreen` and #158's `colorblind_filter` and `screen_shake`. Its default is every column's value, so both pinned lists are unchanged.

## Addendum: #168's key (2026-11-02)

#168 added `widescreen_hud` (edges in every column, Video, `Applies::kAtOnce`, `--widescreen-hud`; [ADR 0090](0090-widescreen.md)'s addendum), right after `widescreen` in the registry and in a new file. `edges` puts the touch key pad's groups and their icons at the wide view's edges, `narrow` keeps the pad and the HUD at 3:2. Its default is its Original column, so both pinned lists are unchanged. `game.cpp`'s turn hook reads it from the live Settings every turn.

## Addendum: #90's keys (2026-11-02)

#90 added three rows ([ADR 0092](0092-quality-of-life-assists-and-ui-animations.md)), last in the registry and in a new file, after #87's `frame_rate`:

- `ui_animations` (normal / normal / normal, Video, `--ui-animations`): the UIKit animations' speed. Normal is the game's own, so every preset keeps it.
- `turbo_attack` (off / off / off, Assists, `--turbo-attack`) and `auto_potion` (off / off / off, Assists, `--auto-potion off|25|50|75`): Assists are neutral in every preset, as this ADR's rule for Assists says.

All three are `Applies::kAtOnce` and game-only. `game.cpp`'s turn hook reads them from the live Settings every turn (`Assists::turn`, `follow_ui_animations`), so `apply_live_setting` marks them `turn_reads_settings`. Each default is every column's value, so both pinned lists are unchanged and the preset-switch test's delta still comes from the registry. #90 also added `MAPLETHIEF_SETTINGS="key = value; …"` (`apply_setting_list`): the game applies it after the settings file and `--preset` and before the other flags, and the test driver applies it at the boot, so a whole suite can run with a feature on.

## Addendum: `fast_text` dropped (2026-11-02)

The planned key `fast_text` (Gameplay, "per #90") is not added. #173 found nothing for it to change: the NPC dialogue and the scenes' speech box show each page's text whole on the tick it is set, and OK moves on in the tick it arrives; there is no reveal counter or minimum time ([re/qol.md](../re/qol.md#fast-text), `play.chat_text`). A toggle that changes nothing would only mislead, so the registry has no row for it and the presets are unchanged. `skip_intros` in the same table row is unaffected (cutscene skip is #174).

## Addendum: #175's key (2026-11-02)

#175 added `auto_loot` (off / off / off, Assists, `--auto-loot`, `Applies::kAtOnce`, game-only; [ADR 0097](0097-auto-loot-through-the-games-contact-test.md)), after `auto_potion`. It is a guest hook rather than a turn-hook read: `apply_live_setting` installs or removes it (`install_auto_loot`), `game.cpp` installs it at start. Its default is every column's value, so both pinned lists are unchanged.

## Addendum: `button_labels` (2026-11-04)

[ADR 0101](0101-button-labels-on-the-field-key-pad.md) added `button_labels` (on / on / on, Controls, `--button-labels`, `Applies::kAtOnce`, game-only), after `button_prompts`. `game.cpp` reads it from the live Settings at every present (`turn_reads_settings`). It is on in every column and by default: like `focus_highlight`, it only shows with a gamepad or the keyboard, which the original game never had. Its default is every column's value, so both pinned lists are unchanged.

## Addendum: #172's keys (2026-11-02)

#172 added two Assists ([ADR 0094](0094-controller-extras-auto-run-and-hold-or-toggle.md)), appended after #90's `auto_potion`: `auto_run` (off / off / off, `--auto-run`) and `hold_toggle` (off / off / off, `--hold-toggle off|walk|fast_forward|all`). Both are `Applies::kAtOnce` and game-only, and are read by `Assists::turn` from the live Settings every turn (`turn_reads_settings`). Each default is every column's value, so both pinned lists are unchanged.
