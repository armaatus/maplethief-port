# Controls

The original game is played on a touch key pad drawn over the bottom and the sides of the 480 by 320 landscape screen. Underneath, the game is a port of a keypad phone game: every touch becomes one of the old phone's key codes, so the pad sends at most one key per tick.

## The field key pad

| Control | Where | What it does |
|---|---|---|
| Direction pad | bottom left | walk left and right, climb up and down, talk (up, near an NPC) |
| Jump | bottom right, left of attack | jump; with a direction held, jump that way |
| Attack | bottom right corner | use the current skill; the attack key shows the weapon's icon |
| Four short keys | right edge, above attack | the quick slots: potions or skills, with their counts |
| Map | top left corner | the map screens (world map and area map) |
| Bag | top right | the popup menu: items, equipment, skills, quests and options |
| Cash | top right corner | the [cash shop](mechanics/cash-shop.html) |

The direction pad has no diagonals: a finger is always on one arm or none, so a jump to the side takes two fingers, one on the arm and one on jump. When several keys are held, attack wins over the short keys, the short keys over the jumps and the jumps over plain walking. Switching from one key to another takes two ticks.

Touching an NPC near the hero also talks to it, like up on the direction pad.

**Example:** holding right and tapping jump sends a jump to the right; holding attack at the same time sends only the attack.

## Other screens

- **Conversations and scenes:** a tap on the speech box area moves the text on.
- **NPC conversations:** buttons for the quest rows and for accepting, refusing and confirming.
- **Shops:** buy and sell tabs, a charge button for stars, and a list that scrolls by dragging.
- **Popups:** OK, yes and no, plus and minus for amounts, and the revive buttons of the death popup.
- **The popup menu:** its tabs and lists are touched directly.
- **Help:** paged help, opened from the options.

## Quirks of the original

- A finger that slides off a button never sends the button's release.
- An interrupted touch (an incoming call, a system gesture) leaves its key held until the next touch.
- Only two fingers are tracked; a third still presses its key, which then stays held until a finger moves.

## In the port

The port maps every one of these actions to keyboard keys and controller buttons, and can move or hide the pad ([the project's key pad notes](https://github.com/armaatus/maplethief-port/blob/main/docs/controls/keypad.md)).

## Sources

`-[TouchKeyPadView initKeyPadType0:]` `0xBA6A1` builds the field pad; the hit areas come from the colour mask `KeyPadArea.png`, read by `colorAtPixel:onImage:` `0xB4DB5`; `SkyCanvas::Tick` `0x2590D` turns the held keys into one key code a tick by fixed priority; the touch handlers are `touchesBegan:` `0xB80F1`, `touchesMoved:` `0xB6BB5`, `touchesEnded:` `0xB5CF1` and the empty `touchesCancelled:` `0xB6BA5`; the quick slots are drawn by `QuickSlots_Paint` `0x4CAED`.
