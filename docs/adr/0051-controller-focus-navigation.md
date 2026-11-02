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

## Addendum: the shops past the tutorial (#127, 2026-10-30)

- **The panel after a popup.** `disableAlertKey` sets `statusOfTouchKeyPad` to −1 when it closes a popup, also over the shop, which never sets it back: panel 6 stays shown and live. With −1 the focus took over and the d-pad moved a frame over Buy, the tab and Close instead of the list. `mt_input_panel` now reports, while the status is −1 and the key pad is shown, the first shown panel container 1–10 but 7 (`shown_panel`, `runtime/src/input/input.cpp`). The menus with no key pad (the main menu, the character screens) keep −1, because the key pad view is hidden there.
- **Charge.** The shop's Charge button is the only way to `'#'` on panel 6, and the focus does not run there. The right trigger, unused in menus, sends `'#'` (`MT_ACTION_SLOT4`), as the keyboard's N does (#68); on the character select it is New. With nothing to charge (no stars, or a full stack) the shop ignores it.
- **Evidence.** `input.menu_shops` (real assets) loads a post-tutorial save and goes by the pad alone through Mr. Wang's shop (both tabs, a Red Potion bought: 15 meso and one more potion in guest memory, a stack of stars bought, RT on it), the cash shop (LT, the focus over four tiles, B closes, nothing bought) and Humanoid's quest offer, refused (B) and then accepted (A), checking the panel, game mode, popup and quest state at each step. Its fixture `input.menu_shops_save` makes the save in two launches on one data directory: a new game through the tutorial to Stan, saved, then Elder's Advice (five Red Snails), saved. The flows are tasks that wait for guest state rather than fixed times (`tests/cpp/guest_state.h`). The cash shop is not reachable on 16 maps (the tutorial's and a few others, `CStateGame::OnKeyPressed` `0x8D4C9`), Henesys is not one of them.

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

## Addendum: menu repeat and a confirm guard for popups (#147, 2026-10-30)

Two gaps that the focus left on the panels where the game's own keys drive the screen.

- The popup menu (panel 1), NPC chat (5), the shop (6) and the map (8) move their cursor once per key press, and the on-screen key pad never repeats. A long shop list takes one press per row.
- A button mashed in the field answers a popup that opens under it. In the field the pad's A is jump and the keyboard's X jump and Z attack; on a popup they are OK and back. The death popup `0x68` opens about 0.83 s after the fatal hit, and its OK is Return to town (popup `0x69`, "Return to town?"), so a player still mashing jump goes to town, or with X (revive) spends a revive.

**Where.** Both live in a new frontend filter, `MenuInput` (`platform/src/menu_input.{h,cpp}`). It is `Focus`'s forward sink: `Focus` keeps the UIKit screens and the cash shop, and everything it passes on to the game goes through `MenuInput` to `mt_input_press` / `mt_input_release`. `focus.cpp` is unchanged. The runtime's FIFO stays the only way into the guest, so ADR 0048's rules hold: one step per game tick, only while `SkyCanvas`'s slots are empty, a release the way its press went. The alternatives were:

- **The runtime** (`mt_input`): it would need the settings and a clock, and it would repeat for every source, scripted runs included.
- **`Focus`**: it already repeats on its own screens, but its file is shared with #146.
- **`Actions`**: it only sees controls, not the panel's history or the popup.

**Repeat (`menu_repeat`).**

- A direction pressed while the panel is 1, 5, 6 or 8 repeats after 0.40 s, then every 0.15 s. That is `Focus::kRepeatDelay` / `kRepeatEvery`, so every menu repeats at one pace.
- A repeat is a release and a press of the action. The guest sees another key press: on the key path that is `Clet_HandleEvent(3, code)` then `(2, code)`, one tick apart.
- The repeats keep to the beat. One that falls due while the runtime still has events queued waits for an empty queue and then starts the beat again, so repeats never pile up.
- Only the last direction pressed repeats.
- Several devices holding one direction are let go and pressed together. The runtime counts presses per action, and the guest sees a press only when the count goes from 0 to 1.
- It stops on the release, on any change of panel (the shop closed, a popup over it) and when the setting goes off. The direction stays held in the game as before.
- The field, cut scenes, popups and the help pages (whose left and right are swipes) never repeat. In the field only walking is a held action, and a repeat would stutter it.
- The NPC chat's quest rows are chosen by `Focus` with touches (#77) and don't repeat. A chat has at most six rows.

**Guard (`confirm_guard`).**

- **The popup** is the game's own popup id at `0x108C64` (−1: none). `Popup_Open` `0x3EAA1` sets it and `Popup_Close` `0x3E945` resets it.
  - `MenuInput` reads it at every turn and at every press of a guarded action (`menu_popup_id`, only where a mapped segment holds it). A change to another id is a popup opening, so a sub-popup counts as a new one.
  - The observed `Popup_Open` entry (ADR 0053) reports only the death popup and would have needed a new event kind for every popup. Its listeners run inside the guest's call, where the frontend has no clock.
- **The rule.** A press of `ok`, `back`, `revive` or `grade_revive` is dropped, the press and its release, in two cases:
  - for 0.30 s after the popup opened;
  - while a control is still held that went down in the field and does one of those four in menus. `Actions` now records each held control's context (`Actions::held_from_field`).
- **Not guarded:** minus and plus (amounts), and the directions. A walk held from the field doesn't hold the guard, since the arrows don't confirm. A release whose press went to the game before the popup opened goes on, so A pressed in the shop and still down when its "buy?" opens is released normally.
- **Logs and counts.** Each drop is logged (`menu: ok dropped on popup 104 (53 ms after it opened)`) and counted in `Stats::guarded`. The drops past 0.30 s because of a field button are also counted in `Stats::held`.

**Level-ups open no popup.** The issue named a level-up popup, but this version shows none. `CHero::LevelUp` draws a white then a black full-screen quad (#76) and the field goes on: `play.confirm_guard` mashes A for 1.5 s after a level-up on 2.3 and sees the popup id stay −1 and A stay jump. The popups that can open under a mashing player are the death popup and the notices that follow an action of the player's own. The guard covers every one of them by the id.

**Settings.** `menu_repeat` and `confirm_guard` (Controls; `--menu-repeat`, `--confirm-guard`; on / off): off in Original, on in Original + controller and Enhanced. The defaults are off, today's behaviour. That breaks one old coincidence: with no settings file the defaults no longer match the Original + controller column (`presets_test`). `game.cpp` applies both every turn from the settings in effect (`Game::settings`, which #145's `apply_settings_at_once` keeps; `Applies::kAtOnce`), so a live settings menu (#89) only has to change those. Scripted runs (`game_driver.h`) have both off unless a flow turns them on.

**Evidence.**

- CI: `tests/cpp/menu_input_test.cpp`, with real `Actions` and the default gamepad and keyboard rows in front of `MenuInput`, on a scripted panel, popup id, clock and queue. It covers:
  - the repeat timing (a press at 1.00 s, repeats at 1.40, 1.55, 1.70, 1.85 and 2.00 s on each of panels 1, 5, 6 and 8) and none on panels 0, 3, 7 and 10;
  - the stop on a panel change and on the setting going off, the wait on a busy queue and the beat after it, the last direction, two devices;
  - the guard's 0.30 s (presses at 49, 149 and 249 ms dropped, 349 ms through), a sub-popup's own window, a popup seen at the press before any turn, the held-from-the-field rule with the keyboard's X, a held walk that doesn't count, and a press from before the popup released to the game;
  - off: everything passes through unchanged;
  - the popup reader on synthetic memory and on a guest address space with and without a mapped segment.
- Real assets, locally (virtual clock, a virtual Xbox 360 pad):
  - `input.menu_shops` holds down on Mr. Wang's Sell list, which has the save's five etc items. With `menu_repeat` off, a 1 s hold is one key press of down (`Clet_HandleEvent(2, -2, 0)`) and one row (−1 → 0). With it on, a 0.6 s hold is three key presses, two of them repeats, and three rows (0 → 3). The Buy tab can't show it, because there the selection stops at row 2.
  - `play.confirm_guard`, after the tutorial in Henesys, mashes A from 0.5 s before a fall until 0.28 s after the death popup opens. The popup opens 0.82–0.85 s after the hit.
    - With the guard off, the mash answers it and its "Return to town?" (`0x69`), and the hero is back in the field 0.3 s later.
    - With it on, 3 presses are dropped (53, 154 and 254 ms after the popup opened), the popup is still `0x68` 0.3 s after the mash, and one A after that opens `0x69`.
    - With X held from the field, A at 0.45 s and 0.75 s after the popup opened is dropped twice (`Stats::held` 2). Letting X go releases the field's attack, and the popup stays. A then answers it.
    - A hunt on 2.3 to level 6 with A mashed for 1.5 s after the level-up sees no popup.

## Addendum: a focus marker or a button prompt on every screen (#146, 2026-10-30)

The focus frame showed only on panels −1 and 9. Panels 1, 5, 6 and 8 relied on the engine's own cursor, which nobody had checked, and the popups (7) showed nothing at all.

- **The survey.** `MT_SURVEY_SHOTS=1` makes the real-asset menu and play flows (`tests/cpp/game_driver.h`) save a shot 0.4 s after each change of screen state (panel, game mode, popup, popup menu tab, focused control). The engine draws its own selection on the popup menu's tab bar, the NPC chat's quest rows (a red arrow and purple text), the shop's list (the chosen row darkened, but nothing on entry: the selected row is −1) and the world map (a red pointer). It draws none on the popups, on the mini map (nothing to select), and in the popup menu's item and equipment lists, although the keys move a cursor there: `CDataMgr +0xD8` and `+0xDC`, indices into the list widgets `0xF14B8` and `0xF14C0` ([controls/keypad.md](../controls/keypad.md#gl-touch-widgets-popup-menu)). The skill, quest and option tabs draw none either (#164, below).
- **Markers.** Where the engine has a cursor but draws none (the item and equipment lists), the port draws the focus frame around the cell (`read_popup_menu`: the cell's rect plus the list's offset, clipped to the widget's rect).
- **Button prompts.** Beside each of the game's buttons, the glyph (ADR 0054) of the control that presses it: the popups' OK / OK2 / Yes / No / Cancel / − / + and Empty (a tap on the text), the death popup's four choices, the NPC chat's Accept / Refuse / OK, the shop's Buy / Sell / tabs / Charge (RT, N) / close, the map's World and Mini, the cut scenes' strip, the character select's New (RT, N; not while a popup covers it), and back where the focus has a back control. A table (`kPromptSpots`, `platform/src/prompts.cpp`) names each button by its target class and selector, its action, the side its glyph goes on (a "no" sits right of its "yes", so its glyph goes on the right) and where its art lies inside its touch rect. A glyph that would leave the screen goes on the other side. The popup menu has no UIKit button: its close and, on the tab bar, the tab keys (LB / RB, Q / E) get fixed prompts. A popup closed over the popup menu (its Save) leaves the panel at −1 with the menu still up, as #127 found for the shop. So the popup menu's hints also show on panel −1 while the game mode (`0x11AA00`) is 1 and the focus has no UIKit control.
- **The device.** The prompts show the device used last (`DeviceTracker`): a key press shows key caps, a pad button, a stick pushed past half or a pad connecting shows that pad's set (`GlyphTracker`), and a mouse click, a touch or the last pad going shows nothing. A touch player sees no change. The control is the action's first binding in menus for that device, so a remap shows at once. For keys, Enter, Esc and M are preferred where bound.
- **The highlight before the first press.** While a pad or the keyboard is the device in use, the focus frame shows as soon as the focus is active, not only after the first action (`focus_highlight_target`). A pad player sees where the focus is on arrival. With the mouse it still shows only once an action has been used.
- **Window only.** The markers go through `Presenter::draw_highlight`. The prompts are drawn with `Canvas` into a transparent premultiplied texture at the placement's scale (`HintLayer`), redrawn only when the prompts, the style or the bindings change. Both come after the read-backs and captures, so `--shot`, `--shot-gl`, `--capture` and the goldens never have them, and not while the remap overlay is open.
- **Settings** (ADR 0070's registry, both applied at once): `focus_highlight` (the focus frame and the markers; on in every preset) and `button_prompts` (off in Original, on in Original + controller and Enhanced; off without a settings file, its Original column, as ADR 0070's defaults rule asks: set so when #146 merged into the epic, where #145 had already parted the defaults from the Original + controller column).
- **Evidence.**
  - CI: `tests/cpp/prompts_test.cpp` (`platform_test`, `prompts: *`) covers the table and the glyph's side, the control shown (defaults, preferred keys, a remap, nothing bound), the device rules, the list cursor read from synthetic guest memory (clipping, scrolled out, no `CDataMgr`), a turn's hints and their gating, and the highlight before the first press. It also checks the glyphs switching live between synthetic keys and virtual Xbox 360, PS5 and Switch Pro pads by hash, and the layer on a software renderer against the same drawing on the CPU (at most 2 apart per channel).
  - Real assets: every flow in `menu_nav_real_test` and `play_real_test` makes a window shot of each settled screen state (the composited screen at 2× with the frame, markers and prompts drawn as the window draws them, kept local) and fails if a state has neither a marker nor a prompt (`window_summary`). On 2026-10-30 all 16 tests (`input.menu_*`, `play.*`) passed with 148 window shots over 19 flows. They cover the main menu, the terms, the character screens, the cut scenes, the NPC chat, every popup menu tab (the item and equipment markers among them: `play.chapter_1_b`), the shop and its popups, the cash shop, the map, the death popup (0x68, four prompts), the save popups, and the popup menu after its Save popup on panel −1. That last state had no prompt before the panel −1 rule (`play.save_quit`, `play.chapter_1_a`, `input.menu_shops_save`).
  - Not automated: how it looks on a TV or a Steam Deck with each pad type (#146's hand-off).
- **The skill, quest and option tabs (#164, 2026-11-02).** The engine draws no cursor there either (window shots before and after each d-pad press are pixel-identical inside the menu). Their cursors join `kLists`: the skill tab's `CDataMgr +0xD4` (the visible row, plus the keys' scroll `*(+0x10C) + 4`; a row is two cells of `0xF14A8`, the skill and its +), the quest tab's `+0xB4` (set: the main quest, cell 0 of `0xF14B4`) or `+0xE4` (the side quest row, plus the scroll, cells 1 on), and the option tab's `+0xE0` (its four buttons, `0xF14A4`); the quests and options only on the tab's list (`+0xAC` 0). A cell's rect follows the engine's (`0xA0370`: alone when its `+0x5C` is set, the widget's drag `+0x6C`). **Behaviour change:** left / right open the skill and quest tabs as they reach them (`Menu_OnKey`) without the touch that shows their lists (`TouchWidget_Show` `0xA0715`, the widget's `+0xB1`): with a pad the skill list was empty and the side quests missing. While such a tab is open with its list hidden, the focus now makes that touch once (`popup_menu_list_hidden`; the touch keeps the tab and its cursor). Evidence: `prompts: the skill, quest and option tabs' list cursors (#164)` and `focus: a skill or quest tab the d-pad opened gets the touch that shows its list` (CI); `play.popup_menu_tabs` from part C's save (real assets, local): each tab's window shot has the frame, at the expected row after each press. The keys' scroll does not move the drawn skill and quest lists (the engine draws them by the touch drag `+0x6C` only), so a cursor scrolled past the visible rows has no frame (#166). OK on a quest ends the process (#167): the flow stops short of it.
