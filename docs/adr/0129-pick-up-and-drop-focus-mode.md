# 129. Pick up and drop: a focus mode that moves items and skills to the quick slots and the trash

- **Status:** Accepted
- **Date:** 2026-11-09
- **Issue:** #140
- **Builds on:** [ADR 0051](0051-controller-focus-navigation.md) (the focus, its touches), [ADR 0048](0048-gamepad-actions-and-delivery.md) (actions), facts in [docs/controls/keypad.md](../controls/keypad.md#gl-touch-widgets-popup-menu)

## Context

Putting an item or skill on a quick slot, and throwing an item away, were touch-only: `TouchWidget_OnEvent` `0xA0EAD` stores touch events for `CDataMgr`'s handlers, and the keys have no way in. A gamepad-only or keyboard-only player could not do either, which left a gap in the M7 exit criterion (every action with only a gamepad, #140).

What the game expects, read from `TouchWidget_HandleTouch` `0xA18E5` and the handlers (and checked by running the synthesised touches against real assets):

- A cell of the item list is selected (`+0x14` of its widget) by a touch's **up** on that cell. A drag moves nothing and selects nothing, so "down on the item, move, up on the slot" does not assign anything new (it re-assigns whatever an earlier tap selected).
- A quick slot's id 12 acts on touch-**up** (`0x4E228`), a trash's id 13 on touch-**down** (`0x487F4`, popup 20 asks to confirm). The selection stays while the finger leaves the list.
- An assign of the same item clears the slot (the game's toggle; `0x6398C`).
- One `CDataMgr` event is stored at a time (`+0x4`...`+0x20`), run by the next `CDataMgr::Update`, so a down and an up within one tick lose the down.

## Decision

**A `carry` mode in `Focus`, entered with `MT_ACTION_REVIVE` on the popup menu (panel 1)**, on the item, equipment or skill tab with a list cursor:

- **The entry control.** The menus' `revive` (West on a pad, R on the keyboard) has no meaning on panel 1: it is the death popup's. No new action or bindings row. On panel 1 the press is decided at the next turn, like OK's: a pick up if there is a cursor, otherwise it goes on to the game as its key, so a press with no list behaves as before. The `original` preset's play is unchanged: nothing is taken until the player enters the mode (the regress digest stays).
- **Pick up: a touch on the cell**, its centre (the cursor's cell, `read_popup_menu`): the same up that selects as a finger's tap. It is held `kCarryHold` (0.1 s) so the game's single event slot runs each phase. Picking up the same cell again within 0.4 s makes no second touch: a double tap would use the item (`HandleTouch`'s 300 ms).
- **Carrying.** The focus moves over the quick slot and trash cells read from guest memory (`popup_menu_drop_spots`), starting on quick slot 1: up / down along the slots by y, left to the trash, right back (`carry_next`). OK touches the spot (down, then up after `kCarryHold`); back cancels; closing the tab or the menu ends the mode. Directions, OK, back and revive are taken while carrying.
- **Prompts** (`popup_menu_prompts`): the pick up's glyph beside the list cursor, OK's beside the spot while carrying, back at the close; the highlight marker follows the spot.

Rejected: synthesising the drag literally (down, moves, up), because the game's up on the slot needs a selection the drag does not make (above); a confirm (OK) as the pick up, because OK already chooses and uses an item (`-5` twice) and must keep doing so; a new bindable action, which needs a free button in the menu column and adds rows to the bindings, overlay and docs for nothing the existing control cannot do.

## Consequences

`input.carry_pad` and `input.carry_keys` (real assets) show the gamepad-only and keyboard-only paths: a Red Potion on quick slot 1, used from it in the field (one fewer potion, HP up), and by the keys a stack thrown away through the trash. Unit tests in `focus_test.cpp` and `prompts_test.cpp` hold the mode's state machine and prompts.

Skills use the same path (the game's up on a slot reads the skill list's selection). The equipment tab's items can be thrown away too.
