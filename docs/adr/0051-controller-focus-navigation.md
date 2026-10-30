# 51. Controller navigation: a focus over the reachable UIKit controls, confirmed by touches

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #69
- **Builds on:** [ADR 0048](0048-gamepad-actions-and-delivery.md) (actions, contexts, touch fallbacks), [ADR 0047](0047-touch-input-mouse-and-fingers.md) (`Input::touch`, virtual fingers), [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (the UIKit view tree, the hit test, the compositor), [ADR 0049](0049-menus-online-buttons-language-and-menu-flows.md) (the main menu, terms and character screens); facts in [docs/controls/keypad.md](../controls/keypad.md)

## Context

#67 gave every key-pad panel its actions: in the field, NPC chat (panel 5), the shop (6), popups (7), the map (8) and the popup menu (1), every button sends a key code, so OK, back and the directions already reach the game. What is left is what has **no key code** ([keypad.md, "Touch-only actions"](../controls/keypad.md#touch-only-actions)):

- **The shell's UIKit screens** with no key pad panel (`statusOfTouchKeyPad` −1): the main menu (Help, About and its four links, Game Center, More Games, the banner and its close), the Korean terms (the agreement button, the paging help), `ChooseCharacterView` (the slots, New, Delete, Back, Start) and its `UIAlertView`-like alerts. #75 drives them with taps.
- **The cash shop** (panel 9): eight purchase tiles, each its own key code (−27 … −34) but no key to choose between them.
- **The popup menu's GL touch widgets** (panel 1): the tab key (`Menu_OnKey` `0x53B5D`) opens a tab, but its list stays empty: the widgets that draw and drive the item, equip and skill lists are only enabled by a touch on the tab (`TouchWidget_OnEvent` `0xA0EAD`, case 1).
- **The shortcut-key toggle** (`TouchWidget_OnEvent` case 11, value 1, "shortkey open/close").

The issue asks for a focus model over the buttons, a non-invasive highlight, confirm / back / tabs, lists scrolled by the stick, and the engine popups' yes / no / OK.

What had to be decided:

- **What the focus moves over**, and how the frontend learns it.
- **How confirm reaches a control**: call its target-action, or touch it.
- **When the focus is active**, given that most panels already have keys.
- **Where the highlight is drawn.**
- **The popup menu's tabs and the shortcut toggle.**

## Options considered

What the focus moves over:

1. **Per-screen tables of buttons** (Play at (46, 148, 120, 48), ...). Exact, but every screen, state and language needs its own table, and an alert or a sliding view breaks it.
2. **The live UIKit tree, filtered by what a touch reaches. Chosen.** The runtime already hit-tests every touch; the same rules (hidden, alpha, `userInteractionEnabled`, `enabled`, the superviews' bounds, what lies in front) decide what is focusable. New screens and alerts need nothing.

How confirm reaches a control:

1. **Send the control's actions** (`sendActionsForControlEvents:`). Skips the touch path: no highlight, no touch-down action, no `touchesBegan:` of views that track touches themselves, and the game's sounds in `click…:` / `release…:` pairs would play out of order.
2. **A touch at the control's point, through `Input::touch`. Chosen.** The same `UITouch` path as a finger, so the control highlights, sends touch-down and touch-up-inside and plays its sounds as on a tap. The point is one the hit test reaches, so the touch lands on that control.

When the focus is active:

1. **Everywhere there are controls.** The key pad's panels are UIKit buttons too: a focus there would compete with #67's keys (the d-pad would move a highlight instead of the hero's menu cursor).
2. **Only where no key pad panel drives the screen (−1) and on the cash shop (9). Chosen.** Every other panel keeps #67's keys; engine popups (panel 7: the New alert's popup 19, "cannot be used right now", save, death) keep their −5 / −16, which are already their yes / OK and no / cancel.

## Decision

**The runtime: `mt_uikit_controls` (`maplethief/uikit.h`, `runtime/src/uikit/focus.cpp`).** Lists what a controller can reach, in tree order (windows back to front): every `UIButton` / `UIControl` with an action, and every scroll view that can scroll or page. A view counts if it and every superview are shown (not hidden, alpha ≥ 0.01), take touches, lie in a window and on the screen, and a touch reaches it: its rect is clipped by every superview's bounds and the screen (the hit test's rule), and its point is the centre, else the first point of a 3×3 grid over the rect where `hit_test_screen` (the touch path's hit test, `touch.cpp`) finds it or a subview. Each entry carries the action a tap sends (touch-up-inside first), its target's class, and a scroll view's offset, largest offset and page. Model values: a view sliding in counts at its destination. It knows nothing of the game.

**The frontend: `Focus` (`platform/src/focus.{h,cpp}`)**, the sink of `Actions` (`Focus::attach`):

- **Active** while the panel is −1 or 9 (`focus_panel`) and something focusable is reachable. While active it takes the directions, OK and back, and drops map (`'*'`): on the character select that key starts the engine's delete with nothing on the screen, and the next −5 (Start's too) would confirm it, so the Delete button is the way. Every other action still goes to the game as its key code (the keyboard's N for New, #68). While inactive everything goes on to `mt_input_press` / `mt_input_release` as before. A release goes where its press went (counted per action), so a press taken by the focus is never released into the game.
- **Directions** (`focus_next`): the nearest control that way, scored by the distance along the axis plus twice the gap across it (so a control in line wins over a nearer one off to the side); a candidate's centre must lie at least a point that way and its near edge not behind the focused one's. With no control that way, the innermost scroll view under the focus that can still move that way (else the largest one) is dragged by a touch (`focus_scroll`, `focus_scroll_drag`: 60 % of its size, or three quarters of a page for a paging one, over 8 turns). A held direction repeats after 0.40 s, every 0.15 s.
- **OK** is a touch at the focused control's point (virtual finger `0x1030`): down on the press, up on the release, at least one turn later. **Back** is the same touch (`0x1031`) on the screen's best back control by `back_rank`: Help's and About's close and the delete alert's No, then the "choose a character first" alert's OK, then Back and the key pad's Clear, then the banner's close, then any other selector containing "close" or "cancel". With none, back does nothing. Drags use `0x1032`–`0x1035`.
- **The focus choice** (`Focus::refresh`, every turn): kept on its control while it stays reachable. A control that went out of reach under a screen gets the focus back when it returns (8 are remembered), unless something newer with a default appeared in front of it. When controls appear, the best default by `default_rank` takes it: the terms' agreement, the delete alert's No, the other alert's OK, Help's and About's close, the first slot, the cash shop's first tile, Play. Else the first new control in reading order, else the nearest one.
- **The highlight** (`focus_highlight`, `Presenter::draw_highlight`): a two-pixel dark frame and a yellow (#FFD400) one, two screen points wide, around the focused rect mapped through the window placement and clipped to the image. It is drawn into the **window only**, after the frame is presented to the read-backs and shots, so `--shot`, `--shot-gl`, the device goldens and the compositor's screen never contain it. It shows once an action has been used since the focus became active (a mouse player never sees it).
- Trace channel `focus` lists the targets whenever they change; `focus: on/off`, moves and touches are logged.

**The popup menu (panel 1).** OK with no tab open (`CDataMgr +0xA8` 0) on a tab with contents (`+0xB0` 1–5) is a touch on that tab (`popup_menu_tab_point`: 54-point tabs from x 75, y 277), which enables its widgets as a finger does. Inside an open tab the keys work: the d-pad moves the cursor, OK chooses and uses an item, back closes the tab. The status tab and every other key stay the key path. `CDataMgr` is read from the global `0x11A984` where `CDataMgr::GetInstance` `0x5BAC1` keeps it.

**The shortcut toggle is not reachable on this version.** A dump of all 11 widgets' items on every tab shows no item with id 11 and value 1 (nor value 9): the toggle's code exists but nothing registers its item, so no touch reaches it either. There is nothing to bind.

## Consequences

- Every listed screen can be completed by a pad alone; the per-screen evidence is `input.menu_main`, `input.menu_terms`, `input.menu_characters` and `input.menu_game` (real assets, `tests/cpp/menu_nav_real_test.cpp`, a virtual Xbox 360 pad, no taps), which check the buttons' actions in order, the files written and the guest's game mode, popup and menu state.
- `tests/cpp/focus_test.cpp` (CI) checks the navigation, scrolling, tables, panels, tab points and the highlight on synthetic targets; the routing, the focus choice, repeat and the popup menu's OK on scripted ones; `mt_uikit_controls` on a synthetic view tree (shown, hidden, faded, disabled, covered, clipped, scroll views); and the whole chain into a synthetic main menu.
- The focus follows any UIKit screen without tables. The selector tables only choose defaults and back buttons; an unknown screen still navigates, with the first control in reading order and "close"/"cancel" as back.
- The cash shop and the NPC shop are not reachable from a fresh game within a scripted run (the tutorial answers the cash button with popup 0x25B; the first shop is past the tutorial). The cash shop's tiles are panel 9 controls like any other (`focus_panel(9)`), and its close is the key pad's Clear (back rank 2); a run that reaches them needs a save past the tutorial (#127).
- The focus reads the view tree every turn (a few hundred views at most); it is not measurable next to a frame.

## Addendum: no confirm before the engine's main menu (#129, 2026-10-30)

The `MainMenuView` is up from the first turn, and Play is focused first, but the engine enters `CStateMainMenu` only on `CStateTitle`'s 5th paint (250 ms at the default speed). Play released before that ends the game: `ChooseCharacterView`'s `drawRect:` reads the still empty `robinImages` and raises `NSRangeException` from `0xA6589` ([ANALYSIS.md, #122](../ANALYSIS.md#program-flow)). A tap does the same on the device. There, though, nothing is drawn at Play's place yet, so only a blind tap gets there. The focus made this easy: Enter or A pressed as the window opens hit it.

- **The rule.** While the engine's current state is a `CStateTitle`, `Focus` drops a confirm. It takes the press and its release, sends nothing to the game, makes no touch, logs `focus: confirm dropped: the engine is not at its main menu yet` and counts it in `Stats::early`. A dropped press doesn't count as using the focus, so the highlight stays off. Directions, back and the other actions work as before. A press held across the change to the main menu stays dropped, and its release does nothing.
- **Dropped, not held.** A held confirm would open the character select at 0.25 s, before the menu has slid in (about 1.3 s), over a screen the player hasn't seen. Pressing again works as soon as the engine is at its main menu.
- **Reading the state.** `focus_engine_at_title` reads `SkyCanvas` from `0x11A950` (`SkyCanvas::GetInstance` `0x2567D`), its current state at `+0xC`, and that state's vtable pointer. It compares the pointer with `CStateTitle`'s `0x119C98`: the vtable at `0x119C90`, past its offset-to-top and `type_info`. Its slot 9 is `CStateTitle::Paint` `0x981B9`. It is a `Focus` source like the panel, the targets and the popup menu (`set_title_source`, for tests). In a guest address space, it reads only when a mapped segment holds `0x11A950`. Without a canvas or a state, and in every other state, nothing changes. That includes `CStateGame`'s cash shop (panel 9).
- **Touches are left alone.** A finger, mouse or `--tap` goes through `Input` to UIKit as before. A tap released before 0.25 s still ends the game, as on the device.
- **Evidence.**
  - CI: `tests/cpp/focus_test.cpp` covers the guard on a scripted state and the reader on synthetic guest memory and on a guest address space with and without a mapped segment.
  - Real assets, virtual clock at 1 ms per turn: `input.keyboard_menus`, step `early` (Enter at 0.05 s), and `input.menu_early` (the virtual pad's A at 0.05 s). Each confirms the engine is in `CStateTitle` at 0.04 s, that the confirm is dropped with no exception, and that Enter or A at 3.5 s is then Play (slot 0 written). `platform.early_tap` (`MapleThief --fixed-step 1 --tap 0.1,106,165`) still exits with status 5 and the `objectAtIndex:` line from `0x000A6589`.
  - Without the guard, the `early` step ends at turn 153 with that exception.
