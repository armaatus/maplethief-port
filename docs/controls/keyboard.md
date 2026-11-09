# Keyboard controls

The port's default keys ([#68](https://github.com/armaatus/maplethief-port/issues/68), epic [#102](https://github.com/armaatus/maplethief-port/issues/102)). The short version for players is [Controls](../play/controls.md). Every action of the game's key pad has a key. The keys go through the same action layer as a gamepad ([ADR 0048](../adr/0048-gamepad-actions-and-delivery.md) and its #68 addendum). The table is `default_keyboard_bindings()` in `platform/src/actions.cpp`, and the SDL events become controls in `platform/src/keyboard.{h,cpp}`. To change them, see [remapping.md](remapping.md): the settings menu (F1) → Controls → Bindings, or `bindings.ini`.

## The defaults

Two contexts, switched by the key pad's panel (`EAGLView.statusOfTouchKeyPad`): the **field** is panel 0, the game pad. **Menus** are every other panel (popup menu, cut scenes and speech boxes, NPC chat, shop, popups, map, cash shop, help) and the screens before the game.

| Action | Field | Menus | Game key code |
|---|---|---|---|
| Move / cursor | arrow keys | arrow keys | pad up / down / left / right (−1 / −2 / −3 / −4) |
| Jump | X, Space, Alt (either) | — | pad jump (`'2'`, with left / right `'1'` / `'3'`) |
| Attack | Z, Ctrl (either) | — | pad attack (`'5'`) |
| Quick slots 1–4 | A, S, D, F, or 1, 2, 3, 4 | — | pad short 1–4 (`'7'` `'9'` `'0'` `'#'`) |
| OK / confirm | Enter (also attacks: −5 is the field's OK) | Enter, Z, Space | −5 |
| Popup menu | Esc | — | −16 |
| Back / cancel / close | — | Esc, X, Backspace | −16 |
| Map | M | M (switch the map's view) | `'*'` |
| Cash shop | C | — | −26 |
| Minus / plus, previous / next tab | — | Q / E (or left / right) | `'4'` / `'6'` |
| Revive / grade revive; R also pick up on the popup menu's lists ([below](#pick-up-and-drop)) | — | R / G (the death popup) | −25 / −35 |
| Auto-run (the port's; only with `auto_run = on`, [ADR 0094](../adr/0094-controller-extras-auto-run-and-hold-or-toggle.md)) | R | — | — (never reaches the game) |
| New character, shop Charge | — | N | `'#'` |
| Page a list (the port's, held with up / down: the shop's list, the popup menu's item and equipment grids, [ADR 0114](../adr/0114-paging-lists-quantity-repeat-crisp-prompts-price-line.md)) | — | Shift (either) | — (never reaches the game) |
| Fast-forward (the port's, held; only with `fast_forward = x2 \| x3`, [ADR 0087](../adr/0087-game-speed-and-fast-forward.md)) | Tab | Tab | — (never reaches the game) |
| Quick-save / quick-load (the port's save states; only with `save_states = on`, [ADR 0105](../adr/0105-save-states-in-process-snapshot-at-safe-points.md), [below](#save-states)) | F5 / F9 | F5 / F9 (refused there, with the reason) | — (never reaches the game) |
| Delete character | — | Delete (or M); on the character select the focus drops it, the Delete button deletes ([ADR 0051](../adr/0051-controller-focus-navigation.md)) | `'*'` |

In menus, the confirm, back and arrow keys send the same actions as the gamepad's South (A, Cross), East (B, Circle) and d-pad. So the controller navigation of [#69](https://github.com/armaatus/maplethief-port/issues/69) gets keyboard input without its own rows. Esc in the field is the gamepad's Start (the popup menu). On the help pages (panel 10), left and right turn the page with a synthetic swipe, from either device ([ADR 0048](../adr/0048-gamepad-actions-and-delivery.md)).

Every key, by its SDL name (`SDL_GetKeyName`) and its actions by their names in `maplethief/input.h` (the names a bindings file of [#70](https://github.com/armaatus/maplethief-port/issues/70) uses). `tests/cpp/keyboard_test.cpp` checks that this table and `default_keyboard_bindings()` are the same, row for row:

<!-- BEGIN keyboard-defaults -->
| Key | Field | Menus |
|---|---|---|
| `Up` | up | up |
| `Down` | down | down |
| `Left` | left | left |
| `Right` | right | right |
| `X` | jump | back |
| `Space` | jump | ok |
| `Left Alt` | jump | — |
| `Right Alt` | jump | — |
| `Z` | attack | ok |
| `Left Ctrl` | attack | — |
| `Right Ctrl` | attack | — |
| `A` | slot1 | — |
| `S` | slot2 | — |
| `D` | slot3 | — |
| `F` | slot4 | — |
| `1` | slot1 | — |
| `2` | slot2 | — |
| `3` | slot3 | — |
| `4` | slot4 | — |
| `Return` | ok | ok |
| `Keypad Enter` | ok | ok |
| `Escape` | menu | back |
| `Backspace` | — | back |
| `M` | map | map |
| `Delete` | — | map |
| `C` | cash_shop | — |
| `Q` | — | minus |
| `E` | — | plus |
| `R` | auto_run | revive |
| `G` | — | grade_revive |
| `N` | — | slot4 |
| `Left Shift` | — | page |
| `Right Shift` | — | page |
| `Tab` | fast_forward | fast_forward |
| `F5` | quick_save | quick_save |
| `F9` | quick_load | quick_load |
<!-- END keyboard-defaults -->

Keys are matched by **keycode** (`SDL_Keycode`): the key with that label in the player's layout. On AZERTY, Z is the key where QWERTY has W, and A is where QWERTY has Q. The letters are the ones this page names, and a bindings file (#70) can move them. Keys pressed while Cmd (macOS) or the Windows key is held are left to the system (Cmd+Q, Cmd+M, Cmd+H), so Cmd+M does not open the map.

### The port's own keys

These are the port's, not the game's: they never reach the bindings and can't be bound. The port's fast-forward key (Tab), auto-run key (R), page keys (Shift, above) and save-state keys (F5, F9) are the exception: they are the bindable actions `fast_forward`, `auto_run`, `page`, `quick_save` and `quick_load`. The window's size keys are bindable too, in `bindings.ini`'s `[window]` section.

| Key | What it does |
|---|---|
| F1 | opens the settings menu ([ADR 0076](../adr/0076-settings-menu.md)); in a scripted run, the remap overlay ([remapping.md](remapping.md)) |
| F3 | shows or hides the frame-time overlay ([ADR 0071](../adr/0071-frame-telemetry-and-frame-time-overlay.md)) |
| F11, Alt+Enter, Cmd+Ctrl+F | fullscreen on or off; written to `settings.ini` as `fullscreen` ([ADR 0077](../adr/0077-desktop-conveniences.md)). On a Mac keyboard F11 is the system's Show Desktop unless Fn is held. On macOS with `fullscreen_mode = desktop`, Cmd+Ctrl+F is the Window menu's own Toggle Full Screen, which the setting follows ([ADR 0099](../adr/0099-fullscreen-modes-and-the-first-start-crash.md)) |
| Ctrl+1 … Ctrl+5 (Cmd+1 … Cmd+5 on macOS) | the window at exactly 1 … 5 times the game's picture (480 × 320 points, 568 × 320 with widescreen), centred on its display; the largest size that fits it; out of fullscreen first ([ADR 0077's addendum](../adr/0077-desktop-conveniences.md#addendum-window-size-keys-and-the-fullscreen-display-196-2026-11-05)). Bindable in `bindings.ini`'s `[window]` ([remapping.md](remapping.md#the-windows-size-keys)). On Windows and Linux Ctrl is also the field's attack: in the field, the Ctrl press attacks once before the size key |
| F12 | a screenshot of the game's screen to `screenshots/` in the data directory (on a Mac keyboard, with Fn). A gamepad's Share / Capture button, or Back while the right stick is held in, does the same |

The mouse cursor hides while the keyboard or a gamepad is used and comes back when the mouse moves. In menus the mouse wheel moves a row up or down, as the arrow keys do.

### Save states

`save_states = on` (Settings, Assists; off in every preset, [ADR 0105](../adr/0105-save-states-in-process-snapshot-at-safe-points.md)) makes F5 a quick-save and F9 a quick-load. A pad has no button for them by default; bind `quick_save` and `quick_load` in the remap overlay or `bindings.ini` to give it one.

- **What a state is.** The whole running game at that instant: the map, the hero, the monsters and their timers, the music. F9 puts the game back exactly there, in the same run. Each character slot has one state, `states/slot0.state` to `slot2.state` in the data directory; F5 replaces it.
- **It is not the game's save.** A state never writes the game's save files (`theif<N>`, `quest<N>`) and they never hold a state. The game still saves as it always did. After F9, the game's next save (yours, or the autosave) writes whatever is loaded, as if you had loaded an older save: what happened after the state is gone from that character.
- **When.** Only in the field with nothing open: not in a menu, a shop, a chat or a popup, not during a scene, not while the screen moves. F5 also needs every other key let go (a walking hero is refused: "let go of the keys first"). Otherwise a short notice says why nothing happened ("Not saved: not in a menu"). F9 also refuses a state of another copy of the game, of another version of the port, or of another character, and a damaged file; nothing changes then.
- The port's menus pause the game and take every key, so F5 and F9 do nothing while one is open.

### Why not W A S D

An early design had W A S D as an alternative for moving, and A S D F for the quick slots. A, S and D cannot be both. The defaults keep **arrows for moving, Z / X next to them for attack and jump, and A S D F above those for the four quick slots**. That is the classic layout of MapleStory on the PC (arrows, a row of action keys under the left hand), and it puts all ten field actions under two hands without a chord. W A S D for moving would push jump, attack and the quick slots to the right hand (J K L and so on), which is a different layout, not an alternative. Players who want it can bind it in a bindings file (#70). The number keys 1–4 stay as the quick slots' second keys.

## Pick up and drop

Items and skills get onto the quick slots, and items into the trash, with **pick up and drop** ([ADR 0129](../adr/0129-pick-up-and-drop-focus-mode.md)): on the popup menu's item, equipment or skill tab, press **R** (a pad's **West** button: X on Xbox, Square on PlayStation) on the list's cursor. The port touches the item's cell as a finger would. Then the arrows move a highlight over the quick slots (up / down, starting on quick slot 1) and the trash (left from the quick slots, right back to them), **Enter** (pad **South**) drops it there, and **Esc** / **X** (pad **East**) cancels. Glyph prompts show the pick up beside the cursor and the drop beside the spot; back's glyph stays at the close button.

- A quick slot that already holds that item is **cleared**: the game's own toggle.
- Only consumable items and skills go on a quick slot; other items do nothing there, as with a finger.
- The trash asks to confirm (the game's popup); Enter throws the whole stack away, Esc keeps it.
- The mode takes nothing until R is pressed, and a press with no cursor still goes to the game.

## Holding and repeating

A held key is **one press until it is released**, like a held finger on the key pad or a held pad button. SDL's key repeats (`event.key.repeat`) are ignored. The runtime turns held keys into the engine's per-tick key ([keypad.md, "Hold and repeat"](keypad.md#hold-and-repeat)): a held left or right walks every frame, a held attack attacks once, up / down climb until released. The game's own two-tick switch and chords apply (left + X is the tick's jump-left `'1'`), because the keys set the same key pad BOOLs a finger does. `input.keyboard_hold` (real assets) holds Left, Z and Right with their repeats once as keys and once as key pad touches, on the same turns: the hero is the same on every tick. `input.chords` does the same for two-button moves (jump left and right, jump then attack, hold attack, the double-tap run, climbing), as two fingers, by the pad and by the keys, with the default `direction_priority = game` ([ADR 0128](../adr/0128-direction-and-chord-input.md)). Mashing a key plays one tap every second tick, at most 2 ticks late, and a finger dragged over the key pad does not make the hero stop while a key stays down.

**Two directions at once** (`direction_priority`, [ADR 0128](../adr/0128-direction-and-chord-input.md)): `game` (Original, Original + controller, and no settings file) leaves both held, and the game's fixed order picks one (left and right before up and down), so Left held and then Up walks on. `last` (Enhanced) lets only the newest held direction through: rolling from Left to Up at a ladder climbs, and letting Up go brings Left back. In menus two directions pressed within 80 ms are one move, by either setting.

**Focus lost**, the window hidden or minimised: every held key is released. A key still held when the focus comes back counts again only when it is let go and pressed (its repeats are ignored), so the hero does not start walking by itself.

**Activity**: every key press that is not a repeat reaches `Input`'s activity listener as keyboard input, bound or not (ADR 0047). #71's key pad auto-hide uses it.

## Text entry

The game never asks for text, so the port starts no text input (SDL3's `SDL_StartTextInput` is never called, and letters are only keys):

- Creating a character asks only Yes or No. The hero is always Robin ([ANALYSIS.md](../ANALYSIS.md), #75).
- The binary imports no `UITextField` and no `UIAlertView` (which could carry a text field), and no `UIKeyboard` notification name or `becomeFirstResponder` selector occurs in it. Its only text class is `UITextView`: the About screen's eight text panels and the Korean terms (`-[MainMenuView clickAbout:]`, `-[TermsView initWithFrame:]`, [imports.md](../re/imports.md)). None of them can be edited. Each one gets `setEditable:`, and the runtime logs `-[UITextView setEditable:YES]: no text input: read-only` for a YES. `menus.help_about` and `menus.terms` (`MAPLETHIEF_LANGUAGE=ko`) open both screens, and their logs have no such line.
- `UIWebView` and `UITableView` belong to Chartboost, whose classes never run in the port (`+[Chartboost sharedChartboost]` answers nil, ADR 0034).

## Menus before the game

The main menu (`MainMenuView`) and the character screens (`ChooseCharacterView`) are UIKit views over the GL view, and the panel is −1 there (menu context). Their buttons send key codes to `CStateMainMenu::OnKeyPressed` `0x9462D` and also change the views. There the keys drive the controller focus ([ADR 0051](../adr/0051-controller-focus-navigation.md), #69) as a gamepad's d-pad, A and B do: the arrows move a highlighted focus over the buttons, Enter (Z, Space) presses the focused one with a touch, Esc (X, Backspace) presses the screen's close, Back or No. The other keys still send their codes, except Delete / M (`'*'`), which the focus drops: on the character select `'*'` starts the engine's delete with nothing on the screen, and the next −5 (Start too) would confirm it. From `input.keyboard_menus` (real assets: a fresh install, then one and two slots, on one data directory):

| Keys | What happens |
|---|---|
| Enter on the main menu, fresh install | Play (focused first): a new game in slot 0 |
| Enter on the main menu, with saves | Play: `ChooseCharacterView`, the focus on the first slot |
| then Esc | its Back: the main menu, the view gone |
| then Enter, the arrows, Enter | a slot, then (right ×3, up ×2) Start: the game runs with that character |
| then N (`'#'`, New) | the engine's popup (panel 7) asks whether to make a new character: Enter Yes (a new game in the next slot), Esc No |
| a slot, then the arrows to Delete and Enter | the delete alert, the focus on No: Enter keeps the slot; Left, Enter (Yes) deletes it |
| Delete or M (`'*'`) | nothing (dropped by the focus) |
| Enter as the window opens, in the first quarter second | nothing: the engine isn't at its main menu yet, and Play would end the game ([#122](https://github.com/armaatus/maplethief-port/issues/122)), so the focus drops it (#129); press again once the menu is there |

Every screen above and the main menu's Help, About (its links and text scroll), Game Center, More Games, the promo banner and its close, and the Korean terms' agreement button are reachable by keys through the focus. The popup menu's tabs open with Enter (a touch on the tab, ADR 0051). The quick slots and the trash are reached with [pick up and drop](#pick-up-and-drop) (R on an item or skill, the arrows, Enter). A list that has more to show scrolls when the arrows reach its end (the focus's drag, ADR 0051). The shortcut-panel toggle is unreachable in this version (keypad.md).

## Evidence

- `tests/cpp/keyboard_test.cpp` (CI, every OS, in `platform_test`): this page's key table equals the code's; every default key presses and releases its action in the field and in a menu; the menu's confirm, back and arrows equal the gamepad's; every action but the gamepad pointer's tap (`pointer_tap`, [ADR 0107](../adr/0107-gamepad-pointer.md): the mouse is the keyboard's pointer) has a key; repeats are ignored; Cmd / Windows key combinations are not pressed; focus loss, hiding and minimising release held keys; a release follows its press across a context change; key presses reach the activity listener once.
- `input.keyboard_play` (real assets): from a fresh install to the first map by Enter alone, then walking (Right, Left), jumping (X, Space), attacking (Z, Left Ctrl), Double Step (D, 3), the potions (S, A, 1, 2), the popup menu, map and cash shop (Esc, M, C), and focus loss during a walk. All of it is checked in the hero's guest state and the key pad's panel.
- `input.keyboard_hold` (real assets): held keys = held key pad touches, tick for tick.
- `input.chords`, `input.chords_mash`, `input.chords_mixed` (real assets): chords as two fingers = by the keys, the 20 Hz mash and a finger over the pad.
- `input.keyboard_menus` (real assets): the main menu and character screens above, plus Enter at 0.05 s on a fresh install (dropped; Enter at 3.5 s is Play). `input.menu_early` does the same with a virtual pad's A, and `platform.early_tap` checks that a tap that early still ends the game, as on the device.
