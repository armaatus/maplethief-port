# 52. Hiding the on-screen key pad: the compositor's opacity, the activity listener, touches still delivered

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #71
- **Builds on:** [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (the compositor, hit testing), [ADR 0047](0047-touch-input-mouse-and-fingers.md) (`Input`, the activity listener, `is_touch_source`), [ADR 0048](0048-gamepad-actions-and-delivery.md) (gamepad actions through the pad's BOOL setters), [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (`settings.ini`); the facts are [docs/controls/keypad.md](../controls/keypad.md) (#66)

## Context

A player with a gamepad or the keyboard doesn't need the touch key pad over the picture: the d-pad, jump, attack, the four quick-slot keys and the Map / Bag / Cash buttons. Someone who touches the screen does need it. The issue asks for the pad to hide on non-touch input, to come back on touch, and for a setting that overrides this. Gameplay must not change.

Facts that limit the options:

- **The game owns the panels' visibility.** `TouchKeyPadView` has eleven full-screen panel containers as subviews 0–10. `enable:` / `disable:` set a panel's `alpha` to 1 or 0, and `disableAll` hides all of them. The game reads that state back, and UIKit's hit test skips views with alpha below 0.01, which the game relies on to switch panels off (keypad.md).
- **Panel 0 is the game pad**, live in the field. The other panels (NPC chat, the shop, popups, the map, the cash shop, help) carry the game's labelled menu buttons (Accept / Refuse, Buy / Sell, OK, ±), placed over the GL picture they belong to.
- **Hit testing doesn't come from panel 0.** `TouchKeyPadView` is full-screen and hit-tests every finger itself against the `KeyPadArea.png` mask, not against the panel's images. A touch on the d-pad lands on panel 0's views and is forwarded up the responder chain to `TouchKeyPadView`.
- **#67's gamepad doesn't touch the pad.** It sets the pad's BOOLs through their setters, so `SkyCanvas::Tick` sees them whether the pad can be seen or not.

What had to be decided:

- where to hide;
- which views to hide;
- when to hide;
- what a touch on the hidden pad does;
- how a hidden pad avoids holding a key;
- what the setting is called.

## Options considered

Where to hide:

1. **The guest's view state** (`setAlpha:` or `setHidden:` on panel 0). The game reads the alpha back and restores it with `enable:`, so the two would fight. Alpha below 0.01 also turns hit testing off, which would change what a touch does. Rejected: it changes game state.
2. **Skip panel 0 when the platform composites**, through a per-view **host opacity** that only the compositor sees. **Chosen.** The guest's alpha, hidden flag and hit testing stay exactly as the game set them, and the same factor gives a fade.

What a touch on the hidden pad does (auto mode):

1. **It only shows the pad again** and is swallowed. The player sees the pad before acting, but a press on the attack key does nothing the first time. To swallow it, `Input` would have to know the pad's mask and the panel, and a whole tap would be lost. In the field almost every touch is a pad key, so the first touch after playing with a gamepad would always be lost.
2. **It shows the pad again and is delivered too. Chosen.** The player knows where the keys are, so the first press already walks or attacks. Nothing in the touch path changes: hiding stays outside the guest's input entirely. With `always`, the pad stays hidden but still works the same way, which is also how the game treats its invisible alert panel (keypad.md: the pad stays live under panel 7).

A hidden pad and held keys:

1. **Release every touch when the pad hides** (`Input::release_all`). Simple, but a player holding the d-pad with the mouse who presses a gamepad button would stop walking.
2. **Wait to hide until no touch is down. Chosen.** The pad hides only once the last finger lets go, so a hidden pad never holds a finger's key, and mixed input (mouse on the d-pad, pad on attack) keeps working. The gamepad's held keys are its own BOOLs, not touches, and they go on while the pad is hidden.

## Decision

**The runtime (`maplethief/uikit.h`).** `mt_uikit_set_host_opacity(view, opacity)` and `mt_uikit_host_opacity(view)` keep a per-view `host_opacity` (default 1, clamped to 0–1) in the host `View`.

- The compositor multiplies it into the view's presentation alpha.
- Below 0.01 it skips the view and its subtree. A translucent view with subviews is drawn as a group, as for the game's own alpha.
- The guest never sees it: not `alpha`, not `isHidden`, not `hitTest:`.

`mt_input_pad_panel_view(index)` (`maplethief/input.h`) returns `TouchKeyPadView.subviews[index]` of the pad seen at the last turn.

**The frontend (`platform/src/keypad_hide.{h,cpp}`, `KeypadHider`).** A pure state machine with no SDL:

- **Modes, `hide_keypad = auto | always | never`** in `settings.ini`, or `--hide-keypad`. The key is named after what it does, so `always` means always hidden. The default is `auto`.
  - `auto`: non-touch input hides the pad and touch input shows it again. This comes from `Input`'s activity listener (`KeypadHider::listener`): `is_touch_source` is true for the mouse, a finger and a scripted tap, and false for a gamepad, the keyboard, and the gamepad's synthetic touch fallbacks.
  - `always`: hidden from the start, with no fade.
  - `never`: the game as it was.
- **Turns.** `turn(now, Input::live())` once per run-loop turn starts the fade that the turn's input asked for. A hide waits while a touch is down; a show never waits.
- **The fade.** Linear, 0.2 s for a full fade. A fade reversed part way takes its share of the time. It runs on the run loop's clock, which is virtual under `--fixed-step`, so scripted runs stay deterministic.
- **What is hidden: panel 0 only**, the game pad (d-pad, jump, attack, the quick-slot keys, Map, Bag, Cash, and the NPC "up" hint on the d-pad). The other panels' buttons are the visible labels of the game's menus and popups, so they stay drawn. The quick slots' item and skill icons are drawn in GL and stay too.
- **`game.cpp`.** Before every composite (each window present and each `--shot`), it puts the opacity at that time on the pad (`apply_keypad_opacity`). It presents at about 60 Hz while a fade runs, and logs each hide and show.
- **`--shot-gl`.** With it, each `--shot` also writes the GL frame under it (`NAME.gl.png`): a test aid.

## Consequences

- Hiding the pad can't change the game: the guest's views, hit testing and input paths are untouched, which the tests check.
  - CI (`platform_test`, `objc_test`) checks:
    - the state machine: every source, the deferred hide, the fade, the modes;
    - `hide_keypad` parsing and writing;
    - the opacity landing on panel 0 of a synthetic `TouchKeyPadView`;
    - the compositor: host opacity 0 skips the subtree, 0.5 blends it as a group, it multiplies the game's alpha, the guest's alpha and hidden flag are unchanged, and a touch still hits the hidden view.
  - Real assets, locally (`input.keypad_hide`):
    - the per-tick hero is byte-identical with the pad hidden and shown, for held touches (`always`) and held gamepad keys (`auto`);
    - every GL frame from the first map on is identical, by hash;
    - the three modes behave as described on one input stream with real SDL mouse events;
    - a screen composited hidden differs from one composited shown only inside panel 0's rects.
  - Real assets, locally (`platform.keypad_hide`): `MapleThief --hide-keypad never|always` with the device golden list's fresh run gives identical GL frames on all six screens; the screens differ only on the first map and only inside the pad's rects, and the shown run equals the device golden's renders.
- In `auto`, the first touch after gamepad play acts at once, before the pad has faded back in. A player who touches blind may press a key without seeing it; in the field that's the normal way to play.
- With `always`, touches still work on the invisible pad. A mouse player who chooses `always` can still play by touch, but sees no keys.
- A key held by a finger keeps the pad shown until it lets go. A player who leaves the mouse button down sees the pad until then.
- Panel 0's NPC "up" hint is hidden with the pad. Pressing up still talks.
- If a later issue wants other panels hidden as well, `mt_input_pad_panel_view` and `apply_keypad_opacity` take any index.
