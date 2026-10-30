# 37. UIKit: host classes, MainWindow.nib through a host NSCoder, touches on the guest thread, a software compositor with a GL slot

- **Status:** Accepted
- **Date:** 2026-10-29
- **Issue:** #54
- **Builds on:** [ADR 0026](0026-objective-c-strategy.md) (the shell runs recompiled on host classes), [ADR 0029](0029-objective-c-runtime-services.md) (host classes, side-table state), [ADR 0032](0032-corefoundation-object-model.md) (the run loop), [ADR 0033](0033-gcd-worker-threads-and-main-queue.md) (one guest thread), [ADR 0035](0035-foundation-subset.md) (NSString, NSSet, NSBundle), [ADR 0036](0036-coregraphics-and-uikit-drawing.md) (the rasterizer, `UIImage`, `UIColor`, `UIFont`, text)

## Context

The 18 shell classes run recompiled and unchanged ([ADR 0026](0026-objective-c-strategy.md)). They need UIKit for four things ([re/objc-shell.md](../re/objc-shell.md), [re/host-surface.md](../re/host-surface.md)):

- **The launch.** `main` calls `UIApplicationMain(1, argv, nil, nil)` at `0x2844`. UIKit then loaded `MainWindow.nib` ([formats/nibarchive.md](../formats/nibarchive.md)): a 320 × 480 `UIWindow` with one `EAGLView`, plus the `MapleThiefAppDelegate`, with the outlets File's Owner.`delegate` and delegate.`window`. Decoding the view (`-[EAGLView initWithCoder:]` `0x4F6D`) sets up EAGL, turns the view to landscape in `-createGLes` (`setTransform:` (0, −1, 1, 0), `setBounds:` (0, 0, 480, 320)) and starts the engine. Then the delegate gets `-application:didFinishLaunchingWithOptions:` (`0x2BFD`), which makes the `EAGLViewController` and sets its view.
- **The view tree.** Every menu, panel and popup is a `UIView` subclass or a plain view, built with `initWithFrame:`, `addSubview:`, `setFrame:`, `setAlpha:`, `setBackgroundColor:`, `setTag:`, `subviews` and `removeFromSuperview`. `MainMenuView` builds its UI in `-drawRect:`. `MainMenuView` and `ChooseCharacterView` animate with `+beginAnimations:context:` … `+commitAnimations` (delays, durations, a curve).
- **Controls.** `UIImageView` (65 `+alloc` sites), `UILabel`, `UIButton` (custom type, images per state, edge insets, touch-down and touch-up-inside actions), `UIScrollView` with paging and `UIPageControl` (help screens), `UITextView` (terms), `UIActivityIndicatorView`.
- **Touches.** `TouchKeyPadView` and the key pads override `touches*` and read `[touch view]`, `locationInView:self` and `previousLocationInView:`; the engine polls the key pad's ivars every tick.

The game draws its world with OpenGL ES into the `EAGLView`'s `CAEAGLLayer`. UIKit's own views (key pad buttons, menus, popups) sit on top of that layer. The GL renderer is #58 and the platform layer #117. Both need a defined interface to UIKit's views: where the GL picture goes, what is drawn over it, and how input gets in.

## Options considered

**The classes.**

1. **Host classes for the UIKit classes the game reaches, with exactly the selectors live game code sends** ([re/host-surface.md](../re/host-surface.md)), as #49 and #52 did. Host state per view in the side tables; SDK instance sizes (`UIView` 46, `UIViewController` 152) so the guest subclasses' ivars land where the compiler put them.
2. **A native UIKit** (on iOS) or AppKit wrapper. It exists only on Apple platforms, and its state would live outside guest memory with no way to keep it in step with the guest's subclasses.
3. **Rewriting the shell** natively. ADR 0026 rejected this.

**The nib.**

1. **Read `MainWindow.nib` at run time** with a hardened C++ NIBArchive reader, and decode it as UIKit does: class swappers, proxies, `-initWithCoder:` with a host `NSCoder` over the nib values, outlet connections.
2. **Hard-code the launch** (create the window, the view and the delegate in C++). Smaller, but the game's own `-[EAGLView initWithCoder:]` must run with a coder anyway, and the nib is the source of truth for the window's colour, bounds and the delegate's class.

**The compositor.**

1. **Draw the view tree on the CPU** with #51's rasterizer into an RGBA image of the 480 × 320-point screen, at any 3:2 pixel size. The view with the `CAEAGLLayer` is a **GL slot**: the platform gets its rectangle and either hands in a GL frame (full mode) or draws the GL picture itself and puts UIKit's image on top (overlay mode, the slot cleared to transparent).
2. **Draw each view as a GPU texture** in #58's renderer. Faster for many views, but it ties UIKit to the GL backend before #55 has chosen it, and every `drawRect:` and label is CPU-drawn anyway.

**Touches.**

1. **Queue them from the host** (any thread) and deliver them on the guest thread at the start of each run-loop turn, as UIKit's event loop did.
2. **Deliver at once** from the platform's input callback. That runs guest code outside the run loop, on whatever thread SDL calls back on.

## Decision

- **Host classes (option 1)** in `runtime/src/uikit/` (`uikit_internal.h` describes the module), registered in `runtime/src/objc/classes.cpp` after #51's drawing classes: `UIResponder`, `UIView`, `UIWindow`, `UIImageView`, `UILabel`, `UIControl`, `UIButton`, `UIPageControl`, `UIScrollView`, `UITextView`, `UIActivityIndicatorView`, `CALayer`, `CAEAGLLayer`, `UIApplication`, `UIViewController`, `UIDevice`, `UITouch`, `UIEvent`, `NSCoder` and `UINibDecoder`. #52's `GKLeaderboardViewController` is now a `UIViewController`.
  - **The selectors** are the UIKit rows of [re/host-surface.md](../re/host-surface.md) (129 selectors in 17 classes, #51's drawing classes included) plus what the runtime's own code sends (`initWithCoder:`, the `touches*` methods, `drawRect:`, `layerClass`). `objc.host_surface` now checks the whole UIKit area. `mtport.hostsurface` fixes two receivers (`setView:` reaches a `UIViewController`, `setOpaque:` the `CAEAGLLayer`) and drops typed rows for `[self …]` after `self = [super init…]` of a selector the sending class implements itself (`createGLes`, `setupView`, `initKeyPadType0:`…).
  - **One `View` per view** (a host struct in the side table) holds every kind's state: geometry as **model values** (center, bounds, transform, alpha, as UIKit stores them), the tree (subviews retained back to front, the superview not), and the fields of labels, buttons, scroll views and the rest.
- **The nib is read at run time (option 1).** `nib.cpp` checks every count, offset, varint (at most 5 bytes), index, class fallback and length against the file, refuses gaps, overlaps and trailing bytes between the tables, and requires the objects' value runs to cover the values table in order: the rules of `tools/mtport/nib.py`. Decoding (`app.cpp`), for each object reached from the root:
  - `UIProxyObject` `IBFilesOwner` is the `UIApplication`, `IBFirstResponder` nil; `NSArray`, `NSString` and `UIColor` (RGB, white or a system colour) are made directly.
  - A `UIClassSwapper` makes its `UIClassName` (falling back to `UIOriginalClassName`, logged); `UICustomObject`s and classes without `-initWithCoder:` get `-init`, every other object `[[cls alloc] initWithCoder:coder]`. The coder is a host `UINibDecoder` over that object's values; `-[UIView initWithCoder:]` reads `UIBounds`, `UICenter`, `UITransform`, `UIAlpha`, `UIBackgroundColor`, `UIOpaque`, `UIHidden`, `UITag`, `UIClipsToBounds`, `UIAutoresizeSubviews`, `UIAutoresizingMask`, `UIClearsContextBeforeDrawing`, `UIMultipleTouchEnabled`, `UIUserInteractionDisabled` and `UISubviews`, and logs the keys it does not read.
  - `UIRuntimeOutletConnection`s are made through the destination's setter (`set<Label>:`); other connection classes are logged and skipped. The `UINibVisibleWindowsKey` windows become visible. Every decoded object that responds gets `-awakeFromNib`. Objects nest at most 64 deep; a cycle is nil.
- **`UIApplicationMain`** is `mt_uikit_app_main`, the boot's app hook ([ADR 0027](0027-process-start-up.md)). It creates the principal class (`UIApplication`), loads Info.plist's `NSMainNibFile` with the application as File's Owner, sends the delegate `-application:didFinishLaunchingWithOptions:` (or `-applicationDidFinishLaunching:`) with the nib's window already connected, then `-applicationDidBecomeActive:`, and runs the main run loop for ever: `mt_uikit_turn` (touches, scrolling, `viewDidAppear:`, finished animations, `drawRect:`), `mt_runloop_run_once`, then the host's turn hook (`mt_uikit_config::turn`), which presents, feeds input, waits and can end the run. Boot milestones `nib` and `app-delegate` mark the steps. `mt_uikit_lifecycle_event` sends `applicationWillResignActive:`, `applicationDidBecomeActive:` and `applicationWillTerminate:`.
- **The screen** is the landscape view of the 320 × 480-point portrait window, as `UIInterfaceOrientation` LandscapeLeft shows it: 480 × 320 points, origin top left, the window's top edge on the right (`screen_from_window`: (x, y) → (480 − y, x)). The `EAGLView`'s own turn cancels it, so its 480 × 320 local space is the screen. Scale is free: the compositor draws at any pixel size.
- **Animations.** `+beginAnimations:context:` … `+commitAnimations` (nested blocks, `setAnimationDuration:`, `setAnimationDelay:`, `setAnimationCurve:`) record, for each property set inside, an animation from its presentation value to the new one. The model value changes at once, as in UIKit, so the game reads back what it set. The compositor draws presentation values at the time it is given (the four curves are Core Animation's cubic Béziers); hit testing uses model values. `mt_uikit_animation_count` tells the host whether anything moves.
- **`drawRect:`** runs at the end of each turn for every view in a visible window whose class overrides it and that needs display (new, or after `setNeedsDisplay`), into a UIGraphics context of its bounds (#51's `push_view_context`). The result is kept as the view's contents.
- **Touches (option 1).** `mt_uikit_touch(finger, phase, x, y)` queues screen points under a mutex. Each turn delivers them: a finger is one `UITouch` for its life, owned by the view the hit test found when it began (the frontmost visible, interactive, not nearly transparent view whose bounds hold the point). The view gets `touches*:withEvent:` with an `NSSet` of its touches; a plain `UIView` forwards them up the responder chain (`UIResponder`). A view that is not `multipleTouchEnabled` gets only the first of simultaneous touches. Controls track their touches with UIKit's 70-point touch-inside margin and send target/actions for touch down, drag, up inside, up outside and cancel. A scroll view holds content touches back for 150 ms and turns a 10-point move into a drag (cancelling the content's touch when it can); a drag ends in paging (nearest page, or the next one for a flick over 300 points/s) or deceleration, both bouncing back inside the content, with `scrollViewDidScroll:` on every change. The trace channel `uikit` (`MAPLETHIEF_TRACE=uikit`) logs each delivery and action.
- **The compositor (option 1)**, `mt_uikit_composite(cpu, now, mode, gl, pixels, w, h, stride)`: each visible window back to front, each view as background colour, its own content (image, text, button image, page dots, spinner, text view text), its `drawRect:` contents, then its subviews, clipped if it clips. A view with opacity below 1 and subviews is drawn as a group into a scratch buffer and blended once, as Core Animation does. The `CAEAGLLayer` view is the GL slot: in `MT_UIKIT_COMPOSITE_FULL` the given GL frame (or black) fills it; in `MT_UIKIT_COMPOSITE_OVERLAY` it is cleared to transparent, so the platform draws GL first and this image on top. `mt_uikit_gl_rect` gives the slot's screen rectangle and drawable size (480 × 320 for the `EAGLView`).
- **The rest of UIKit's surface.** `UIDevice` says model and name `iPhone`, system name `iPhone OS`, version `4.3.5` (above the game's 4.1 Game Center check). `-[UIApplication openURL:]` goes to the host's `open_url` hook (logged and refused without one). `setApplicationIconBadgeNumber:` is stored, `registerForRemoteNotificationTypes:` logged. `UIViewController` keeps its view; `presentModalViewController:animated:` sends `-viewDidAppear:` at the next turn and `dismissModalViewControllerAnimated:` works from either side. `CAEAGLLayer` keeps `drawableProperties` and `opaque` for #58. Core Animation, `UIWebView`, `UITableView` and `UIScreen` are Chartboost's or unused: not built.

## Consequences

- **Tests.**
  - In CI: `tests/cpp/uikit_test.cpp` (in `objc_test`). A synthetic NIBArchive writer builds a `MainWindow.nib` like the game's; the reader is checked on it and on every truncation, bad magic, counts, offsets, indices, types, varints, class names and fallbacks. The launch test checks that the delegate's launch callback sees the nib's window, the swapped view made by `-initWithCoder:`, the visible window and the lifecycle events; then geometry, the tree and retain counts, animations, touches (hit testing, forwarding, multitouch, `locationInView:`), button actions, scroll paging with its delegate, the controls, `UIDevice`, view controllers, and the compositor: a **golden image** of a synthetic screen (`tests/golden/data/uikit_screen.png`, tolerance 2 per channel on 0.2 % of the pixels) plus exact pixel checks of group opacity, clipping, `drawRect:`, the GL slot in both modes, an animation in flight and 2× output. `objc.host_surface` checks every UIKit row.
  - Locally (real assets): `runtime.boot_real` boots the real app through `UIApplicationMain` with stand-ins for the EAGL, GL, audio and `CC_MD5` calls that are #117's and M6's. It checks the launch (nib with 23 archive objects, `-[MapleThiefAppDelegate application:didFinishLaunchingWithOptions:]` with the 320 × 480 window, the `EAGLView` in it, the GL rect 480 × 320, no unimplemented import or selector), runs the run loop for 4 s, checks the main menu's view tree (`TouchKeyPadView`, `MainMenuView` and its 480 × 320 background, the Play button at (46, 148, 120, 48)), that the composited main menu covers the screen, and that a tap on Play reaches `-[MainMenuView clickPlay:]`. It writes the renders to `build/test-output/<preset>/uikit_real/`; they are never committed.
- **Interface for #58 and #117:** `maplethief/uikit.h`. The platform installs the turn hook, feeds `mt_uikit_touch` in screen points, composites in overlay mode over its GL picture at `mt_uikit_gl_rect`, and sends the lifecycle events. Until then, nothing calls `mt_uikit_app_main` outside the tests.
- **Correct positions are judged against the device.** The frames come from the nib and the game's own code, and the test pins them, but whether the whole screen matches needs a reference screenshot from a device (needs-human on #54). Text uses #51's NanumGothic, so labels differ slightly (#118).
- **Model values versus presentation values** differ only inside animations. Game code that reads a frame during an animation gets the end value, as on iOS.
- **The compositor costs CPU** in proportion to the views on screen and the pixel size. The game shows few views over the GL layer (the key pad, popups), and the platform can skip re-compositing while nothing changed (no touch, no animation, no turn that touched a view).
- **How we'd know this was wrong:**
  - **A view appears in the wrong place or orientation** next to the device screenshot. Then the window-to-screen map or a view's geometry handling is wrong.
  - **A touch lands on the wrong view or the key pad misses fingers.** Then hit testing, the multitouch rule or the scroll view's delay needs to follow UIKit more closely; the `uikit` trace shows each delivery.
  - **An unimplemented UIKit selector shows up** in the unimplemented report during play. Then #11's analysis missed a send, and `hostsurface.RECEIVERS` and the class need the row.

## Addendum: the compositor at the render scale (#134, 2026-10-30)

After [ADR 0068](0068-gpu-presentation-without-read-back.md) the 4x frame was almost all `mt_uikit_composite`: 42.4 ms per window frame on the GPU path (overlay mode) and 69.5 ms on the software path (full mode), at 1920 × 1280 (`platform.gpu_compare`, Release, Apple M4).

- **Where the time went.** The trace channel `composite` (`MAPLETHIEF_TRACE=composite`) logs, per composite, each drawn view (class, address, kind, device box, opacity, whether it has `drawRect:` contents) with the time of its own drawing, indented by depth, then the composite's size, mode and total. With the shortcuts below turned off, a 4x GPU run of the fresh list (1,318 composites) averaged 48.9 ms per composite (traced):
  - the `UIWindow`'s background fill (under the GL view), 14.0 ms every composite;
  - the `EAGLView` slot's clear, 9.8 ms every composite;
  - on the main menu (143 composites), the full-screen image views (54–56 ms each, three of them, bilinear ×4 from 480–580 × 320 images), the 1920 × 520 promo button (22.9 ms) and `MainMenuView`'s `drawRect:` contents (14.6 ms).

  Each was the rasterizer's generic path over millions of pixels: a full-size float coverage accumulator per rectangle, then the inverse map, four clamped loads and a double-precision mix per pixel. At 1x the same run averaged 2.3 ms per composite, the same views on top.
- **Decision: exact shortcuts in the rasterizer (`runtime/src/cg/raster.cpp`)**, each giving the generic path's bytes:
  - A raster path that is one axis-aligned rectangle with whole-pixel corners gets full coverage (255) without the accumulator. The accumulator gives such a rectangle exactly ±1 per row at whole cells, so its running sum is exactly 1 across the box.
  - A solid fill over full coverage, with no clip and alpha 1, whose result doesn't depend on the pixel under it (Clear, Copy, or Normal with an opaque colour), stores one pixel's bytes across the box.
  - `draw_image` with a map without rotation or shear (the inverse's b and c exactly 0) works out each column's u and each row's v once, from the same `inv.apply` as the generic path, and with them `sample()`'s clamped taps and weights:
    - When every weight is a multiple of 1/256 (integer scales with whole-pixel offsets, and nearest sampling), the doubles are exact and the mix is done in integers. The two tapped rows are mixed once per source column: (256 − wx) · V(i0) + wx · V(i1), rounded.
    - Otherwise the doubles are computed as `sample()` computes them.
    - In the Normal mode, columns whose four taps are zero are not visited, provided the destination stores a pixel back unchanged (premultiplied with alpha).
    - Nearest 1:1 draws between plain premultiplied RGBA surfaces (the full mode's GL frame) copy opaque pixels.
  - Blend in the Normal mode: an opaque pixel with full coverage is the source, and a zero source leaves the destination as it was.
- **Reference.** `mt_cg::set_raster_shortcuts(false)` (tests only) sends every drawing through the generic path. CI checks against it:
  - `CG: the raster shortcuts draw the generic path's bytes (#134)`: 2,880 image and fill draws, byte for byte. The draws cover destination formats premultiplied RGBA, BGRA and RGBX; source formats those plus straight RGBA and 24-bit RGB; y-flipped CTMs at 1x to 4x and 1.7x, an x flip and a quarter turn; whole-pixel, fractional and off-surface rects; rounded and rectangular clips; alpha 0.6; and the Normal, Copy and Clear modes. Two deliberate one-off errors (in the integer rounding and in the zero-run columns) each make it fail.
  - `compositor: views, group opacity, ...`: `uikit_screen`'s scene at render scale 1–4 in both modes, a GL frame in the slot, against the generic path.
  - `compositor: the composite trace channel times each view (#134)`.
- **Not done:**
  - Skipping views wholly under the opaque GL view, which the issue suggested. The only such drawing in the game is the window's background, which is now one 10 MB store (0.09 ms traced). A skip would need the GL view's box, its opacity and every ancestor's clip and alpha proved exact, which isn't worth it for that.
  - A dirty flag that skips re-compositing an unchanged overlay. It would need every input of the picture tracked (the tree, animations, the key pad's host opacity, `drawRect:` contents, the activity spinner's clock), and the composite is now a few milliseconds.
- **Evidence (Release, Apple M4, `platform.gpu_compare`'s fresh run, `game: per window frame: compositing`):**

  | | before (ADR 0068's build) | after |
  |---|---|---|
  | **4x, GPU path** (overlay) | **42.36 ms** | **3.72 ms** |
  | **4x, software path** (full) | **69.50 ms** | **5.01 ms** |
  | 1x, GPU path | 3.63 ms | 0.34 ms |
  | 1x, software path | 3.80 ms | 0.56 ms |
  | 4x, frontend time per window frame, GPU / software | 42.60 / 70.63 ms | 3.99 / 6.21 ms |

  The machine was shared with other builds during the "after" runs (load average 4–6), so the numbers are upper bounds. Traced per view (4x GPU), the window's fill and the slot's clear now take 0.09 ms each. The main menu's full-screen image takes 7.3 ms, its other full-screen layers 2–4 ms, and in-game composites are well under a millisecond.
- **Same pixels:** every `--shot` and `--shot-gl` screen of the four runs (1x and 4x, software and GPU, 52 PNGs) is byte-identical before and after. `platform.device_golden` matches our recorded renders exactly (7 of 7). The software goldens (`ctest`, `uikit_screen` and the CG goldens among them) are unchanged.
