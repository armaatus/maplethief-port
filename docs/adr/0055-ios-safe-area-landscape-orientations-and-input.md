# 55. iOS: the safe area, both landscape orientations, and touch and gamepads on iOS

- **Status:** Accepted (the device check is open: #73 is `needs-human`, after #83)
- **Date:** 2026-10-30
- **Issue:** #73
- **Builds on:** [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (`place()`, the window → screen inverse), [ADR 0047](0047-touch-input-mouse-and-fingers.md) (`Input`, interruptions end touches), [ADR 0048](0048-gamepad-actions-and-delivery.md) (SDL3 gamepads), [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (the 480 × 320 landscape-left screen)

## Context

The original runs on a 3:2 iPhone in landscape left only (Info.plist `UIInterfaceOrientation` = `UIInterfaceOrientationLandscapeLeft`, docs/ANALYSIS.md). A modern iPhone is about 19.5:9 and has strips that the system covers or reserves:

- the notch or the Dynamic Island on one long side;
- the home indicator along the bottom.

UIKit reports these strips as the view's `safeAreaInsets`. SDL 3.4 turns them into the window's safe area (`SDL_GetWindowSafeArea`, `SDL_EVENT_WINDOW_SAFE_AREA_CHANGED`; `SDL_uikitview.m` `safeAreaInsetsDidChange`) in window points, in the current orientation. In landscape, iOS reports equal left and right insets on notched phones (59 / 59 points, plus 21 at the bottom, on an iPhone 15). The code must not rely on that symmetry.

This Mac has no Xcode or iOS SDK, and the iOS app target (#83) doesn't exist yet. Everything here is built and tested on the desktop; the device check waits for #83.

What had to be decided:

- where the image goes when the window has a safe area;
- which orientations the port allows, and what the game sees when the phone turns;
- what the touch, mouse, focus and gamepad code needs on iOS.

## Options considered

**Where the image goes.**

1. **Ignore the safe area.** The image's edges would sit under the island and the home indicator. On an iPhone 15 the fit mode is 1769 × 1179 pixels and covers all 1179 rows. Rejected.
2. **Centre the image in the safe area.** It stays clear of the strips. But when the strips are uneven (only a home indicator, or a notch on one side) the image moves off the screen's centre by half the difference, even when it would have fit centred.
3. **Size the image for the safe area, centre it in the whole drawable, then clamp it into the safe area. Chosen.** It stays centred on the physical screen whenever it fits, and is moved only as far as a strip needs. With zero insets this is exactly ADR 0043's placement.
4. **`SDL_SetRenderViewport` to the safe area.** This is option 2 done by the renderer, and it would also clip the bars, which should extend under the strips. Rejected.

**Orientations.**

1. **Landscape left only, as the original.** A player holding the phone the other way gets the picture upside down. With a notch, the notch is then always on the same side.
2. **Landscape left and right. Chosen.** This is what iOS games do now. SDL turns the window's coordinate system with the phone, so the frontend sees a landscape window in either case, and only the insets swap sides. The game is never told: the runtime always presents landscape left (`screen_from_window`, `uikit.h`). `UIDevice` has no orientation, nothing calls the game's `shouldAutorotateToInterfaceOrientation:` (0x4DB1, 0xBD30D), and Chartboost's `statusBarOrientation` code never runs (ADR 0026). The guest renders its 480 × 320 image the same way in both orientations.

## Decision

**The placement (`platform/src/scaling.{h,cpp}`, pure).**

- `Insets {top, left, bottom, right}` are in window points, the complement of `SDL_GetWindowSafeArea`. Negative and non-finite sides count as 0.
- `safe_rect(output, insets, density)` is the safe area in drawable pixels. Each inset is multiplied by the window's density and rounded up, so the image never reaches into a strip. A tolerance of 0.001 pixel keeps 59 points at 3× at 177 pixels, not 178.
- `place(mode, output, image, insets, density_x, density_y)` works in three steps:
  1. **Size.** The mode's size for the safe area: integer k = min(safe w / 480, safe h / 320), or the fit rectangle of the safe area. Under 1× it falls back to fit.
  2. **Centre.** It is centred in the whole drawable, with the same formulas as ADR 0043; the odd pixel goes right and below.
  3. **Clamp.** It is clamped into the safe area.
- `Placement::safe` records the safe area. The old `place()` is this one with zero insets.
- The bars, in the border colour, fill the whole drawable, so only they extend under the strips.
- The touch inverse doesn't change. `drawable_to_screen` uses only `Placement::rect`, so a window point over a strip is outside the image, like any other bar.

**Examples on an iPhone 15** (2556 × 1179 pixels; insets 0, 59, 21, 59 points):

- **The safe area** is 2202 × 1116 at (177, 0).
- **Integer** stays 3×, 1440 × 960 at (558, 109), the same as without insets.
- **Fit** shrinks from 1769 × 1179 at (393, 0), which lay under the island and the home indicator, to 1674 × 1116 at (441, 0).

**The presenter (`present.{h,cpp}`).**

- `Presenter::draw()` reads `window_insets(window)` (window size less `SDL_GetWindowSafeArea`) and the density every frame. A rotation, or a `SAFE_AREA_CHANGED` event, takes effect at the next frame.
- `set_forced_insets()` replaces the window's insets.
- `insets()` is what the last frame used. `game: drawable …` logs the insets and the safe area when there are any.

**`--safe-area T,L,B,R`** (window points, each 0 to 4096, decimals allowed; any mode) forces the insets. It is a test aid, so a desktop run can show the layout of a phone. It isn't a setting: a desktop window has no strips. On the desktop, SDL reports zero insets, except for a fullscreen window on a MacBook with a notch. There Cocoa now gets the same treatment, which is correct.

**Both landscape orientations (`main.cpp`, under `#if defined(SDL_PLATFORM_IOS)`).**

- `SDL_HINT_ORIENTATIONS = "LandscapeLeft LandscapeRight"` is set before `SDL_Init`. The window is resizable, and without the hint SDL would allow portrait too (`UIKit_GetSupportedOrientations`).
- SDL intersects the hint with Info.plist's `UISupportedInterfaceOrientations`, so **#83's Info.plist must list both** landscape orientations, and no portrait one.
- The window is created `SDL_WINDOW_FULLSCREEN`. SDL then hides the status bar (`prefersStatusBarHidden`). It also uses `SDL_HINT_IOS_HIDE_HOME_INDICATOR`'s default for fullscreen windows: the indicator is dim and edge swipes are deferred, so the first swipe shows the indicator and the second goes home. A swipe that starts on the key pad near the bottom edge therefore doesn't leave the game.
- **Turning the phone ends the touches that are down.** `Input` treats `SDL_EVENT_DISPLAY_ORIENTATION` and `SDL_EVENT_WINDOW_SAFE_AREA_CHANGED` like focus loss: every live touch is Ended at its last point, never Cancelled (ADR 0047). A finger held across a turn would otherwise jump to the mirrored point and drag the d-pad across the screen. The gamepad and the keyboard are not affected by a turn.

**The iOS audit.** Each item is checked against SDL 3.4.18's UIKit backend and our code.

| What | On iOS | Our code |
|---|---|---|
| Real touches | `SDL_TOUCH_DEVICE_DIRECT` | taken as multi-touch (ADR 0047): unchanged |
| `SDL_HINT_TOUCH_MOUSE_EVENTS` | on: a mouse event from each touch, `which == SDL_TOUCH_MOUSEID` | dropped: unchanged |
| `SDL_HINT_MOUSE_TOUCH_EVENTS` | **on by default on iOS** (off on the desktop): a touch from each mouse event, `SDL_MOUSE_TOUCHID` | dropped: unchanged |
| A pen (Apple Pencil on iPad, drawing tablets on the desktop) | both a mouse press (`SDL_PEN_MOUSEID`) and a touch (`SDL_PEN_TOUCHID`, a direct device) | **fixed:** the pen's mouse events are dropped and its touch counts, so a press is one touch, not two |
| A real mouse or trackpad (iOS 14+, `GCMouse`) | a mouse with its own id | the left button is one touch, as on the desktop |
| Backgrounding: Control Center, a call, the home gesture | `sceneWillResignActive` → `SDL_EVENT_WINDOW_MINIMIZED` + keyboard focus lost; coming back → `RESTORED` + `FOCUS_GAINED` | `Input`, `Gamepads` and `Keyboard` release on `MINIMIZED`/`FOCUS_LOST`; `FocusMute` mutes and unmutes: unchanged |
| Gamepads (MFi, Xbox, PlayStation) | `SDL_INIT_GAMEPAD` → the GameController framework driver; hot-plug through `SDL_EVENT_GAMEPAD_ADDED` | `Gamepads` uses only the gamepad API, no desktop-only hints: unchanged |
| Gamepad events in the background | off (`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` default) | holds are lifted at `MINIMIZED` already |
| The accelerometer | SDL 3 has no accelerometer joystick (it is a sensor), so it can't become a "first active pad" | nothing to do |
| The on-screen keyboard | appears only on `SDL_StartTextInput` | never called (the game has no text entry, #68) |
| Hints | none set by the frontend, except the iOS orientation hint above | — |

The only code change from the audit is the pen fix. Everything else already holds on iOS, or depends on #83.

**Left for #83 (the app target), found by the audit:**

- Info.plist: both landscape orientations (above), `UIRequiresFullScreen`, `UIStatusBarHidden`, and `CADisableMinimumFrameDurationOnPhone` for 120 Hz ProMotion (SDL reads it).
- Link GameController and CoreHaptics. #72's rumble goes through CoreHaptics on iOS.
- Our blocking `main()` runs inside SDL's `SDL_main` on iOS (`SDL_main.h` is already included). The bundle directory is `SDL_GetBasePath()` and the data directory is `SDL_GetPrefPath()`.
- **The game's own pause and resume are not delivered.** The runtime has `mt_uikit_lifecycle_event` (`applicationWillResignActive:` = `Clet_Pause`, `applicationDidBecomeActive:`, `applicationWillTerminate:`; the game's delegate implements all three: 0x2BD9, 0x2BB5, 0x2ACD), but the frontend never calls it. On iOS the game would keep running, and rendering, in the background. Follow-up #128 covers this: SDL's background and foreground events to these messages, no presenting while in the background, and the same on a desktop minimise.

## Device checks (needs-human, once #83 runs on the iPhone)

Each is a yes/no check; a no gets a comment on #73 with the iPhone model and iOS version.

1. **Touch only, no controller paired.**
   - The main menu, Play, character select and the first map work by touch.
   - Two fingers at once work: hold the d-pad and attack.
   - A tap in the black bars does nothing.
   - The key pad is visible (`auto`, no pad used yet).
2. **An Xbox controller paired over Bluetooth**, then **a PlayStation controller** (DualShock 4 or DualSense):
   - each pad drives the menus and the field (ADR 0048's bindings);
   - the on-screen key pad fades out on the first press and comes back on a touch (ADR 0052);
   - turning the pad off mid-walk stops the hero (hot-plug release);
   - rumble on damage, if #72 is merged.
3. **The notch or Dynamic Island, in both orientations.** Hold the phone landscape with the island on the left, then on the right:
   - no part of the 480 × 320 image is under the island or the home indicator; only the bars are;
   - the image is centred, or moved only as far as a strip needs;
   - repeat with `scale_mode = fit` in `settings.ini`, where the strips matter most.
4. **Rotating while playing:**
   - turn the phone over (left ↔ right) while walking with a held d-pad finger: the hero stops (the touch ends), nothing is pressed at the mirrored point, and the next touch works at once;
   - with a controller, turning the phone doesn't interrupt the walk;
   - the picture is never shown in portrait.

## Evidence

- **CI** (synthetic, `tests/cpp/safe_area_test.cpp`, `platform.safe_area: *`):
  - **What runs.** Every scale mode, render scales 1 and 2. Thirteen screens:
    - the iPhone 15, X and XR at their real landscape insets and densities 3, 3 and 2;
    - an iPhone SE and two desktop windows, with no insets;
    - asymmetric cases: a notch on one side only, a notch with a home indicator, a top strip, all four sides at 2×, 1.5× with fractional insets, a safe area under 1×, and wide strips.
  - **Each placement** is run in both orientations (the insets mirrored), and the test checks that:
    - the safe area equals each inset × density rounded up;
    - the image lies wholly inside it, in whole pixels, at the mode's size for it;
    - the image is centred in the drawable, then clamped;
    - a touch in a strip is outside;
    - the window → screen → window round trip holds on a grid, every point inside the safe area.
  - **The two orientations** mirror each other: the same size and y, x mirrored within the odd pixel, and exactly where a strip moved the image.
  - **Worked examples.** The iPhone 15 numbers above, and 1000 × 700 with a 30-point strip: x 30 for landscape left, 10 for landscape right, unmoved under 20 points.
  - **The focus highlight** (#69, ADR 0051): `focus_highlight` clamps the outline to the image, so for every screen above, every mode and both orientations it stays inside the safe area. That includes full-screen and edge controls with the outset.
  - **Zero insets equal ADR 0043's `place()`** at 309 sizes (300 random), every mode, render scales 1 and 3, densities 1 to 3.
  - **`--safe-area`'s parser**, with 16 bad forms.
  - **A dummy-driver presenter with forced insets:** in every mode and with four inset sets, the read-back has 0 mismatches, and the bars in the border colour cover the strips. The dummy window's own insets are zero.
  - **`input_test.cpp`** (`platform.input: iOS: …`):
    - SDL fingers through an inset placement in both orientations (the d-pad and attack corners, a drag);
    - a finger in the strip is dropped;
    - `DISPLAY_ORIENTATION` and `SAFE_AREA_CHANGED` end a held finger and a held mouse press at their last point;
    - a pen's mouse press plus its touch give one touch.
  - **Mutation check:** without the clamp, 125 assertions in 4 of the 6 new cases fail.
- **Local, real assets** (macOS, `MapleThief --window 1100x700 --border-color #203040 --fixed-step 16.666667 --no-audio --frames 240 --capture build/w73-capture-*`; captures in `build/`, never committed):

  | Run | Image (pixels) | Strip pixels in the border colour |
  |---|---|---|
  | no insets | 960 × 640 at (70, 30) | — |
  | `--safe-area 0,100,0,0` (landscape left) | 960 × 640 at (100, 30) | 70,000 / 70,000 |
  | `--safe-area 0,0,0,100` (landscape right) | 960 × 640 at (40, 30) | 70,000 / 70,000 |
  | `--safe-area 0,59,21,59` (iPhone-like strips, fits unmoved) | 960 × 640 at (70, 30) | 103,222 / 103,222 |

  - **The pixels.** In all four runs the 960 × 640 image's pixels are identical (sha256 `92c487f2f320a039…`), so only its position changed. Every bar pixel is the border colour, and no image pixel lies in a strip.
  - **Drivers.** Each run read back with 0 mismatches, on the dummy driver and on a real Cocoa window with Metal.
  - **The phone-shaped fit run.** At 852 × 393, `--scale-mode fit` goes from 590 × 393 at (131, 0) (under the home indicator's 21 rows) to 558 × 372 at (147, 0) with `--safe-area 0,59,21,59`, with all 61,788 strip pixels in the border colour.
  - **The device goldens** (`platform.device_golden`, with `--golden-dir work/golden`) are unchanged with zero insets: all 7 of our screens match exactly (0 of 153,600 pixels differ). The device screenshots aren't captured yet, so those 7 cases skip.
- **Not run:** anything on iOS. There is no Xcode or iOS SDK here, and no app target yet (#83). The `SDL_PLATFORM_IOS` blocks in `main.cpp` were only syntax-checked, by forcing the macro on with the macOS SDK (`-fsyntax-only -DSDL_PLATFORM_IOS=1`); they were never compiled for iOS.

## Consequences

- The desktop doesn't change: zero insets give ADR 0043's placement bit for bit (tested), and no flag or setting is needed.
- On a phone, the integer mode usually keeps its scale (3× on an iPhone 15). The fit modes shrink a little to stay above the home indicator.
- A touch that begins in a strip is a bar touch and is dropped, so the island's strip can't press the d-pad by accident.
- A touch held across a turn of the phone ends. The player has to put the finger down again, which a 180° turn needs anyway.
- If Joris prefers the original's single orientation, removing `LandscapeRight` from the hint and from Info.plist is enough; nothing else depends on it.
- **How we'd know this was wrong:**
  - On the device, the image sits under the island or the indicator. Then SDL's safe area isn't the view's, and we'd read `safeAreaInsets` of the window instead.
  - A rotation briefly shows the old placement at the new orientation, for one frame. Then the placement should be recomputed on `SAFE_AREA_CHANGED` before the next draw, rather than at it.
  - Players find the off-centre move annoying with a single strip. Then we'd centre in the safe area instead (option 2).

## Addendum: the safe area with the GPU renderer, the filters and GPU presentation (#73 with #120, #88 and #130, 2026-10-30)

- **One placement for every renderer.** `Presenter::draw` reads the insets (forced, or `SDL_GetWindowSafeArea`) and places the image before it picks the pass. The identity pass (`present_copy.frag`), #88's filters (their `PresentSizes` take the placed rectangle) and GPU presentation's render-target screen texture (ADR 0068) all draw into that rectangle. So `--safe-area` and a phone's insets need nothing per renderer. `run_game` sets the forced insets right after the scaling, before the filter's pass.
- **`--gfx-prototype`** gets both: the forced insets and, on SDL's GPU renderer, the identity pass.
- **The window.** #120's `GfxBackendHost` makes the renderer (SDL's GPU renderer for `gfx_backend = gpu` or a filter), so main.cpp creates the window with `SDL_CreateWindow` and no longer uses `SDL_CreateWindowAndRenderer`. #73's flags go to that window: `SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY`, plus `SDL_WINDOW_FULLSCREEN` on iOS.
- **Evidence.** `gfx_gpu.present filters: forced safe-area insets on SDL's GPU renderer, every pass` (Metal, local) covers the identity pass and xbr, crt and lcd, each in the integer and fit modes, with insets 40,59,21,59 in a 1000 × 700 output. The image lies inside the safe area, the read-back check passes (exact for the identity pass) and the strip shows the border colour. `platform.safe_area` (CI) still covers the software renderer.

## Addendum: haptic taps on the touch key pad (#194, 2026-11-05)

- **What.** On an iPhone each press of a touch key pad key gives one haptic tap, so the glass feels more like buttons. `keypad_haptics = off | light | medium` (Controls; `--keypad-haptics`; outside the presets; applied live). The default is `light` on iOS and `off` elsewhere, where the row is hidden from the settings menu. The row is still read and written there, so a synced `settings.ini` stays clean. It is separate from the game's own vibration option and from the gamepad's `rumble` (ADR 0053).
- **What a press is: the game's own hit test.** The pad hit-tests every finger against `KeyPadArea.png` with `-[TouchKeyPadView colorAtPixel:onImage:]` `0xB4DB5`, when the finger lands and at each move (docs/controls/keypad.md). A wrapper on it (`mt_hook_wrap`, so ADR 0086's `keypad_layout` handler stays inside it) lets the read run, then turns the returned UIColor into a key with the runtime's new `mt_cg_color_rgba` (alpha 0 or nil: no key). It attributes the key to the touch being delivered, via the new `mt_uikit_touch_delivering`, which gives the finger and phase while ADR 0047's queued touch is sent to its view. A custom layout, widescreen's offset (#168) and the game's own rules come along for free. A rejected alternative: decoding the mask in the frontend at the SDL event. It would save at most a frame of latency, but it needs the bundle's PNG in the frontend and a second copy of the layout's mapping.
- **The decision** (`KeypadHapticsTracker`, `platform/src/keypad_haptics.{h,cpp}`, pure):
  - One tap when a finger's key changes to a new key: a landing, or a slide onto another d-pad arm.
  - Nothing for the same key read again (a hold, a move within it), for no key, or for the read at a touch's end, which forgets the finger.
  - No tap unless the game pad is the key pad's panel in use (`mt_input_panel() == 0`, so not under a popup or on a menu panel), it is shown (`hide_keypad` hasn't hidden it) and the key pad editor is closed. The fingers' keys are still followed meanwhile, so a key held through a popup doesn't tap when the popup closes.
- **The sink.** `HapticsSink` (prepare, impact, release) is prepared exactly while taps are possible, at the level in effect, and released otherwise. On iOS it is a `UIImpactFeedbackGenerator` (light or medium; `platform/ios/keypad_haptics_ios.mm`, ObjC++ with ARC, added to the frontend only when `CMAKE_SYSTEM_NAME` is iOS). It is made and prepared when the pad shows, so the first tap has no wake-up latency, and prepared again after each tap. On other platforms `make_platform_haptics_sink()` returns nullptr and `game.cpp` installs nothing, so the desktop runs exactly as before.
- **Evidence.**
  - CI, every OS (`platform_test`, `keypad_haptics: *`): a recording sink checks one tap per new key under a finger, none for holds, off-mask reads or a touch's end, one per slide, nothing while inactive, hidden or off, and prepare and release at the right moments. The wrapper is checked on synthetic calls as the recompiler writes them, alone and around `keypad_layout`'s handler, and the row is checked (choices, presets, `ios_only`). `objc_test` covers `mt_uikit_touch_delivering` inside each phase's delivery and only there, and `mt_cg_color_rgba`.
  - Local stand-in for iOS: `platform/ios/keypad_haptics_ios.mm` compiles against UIKit for Mac Catalyst (`-target arm64-apple-ios16.0-macabi`, `-fobjc-arc -Wall -Wextra -Werror`). `keypad_haptics.cpp`, `settings.cpp`, `settings_menu.cpp` and `game.cpp` pass a syntax check for that target with `SDL_PLATFORM_IOS` defined. Catalyst is not an iOS build.
- **Open (needs-human, after #83).** An iOS device build with the generator (Xcode), and the feel on Joris's iPhone at `light` and `medium`, including no lag on the first tap.
