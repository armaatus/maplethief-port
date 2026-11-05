# 114. Controller and scaling polish: paging lists, the quantity popup's repeat, prompts at the window's pixels, the price line's place

- **Status:** Accepted
- **Date:** 2026-11-05
- **Issue:** #200 (epic #201)
- **Builds on:** [ADR 0051](0051-controller-focus-navigation.md) (the focus; #147's `MenuInput` repeat), [ADR 0101](0101-button-labels-on-the-field-key-pad.md) (the labels' layer), [ADR 0102](0102-controller-lists-and-the-character-select-hand-off.md) (the shop's list and the grids by the keys), [ADR 0103](0103-ui-scaling-window-text-veil-and-minimum-window.md) (the window text's frame)

## Context

ADRs 0101–0103 left four small gaps:

1. **Long lists move one row a press.** The shop's Buy and Sell lists and the popup menu's item and equipment grids (ADR 0102) move one row or cell per d-pad press. LB / RB switch the shop's tabs and RT is the shop's Charge (`'#'`, #127). Nothing pages. A hero's Sell list can reach 48 rows, and the equipment grid shows two rows at a time.
2. **The quantity popup doesn't repeat.** Popup 5 ("how many", over the shop) takes left / right ±1 and up / down ±10 in the original. `menu_repeat` (#147) repeats a held direction on panels 1, 5, 6 and 8 only, and a popup over the shop is panel 7. Choosing 37 potions takes 7 presses.
3. **Soft prompts at fractional scales.** `HintLayer` drew the prompts into a texture at the *rounded* pixels per point (2 at a 2.25x fit) and stretched it linearly over the image, so every glyph edge was resampled.
4. **The price line overlapped the picture's edge.** ADR 0103 puts the price line in the safe area's bottom-left corner. At 1920 × 1080 (the picture 3x, a 60-pixel bar below it) with a long enough line the box started in the bar and reached into the picture.

The ADR 0102 real-asset save also had an empty equipment tab, so the equipment grid's hook (`PopupMenu_EquipTabOnKey` `0x519F9`) was covered only by synthetic tests.

#185 (next wave) will give the right stick a gamepad pointer, so the right stick must stay free.

## Options considered

**The page binding.**

1. **The right stick, while a list owns the focus.** This clashes with #185's pointer, and a stick's push would need its own repeat and dead zone.
2. **The shoulders.** They are the shop's tabs, and on popups minus / plus.
3. **RT.** It is Charge in the shop and New on the character select.
4. **LT held as a modifier, with up / down. Chosen.** LT does nothing in menus (it is the cash shop in the field only), and a held trigger with the d-pad reads as "page". On the keyboard it is **Shift** with the arrows. It is a bindable port action, `page` (`MT_ACTION_PAGE`, after `auto_run`; the remap overlay and `bindings.ini` name it), which never reaches the guest, like `fast_forward`.

**Where the page happens.**

1. **N presses of the direction.** A 48-row list takes N × 2 game ticks, and a grid's rows in view are not the shop's 5.
2. **A key code of the port's own, through the hooked handlers.** The press goes to every state's key handler, and unknown codes are a risk there.
3. **The frontend writes the page into guest memory, as the hooks do. Chosen.** `MenuInput` (the focus's forward sink) sees a press of up or down while `Actions::holding(MT_ACTION_PAGE)`. It asks the page sink (`list_page(cpu, panel, dir, false)`) whether a list that pages owns the focus. If so, the direction's press and release are dropped and a page is queued. The page is applied at the first turn with the runtime's input queue empty, so it never jumps ahead of keys pressed before it, and only on the panel it was asked on. `list_page(..., true)` then moves the cursor and the drag with the same writes as ADR 0102's hooks, and runs `ResetSubTimers01` `0x386E1` when the cursor moved, as the key handlers do.

**What a page is.** The cursor moves by the rows in view: the shop's 5, or for a grid the rows wholly inside the widget's rect (`grid_rows_in_view`: the rect's height against the rows' pitch). The view moves by as much, then is clamped as the paint clamps it, and then moves only as far as the cursor's row needs to be wholly in view.

- On the shop's list, the cursor is clamped to the list (`shop_list_page`).
- On a grid, the cursor moves row by row with `grid_step`, stopping at the last row (`grid_page`, `grid_page_drag`).
- A page up on a grid's top row stays there. Only ADR 0102's plain up leaves the grid for the tab bar.
- With `menu_repeat` a held page repeats at the menu's timing. It stops when LT or the direction is let go, or when the list no longer owns the focus.
- Anywhere else LT and a direction is a plain direction.

**The quantity popup's repeat.** `menu_repeat_at(panel, popup)`: the menu panels as before, plus panel 7 while popup 5 is open (`kQuantityPopup`), at `MenuInput::kRepeatDelay` / `kRepeatEvery`. The repeat stops when the popup changes (bought: popup 111), as it stops when the panel changes. It stays under `menu_repeat` (off by default, the original) like every other menu repeat.

**Prompts at the window's pixels.**

1. **Keep the texture at the rounded scale and filter it nearest.** At 2.25x that drops or doubles whole glyph pixel rows and columns.
2. **A layer the placed image's size in pixels, drawn 1:1. Chosen.** Each glyph point is `prompt_unit` whole pixels (the picture's pixels per point, rounded, 1–8). Each prompt's backing is a box of whole pixels (`prompt_box`), placed as `prompt_left` places it in points but at the picture's own scale, and kept inside the image. `draw_prompt_layer` draws each prompt on its own small canvas and puts it over the layer ("over", premultiplied). `HintLayer` renders the layer at the image's rect with nearest filtering, which at 1:1 is no resampling. The glyphs are a little smaller than the picture's scale at fractional scales (unit 2 at 2.25x), and crisp.

**The price line's place.** `place_price_line(frame, picture, w, h)`:

1. The box goes at the bottom left of the strip of the safe area below the picture (the wide picture with widescreen), when it fits there whole at the frame's text scale.
2. Otherwise it goes at the bottom left of the part of the safe area the picture covers, shrinking (as `place_text_box` does) until it fits.

It is never across the picture's edge, and at 1920 × 1080 a two-line price line sits in the bar at 2x. **Rejected:** a smaller scale in the bar. It is less legible than the same text over the picture's corner, which the box's dark backing keeps readable.

## Decision

- **Runtime:** `MT_ACTION_PAGE` (`"page"`, "Page (with up / down)"), a port action.
- **Bindings:** LT in menus (`default_gamepad_bindings`, was none) and Left / Right Shift in menus (`default_keyboard_bindings`). Documented in [controls/keypad.md](../controls/keypad.md), [controls/keyboard.md](../controls/keyboard.md) and [controls/remapping.md](../controls/remapping.md).
- **`platform/src/list_keys.{h,cpp}`:** `shop_list_page`, `shop_list_page_key`, `grid_rows_in_view`, `grid_page`, `grid_page_drag`, `grid_page_key`, `list_page`, `page_stats`. ADR 0102's keys share the guest writes (`shop_list_move`, `grid_now`, `grid_set`).
- **`platform/src/menu_input.{h,cpp}`:**
  - The page: `PageSink`, `set_page_sink`, the queued pages, `Stats::pages`, `page_repeats`.
  - The quantity popup's repeat: `menu_repeat_at`, `kQuantityPopup`.
- **`platform/src/prompts.{h,cpp}`:** `prompt_unit`, `prompt_box`, `draw_prompt_layer`, and `HintLayer` at the image's pixels.
- **`platform/src/price_line.{h,cpp}`:** `price_line_size` and `place_price_line`. `draw_price_line` takes the picture's rect (`Presenter::wide_rect()`).

**Inputs consumed (for #185).** Only LT (as `page`, in menus) and, while it is held and a list owns the focus, the d-pad's or left stick's up / down. The right stick and its click are untouched.

## Consequences

- With a pad, LT + down pages the shop's list or a grid. A 48-row Sell list is 10 pages instead of 47 presses. LB / RB, RT and every plain direction are unchanged.
- With `menu_repeat` on, holding right in the "how many" popup counts up 1 + 1 every 0.15 s after 0.40 s.
- The prompts are crisp at every scale. At a fractional scale they are drawn at the rounded unit, up to a third of a point smaller or larger per glyph point than the picture's scale.
- The price line never straddles the picture's edge. When the bar below is too small it covers the picture's bottom-left corner, on its dark backing.
- How we'd know this is wrong: a page that leaves the cursor off screen or skips a row it shouldn't, LT doing something in a menu without a list, a quantity that keeps climbing after the popup closed, a prompt with blurred edges, or a price box cut by the picture's edge.

## Evidence

- **CI (synthetic), `platform_test`:**
  - `list keys: a page moves the shop's cursor by the rows in view, kept in view`: every list of 1–30 rows from every cursor and drag.
  - `list keys: a page on the shop's list on a guest, and from the frontend`.
  - `list keys: a page on a grid moves by the rows in view and keeps the cell in view`: every cell of a 30-cell grid, the drag clamped.
  - `menu input: LT with up or down pages a list that owns the focus, never the game`, `menu input: a held page repeats with menu_repeat, and stops with LT`.
  - `menu input: the quantity popup repeats a held direction with the menu's timing`: presses at 0, 0.40, 0.55, 0.70, 0.85 and 1.00 s; stops at popup 111; none for other popups or with `menu_repeat` off.
  - `ui scaling: the picture, the mapping, the gear and the text across the matrix`, which now also covers:
    - every prompt box: whole pixels, inside the image, unit = round(pixels per point);
    - the price line's box at text scales 1–8: whole pixels, inside the safe area, wholly below or wholly inside the picture.
    - The one configuration with no room (a 480 × 320 window at density 1 with a phone's insets, a 362-pixel safe area) is counted.
  - `ui scaling: the notice and the price line drawn at 1080p and inside a safe area`: in the bar without a notch, inside the picture with one.
  - `prompts: at a fractional scale the layer is the window's pixels, glyphs whole (ADR 0114)`: at 2.25x, the renderer's output equals the CPU layer over the image.
  - `keyboard: the default rows are docs/controls/keyboard.md's table` (34 rows).
- **Real assets, local:**
  - `input.menu_shop_scroll`:
    - With `menu_repeat` on, right held 0.8 s in popup 5 takes the amount from 1 to 5 (three repeats).
    - On the Sell tab (7 rows), LT + up from row 6 goes to row 1 with the drag at 0. LT + down goes back as `shop_list_page` has it, the row in view, and no up or down reaches the game.
    - Four pages, each moving the cursor.
    - LB goes back to Buy.
  - `input.menu_popup_grids`:
    - `equip_save` (a mode of `menu_nav_real_test`, run first by `menu_nav_test.cmake`) writes 30 equipment records into the copied save's `theif0` at test time; nothing derived is committed.
    - The equipment grid shows 30 cells. Right, down, left and up go through the hook.
    - LT + down pages two rows (8 cells) at a time to the last row, with the drag at −76, −152 and −224. LT + up goes back to the first row with the drag at 0, the grid still open.
    - 14 hook keys and 8 pages in all.
