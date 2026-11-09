# Controls

You can play with a mouse or touch screen, a keyboard, or a gamepad, and switch between them at any time. The iPhone's on-screen buttons fade out while you use a keyboard or gamepad, and come back when you click or touch the screen.

Every key and button can be changed: see [Changing the controls](#changing-the-controls).

## Mouse and touch screen

The left mouse button is your finger: click, drag and release on the game's screen, as on the iPhone. On a touch screen every finger counts, so you can hold the d-pad and attack at once.

Want a second "finger" on the mouse? Set `right_touch = 440,290` in [settings](settings.md) and the right mouse button presses the attack button.

On an iPhone, each press of an on-screen key gives a light tap you can feel. Make it stronger or turn it off in the settings menu (Controls → Key pad haptics: off, light or medium), or with `keypad_haptics = medium` in [settings.ini](settings.md#the-settings-file).

## Keyboard

| | In the game | In menus and popups |
|---|---|---|
| Move | Arrow keys | Arrow keys |
| Jump | X, Space or Alt | |
| Attack | Z or Ctrl | |
| Quick slots 1–4 (potions) | A, S, D, F (or 1, 2, 3, 4) | |
| OK | Enter | Enter, Z or Space |
| Back / close | | Esc, X or Backspace |
| Popup menu | Esc | |
| Map | M | M |
| Cash shop | C | |
| Previous / next tab | | Q / E |
| Revive (when you fall) | | R / G |
| New character (character select), the shop's Charge | | N |
| Page through a list (held, with up / down) | | Shift |

A new character has its Red Potions on A and Blue Potions on S.

The full list, with the reasons behind each key: [keyboard.md](../controls/keyboard.md). In the game, the settings menu's Controls → Controls card shows the controls in effect.

## Gamepad

Xbox, PlayStation, Nintendo Switch and most other controllers work. Plug it in before or while the game runs; the first one you press is the one that plays. Buttons are named here by their Xbox label; on a PlayStation pad the button in the same place does the same thing (A = Cross, B = Circle, X = Square, Y = Triangle).

| | In the game | In menus and popups |
|---|---|---|
| Move | D-pad or left stick | D-pad or left stick |
| Jump | A | A = OK |
| Attack | X or right trigger | X = revive (when you fall) |
| Quick slot 1 | Y | Y = grade revive (when you fall) |
| Quick slot 2 | B | B = back |
| Quick slots 3 and 4 | LB and RB | LB / RB = previous / next tab |
| Cash shop | Left trigger | Left trigger held, with up / down = page through a list |
| New character (character select), the shop's Charge | | Right trigger |
| Popup menu | Start | Start = back |
| Map | Back / View | Back / View |

On menus a yellow frame shows which button is selected: move it with the d-pad and press A.

The full list, by button: [gamepad defaults](../controls/keypad.md#gamepad-xbox-names-sdl3-names).

The controller rumbles when you're hit and when you level up. Turn it off or make it weaker in the settings menu (Controls), or with `rumble = off` / `rumble_strength = 50` in [settings.ini](settings.md#the-settings-file).

## Port shortcuts

| | Keyboard | Gamepad |
|---|---|---|
| Settings menu (pauses the game) | F1 | Guide button, or click both sticks |
| Fullscreen on / off | F11 or Alt+Enter (Mac: Cmd+Ctrl+F) | |
| Window at 1× … 5× the picture | Ctrl+1 … Ctrl+5 (Mac: Cmd+1 … Cmd+5) | |
| Screenshot | F12 | Share, or R3 + Back |
| Frame-time overlay | F3 | |
| Fast-forward, held (when turned on in settings) | Tab | R3 |
| Auto-run (when turned on in settings) | R | L3 |
| Quick-save and quick-load (when turned on in settings) | F5 and F9 | |

Screenshots go to the `screenshots` folder next to your [saves](saves.md#where-your-saves-are).

## Changing the controls

Open the settings menu (**F1**) → **Controls** → **Bindings**. Choose an action, press the new key or button, then **Save** or back out with Esc / B: both keep your changes (only **Cancel** throws them away). The screen shows the buttons of the controller you're using.

With a Nintendo controller, buttons go by position by default (the bottom button jumps). The Bindings screen's **Nintendo buttons** row, or `nintendo_buttons = label` in `bindings.ini`, makes the button labelled A jump instead.

More on remapping, including the `bindings.ini` file: [remapping.md](../controls/remapping.md). All the control pages: [Controls reference](../controls/README.md).

## Steam Deck

The Deck's controls work out of the box with Steam's Gamepad layout, and the touch screen works like the iPhone's. See [Linux and Steam Deck](linux-and-steam-deck.md#controls) for the button table and tips for the back grips.
