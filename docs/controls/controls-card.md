# The controls card

One page that shows how to play with the device you are using (#193, [ADR 0054's addendum](../adr/0054-bindings-file-remap-overlay-and-button-glyphs.md#addendum-the-controls-card-193-2026-11-05)). The game has no controls screen of its own. The card is the port's, drawn in code (shapes and the port's font), and is never in a screenshot or a golden of the game.

## Opening it

- **Settings menu → Controls → Controls card** (F1, Guide, L3 + R3 or the gear opens the menu). The card opens over the menu; closing it brings the menu back.
- **After the first-launch preset picker.** Once you pick a preset, a second step asks *Show controls* or *Start playing*. *Show controls* opens the card a few seconds into the game, once the main menu is up. It is offered only that once; the settings menu has it any time after.

It never opens in a scripted run (`--frames`, `--shot`, `--tap` and the like). While it is open the game is paused, as under the remap overlay. **Any key, click, tap or pad button closes it.** The press that closes it does nothing else.

## What it shows

The card follows the device you used last, as the button prompts do:

| Device | Picture | Lists |
|---|---|---|
| Keyboard (a key press) | a small keyboard, the keys bound in the field lit (keys bound only in menus in blue) | in the field and in menus: each action with up to two keys |
| Gamepad (a button, a stick push, a pad connecting) | a pad in its own glyphs: Xbox, PlayStation or Nintendo; the triggers and shoulders labelled with what they do in the field | the same lists with that pad's glyphs |
| Touch (a click or a tap) | the touch key pad as you laid it out (`keypad_layout`), each key named | none: the notes say what touch cannot do (fast-forward, auto-run) |

The lists are read from your bindings as they are now (`bindings.ini`, [remapping.md](remapping.md)): rebind attack to V and the card shows V. An action that has no control of your device there shows **unbound**; an action your device never had a control for by default (the pad's field OK) is left out. The four directions show as one *Move* line. *Settings* is F1 or Guide, which are never bound.
