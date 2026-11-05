# 54. Remapping: bindings.ini, the port's own remap overlay, button glyphs drawn in code

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #70
- **Builds on:** [ADR 0048](0048-gamepad-actions-and-delivery.md) (actions, contexts, the binding table and its #68 keyboard addendum), [ADR 0051](0051-controller-focus-navigation.md) (`Focus`, the highlight drawn into the window only), [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (`settings.ini`, the data directory), [ADR 0047](0047-touch-input-mouse-and-fingers.md) (`Input`); the player's guide is [docs/controls/remapping.md](../controls/remapping.md)

## Context

#67 and #68 bind every action to gamepad controls and keys through one table (`default_bindings()`, `actions.h`): each row is a control and the action it sends in the field and in menus. Nothing lets a player change it. The issue asks for:

- a bindings file a person can edit, with validation;
- a simple remap screen drawn by the port, not by the game;
- glyphs for Xbox, PlayStation and Nintendo pads, from original or permissively licensed art, and a face-button swap for Nintendo pads;
- remapped bindings that persist and work, and glyphs that follow the controller type.

Facts that limit the options:

- **The game has no settings screen for controls** and no text input; it can't host a remap screen, and changing its UIKit or GL screens would break faithfulness.
- **Gamepad controls are positions.** SDL3's `a` / South is the bottom face button on every pad: A on Xbox, Cross on PlayStation, B on a Nintendo pad. #67 binds by position.
- **Every keyboard and gamepad button the defaults use is bound in some context**: Back / View is the map, Start the popup menu, Esc the popup menu in the field and back in menus. Only F1 among the function keys, the Guide button and the stick clicks (L3, R3) are free.
- **`Clet_Pause`** (the game's own pause on `applicationWillResignActive:`) stops the BGM and effects and saves the game state; it isn't a neutral "stop for a moment".
- **#69's `Focus`** takes the directions, OK and back on the UIKit screens, and its highlight is drawn into the window after shots and read-backs, so `--shot` PNGs and the device goldens never contain it.

What had to be decided:

- where the bindings live and what the file looks like;
- what a bad file does;
- what opens the overlay, and what the game does while it is open;
- where the overlay is drawn, and how it takes input ahead of `Focus` and the game;
- where the glyphs and the font come from, and which set shows;
- the Nintendo face-button option.

## Options considered

Where the bindings live:

1. **A `[bindings]` section in `settings.ini`.** One file, but `settings.ini` has no sections ([ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md)) and two contexts need two sections. A broken binding line would sit next to the window settings.
2. **`bindings.ini` next to `settings.ini` in the data directory. Chosen.** One job per file. `--bindings FILE` reads another file, and `--save-bindings` writes it back whole.

What a file lists:

1. **Only the player's changes.** Short, but a player who wants to change something has to know the defaults from the docs.
2. **The overlay writes every action of both contexts, the defaults included. Chosen.** A saved file is the whole table in one place. A hand-written file may still list only the lines it changes: an action without a line keeps its defaults.

What a bad file does:

1. **Refuse the file.** One typo would drop all of a player's bindings.
2. **Skip what is wrong, apply the rest, report each problem by file and line. Chosen.** Never fatal: the game always starts with usable controls.

What opens the overlay:

1. **Back + Start**, the usual chord. Both are bound (map, popup menu), so the first press would reach the game before the chord is complete.
2. **F1, Guide, or L3 + R3. Chosen.** None of them is bound by default, so opening never sends an action. F1 and Guide are reserved (they can't be bound); the stick clicks can be, and a player who binds one sends its action before the second click completes the chord.

What the game does while the overlay is open:

1. **Keep running.** Monsters keep attacking a player who is reading the list.
2. **`Clet_Pause`.** Stops the music and saves the game state; that's the game's own suspend behaviour with its side effects, not ours to fire.
3. **Give the guest no run-loop turn until the overlay closes. Chosen.** The same thing as an iOS app suspended between two run-loop turns: no timer, no tick, no UIKit animation. The engine already folds a long gap into its timers ([docs/re/timing-threading.md](../re/timing-threading.md)). Audio keeps playing (the mixer runs on SDL's thread).

Where the overlay is drawn:

1. **Into the composited screen.** It would appear in `--shot` PNGs and the goldens.
2. **Like #69's highlight, with SDL renderer calls after the read-backs.** Fine for one frame, but a full screen of text and glyphs in renderer calls can't be tested by bytes.
3. **Into a copy of the composited screen, which is what the window uploads. Chosen.** The game's screen (`g.screen.pixels()`, what shots write) stays unchanged, so shots and goldens never contain the overlay; the window and its read-backs show it. Drawing into a byte buffer makes the overlay testable by hash on every CPU.

Glyphs and font:

1. **The consoles' own button art** or font packs that copy it. Not allowed (issue: no Nintendo or Sony art).
2. **A permissively licensed font.** The runtime ships NanumGothic (OFL), but its rasteriser is the guest's `UILabel` path; using it here would tie the overlay to the runtime's text code.
3. **Shapes drawn in code and an original 5x7 pixel font. Chosen.** Discs with letters (Xbox, Nintendo), a cross, circle, square and triangle as plain outlines (PlayStation), pills for shoulders, triggers and menu buttons, a plus for the d-pad, key caps for keys. All of it is integer arithmetic, so the same bytes on every platform.

## Decision

**The file: `bindings.ini`** (`platform/src/bindings.{h,cpp}`), in the data directory next to `settings.ini`, or `--bindings FILE`. The full format is in `bindings.h` and [docs/controls/remapping.md](../controls/remapping.md):

- An optional `nintendo_buttons = position | label` before the first section, then `[field]` and `[menu]` sections of `action = control, control …` lines (`none` unbinds). A listed action gets exactly those controls in that context; each listed control leaves the action it had there.
- Controls by SDL's names: `key NAME` (`SDL_GetKeyFromName`, `key Comma` for the comma), `button NAME` (SDL's positional `a b x y`, also `south east west north`, shoulders, sticks, back, start, paddles), `trigger left|right`, `direction up|down|left|right` (the d-pad and left stick merged).
- Reserved: `key F1` and `button guide`.
- Each problem is reported as `FILE:LINE: …` and its line (or control) skipped. An action listed twice in a section loses its later line, and a control claimed twice is kept by the earlier action. After the file, each action that lost every control in a context it had by default gets a warning. A file over 64 KiB is ignored.
- Read at startup after `settings.ini`. The overlay's Save writes the full table (with header comments) through a temporary file and a rename.

**The overlay: `Remap`** (`platform/src/remap.{h,cpp}`):

- Opens on F1, Guide or L3 + R3, on any screen. Lists every action of the field and of menus with its keys and its gamepad controls, then the Nintendo-buttons row, Reset to defaults, Save and Cancel. Choosing an action waits up to 5 s for a key, a gamepad button, a trigger or a d-pad direction. Pressing a key makes it the action's only key in that context, and pressing a pad control makes it the only gamepad control. A control taken from another action is named in the status line, with a warning if that action is left with nothing. Esc, East, F1 or Guide cancels (back keeps the changes since 2026-11-03: see the addendum at the end). Only Save changes `Actions`, `Gamepads`' face-button mode and the file. If the write fails, the new bindings still apply for the session and the overlay says so.
- **Input first:** `handle_event` gives every event to `Remap::handle` before `Input`, `Gamepads` and `Keyboard`. An event that opens the overlay, and every key, pad, mouse and touch event while it is open, is consumed: `Actions`, and so `Focus` (the sink of `Actions`), never see it. Opening releases every held key and pad control, so nothing stays pressed in the game. Device add and remove events still reach `Gamepads`.
- **Pause:** while it is open, `on_turn` runs `run_remap`, which waits on SDL events, redraws the overlay over the last composited screen (not composited again: compositing could run guest drawing) and returns when it closes. The guest gets no turn, so `Actions::turn`, `Focus::turn` and `Rumble::turn` don't run either, and opening stops any rumble ([ADR 0053](0053-observed-functions-game-events-and-rumble.md)). The log notes how long it was open.
- **Drawing:** `shown_pixels` draws the overlay over a copy of the composited screen. `present` and `run_remap` upload that copy, so shots (`g.screen.pixels()`) never contain the overlay. Being part of the image, it is placed as the image is: inside the safe area on a phone ([ADR 0055](0055-ios-safe-area-landscape-orientations-and-input.md)), never under a notch or in the bars. #69's focus highlight is not drawn while the overlay is open.

**Glyphs** (`platform/src/glyphs.{h,cpp}`, `platform/src/canvas.{h,cpp}`):

- Three sets: Xbox (A B X Y, LB RB, LT RT, View, Menu), PlayStation (cross, circle, square, triangle, L1 R1, L2 R2, Share/Create, Options) and Nintendo (B A Y X at South East West North, L R, ZL ZR, − +). Keys show as key caps. Every glyph is drawn in code with the port's own 5x7 font.
- **Which set:** `GlyphTracker` follows the pad in use: `SDL_GetGamepadType` of a pad that is connected or pressed (a button, or an axis past half). When the shown pad goes, the set falls back to the active pad, else the last connected one, else Xbox.

**Nintendo face buttons** (`FaceButtons`, `gamepad.h`): `position` (the default, as #67) binds by place, so the bottom button is jump on every pad. `label` binds by printed letter on pads SDL reports with Nintendo labels (`SDL_GetGamepadButtonLabelForType`): the button labelled A acts as `a`, and so on. That puts A (right) on jump, as Xbox players expect from the letter. The glyphs follow the mode.

## Consequences

- A player can rebind every action of both contexts in a file or in the game, and the change persists: `input.remap_play` (real assets, `tests/cpp/remap_real_test.cpp`) runs the real game with a `bindings.ini` that moves attack to V and the right stick click. In the field V and R3 attack and Z, X and RT don't. It then opens the overlay with F1, binds quick slot 1 to Q, saves, and checks that Q drinks the Red Potion. A restart on the same data directory checks all of it again.
- `input.bindings_flags` checks `--bindings FILE --save-bindings` on the shipped binary: a bad line reported by file and line, the rest applied, the file written back whole, the data directory's file untouched.
- `platform.bindings*` and `platform.remap*` (CI, `bindings_test.cpp`, `remap_test.cpp`) check the parser, every validation message, the round trip, the overrides in `Actions`, the overlay driven by synthetic events (open, navigate, capture, take-over warnings, timeout, cancel, reset, save, save failure), the glyph set switching on virtual Xbox 360, PS5 and Switch Pro pads connecting, pressing and disconnecting, the Nintendo label mode, and one capture of the overlay per set by FNV-1a hash.
- The overlay is never in `--shot` PNGs or goldens, but it is in window read-backs.
- The game is frozen while the overlay is open. A loading screen that presents from inside a long call (no run-loop turn) keeps going under the overlay until its next turn.
- Not covered by an automated test: the interactive `run_remap` loop (waiting on window events) in a real window. The real-asset test drives the overlay with SDL events pushed through the same routing as `handle_event` (the overlay first, the devices with what it does not take), with SDL's dummy video driver, between the game's own run-loop turns.
- A later overlay of the port's own (settings, hints) can reuse `Canvas`, the font and the glyphs.

## Addendum: with GPU presentation (#130, 2026-10-30)

With `gfx_backend = gpu` the composited screen stays on the GPU (ADR 0068), so there is no CPU copy to draw into. The overlay is drawn over UIKit's premultiplied overlay layer instead, and the screen texture is composed again. Shots still don't have it. `Canvas` now blends alpha as well, and over an opaque image that gives the same bytes. While the overlay holds the turn, a minimise draws nothing, and a quit ends the hold (#128). The details are in [ADR 0068's addendum](0068-gpu-presentation-without-read-back.md#addendum-the-remap-overlay-on-the-gpu-and-its-hold-under-the-lifecycle-70-with-130-and-128-2026-10-30).

## Addendum: the glyphs beside the game's buttons (#146, 2026-10-30)

The glyphs are also drawn as button prompts next to the game's own buttons ([ADR 0051's addendum](0051-controller-focus-navigation.md#addendum-a-focus-marker-or-a-button-prompt-on-every-screen-146-2026-10-30)). There the set follows the device used last, not only the pad: `DeviceTracker` (`platform/src/prompts.h`) picks key caps after a key press, `GlyphTracker`'s set after a pad button, stick push or a pad connecting, and nothing after a click or a touch. The control shown is the one bound to the action in menus (`prompt_control`), so a remap or the Nintendo label mode shows at once; for the keyboard Enter, Esc and M are preferred where bound. `prompts_test.cpp` checks the switch live on virtual Xbox 360, PS5 and Switch Pro pads and synthetic keys, by hash.

## Addendum: back keeps the changes (2026-11-03)

With a controller, rebinds never stuck and `bindings.ini` was never written. The overlay opens from the settings menu (Controls → Bindings), where every change applies at once and B goes back. In the overlay, B (and Esc, F1, Guide) was Cancel: a player who rebound a button and left with B, as the menu around it taught them, lost every change, and the hints only said "cancel". **Behaviour change:** back (`Remap::back`) now saves when the copy differs from the bindings in effect at open: applied, written, closed, as the Save row. With nothing changed it just closes. The Cancel row is the one way to discard. A failed write applies the bindings and keeps the overlay open with the error; back then closes it. The hints say "done". Evidence: `remap: with a pad only, a rebind then B: applied and written` and `remap: Cancel discards; Esc, F1 and B keep the changes; Reset restores the defaults` (CI); the overlay's capture hashes were updated for the new hint text.

## Addendum: the controls card (#193, 2026-11-05)

The game has no controls screen, and the remap overlay is a long table for changing bindings, not for learning them. New players of a 2010 touch game on a pad cannot guess that Y is the Red Potion and LT the cash shop. The port therefore draws a **controls card**: one page of the controls for the device in use (`platform/src/controls_card.{h,cpp}`, [controls/controls-card.md](../controls/controls-card.md)).

- **What it shows.** For a pad, a picture of a pad drawn in code (triggers, shoulders, d-pad, face buttons, View / Menu, the sticks) in the set `GlyphTracker` picks, its triggers and shoulders labelled with their field action. For the keyboard, a small keyboard with the keys bound in the field lit. Below the picture come two lists, *in the field* and *in menus*: each action with up to two of the device's controls, or *unbound*. For touch (the device none: a click or a tap was the last input), the touch key pad's schematic in the player's `keypad_layout`, with each key's name. The device follows `DeviceTracker`, as the button prompts do.
- **Labels from the live bindings.** The lists are built from `Actions::bindings()` by `controls_card_lines`, over `action_slots()`. Each list takes every (context, action) the device has a control for, now or by default, so an action added later (the menus' `page`, ADR 0114, or a pointer action) shows without code in the card. The four directions are one *Move* line when each has a control of the device, and one line each otherwise. An action the player unbound stays on the card and says *unbound*. F1 / Guide (reserved, never bound) close the field's list as *Settings*.
- **Opening.** From the settings menu's Controls tab (*Controls card*, an action row: it opens over the menu, which shows again when it closes). It is also offered once after the first-launch preset picker, whose second step asks *Show controls* / *Start playing*. The answer goes to `GameOptions::show_controls_card`, with the device the player answered with (`controls_card_device`: the card shows it until the game sees input of its own). The card opens 3 s into the run, once the game's main menu is up. Never in a scripted run.
- **Pause and input.** It counts in `overlay_open`, so `run_overlays` holds the run-loop turn as for the remap overlay. `handle_event` gives it every event after the disconnect pause and before the other overlays. While open it takes every input event. Any press (a key, a click, a tap, a pad button; not a repeat or a stick) closes it, and that press's release is taken too. Opening lets go of every held key and pad control.
- **Evidence.** `controls_card_test.cpp` (CI) checks that rebinding attack to V shows V, both in the lists and on the small keyboard, and that every action on the card has a control of the device or says unbound (unbinding jump and left shows it). `page` is on the menus side. The device and the pad set follow the input on virtual Xbox 360, PS5 and Switch Pro pads, and an open card redraws when a pad connects. It also checks what the card takes and the release it swallows, the touch card following a custom layout, and the picker's second step, both alone and in its window loop. The card is drawn for the keyboard, Xbox, PlayStation, Nintendo, touch and a custom touch layout and compared exactly with synthetic goldens (`tests/golden/data/controls_card_*.png`, port art only).
