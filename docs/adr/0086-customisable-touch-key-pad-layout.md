# 86. A customisable touch key pad: host transforms, a remapped hit mask, moved GL icons and an editor

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #159
- **Builds on:** [ADR 0052](0052-hiding-the-on-screen-key-pad.md) (the compositor's host opacity, panel 0), [ADR 0069](0069-guest-function-hooks.md) (guest function hooks), [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (the compositor, hit testing), [ADR 0070](0070-settings-registry-and-presets.md) (the settings registry), [ADR 0054](0054-bindings-file-remap-overlay-and-button-glyphs.md) (an overlay that pauses the game); the facts are [docs/controls/keypad.md](../controls/keypad.md)

## Context

On a phone, the original key pad is fixed: the d-pad covers the hero at the bottom left, and the keys have one size for every hand and screen. The issue asks for each group of keys to get its own size, position and opacity, with an edit mode in the settings (drag, pinch, reset), stored in `settings.ini` as `keypad_layout`. The default stays the original. Two acceptance criteria: **the hit areas follow the drawn buttons**, and **the original layout is byte-identical** when nothing is changed.

Facts that limit the options (keypad.md, "Panel 0"):

- **What is drawn is three different things.** Panel 0's keys are `UIImageView`s, built by `-[TouchKeyPadView initKeyPadType0:]` `0xBA6A1`: the d-pad's container (its pressed images and the NPC hint are its subviews), the jump icon, the jump and attack keys and their pressed images, the four short keys and theirs, then the Map, Bag and Cash `UIButton`s. The **attack key's weapon icon and the quick-slot items and skills with their counts are drawn in GL** by the engine: `QuickSlots_Paint(g, mode, x, y, state)` `0x4CAED`, which the field HUD `HUD_Paint` `0x5A465` calls with mode 1 (slot 0 at (441, 292), slots 1–4 at (462, 239 / 193 / 147 / 101)); mode 0 is the popup menu's copy (callers `0x4E499`, `0x4FAD9`), which has its own touch widgets.
- **The hit areas don't come from the views.** `TouchKeyPadView` is full-screen and reads the colour mask `KeyPadArea.png` at each touch with `-[TouchKeyPadView colorAtPixel:onImage:]` `0xB4DB5` (`@20@0:4{CGPoint=ff}8@16`: the point's x and y in r2 / r3 as float bits, the image on the stack). It returns **nil for a point outside the image**, and in `touchesBegan:` `0xB80F1` a nil colour skips everything, including the NPC-touch check, which only runs in the "no key colour" branch, on the raw touch point. A transparent pixel is "no key"; an off-image point is "ignore this touch".
- **Map, Bag and Cash are real `UIButton`s**, hit-tested by UIKit at their frames.

What had to be decided:

- what a group is;
- how the views move without the game seeing it;
- how the hit areas follow;
- how the GL icons follow;
- what keeps the original byte-identical;
- the setting's form, and the editor.

## Options considered

How the hit areas follow:

1. **Move the touch**: rewrite the touch point that `Input` sends to UIKit. Every touch in the field goes to the pad, but so do touches on NPCs and the map (the NPC check uses the raw point), and a pad key moved over an NPC would steal its touch both ways. The mapping would have to know which group a touch is in before UIKit decides anything. Rejected: it changes what every other touch means.
2. **Edit the mask image**: redraw `KeyPadArea.png` with the regions moved. The image is a guest `UIImage` loaded once, so the host would have to rewrite guest pixels after each edit, at the pad's 1:1 scale only. Rejected: fragile, and it writes guest memory.
3. **Hook the mask read. Chosen.** A hook on `colorAtPixel:onImage:` reads the mask at `keypad_mask_point(layout, p)` instead of p: in the topmost drawn group, the point under the touch in the original layout (the inverse of the group's transform); inside a group's old place that nothing is drawn over any more, `kKeypadNoKeyPoint` (240, 160), a transparent pixel (no key, so the game's NPC check still runs on the real point); anywhere else, p itself. The touch, its phases, the game's held set and everything after it are untouched.

How the views move:

1. **Set the guest's frames** (`setFrame:` on the views). The game reads frames back, animates, and switches its pressed images by frame; it would fight a host that rewrote them. Rejected, as ADR 0052 rejected the guest's alpha.
2. **A host transform in the compositor. Chosen.** `mt_uikit_set_host_transform(view, m)` keeps an affine map of landscape screen points per view, applied after the view's own geometry to it and its subviews, only when compositing. Hit testing, frames and the guest never see it (the hit areas come from the hook). The group opacity is ADR 0052's host opacity on the same views.

How the GL icons follow:

1. **Leave them**: the attack key's icon and the quick-slot items would stay where the keys were. Rejected: the issue says the drawn buttons move.
2. **A GLES draw filter around the painter. Chosen.** A hook on `QuickSlots_Paint` installs `mt_gles_set_draw_filter` for mode 1 only, calls the original, and removes the filter. The filter sees each draw's box in normalised device coordinates and, if its centre lies in a group's original rect, moves its vertices with that group's transform (in NDC: x' = s x + s − 1 + tx / 240, y' = s y + 1 − s − ty / 160). Every draw outside the painter, and every draw without a custom layout, goes through unchanged.

## Decision

**The groups (`platform/src/keypad_layout.{h,cpp}`).** Three groups, each the union of its views and mask regions in screen points:

| Group | Rect | What moves with it |
|---|---|---|
| `dpad` | (0, 176, 168, 144) | the d-pad's container (with its pressed images and NPC hint) and its four mask regions |
| `actions` | (335, 263, 145, 57) | jump, its icon, attack, their pressed images, their mask regions and the attack key's GL icon |
| `slots` | (430, 77, 50, 186) | the four short keys, their pressed images, their mask regions and the GL items, skills and counts on them |

Map, Bag and Cash stay where the game puts them. A group's transform scales about its rect's centre (50–200 %), then moves it by (dx, dy) points, **clamped so the scaled rect stays on the screen** (centred if it is wider than the screen). A view belongs to the group whose rect holds its frame's centre. Touches find groups top-down as the views stack: slots, actions, d-pad.

**The setting.** `keypad_layout = original` (the default, in no preset) or `GROUP=SIZE,DX,DY,OPACITY` items joined by `;`, e.g. `dpad=80,12,-20,70;actions=120,0,-6,100`. A group left out is the original. It is written back in the order dpad, actions, slots without the original groups. Its registry row is in Controls, with `--keypad-layout`, applied at once.

**The runtime.**

- `mt_uikit_set_host_transform` / `mt_uikit_host_transform`. The identity is stored as "no transform", so the compositor's path is unchanged.
- `mt_uikit_set_host_reveal(view, on)`: the editor draws panel 0 even where the game has it off.
- `mt_uikit_subviews` and `mt_uikit_screen_frame` let the frontend find panel 0's views.
- `mt_gles_set_draw_filter` / `mt_gles_get_draw_filter`.
- The hookable functions `MT_HOOK_FN_KEYPAD_COLOR_AT_PIXEL` `0xB4DB5` and `MT_HOOK_FN_QUICK_SLOTS_PAINT` `0x4CAED` (`recomp/hooks.py`).

**The frontend.** `install_keypad_layout` registers both hooks for the whole run, **enabled only when the layout is not the original**. `apply_keypad_layout` runs before every composite and puts each group's transform and opacity on panel 0's views.

**The original stays byte-identical.** With `original`:

- both hooks are registered but off, so the generated code tail-calls the originals as without a hook;
- no view has a transform, and host opacity 1 is the compositor's no-op;
- no draw filter is ever installed.

The device goldens (`platform.device_golden`) and the gamepad / hiding runs (`input.gamepad_hold`, `input.keypad_hide`) are unchanged.

**The editor (`platform/src/keypad_editor.{h,cpp}`, `KeypadEditor`).** The port's own mode, drawn over the game:

- **Opening and pausing.** The settings menu's action opens it (#89: `game.cpp`'s `request_keypad_editor`), or `--edit-keypad` once the game has its pad. Like the remap overlay (ADR 0054) it holds the run-loop turn, so the game is paused. The screen is composited again at the paused time after each change, with panel 0 revealed and not faded. Compositing calls no guest code: views draw in the run-loop turn.
- **Touch and mouse.** A finger or the mouse on a group selects and drags it. A second finger pinches it: the distance ratio sets the size, and the midpoint moves it. The bar's buttons are Size −/+, Opacity −/+, Reset, Reset all, Save and Cancel.
- **Keys, wheel and gamepad.** Tab cycles the groups, arrows move by 1 point (Shift: 10), + / − set the size, [ / ] the opacity, R resets, Enter saves and Esc cancels. The wheel sizes the group under the pointer. On a gamepad the d-pad moves, LB / RB select, X / Y set the size, Back resets, A or Start saves and B cancels.
- **Saving.** Save installs the layout and writes `keypad_layout` to `settings.ini` (the file's settings plus the new value, not the command line's). Cancel reinstalls the layout it opened on.
- **What isn't drawn while editing.** The GL icons are those of the last frame, so they follow the slots' group once the game goes on.
- **Where it is drawn.** The editor is drawn only into the window, never into `--shot` or the goldens.

## Consequences

- The hit areas follow the drawn keys exactly, including the diagonal cuts of the d-pad arms, because the mask itself is read under the drawn key. Holding the moved left, attack and right keys moves the hero tick for tick as holding the original ones does (`input.keypad_layout`). Touching where a key was does nothing, and an NPC that a moved d-pad no longer covers can be touched.
- A group moved over another group's old place takes the touches there, because drawn groups come first and the topmost wins. A group moved over an NPC hides it from touches, as the original pad does.
- The pressed images move with their keys (same group, same transform). The NPC hint is inside the d-pad's container, so it moves with the d-pad.
- The popup menu's quick slots (mode 0) don't move: they belong to the menu's own GL touch widgets.
- Two new hooks run at every key-pad touch and every field HUD paint while a custom layout is in use. Each is a few float operations; the paint hook installs a filter that is checked once per draw only inside the painter.
- `keypad_layout` and the editor are personal settings, in no preset. #89's settings menu gets an action row for the editor; `apply_live_setting` should call `install_keypad_layout` when the row changes. (Done when #159 and #89 were merged, wave 4 of #105: the Controls tab's `keypad_layout_edit` row, and the live case, ADR 0076's addendum.)
- Real fingers on the iPhone (#83's build) are a human check: the pinch and drag feel, and whether the clamped edges suit the notch's safe area.
