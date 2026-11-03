# Remapping controls

How a player changes the port's keys and gamepad buttons ([#70](https://github.com/armaatus/maplethief-port/issues/70), epic [#102](https://github.com/armaatus/maplethief-port/issues/102), [ADR 0054](../adr/0054-bindings-file-remap-overlay-and-button-glyphs.md)). There are two ways: the **remap overlay** in the game, or the **bindings file** `bindings.ini`, which the overlay writes. The defaults and why they are what they are: [keyboard.md](keyboard.md) for keys, [ADR 0048](../adr/0048-gamepad-actions-and-delivery.md) for gamepads.

## The remap overlay

Open the settings menu ([ADR 0076](../adr/0076-settings-menu.md)): **F1** on the keyboard, or the **Guide** button (Xbox button, PS button, Home), or **both stick clicks** (L3 + R3) on a gamepad, on any screen, or the gear button on a touch screen. Then choose **Controls → Bindings**. (In a scripted run, where the menu never opens, those controls open the overlay directly.) The game pauses: it gets no run-loop turn until the overlay closes, as when an iPhone app is suspended. The music keeps playing. While the overlay is open, every key and button goes to it and none to the game.

| | Keyboard | Gamepad |
|---|---|---|
| Move | Up / Down, Page Up / Page Down | d-pad, left stick |
| Choose | Enter, Space | South (A, Cross; B on a Nintendo pad by position) |
| Change the Nintendo-buttons row | Left / Right, or choose | d-pad left / right, or choose |
| Close, keeping the changes (as Save) | Esc, F1 | East (B, Circle), Guide |

The list shows every action twice: first **in the field** (the game pad, panel 0), then **in menus** (every other screen). Each row shows the action's keys and its gamepad controls, drawn as the glyphs of the pad in use. Under the actions:

- **Nintendo buttons**: by position or by label ([below](#nintendo-face-buttons)).
- **Reset to defaults**: every action back to the defaults (still to be saved).
- **Save**: applies the changes, writes `bindings.ini` and closes (back to the settings menu). If the file can't be written, the new bindings still apply until the game quits, and the overlay says so.
- **Cancel**: closes; nothing changes. This is the only way to throw changes away: back (Esc, B) keeps them.

**To rebind**, choose an action and, within 5 seconds, press a key, a gamepad button, a trigger or a d-pad direction:

- A **key** becomes the action's only key in that context. Its gamepad controls stay.
- A **gamepad control** becomes its only gamepad control there. Its keys stay.
- A control that another action had in that context leaves it. The status line names that action, and warns if it has no key or gamepad control left.
- F1 or Guide stops waiting; after 5 s nothing changes.

F1 and Guide always open the settings menu (or the overlay, in a scripted run), so they can't be bound. The stick clicks can: a stick click bound to an action sends it, and pressing the other stick too still opens the menu.

## Button glyphs

The overlay draws each control as its button on the pad you use. The glyphs are the port's own drawings (plain shapes and a 5×7 pixel font): no console maker's art.

| Set | Pads (`SDL_GetGamepadType`) | South / East / West / North | Shoulders, triggers | Back, Start |
|---|---|---|---|---|
| Xbox | Xbox 360, Xbox One / Series, Steam, standard and unknown pads | A / B / X / Y | LB RB, LT RT | View, Menu |
| PlayStation | PS3, PS4, PS5 | cross / circle / square / triangle | L1 R1, L2 R2 | Share (PS4) or Create (PS5), Options |
| Nintendo | Switch Pro, Joy-Cons, GameCube adapter | B / A / Y / X | L R, ZL ZR | −, + |

Keys show as key caps ("Z", "Enter", "LCtrl"). The set follows the pad: a pad that connects, or one you press a button or move a stick on, shows its set at once. When that pad disconnects, the set falls back to the pad in use, else the last connected one, else Xbox.

## Nintendo face buttons

`nintendo_buttons` sets how a Nintendo pad's A B X Y map onto the game's buttons:

- **`position`** (the default): by place, as on every other pad. The bottom button (B on a Nintendo pad) is jump, as A is on an Xbox pad.
- **`label`**: by printed letter. The button labelled A (on the right) acts as an Xbox pad's A: jump. B is the bottom one, X the top and Y the left.

Other pads aren't affected. The glyphs follow the setting.

## Button prompts

With `button_prompts = on` (on in every preset but Original), the game's buttons on the menus and popups show the glyph of the control that presses them: OK and Cancel on a popup, the death popup's four choices, Accept and Refuse in the NPC chat, the shop's Buy, Sell, tabs, Charge and close, the map's buttons, New on the character select, and back on the screens the focus drives. They show the bindings in menus, so a remap shows at once.

The glyphs are those of the device you used last: a key press shows key caps (Enter for OK and Esc for back when they are bound, else the first key bound), a pad button or stick push shows that pad's set, and a mouse click or a touch hides the prompts. Unplugging the last pad hides them too. `focus_highlight = on` frames the focused button as soon as a pad or the keyboard is in use, and the chosen item in the popup menu's item and equipment lists, where the game draws no cursor. Neither is ever in a shot or a capture.

## The bindings file

`bindings.ini` lives in the data directory next to `settings.ini` and the saves: `~/Library/Application Support/MapleThief` on macOS, `%APPDATA%\MapleThief` on Windows, `~/.local/share/MapleThief` on Linux, or `MAPLETHIEF_DATA_DIR`. The game reads it at startup; with no file, the defaults apply.

- `--bindings FILE` reads `FILE` instead (and the overlay saves there).
- `--save-bindings` writes the bindings in effect, every action and the defaults included, to the bindings file before the game runs. Use it to get a full file to edit.

The overlay writes this file with the defaults unchanged:

```ini
# MapleThief bindings (docs/controls/remapping.md). One line per action and context:
#   action = control, control, ...   (or: action = none)
# Controls by SDL's names: key NAME (Z, Return, Left Ctrl, F5, Comma), button NAME (a b x y
# by the Xbox layout's positions, back start leftshoulder rightshoulder leftstick
# rightstick ...), trigger lefttrigger | righttrigger, direction up | down | left | right
# (the d-pad and the left stick). F1 and the gamepad's Guide button open the remap
# overlay and cannot be bound.

# A Nintendo pad's face buttons by position (SDL's way: the bottom one is 'a') or by
# label (its A is 'a'): position | label
nintendo_buttons = position

# In the field (the game's key pad)
[field]
up = direction up, key Up
down = direction down, key Down
left = direction left, key Left
right = direction right, key Right
jump = button a, key X, key Space, key Left Alt, key Right Alt
attack = button x, trigger righttrigger, key Z, key Left Ctrl, key Right Ctrl
slot1 = button y, key A, key 1
slot2 = button b, key S, key 2
slot3 = button leftshoulder, key D, key 3
slot4 = button rightshoulder, key F, key 4
ok = key Return, key Keypad Enter
menu = button start, key Escape
map = button back, key M
cash_shop = trigger lefttrigger, key C
fast_forward = button rightstick, key Tab
auto_run = button leftstick, key R

# In menus, popups and every other screen
[menu]
up = direction up, key Up
down = direction down, key Down
left = direction left, key Left
right = direction right, key Right
slot4 = trigger righttrigger, key N
ok = button a, key Space, key Z, key Return, key Keypad Enter
back = button b, button start, key X, key Escape, key Backspace
map = button back, key M, key Delete
minus = button leftshoulder, key Q
plus = button rightshoulder, key E
revive = button x, key R
grade_revive = button y, key G
fast_forward = button rightstick, key Tab
```

A file you write yourself needs only the lines it changes. An action without a line keeps its defaults, less any control that another line takes:

```ini
[field]
attack = key V, button leftstick
slot1 = key Q
```

### Format

- UTF-8 text (a byte order mark is skipped), lines ending in LF or CR LF, at most 64 KiB (a larger file is ignored and the defaults apply).
- Blank lines and lines starting with `#` are comments.
- `nintendo_buttons = position | label` may stand before the first section.
- `[field]` and `[menu]` (also `[menus]`) start a context.
- `action = control, control, …` gives the action exactly these controls in that context; `action = none` leaves it with none (with a warning). Names and controls are case-insensitive.
- **Actions**: `up down left right jump attack slot1 slot2 slot3 slot4 ok back menu map cash_shop minus plus revive grade_revive`, and the port's own `fast_forward` (held for `fast_forward = x2 | x3` in `settings.ini`, [ADR 0087](../adr/0087-game-speed-and-fast-forward.md); it does nothing while that is `off`) and `auto_run` (pressed, for `auto_run = on`, [ADR 0094](../adr/0094-controller-extras-auto-run-and-hold-or-toggle.md); it does nothing while that is `off`). Leaving either with no control gives no warning. Which of the game's actions do something in which context is the game's business: see [keyboard.md](keyboard.md).
- **Controls**, by SDL's names:

| Control | Names |
|---|---|
| `key NAME` | SDL key names: `Z`, `1`, `Return`, `Space`, `Left Ctrl`, `Right Alt`, `Escape`, `Backspace`, `F5`, `Keypad Enter`, `Up` … The comma key is `key Comma`. Keys are by the label in your layout (SDL keycodes), so `key Z` is the key marked Z on QWERTZ too. |
| `button NAME` | `a b x y` (positions: `a` is the bottom face button on every pad, see [above](#nintendo-face-buttons)), also `south east west north`; `back start leftshoulder rightshoulder leftstick rightstick misc1 paddle1 paddle2 paddle3 paddle4 touchpad` |
| `trigger NAME` | `lefttrigger righttrigger` (also `left right`) |
| `direction NAME` | `up down left right`: the d-pad and the left stick together |

- **Reserved**: `key F1` and `button guide` open the overlay and can't be bound. The d-pad is `direction`, not `button dpup`.

### Mistakes in the file

The game never refuses the file. Each problem is logged with the file and the line, the line or the one control is skipped, and the rest applies:

```text
bindings.ini:4: unknown key 'Nonsense': skipped
bindings.ini:7: unknown action 'atack'
bindings.ini:9: key F1 opens the remap overlay and cannot be bound: skipped
bindings.ini:12: attack in the field is already set on line 5: this line is skipped
bindings.ini:14: key Q is already bound to slot1 in the field on line 13: skipped here
bindings.ini: attack has no gamepad control in the field
```

- **Skipped lines**: unknown actions, malformed lines, sections other than `[field]` and `[menu]` (and their lines), an action listed twice in one section (the later line goes), a line left with no control.
- **Skipped controls**: unknown keys, buttons or kinds, a reserved control, a control that an earlier line in the section already gave to another action.
- **Warnings**: after the file, each action that had a key or a gamepad control by default and has none now.

## Where it lives in the code

`platform/src/bindings.{h,cpp}` (the file), `platform/src/remap.{h,cpp}` (the overlay), `platform/src/glyphs.{h,cpp}` and `platform/src/canvas.{h,cpp}` (glyphs, font, drawing), `FaceButtons` in `platform/src/gamepad.h`. Tests: `tests/cpp/bindings_test.cpp`, `tests/cpp/remap_test.cpp` (CI), `tests/cpp/remap_real_test.cpp` and `platform/remap_test.cmake` (real assets).
