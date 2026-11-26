# iPhone controls

On the iPhone you play by touch, as in the original game, or with a controller paired over Bluetooth. You can switch between them at any time. What a keyboard and gamepad do on a computer is in [Controls](controls.md); this page is about the phone.

## Touch

The game's own on-screen key pad works exactly as it did in 2010: the d-pad on the left, jump and attack at the bottom right, the four quick slots above them on the right edge, Map in the top left corner, and Bag and Cash in the top right. Every button of the game's menus is a tap.

- **Two fingers at once** work: hold the d-pad with one and jump or attack with the other. A diagonal jump is a finger on a d-pad arm plus one on jump.
- **Talk to someone** by touching them when you stand next to them, as on the original.
- **The black bars** beside the picture do nothing when touched, except for the gear.
- Each press of a key pad key gives a **light tap** you can feel. Settings menu → Controls → **Key pad haptics** sets it to off, light or medium (`keypad_haptics`).

The picture stays clear of the notch or Dynamic Island and the home indicator. The game runs in landscape, either way round: turn the phone over and the picture turns with it. A finger that is down while you turn ends its press, so the hero stops; touch again to go on.

## The settings menu: the gear

The **gear** beside the picture opens the settings menu and pauses the game. Tap a row to choose it, tap a tab to switch to it, and tap the close box to close the menu. With a controller, the Guide button or a click of both sticks opens it too. The menu is described in [Settings](settings.md#the-settings-menu).

## Moving the key pad

Settings menu → Controls → **Edit the touch key pad** lets you move and resize the key pad for your hands. The pad has three groups: the d-pad, jump and attack, and the quick slots.

1. Drag a group with one finger to move it. Pinch it with two to make it larger or smaller (50 to 200 %).
2. The bar's buttons set the size and the opacity of the selected group, and **Reset** puts it back where the game had it. **Reset all** resets every group.
3. **Save** keeps your layout; **Cancel** goes back to how it was.

The touch areas move with the keys, so a key works exactly where you see it. Map, Bag and Cash stay where they are. The layout is saved as `keypad_layout`; `original` is the game's own.

## A controller

Pair an Xbox, PlayStation or other MFi controller in the iPhone's Settings → Bluetooth, then start the game, or press a button on it while the game runs. The buttons are the same as on a computer: A jumps, X attacks, Y and B are the first two quick slots, Start opens the popup menu ([Controls → Gamepad](controls.md#gamepad)).

- The on-screen key pad fades out on your first press and comes back when you touch the screen (`hide_keypad = auto`). Settings menu → Controls → **Hide the key pad** sets it to always or never instead.
- Settings menu → Controls → **Bindings** remaps the buttons ([Changing the controls](controls.md#changing-the-controls)).
- The controller rumbles when you're hit and when you level up (Controls → **Rumble**).
- Turning the controller off while you walk stops the hero; turning the phone over doesn't interrupt a controller.

## Not checked yet

The port's iPhone build is checked on one iPhone 12 so far ([Playing on your iPhone](iphone.md)). Touch play reaches the first map there. Controllers, the haptics, both orientations and the key pad editor on a real phone are still to be confirmed ([#73](https://github.com/armaatus/maplethief-port/issues/73)). If something doesn't work as this page says, please [tell us](https://github.com/armaatus/maplethief-port/issues/new?template=bug.yml).
