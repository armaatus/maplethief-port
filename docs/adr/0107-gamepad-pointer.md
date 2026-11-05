# 107. The gamepad pointer: the right stick's pointer, touchpads as the screen, R3 held to tap

- **Status:** Accepted
- **Date:** 2026-11-05
- **Issue:** #185 (epic #201)
- **Builds on:** [ADR 0047](0047-touch-input-mouse-and-fingers.md) (one path for every touch), [ADR 0048](0048-gamepad-actions-and-delivery.md) (gamepads to actions), [ADR 0051](0051-controller-focus-navigation.md) (the focus), [ADR 0054](0054-bindings-file-remap-overlay-and-button-glyphs.md) (bindings, the remap overlay, glyphs), [ADR 0087](0087-game-speed-and-fast-forward.md) (`fast_forward` on R3), [ADR 0114](0114-paging-lists-quantity-repeat-crisp-prompts-price-line.md) (which kept the right stick free)

## Context

The game was made for touch. The focus (ADR 0051) covers the screens it lists, and #140 covers item drags. Some things still need a finger: the help pages' swipe, the main menu's UIKit buttons other than Play when the focus is not wanted, and any screen the focus misses. Ports of touch games to consoles keep a virtual cursor as the safety net.

Nothing used the right stick's axes. Its click (R3) is `fast_forward`, a held port action (ADR 0087). SDL3 reports a pad's touchpad fingers (`SDL_EVENT_GAMEPAD_TOUCHPAD_*`, absolute 0..1) on DualShock 4, DualSense and Steam Input's trackpads.

## Options considered

**What taps.**

1. **A face button.** Every face button is bound in both contexts.
2. **R3 as a press.** It is `fast_forward`; held, the game would fast-forward and tap at once.
3. **R3 held 0.3 s, only while the pointer is shown, and the touchpad's click. Chosen.** A new port action `pointer_tap` (`MT_ACTION_POINTER_TAP`, after `page`), bound to `button touchpad` and to a new control kind, `hold NAME`: a pad button held `Gamepads::kHoldSeconds` (0.3 s).

**How R3 is shared with `fast_forward`.** `Gamepads` gets a *hold gate* (a callback; `PadPointer::hold_gate`: the pointer shown or its touch down). A button pressed while the gate is open, and with a `hold` binding in the context now, is *held back*:

- Let go within 0.3 s, it is pressed and released at once as itself. A short R3 still reaches `fast_forward` (with `hold_toggle` a toggle; held, a blip) and never taps.
- Held 0.3 s (`Gamepads::turn`, on the guest's clock), it becomes the `hold` control until let go. `fast_forward` never sees that press.
- With the gate shut (the pointer hidden, `pad_pointer = off`, or no gate) every button is itself at once, exactly as before. Holding R3 to fast-forward works whenever the pointer isn't shown, and the pointer hides 3 s after its last push.

**Where the touches go.** Through `Input::touch` (ADR 0047), as the focus's and the help swipe's virtual fingers do, with a new `InputSource::kPointer`. Unlike the port's other virtual fingers it is aimed by the player, so Input's redirects apply to it (the minimap's moved compass, #176). It is not a touch for the key pad's auto-hide (`is_touch_source` false): a gamepad is in use.

**The focus.** The two must not fight:

1. A pointer tap drops the focus highlight (`PadPointer::highlight_dropped`; game.cpp draws none while set).
2. The focus's next use (a direction, OK or back it takes: `Focus::Stats::uses`) brings the highlight back and hides the pointer.

**Speed.** A radial dead zone of 0.2; past it the speed is 240 points/s × ((r − 0.2) / 0.8)², and it ramps to twice that over 0.6 s of pushing (`pointer_velocity`). Small pushes place the pointer precisely, and a held push crosses the 480 points in about 1.2 s. The pointer is clamped to the picture: 480 × 320 screen points, or with widescreen the view's width centred on them (`pointer_bounds(mt_widescreen_view())`, x from −44 to 524 at 568).

**The touchpad.** Absolute, as on a phone: a finger at (nx, ny) is a touch at the picture's (x0 + nx × width, ny × 320) (`touchpad_point`), one virtual finger per pad finger (`0x60 + (touchpad × 8 + finger) % 16`), while it is down. No click is needed. A finger on the pad hides the stick's pointer. **Rejected:** a relative trackpad that moves the stick's pointer. It needs a click to tap and gives no multi-touch, and the issue asks for the pad as the screen.

## Decision

- **Setting** `pad_pointer = off | stick | touchpad | both` (Controls, "Gamepad pointer", live). Original: off. Original + controller and Enhanced: both. No preset (no file): off.
- **Runtime:** `MT_ACTION_POINTER_TAP` (`"pointer_tap"`, "Pointer tap"), a port action, never delivered to the guest.
- **Bindings:** `pointer_tap = button touchpad, hold rightstick` in the field and in menus. `hold NAME` takes `button`'s names (not the d-pad; `hold guide` is reserved like `button guide`). The remap overlay lists `pointer_tap` and draws a hold as a pill, "Hold RS" / "Hold R3". The keyboard has no `pointer_tap`, because the mouse is its pointer.
- **Frontend:** `platform/src/pad_pointer.{h,cpp}` (`PadPointer`, `pointer_velocity`, `pointer_step`, `pointer_bounds`, `touchpad_point`, `draw_pad_pointer`), the hold in `Gamepads` (`set_hold_gate`, `turn`, `held_back`). game.cpp hands the pointer every SDL event before `Gamepads`, turns it after the focus, and releases it whenever an overlay takes the events: the disconnect pause (#184), the leaderboard, the settings menu, the remap overlay, the key pad editor. While the pause is open no axis, touchpad or button event reaches it.
- **Drawing:** an arrow, 12 screen points tall at the picture's scale, white (amber while its touch is down) with a dark outline. It is drawn in the window after the read-back and the captures, like the focus highlight, so shots, captures and goldens never contain it, and it is not drawn over the port's overlays.
- **Hides** after 3 s with no push and no touch (`PadPointer::kIdleSeconds`), on the focus's use, on a touchpad finger, and on `release_all`.

## Consequences

- With `pad_pointer = off` nothing changes: no gate, no pointer, R3 and the touchpad's click do what they did (the touchpad's click held a `pointer_tap` that does nothing). The gamepad, keyboard and remap real-asset runs, `input.menu_*` and the device goldens pass unchanged.
- With the pointer shown, a short R3 reaches `fast_forward` on its release instead of its press, and holding R3 for 0.3 s taps instead of fast-forwarding.
- `hold NAME` is a general control kind: any button can carry a second, held action while a gate is open. Only the pointer opens one today.
- Tests: `tests/cpp/pad_pointer_test.cpp` (CI: the curve, the clamp, touchpad points, one touch pair per tap, R3 shared, the hides, off, `hold NAME` in the file and the glyphs). `input.menu_pointer` (real assets, local): with a virtual Xbox 360 pad only, right-stick axis events steer the pointer and R3 held taps the Korean terms' agreement, the banner's close, About and its close and Help, then a held R3 and the stick swipe Help to its 2nd page (`drag ended, pages to page 1`) and the pointer closes it. The focus is never used.
- Needs a human: a DualSense's touchpad and a Steam Deck's right trackpad (Steam Input's gamepad layout) as the screen (#185).
