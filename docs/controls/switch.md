# Nintendo Switch controls

The defaults on the Nintendo Switch ([#239](https://github.com/armaatus/maplethief-port/issues/239), [ADR 0140](../adr/0140-switch-input-nintendo-layout-and-touch.md)). They work the same way however you hold the console: Joy-Con attached (handheld), a detached pair in the grip or in two hands, one Joy-Con on its side, or a Pro Controller. In handheld mode the touch screen works too. Every other platform's defaults are in [keypad.md](keypad.md#gamepad-xbox-names-sdl3-names) and [keyboard.md](keyboard.md).

Buttons are named by the letters printed on them. **A confirms and B goes back** in every menu, the game's and the port's. In the field the bottom button (B) jumps and the left one (Y) attacks, as on every other pad.

## The defaults

| Button | In the field | In menus |
|---|---|---|
| Left stick, d-pad | move, climb, enter a portal | move the cursor |
| B (bottom) | jump | back, cancel, no |
| Y (left) | attack | revive with an item (the death popup) |
| X (top) | quick slot 1 | the premium revive (the death popup) |
| A (right) | quick slot 2 | OK, yes, accept |
| ZL | quick slot 3 | held: up and down page a list |
| ZR | quick slot 4 | `#` (the shop's Charge, the character screen's New) |
| L | the map | minus, previous tab |
| R | the cash shop | plus, next tab |
| `-` | the game's menu (items, skills, options) | the map's view switch |
| `+` | the port's settings menu | the port's settings menu |
| Left stick click | auto-run (when `auto_run` is on) | |
| Right stick click | fast-forward, held (when `fast_forward` is on) | fast-forward |
| Right stick click held 0.3 s | a tap with the gamepad pointer, while it shows | the same |
| Right stick | the gamepad pointer (when `pad_pointer` is on) | the same |

HOME and Capture are the system's. Both stick clicks together also open the settings menu, as on every pad.

**One Joy-Con on its side** has four buttons, a stick, SL and SR, and one of `-` or `+`. Its buttons work by their place: the bottom one jumps (back in menus), the right one is quick slot 2 (OK), the left one attacks, the top one is quick slot 1. The stick moves. **SL and SR are quick slots 3 and 4** (ZL and ZR's place). **The stick's click opens the settings menu**, and its `-` or `+` opens the game's menu. It has no L or R: the map and the cash shop are on the key pad's touch keys, and menus' tabs move with left and right.

**To quit**: `+`, then **Quit** at the bottom of any tab of the settings menu, then confirm. HOME and closing the app from the HOME menu work as for any homebrew (the game hears it and, with `autosave = on_quit`, saves first).

## The touch screen

In handheld mode a finger is a finger, as on the iPhone ([ADR 0047](../adr/0047-touch-input-mouse-and-fingers.md)): tap the game's buttons, the key pad, the menus. Docked there is no touch screen.

The on-screen key pad starts **hidden**, because a controller is always attached on the Switch, and hides again while you use one. **Touch the screen and it shows** (that first touch already works). This is `hide_keypad = auto`, the Switch's value in every preset; `never` keeps the pad shown, `always` keeps it hidden.

## Button glyphs and prompts

Every controller shows Nintendo's buttons: A B X Y in their places, L R, ZL ZR, `-` `+`, LS RS. A Joy-Con on its side shows SL SR for its triggers, LS or RS (its stick's click) for `+`, and its own `-` or `+`. The glyphs follow the controller you use, and change when you attach or detach Joy-Con. The prompts on the game's buttons (`button_prompts`) and the remap overlay use them.

## Changing them

As everywhere: `+`, then **Controls → Bindings** ([remapping.md](remapping.md)). The overlay saves to `bindings.ini` in `sdmc:/switch/maplethief/data/`. `+` can't be bound, as Guide can't elsewhere.

The Switch's `bindings.ini` starts with `nintendo_buttons = label`: in the file, `button a` is the button labelled A, `button b` B, `button x` X, `button y` Y (on other platforms `a` is the bottom button). The defaults above are written that way: `jump = button b`, `attack = button y`, `ok = button a`, `back = button b`. If you switch the overlay's **Nintendo buttons** row to `position`, the same lines mean other buttons (`a` becomes the bottom one) and A and B swap roles. **Reset to defaults** brings everything back.

## Rumble

The controller you play with rumbles on the game's vibration, a hit and a level-up, at `rumble_strength` (Controls → Rumble strength), on both sides of a pair or a Pro Controller and in handheld mode ([ADR 0053](../adr/0053-observed-functions-game-events-and-rumble.md)). The Switch's HD rumble plays each effect at its two default frequencies.

## Several controllers

Player 1 is whichever controller presses a button first. Another one takes over when it presses while player 1 holds nothing ([ADR 0048](../adr/0048-gamepad-actions-and-delivery.md)). Attaching, detaching or pairing controllers while playing is fine: when nothing is held the game goes on, and the console shows its controller screen when it needs one (undocked).

## What SDL reports

What the game's SDL says of the Switch's controllers (read from its source; ADR 0140): eight pads, always there, all named `Switch Controller`, of the Standard type with Xbox labels. So the port reads the controller's mode from the console instead. The log (`sdmc:/switch/maplethief/data/logs/`) has one line per pad at the start and one whenever a controller changes, for example:

```text
input: pad 1: 'Switch Controller', SDL type standard (real standard), SDL labels S=A E=B W=X N=Y, rumble yes; libnx style 0x0 device 0x0 handheld 0x2: handheld, shown as Nintendo (joyconpair)
```
