# 102. Controller lists: the shop and the popup menu grids by item, and Start after a chosen slot

- **Status:** Accepted
- **Date:** 2026-11-04
- **Issue:** none (Joris's report: "we cannot really start the game with the controller or scroll correctly in shops")
- **Builds on:** [ADR 0051](0051-controller-focus-navigation.md) (the focus, the menu repeat, #166's list follow), [ADR 0069](0069-guest-function-hooks.md) (guest hooks), [re/original-bugs.md](../re/original-bugs.md) (fixes the device never needs)

## Context

With a pad alone the game could be started and the shops reached (`input.menu_*`), but three things made it feel broken.

1. **Character select.** A on a slot (`-[ChooseCharacterView chooseCharacterOne:]` and the others) chooses it, and the focus stayed on the slot. Start (`clickStart:`) is in the column on the right, so reaching it took up to five d-pad presses (right × 3, up × 2), which is not obvious.
2. **The shop's list.** `Shop_OnKey` `0x46DB9` moves the tab's cursor (`shop +0x60 + 4 × tab`) by the key scroll's view row (`*(shop +0x114)[tab]`, +4 the first row), but `Shop_Paint` `0x56E91` rewrites that view every frame from the touch drag (`MarketKeyPadView`'s moved point + scroll point, clamped to [38 × (5 − rows), 0], first row = −drag / 38). The keys never move the drag, so past the third row the cursor sticks: in Mr. Wang's shop down went no further than row 2, and nothing scrolled.
3. **The popup menu's item and equipment grids.** `CDataMgr::keyPopupItemInven` `0x5285D` (cursor `CDataMgr +0xD8`, widget `*0xF14B8`) and the equipment tab's handler `0x519F9` (`+0xDC`, `*0xF14C0`) step a grid of 6 columns (down is +6) and move the key scroll `*(CDataMgr +0x10C)` past the rows they think are shown. The grids are drawn with 4 columns and scrolled by the widget's touch drag (+0x6C) only. Down from the first cell went to cell 6, an empty cell off the drawn row, and the drawn grid never scrolled to the cursor.

On the device none of these keys exists: panel 6 and panel 1 have no d-pad, and a touch picks a row or a cell directly. So the fixes only change what the port's own input does.

The skill and quest lists already follow the keys (#166); the quantity popup (popup 5, "how many") works with the d-pad in the original (left / right ±1, up / down ±10 within 1 and meso / price, at most 100); the intro, the cut scenes and a fresh install's start already work with A.

## Options considered

**Character select.**

1. **Put Start first in the focus order.** The slot still has to be chosen first, and the order would no longer match the screen.
2. **Move the focus to Start once the slot's touch has ended.** **Chosen.** A table in `focus.cpp` (`focus_after_confirm`) names the control the focus goes to after a confirm on a given action. Only the three slots have an entry, all `clickStart:`. The move happens on the first turn after the confirm's touch has ended (`Focus::follow_confirm`), only if Start is on screen and focusable, and it is logged and counted (`Stats::follows`). The flow becomes A, A.

**The shop's list.**

1. **Write the drag after the original.** The original's cursor is already wrong once it sticks, so it would have to be recomputed anyway.
2. **Replace the up / down keys.** **Chosen.** A hook (ADR 0069) on `Shop_OnKey` takes −1 and −2 when no popup is open (`0x108C64` = −1) on either tab. It moves the cursor one item, clamped to the list (the Buy rows `shop +0x98`, or the hero's Sell list, `CStateGame +0x30` → `+0xD4` or `+0xD8` by `shop +0x90`, up to its first empty entry, at most 48). It then moves the drag only as far as the cursor's row needs to be wholly in the five rows shown. The drag goes into `MarketKeyPadView`'s moved point, with the scroll point and `forceScrollValue` zeroed, so `Shop_Paint` draws it. The tab's key scroll is written as the paint would write it. As the original does after a key, it then calls `ResetSubTimers01` `0x386E1`. With no cursor yet (−1 on entry) the first key picks the first row wholly in view. Every other key runs the original, and if that changed the tab (left / right, the tabs' `'4'` / `'6'`) the drag goes back to the top, so the new tab's list starts at its first row.

**The grids.**

1. **Make the handlers think in 4 columns** by patching their constants. That means several sites in each handler, and the key scroll they move is still not drawn.
2. **Replace the directions.** **Chosen.** Hooks on `0x5285D` and `0x519F9` take −1 … −4 while the grid's widget is shown (+0xB1), it has cells in use (+8), and neither the shortcut panel (`CDataMgr +0xCC`) nor the quick slot choice (`+0xBC`) is open. They move over the cells as drawn (each cell's rect at +0x20, plus the list's offset +0x08 / +0x0C unless the cell is placed alone, +0x5C):
   - left and right go one cell;
   - up and down go to the nearest cell of the nearest row above or below, by its centre;
   - at an end the cursor stays;
   - up from the top row runs the original, which leaves the grid for the tab bar.
   The cursor is kept as the cell's index with the key scroll at 0. That is how OK (the original's) and the port's cell marker (`prompts.h`) read it. Then the drag follows the cursor's cell as #166's does: just far enough to be wholly inside the widget's rect (+0x20 y, +0x28 height), within the engine's clamp [+0x48 − +0x38, 0], with the fling (+0x74) zeroed.

**On or off.** The keys these hooks take come only from the port's input, so like the quest page fix (#167) they are on in every mode. The game installs them at start-up (`game.cpp`), and so do the scripted runs (`tests/cpp/game_driver.h`). A touch on a row or a cell is unchanged.

## Decision

- `platform/src/focus.{h,cpp}`: `focus_after_confirm`, `Focus::follow_confirm`, `Stats::follows`.
- `platform/src/list_keys.{h,cpp}`: `shop_list_step` / `shop_list_clamp` (the pure cursor and drag), `shop_list_key`, `shop_list_tab_changed`, `install_shop_list_keys`; `grid_step` / `grid_follow` (pure), `grid_key`, `install_grid_keys`.
- Three hooked functions: `Shop_OnKey` `0x46DB9`, `PopupMenu_EquipTabOnKey` `0x519F9` and `CDataMgr::keyPopupItemInven` `0x5285D` (`recomp/hooks.py`, `MT_HOOK_FN_*`, `k_hookable`).
- `mt_input_pad_view()` (runtime): the key pad's `TouchKeyPadView`, for its `marketKeyPadView` ivar (+60).

## Consequences

- From boot a pad reaches the field with A on Play, A on a slot, and A again on Start. The keyboard does the same with Enter.
- In the shops the d-pad walks the whole list on both tabs and the drawn list scrolls with it. Shoulders (LB / RB, `'4'` / `'6'`) switch the tabs, as before. Buying past row 5 buys the row shown.
- The item and equipment grids move as drawn, and the drag follows the cursor on a grid longer than its rect.
- With a touch, nothing changes. A touch drag after a key move starts from the drag the key left.
- The cursor values the hooks write are ones the original could hold, so saves and the original's OK are unaffected.

## Evidence

- CI (synthetic): `platform_test`.
  - `list keys: *` (`tests/cpp/list_keys_test.cpp`): the shop's step and clamp; its keys on a synthetic guest (cursor, drag, key scroll, the Sell list, a popup, a bad shop); the grids' steps and follow; their keys (rows of 4, the drag and fling, the key scroll folded in, the shortcut panel, assign mode, a hidden grid); the hooks' install and pass-through.
  - `focus: a chosen character slot hands the focus to Start (ADR 0102)`.
- Real assets (local):
  - `input.menu_shop_scroll`: with the hook off, five downs leave the cursor on row 2. With it on, down reaches row 5 with the drag at −38 and row 1 drawn first. The amount popup takes left / right / up / down. Two are bought. On the Sell tab (7 rows) RB starts from its top and down reaches the last row with the drag at −76.
  - `input.menu_popup_grids`: with the hooks off, down goes to cell 6 of 4. With them on, left and right walk the 4 cells and stay at the ends, and up goes back to the tab bar. The empty equipment grid is the original's.
  - `input.menu_characters`, `input.keyboard_menus` and every flow that loads slot 0: A (Enter) on the slot, then A (Enter) on Start.
