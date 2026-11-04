# 101. Button labels on the field's key pad, beside each key as it is drawn

- **Status:** Accepted
- **Date:** 2026-11-04
- **Issue:** none (Joris's request: "a better UI overlay on the items and fight buttons, so that we know which button is which item or fight")
- **Builds on:** [ADR 0054](0054-bindings-file-remap-overlay-and-button-glyphs.md) (bindings, the remap overlay, the glyphs), [ADR 0051](0051-controller-focus-navigation.md)'s #146 addendum (button prompts, `DeviceTracker`, `HintLayer`), [ADR 0086](0086-customisable-touch-key-pad-layout.md) (the key pad's groups and their transforms), [ADR 0090](0090-widescreen.md)'s #168 addendum (the pad at the wide view's edges), [ADR 0052](0052-hiding-the-on-screen-key-pad.md) (hiding the pad), [ADR 0070](0070-settings-registry-and-presets.md) (the registry)

## Context

The field's key pad (panel 0) is always on screen: the d-pad, jump, attack, the four quick slots with their items and skills, and Map, Bag and Cash. With a gamepad or the keyboard, nothing tells the player which button drinks the potion on slot 1 or casts the skill on slot 3. The button prompts (#146) deliberately skip panel 0 (`prompts_for`: "its key pad is the game's own"). They place glyphs from a static table of UIKit targets, which can't follow the pad's custom layout or widescreen move.

## Options considered

**What gets a label.** The quick slots, jump and attack are what the request names. Map, Bag (the popup menu) and Cash are also always visible and have a binding, so they get one too. The d-pad doesn't: its arrows are the directions on every device.

**Which control.** The field's bindings (`Binding::field`), not the menu's that the prompts read: the same button can mean OK in a popup and jump in the field. The first control of the device in table order is shown. A remap through the overlay (`bind_control`) leaves the new control as the action's only one of that device, so the first is the remapped one. No preferred keys: the defaults' first keys (X, Z, A S D F, M, Esc, C) are the ones `docs/controls/keyboard.md` lists. The device and glyph set are the prompts' (`DeviceTracker`, `GlyphTracker`, face-button mode). Touch only gives no labels.

**Where.**

1. **Inside the key** (a corner badge). It covers the quick slot's item and its count, and the attack key's weapon, which the game draws in GL over the keys.
2. **Beside the key, toward the screen's middle.** Left of each quick slot, above jump and attack, below Map, Bag and Cash. **Chosen.** It never covers the game's art on a key, and there is only field there. Where a custom layout puts the key against an edge, the label goes on the other side. Then it is moved inside the picture.

Each key's rect is its view's frame from `docs/controls/keypad.md`'s Panel 0 table, put through `keypad_view_transform` (the custom layout's scale and offset, then widescreen's edges move). That is the same transform `apply_keypad_layout` gives the views, so the label is wherever the compositor draws the key. Map, Bag and Cash never move.

**How big, how drawn.** The labels go on a layer of their own in the window, drawn after the read-backs (like the prompts' `HintLayer`). They never show in shots, captures or goldens, and they're drawn at the window's pixels, not the render scale's. The glyphs are those of `glyphs.h` on the prompts' dark backing, at about three quarters of a prompt's size: the canvas unit is `round(0.75 × pixels a point)`, at least one pixel a font pixel. At a 480 × 320 window they're 12 pixels high, the 5 × 7 font's minimum. At 2× they're 24 pixels (12 points), and from 3× on they're smaller than a prompt (8–10 points). The layer spans the wide picture, so labels can sit in widescreen's wings.

**With the pad hidden** (`hide_keypad = auto` hides it on pad or key input). The game keeps drawing the attack key's weapon and the quick slots' items in GL whatever the pad's opacity, so their labels stay. Jump, Map, Bag and Cash are UIKit views only, so their labels fade with the pad. Without this, with the default `hide_keypad = auto`, a pad player would never see a label.

## Decision

- **`platform/src/hud_labels.{h,cpp}`**:
  - `hud_keys()` is the table of keys, `hud_key_rect` gives a key's rect with a layout and widescreen move, and `hud_labels(layout, wide, pad_opacity)` gives the labels.
  - `hud_label_control` picks the control a label shows, and `hud_label_box` places it (side, flip, clamp).
  - `hud_label_unit` sets the glyph size.
  - `draw_hud_labels` (Canvas) and `fade_hud_labels` draw them, and `hud_label_span` gives the span over the wide picture.
  - `HudLabelLayer` is the texture, redrawn only when the labels, style, bindings or size change.
- **`game.cpp`** draws the labels after the prompts, when `button_labels` is on, the key pad's panel in use is 0 (the field) and neither the settings menu nor the remap overlay is open. It uses `installed_keypad_layout()`, `installed_keypad_wide()` and `KeypadHider::opacity`. The key pad editor presents on its own, so it shows none.
- **Row** `button_labels = on | off` (Controls, "Key pad labels", `--button-labels`, game-only, `Applies::kAtOnce`: read at every present). It is **on in every preset column** and on by default. Like `focus_highlight` it only shows with a pad or the keyboard, which the original game never had, so Original keeps it too, and the defaults rule (the default is the Original column) holds with both of ADR 0070's pinned lists unchanged.

## Consequences

- A pad or keyboard player sees, beside each quick slot, jump, attack, Map, Bag and Cash, the button that presses it, in the glyphs of the pad in use. A remap, a pad change, a switch to the keyboard, a layout change in the editor and `widescreen_hud` show at the next present.
- A touch player sees nothing new. `button_labels = off` draws nothing and the window is as before.
- Shots, `--capture` and the device goldens never contain the labels.

## Evidence

- `platform_test`'s `hud labels:` cases:
  - the table against the groups
  - each key's control for the keyboard, Xbox, PlayStation and Nintendo (by position and by label), and for none
  - a remap through the overlay (F1, quick slot 1 → K, Save) shown at once
  - the places in the original layout at 1–6 pixels a point, widescreen's edges and narrow, custom layouts (moved, scaled, flipped at the top and left edges)
  - the glyph unit, and the fade
  - the golden `tests/golden/data/hud_labels_synthetic.png` (a synthetic pad, keyboard at the edges and Xbox narrow)
  - the layer on a software renderer equal to the CPU drawing, redrawn only on a change, nothing for a touch player.
- `presets_test` (the row's columns), `settings_menu_test` (the Controls tab's drawing, by hash).
- Real assets: `play.hud_labels` (widescreen_pad's way to Henesys at 568 points with the virtual Xbox 360 pad). With the pad at the view's edges, narrow and a custom layout:
  - every key of the table is a view of the game pad at that rect
  - the label's key is that view's frame through its host transform
  - with the pad's glyphs and the keyboard's, all 9 labels draw a point or two beside their keys, inside the wide picture, over no key and no other label.

  Local, Debug arm64, 2026-11-04: passed. Slot 1's key at (490, 224) at the edges, (446, 224) narrow, (427.3, 260.2) with the slots at 130 % and moved 60 points left (the 44-point edge move on top).
