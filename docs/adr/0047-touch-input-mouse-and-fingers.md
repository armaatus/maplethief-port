# 47. Touch input: the mouse and touch screens as UITouches, through one platform entry point

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #65
- **Builds on:** [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (`mt_uikit_touch` in screen points, hit testing, UIControl tracking, a touch stays with the view it began on), [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (`window_point_to_screen`, `inside` over the bars, `settings.ini`), [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (UIKit's clock, virtual under `--fixed-step`; the device golden runs)

## Context

The game only knows touches. Its shell overrides `touchesBegan/Moved/Ended/Cancelled:withEvent:` in a handful of views. The most important is the full-screen, multi-touch `TouchKeyPadView`, which tracks two fingers itself: it hit-tests each finger against the `KeyPadArea.png` mask, matches a moved touch to its finger by `previousLocationInView:` within 50 points, and sets the BOOLs that `SkyCanvas::Tick` polls ([docs/controls/keypad.md](../controls/keypad.md), #66). Everything else is a `UIButton` target/action. The runtime already delivered queued touches (ADR 0037). Before this ADR, only scripted taps reached it (`--tap`, straight to `mt_uikit_touch`).

What had to be decided:

- **How desktop input maps to touches**: the mouse (one pointer, buttons), touch screens (several fingers), and SDL's synthetic events in both directions (mouse-from-touch, touch-from-mouse), which would otherwise turn one press into two touches.
- **Points outside the game**: the bars around the image (ADR 0043), and drags that leave it.
- **Interruptions**: what a touch that is down becomes when the window loses focus, is hidden or minimised, or the mouse leaves. A fact settles part of this: `-[TouchKeyPadView touchesCancelled:withEvent:]` (`0xB6BA5`) is an empty 16-byte method, so a cancelled touch leaves its d-pad or attack key held until the next touch (docs/controls/keypad.md).
- **What the guest's `UITouch`/`UIEvent` must answer**: `tapCount`, `touchesForView:`, and how many moves a frame delivers.
- **One code path**: `--tap`, real input, and the input still to come (the gamepad's virtual touches, #67; the key pad's auto-hide, #71, which needs to know whether touch or non-touch input happened) should share one entry point.

## Options considered

1. **Map SDL events in game.cpp's event loop directly.** Little code, but untestable without the game, and #67 and #71 would each grow their own copy of the rules.
2. **SDL's touch emulation** (`SDL_HINT_MOUSE_TOUCH_EVENTS`: the mouse as touch events, then handle finger events only). One path for both. But it uses one finger id for every button (the right button can't be a second touch), it can't say whether input came from the mouse or a finger (#71 needs that), and its normalised coordinates lose the window's HiDPI points.
3. **A platform `Input` object** (`platform/src/input.{h,cpp}`) that takes every SDL event and calls a sink (by default `mt_uikit_touch`). It also has an entry point for virtual touches and an activity listener. **Chosen**: the rules live in one place, testable on a dummy window with `SDL_PushEvent`.

For interruptions:

1. **Cancelled**, as UIKit sends when the system interrupts. Faithful to UIKit, but the game's key pad ignores it. Alt-tabbing while holding the d-pad would leave Robin walking until the next touch. On a desktop, focus changes are everyday events, not phone calls.
2. **Ended at the touch's last point. Chosen.** The key pad releases the key (`touchesEnded:` clears the matched finger's key). The cost: a `UIButton` held at that moment gets touch-up-inside, so it fires as if the player had lifted the finger there. The player had already pressed it, and lifting early is what a finger does too.

## Decision

**`Input` (platform/src/input.{h,cpp})** turns SDL events into `mt_uikit_touch(finger, phase, x, y)` calls in the game's 480 × 320 screen points:

- **The left mouse button is one touch** (finger 1): button down is Began, motion while it is held is Moved, button up is Ended. Motion that shows the button no longer held (its up was lost) ends the touch there.
- **The right mouse button** is optionally a second touch (finger 2) at a fixed screen point while it is held, wherever the pointer is. One hand can then hold the d-pad with the left button and attack with the right. It is set with `right_touch = X,Y | off` in `settings.ini` (whole points, x 0–479, y 0–319) or `--right-touch`, and is off by default (#67's controller layer and #68's keyboard keys are the main second input).
- **Touch screens are multi-touch**: one finger id per SDL finger while it is down (`0x100` + a slot of 20; more fingers at once are ignored). Coordinates come from the finger's normalised position × the window's size in points.
  - Only direct touch screens count (`SDL_TOUCH_DEVICE_DIRECT`), plus devices SDL doesn't know, such as the events a test pushes. A trackpad's fingers (`INDIRECT_*`) move the pointer; they are not touches.
- **SDL's synthetic events are dropped**: a mouse event made from a touch (`which == SDL_TOUCH_MOUSEID`) and a touch made from the mouse (`touchID == SDL_MOUSE_TOUCHID`). Each press is exactly one touch.
- **The bars.** A touch that begins over the bars around the image (`window_point_to_screen`'s `inside` false), or before the first frame is placed, is dropped, together with the rest of its press. A touch that leaves the image while down is clamped to its edge (x in [0, 480), y in [0, 320)) and ends there. Clamping keeps a d-pad drag on the screen's edge instead of losing it. While a button is held, SDL captures the mouse (auto-capture), so motion and the up keep coming from outside the window.
- **Interruptions end touches, they never cancel them**, because the game's key pad releases a key only on `touchesEnded:` (its `touchesCancelled:` is empty, `0xB6BA5`):
  - focus lost, window hidden, window minimised: every live touch;
  - `SDL_EVENT_WINDOW_MOUSE_LEAVE` (without capture): the mouse's touches;
  - `SDL_EVENT_FINGER_CANCELED`: that finger;
  - a second down of a finger that is already down: the first touch.
  Each ends with Ended at its last screen point. `Input::release_all()` does the same on demand.
- **Merged moves.** A Moved that lands on the same screen point as the last one is not sent.
- **The shared entry point.** `Input::touch(source, id, phase, x, y)` takes touches that are not SDL's, as virtual finger `0x1000 + id`, under the same rules: a Began off the screen is dropped, later phases are clamped, a stray phase is ignored. #67's gamepad uses it, and so do the scripted taps before the first frame. `Input::note(source)` reports non-touch input.
- **The activity listener** hears every touch that begins, with its `InputSource` (mouse, finger, scripted: `is_touch_source`), and every key press (not repeats) and gamepad button press (non-touch). #71's key pad auto-hide uses it.

**`--tap` takes the same path.** game.cpp's `deliver_taps` builds a left-button down and up at the window point that shows the screen point (`screen_to_window_point`). It hands them to `handle_event`, the function every polled SDL event goes through, in the turn the tap is due. A tap's touch is queued in the same turn and at the same screen point as before, so the scripted fixed-step runs are unchanged. All seven device-golden "ours" screens match pixel for pixel, the popup from the key pad's cash button among them.

**The runtime's touches** (`runtime/src/uikit/touch.cpp`):

- **`-[UITouch tapCount]`**: a touch that begins within 0.35 s of the previous tap's end and within 25 points of it counts one more (2 for a double tap); otherwise 1. A touch that moved 10 points from its start, or was cancelled, is no tap and ends the run.
- **`-[UIEvent touchesForView:]`**: the live touches whose view is the given view or inside it. **`-touchesForWindow:`**: the touches in that window. `allTouches` is unchanged.
- **Moves merged within a turn.** A Moved queued after a queued Moved of the same finger (no other event of that finger in between) replaces it, as UIKit merges a frame's moves. `previousLocationInView:` is then where the last delivered move was, which is what the key pad's 50-point finger matching expects.
- **Unchanged**: hit testing on Began, a touch stays with its view, timestamps from UIKit's clock (virtual under `--fixed-step`), single-touch views get only the first finger.

## Consequences

- One code path for real clicks, touch screens, scripted taps and (from #67) the gamepad. `tests/cpp/input_test.cpp` checks the rules with SDL events pushed on a dummy-driver window, into a recording sink and into a synthetic main menu and key pad. CI runs it on every OS.
- Real assets, locally:
  - `platform.mouse_clicks` clicks through the device goldens' fresh run and finds `-[MainMenuView clickPlay:]` and the key pad's `clickCashButton:`/`releaseCashButton:` in the uikit trace;
  - `platform.device_golden` and `platform.game_boot` run their taps through `Input`.
- A button held when the window loses focus fires its touch-up-inside. If that turns out to matter (a purchase, a delete), the fix is a per-view choice in the runtime (Cancelled for `UIControl`, Ended for the key pad), not a change of this rule.
- A press that begins in the bars does nothing. With integer scaling the bars can be wide; a player who expects a touch near the edge to reach the edge has to touch the image.
- Trackpad gestures are not touches. A player on a laptop uses the trackpad as a mouse (one touch) and the keyboard or a gamepad for the rest.
- The right button's point is a screen point, not a key: #66's key areas are the reference for choosing one (attack is x 405–479, y 263–319, so `right_touch = 440,290`).
