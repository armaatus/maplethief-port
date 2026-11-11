# 140. Switch input: the controllers as Nintendo pads, A to confirm, + for the settings menu, and the touch screen fixed

- **Status:** Accepted
- **Date:** 2026-11-11
- **Issue:** [#239](https://github.com/armaatus/maplethief-port/issues/239) (epic [#233](https://github.com/armaatus/maplethief-port/issues/233))
- **Builds on:** [ADR 0138](0138-nintendo-switch-homebrew-target.md) (the Switch target and its SDL), [ADR 0047](0047-touch-input-mouse-and-fingers.md) (fingers), [ADR 0048](0048-gamepad-actions-and-delivery.md) (gamepads, the active pad), [ADR 0051](0051-controller-focus-navigation.md) (menus), [ADR 0052](0052-hiding-the-on-screen-key-pad.md) (the key pad's hiding), [ADR 0053](0053-observed-functions-game-events-and-rumble.md) (rumble), [ADR 0054](0054-bindings-file-remap-overlay-and-button-glyphs.md) (bindings, glyphs, `nintendo_buttons`), [ADR 0070](0070-settings-registry-and-presets.md) (the platform rows), [ADR 0076](0076-settings-menu.md) (the settings menu)

## Context

ADR 0138's consequence: the pinned SDL (`nx-mod/sdl3-nx` 07ea26a3) reports the Switch's controllers as Standard pads with Xbox labels, while their layout is Nintendo's. Read from its source (`src/joystick/switch/SDL_sysjoystick.c`, `src/video/switch/SDL_switchtouch.c`); nothing here ran on a console:

- **Eight pads, always.** `SWITCH_JoystickInit` adds eight joysticks (instance ids 1 to 8 for libnx's npads No1 to No8; the first also reads the handheld Joy-Cons) whether anything is connected or not, and `SWITCH_JoystickDetect` is empty: no pad is ever added or removed later. A change of controller (Joy-Cons attached or detached, a Pro Controller paired) changes the npad's style; SDL then releases that pad's buttons and, undocked, opens the controller applet.
- **One name, no ids.** Every pad is `Switch Controller`, its GUID made from the name (no vendor or product id), so `SDL_GetGamepadType` says Standard and the labels are `SDL_GAMEPAD_FACE_STYLE_ABXY`'s: South A.
- **The mapping is positional.** `a:b1` (libnx's B), `b:b0` (A), `x:b3` (Y), `y:b2` (X): South is the bottom button, as on every other pad. ZL and ZR are buttons mapped to the triggers, `-` is back, `+` start; no guide (HOME is the system's), no capture.
- **A Joy-Con alone** on its side (`hidSetNpadJoyHoldType(Horizontal)`) gets its own mapping when the npad is not JoyDual and its device type is JoyLeft or JoyRight: its four buttons by their place on the side, the stick as the d-pad (no axes), SL and SR as the triggers, its stick's click as start and its `-` (left) or `+` (right) as back; no shoulders.
- **Rumble** goes to both sides of the controller (two handles, one on a Joy-Con alone), handheld included.
- **Touch is broken.** `SWITCH_PollTouch` was ported from SDL 2 and still passes `false` where SDL 3's `SDL_SendTouch` takes the event type, and never sets `found` when a finger id matches. No finger ever goes down, so no `SDL_EVENT_FINGER_DOWN` reaches the game, and the motions of a finger that is not down are dropped. The fork also sets `SDL_HINT_TOUCH_MOUSE_EVENTS` to 0.

The issue asks for A to confirm and B to go back in menus, a field layout that fits Joy-Con, `+` for the port's settings menu (Guide, its other way in, does not exist here), Nintendo glyphs, the key pad hidden with a controller and shown on a touch, rumble with the strength setting, and player 1 as the first pad pressed.

## Options considered

### The pads' type

1. **Trust SDL.** Rejected: the Xbox glyphs would put A where B is, and `nintendo_buttons = label` would do nothing (it only moves labels that differ from the Xbox layout).
2. **A mapping with a `type:` field** added through `SDL_AddGamepadMapping`. Rejected: one name for every controller mode, so one type for a Pro Controller and a Joy-Con alone, and it changes what SDL reports, which the device log should show as it is.
3. **The port's own type per pad, from libnx's mode.** **Chosen.** `platform/src/switch_pads.{h,cpp}`: `switch_pad_mode()` reads the npad's style set and device type (`hidGetNpadStyleSet`, `hidGetNpadDeviceType`, and the handheld npad's style for pad 1) with the same test SDL uses to pick a single Joy-Con's mapping; `pad_type()` gives Joy-Con pair (handheld and the pair), Pro Controller, left or right Joy-Con (and Pro for an npad with nothing connected) to a pad named `Switch Controller` of Standard type, and SDL's own type to every other pad. `Gamepads`, the glyph tracker, the remap overlay and the settings menu use it. Off the console the rule is off unless a test installs a source of synthetic reports.

### A to confirm, B to go back

1. **By position, with the menu rows swapped** (South back, East OK) and every port screen taught that East confirms on the Switch. Rejected: a dozen screens with their own South / East tests, and `nintendo_buttons = label` would then turn it back.
2. **`nintendo_buttons = label` as the Switch's default**, with a gamepad table written for it. **Chosen.** Under `label` the button labelled A is the control `a`, so the game's menus (`ok = button a`) and the port's screens that read the face buttons through the pad's face mode (the settings menu, the remap overlay) confirm on A and go back on B with no change of their own. The screens with no `Gamepads` (the preset picker, the key pad editor, the import screen) read presses through `screen_button_control()`, which uses the platform's default, and draw their hints with it.

### The field layout

The table keeps the field as every other pad has it, by place: the bottom button jumps and the left one attacks, so that jump is under the thumb on a Pro Controller, a pair and a Joy-Con on its side alike. Written for `label`: `jump = button b`, `attack = button y`, `slot1 = button x`, `slot2 = button a`. The triggers (ZL / ZR, and SL / SR on a Joy-Con alone, which has no shoulders) are quick slots 3 and 4, so all four slots are on a lone Joy-Con too. L is the map and R the cash shop. `-` opens the game's menu (Start does elsewhere, but `+` is the settings menu's). Menus: A OK, B back, Y revive, X the premium revive, L / R minus / plus, ZL the page modifier, ZR `#`, `-` the map. The stick clicks keep auto-run and fast-forward.

### The settings menu

`+` (start) is the settings menu's on the Switch, as Guide is elsewhere: `settings_button()` opens and closes it, and `reserved_control()` keeps it out of the table (the bindings file refuses `button start`). On a Joy-Con alone start is the stick's click, so that click opens it there. Quit is the settings menu's Quit row (#179), which sends `SDL_EVENT_QUIT`; HOME and closing the app from it stay the system's.

### The key pad

Handheld, the Joy-Cons are attached; docked, a controller is needed to play at all: a controller is always there. So `hide_keypad` is `auto` in every preset on the Switch (a platform row: Original's `never` would cover the picture for nothing), and `auto` starts hidden there (`kKeypadAutoStartsHidden`). Any touch shows it and is delivered, as ADR 0052 says.

### Touch

Patch the fork's `SWITCH_PollTouch` at configure time, as #238 patched `SWITCH_SetWindowSize`: `cmake/patches/sdl3-nx-touch.cmake`, a second `PATCH_COMMAND`. A finger that left is `SDL_EVENT_FINGER_UP`, a new one `SDL_EVENT_FINGER_DOWN`, one that moved a motion, matched by libnx's finger id. The script changes the file only if it holds the expected text and fails the configure step otherwise; a second run does nothing. ADR 0047's `Input` then maps the fingers through the scaling's inverse unchanged (the backend normalises them against 1280 x 720, the handheld screen, which is the window then). Docked there is no touch screen.

## Decision

As chosen above: the Switch's own pads get Nintendo types by their mode (`switch_pads.h`); `nintendo_buttons = label` and the Switch's gamepad table are its defaults (`face_buttons_default`, `default_gamepad_bindings(SettingsPlatform::kSwitch)`); `+` opens the settings menu and is never bound; `hide_keypad = auto` is a platform row and `auto` starts hidden there; the fork's touch polling is fixed at configure time. Rumble and player 1 need nothing new: rumble goes through `SDL_RumbleGamepad` on the active pad with `rumble_strength` (ADR 0053), and the active pad is the first one pressed while no other holds anything (ADR 0048), among the eight that are always there.

Each pad's report is logged when it is opened and whenever its mode changes (`input: pad N: 'Switch Controller', SDL type standard (real standard), SDL labels S=A E=B W=X N=Y, rumble yes; libnx style 0x2 device 0x0 handheld 0x2: handheld, shown as Nintendo (joyconpair)`), so a device run confirms what this ADR read from the source.

The glyphs for a Joy-Con alone follow the Switch's SDL: SL and SR for its triggers (and its shoulders, which other drivers use for them), its stick's click (LS or RS) for start, its `-` or `+` for back. The face letters stay Nintendo's by place (B at the bottom, A on the right), as the console's own menus show a Joy-Con on its side.

## Consequences

- A player who switches `nintendo_buttons` to `position` on the Switch keeps the table written for `label`, so A and B swap roles in the field and in menus; the remap overlay's Reset brings the defaults back. `docs/controls/switch.md` says so.
- A lone Joy-Con has no L or R: the map and the cash shop are on the key pad's touch keys or the game's menu there; menus' tabs go by left and right.
- The touch fix and #238's window fix are candidates for the fork's upstream. A new pin must drop or redo them; both fail the configure step if the text moved.
- Nothing here ran on a console. Joris's run checks every controller mode, a remap that survives a restart, rumble on a hit and touch only (#239's `needs-human` steps), and the log lines above show what SDL really reports.

**How we'd know this was wrong:** the device log shows another name, type or labels than the source says (then `is_switch_console_pad` or the mode test changes), the face buttons come out swapped on hardware, or touch still does nothing after the patch.
