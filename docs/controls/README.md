# Controls reference

The player's short version is [Controls](../play/controls.md). These pages are the reference behind it.

| Page | What it has |
|---|---|
| [Keyboard](keyboard.md) | Every default key, in the field and in menus, and the reasons behind them |
| [Gamepad defaults](keypad.md#gamepad-xbox-names-sdl3-names) | Every default button, by its Xbox and SDL3 name |
| [Nintendo Switch](switch.md) | The Switch's defaults (A confirms, `+` for the settings menu), each way of holding it, touch |
| [Remapping](remapping.md) | The remap overlay, the `bindings.ini` file, button glyphs, the gamepad pointer |
| [Controls card](controls-card.md) | The one-page card in the settings menu |
| [The on-screen key pad](keypad.md) | How touches become game actions, panel by panel |

The defaults live in `default_keyboard_bindings()` and `default_gamepad_bindings()` in `platform/src/actions.cpp` (the Switch's are a table of their own there). In the game, Settings → Controls → Controls card shows the controls in effect, and open issues about controls are in [Known issues](../KNOWN_ISSUES.md).
