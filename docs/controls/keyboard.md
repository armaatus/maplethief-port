# Keyboard controls

The port's default keys ([#68](https://github.com/armaatus/maplethief-port/issues/68), epic [#102](https://github.com/armaatus/maplethief-port/issues/102)). They follow the proposal in [keypad.md](keypad.md#keyboard), with one collision resolved ([below](#why-not-w-a-s-d)). Every action of the game's key pad has a key. The keys go through the same action layer as a gamepad ([ADR 0048](../adr/0048-gamepad-actions-and-delivery.md) and its #68 addendum). The table is `default_keyboard_bindings()` in `platform/src/actions.cpp`, and the SDL events become controls in `platform/src/keyboard.{h,cpp}`.

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
| Revive / grade revive | — | R / G (the death popup) | −25 / −35 |
| New character, shop Charge | — | N | `'#'` |
| Delete character | — | Delete (or M) | `'*'` |

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
| `R` | — | revive |
| `G` | — | grade_revive |
| `N` | — | slot4 |
<!-- END keyboard-defaults -->

Keys are matched by **keycode** (`SDL_Keycode`): the key with that label in the player's layout. On AZERTY, Z is the key where QWERTY has W, and A is where QWERTY has Q. The letters are the ones this page names, and a bindings file (#70) can move them. Keys pressed while Cmd (macOS) or the Windows key is held are left to the system (Cmd+Q, Cmd+M, Cmd+H), so Cmd+M does not open the map.

### Why not W A S D

keypad.md proposed W A S D as an alternative for moving, and A S D F for the quick slots. A, S and D cannot be both. The defaults keep **arrows for moving, Z / X next to them for attack and jump, and A S D F above those for the four quick slots**. That is the classic layout of MapleStory on the PC (arrows, a row of action keys under the left hand), and it puts all ten field actions under two hands without a chord. W A S D for moving would push jump, attack and the quick slots to the right hand (J K L and so on), which is a different layout, not an alternative. Players who want it can bind it in a bindings file (#70). The number keys 1–4 stay as the quick slots' second keys.

## Holding and repeating

A held key is **one press until it is released**, like a held finger on the key pad or a held pad button. SDL's key repeats (`event.key.repeat`) are ignored. The runtime turns held keys into the engine's per-tick key ([keypad.md, "Hold and repeat"](keypad.md#hold-and-repeat)): a held left or right walks every frame, a held attack attacks once, up / down climb until released. The game's own two-tick switch and chords apply (left + X is the tick's jump-left `'1'`), because the keys set the same key pad BOOLs a finger does. `input.keyboard_hold` (real assets) holds Left, Z and Right with their repeats once as keys and once as key pad touches, on the same turns: the hero is the same on every tick.

**Focus lost**, the window hidden or minimised: every held key is released. A key still held when the focus comes back counts again only when it is let go and pressed (its repeats are ignored), so the hero does not start walking by itself.

**Activity**: every key press that is not a repeat reaches `Input`'s activity listener as keyboard input, bound or not (ADR 0047). #71's key pad auto-hide uses it.

## Text entry

The game never asks for text, so the port starts no text input (SDL3's `SDL_StartTextInput` is never called, and letters are only keys):

- Creating a character asks only Yes or No. The hero is always Robin ([ANALYSIS.md](../ANALYSIS.md), #75).
- The binary imports no `UITextField` and no `UIAlertView` (which could carry a text field), and no `UIKeyboard` notification name or `becomeFirstResponder` selector occurs in it. Its only text class is `UITextView`: the About screen's eight text panels and the Korean terms (`-[MainMenuView clickAbout:]`, `-[TermsView initWithFrame:]`, [imports.md](../re/imports.md)). None of them can be edited. Each one gets `setEditable:`, and the runtime logs `-[UITextView setEditable:YES]: no text input: read-only` for a YES. `menus.help_about` and `menus.terms` (`MAPLETHIEF_LANGUAGE=ko`) open both screens, and their logs have no such line.
- `UIWebView` and `UITableView` belong to Chartboost, whose classes never run in the port (`+[Chartboost sharedChartboost]` answers nil, ADR 0034).

## Menus before the game

The main menu (`MainMenuView`) and the character screens (`ChooseCharacterView`) are UIKit views over the GL view, and the panel is −1 there (menu context). Their buttons send key codes to `CStateMainMenu::OnKeyPressed` `0x9462D` and also change the views. A key sends only the code. What that reaches, from `input.keyboard_menus` (real assets: a fresh install, then one and two slots, on one data directory):

| Keys | What happens |
|---|---|
| Enter on the main menu, fresh install | a new game in slot 0, as Play |
| Enter on the main menu, with saves | the engine's character select. `ChooseCharacterView` does **not** appear: only Play's touch (`-[MainMenuView clickPlay:]`) makes it |
| then Enter | Start: the game runs with a saved character |
| then N (`'#'`, New) | the engine's popup (panel 7) "create a new character?": Enter Yes (a new game in the next slot), Esc No |
| then Esc | back to the main menu |
| Delete or M (`'*'`), with a slot chosen | the delete alert: Enter Yes (the slot is deleted), Esc No |

Touch only, for #69 (keypad.md, "Touch-only actions" 4): choosing a slot (`chooseCharacterOne:` … `Three:`, needed before Delete), showing `ChooseCharacterView` at all, its Back button's removal of the view (Esc goes back in the engine but leaves the view drawn), the main menu's Help, About, Game Center, More Games, the promo banner and its close button, and the Korean terms' agreement button. The popup menu's shortcut-panel toggle and the item double tap and drag in its submenus are touch only as well.

## Evidence

- `tests/cpp/keyboard_test.cpp` (CI, every OS, in `platform_test`): this page's key table equals the code's; every default key presses and releases its action in the field and in a menu; the menu's confirm, back and arrows equal the gamepad's; every action has a key; repeats are ignored; Cmd / Windows key combinations are not pressed; focus loss, hiding and minimising release held keys; a release follows its press across a context change; key presses reach the activity listener once.
- `input.keyboard_play` (real assets): from a fresh install to the first map by Enter alone, then walking (Right, Left), jumping (X, Space), attacking (Z, Left Ctrl), Double Step (D, 3), the potions (S, A, 1, 2), the popup menu, map and cash shop (Esc, M, C), and focus loss during a walk. All of it is checked in the hero's guest state and the key pad's panel.
- `input.keyboard_hold` (real assets): held keys = held key pad touches, tick for tick.
- `input.keyboard_menus` (real assets): the main menu and character screens above.
