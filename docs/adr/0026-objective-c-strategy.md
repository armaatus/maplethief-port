# 26. Objective-C: a runtime over the guest's metadata, with host framework classes

- **Status:** Accepted
- **Date:** 2026-10-27
- **Issue:** #46

## Context

The armv7 slice has 75 Objective-C classes: 18 app classes (the "shell") and 57 Chartboost classes ([re/objc.md](../re/objc.md), #8). The shell is a thin UIKit layer over one OpenGL ES view: boot, two menus, a Korean terms screen, the touch pad and ten button panels ([re/objc-shell.md](../re/objc-shell.md), #22). The engine is C++, but it sends messages too. It reaches the shell through `+[EAGLView_Instance Instance].objView`, and it calls Foundation, UIKit and AVFoundation directly. The issue asks for one of two options, or a hybrid:

- **(a)** A minimal Objective-C 2 runtime in the host. `objc_msgSend` looks methods up in the guest's own class metadata, guest IMPs run as recompiled code, and framework classes (UIKit, Foundation, …) are host-implemented classes that guest code sees as ordinary classes.
- **(b)** Rewrite the 18 app classes natively in C++, calling recompiled engine functions directly, and drop Chartboost.
- **Hybrid:** emulate Foundation, but rewrite the shell's views natively.

### Facts

The counts come from `work/re/imports.json` (#11), `work/re/function-tags.json` (#10) and `work/re/objc.json` (#8). A send counts as *game* when the sending function is not Chartboost's. A send to a receiver the scan can't type counts as a framework send when no app class implements the selector, the rule of [objc-shell.md](../re/objc-shell.md#method-and-caveats).

| | Engine (C++) code | Shell (ObjC) code | All game code |
|---|---|---|---|
| Framework selectors sent | 72, to 14 typed classes | 184, to 42 typed classes | **234**, 47 classes |
| … still reached after the opt-outs below | 72 | 111 more | **183**, 34 classes |
| Shell selectors sent | **109** (509 sends) | 120 | |

- **The engine is coupled to shell objects, not just to calls.** Every frame, `SkyCanvas::Tick` reads `TouchKeyPadView`'s `on…Key` properties. `CChatBox::paint` reads the NPC panel's quest buttons, and the market and map code read tracking points. `CStateMainMenu` writes the character slots into `EAGLView` properties as an `NSMutableArray` of `NSString` arrays and an array of `UIImage`/`NSNull`. Those are guest objects that engine code creates and sends messages to.
- **The engine needs a runtime under either option.** Its 72 framework selectors go to `NSString`, `NSData`, `NSFileHandle`/`NSFileManager` (the save files), `NSBundle`, `NSLocale`, `UIDevice`, `UIFont`/`UIImage` (text and portraits) and `AVAudioPlayer`. That is `objc_msgSend` with host classes behind it, whatever happens to the shell.
- **Opt-outs.** Some classes are never created when the host answers *no* to an availability check. `InAppPurchaseManager` exists only if `+[SKPaymentQueue canMakePayments]` says yes, and `GameCenterManager` only if `+[GameCenterManager IsGameCenterAvailable]` does. `EAGLViewController` only presents the Game Center leaderboard, and `MoviePlayerController`/`MovieOverlayView` are dead code ([objc-shell.md](../re/objc-shell.md#how-the-shell-works)). Leaving these classes out removes 51 framework selectors, all of StoreKit, GameKit and MediaPlayer, and the game's only uses of blocks and GCD ([re/timing-threading.md](../re/timing-threading.md)).
- **Chartboost** is reached from the engine at 6 sites (`sharedChartboost`, `setAppId:`, `setAppSignature:`, `startSession`, `showInterstitial`), fire-and-forget. If `+[Chartboost sharedChartboost]` returns nil, every one of those sends is a message to nil and returns 0. None of Chartboost's 486 methods or 87 Chartboost-only imports then run. The same holds for both options.
- **Instance layout.** App classes subclass `NSObject` (28), `UIView` (17) and `UIViewController` (2). Their ivars start where the SDK they were built against put them. `NSObject` subclasses start at 4. `UIView` is 46 bytes: `MovieOverlayView` has no ivars and `instanceStart = instanceSize = 46`, and subclasses with ivars start at 48. `UIViewController` subclasses start at 152. Compiled code reads ivar offsets from `__objc_ivar` variables, so a host class whose guest instance size fits these numbers needs no ivar sliding.

### The prototype

`runtime/src/objc/` (`maplethief/objc/objc.h`) is the option (a) core, written for this ADR in about 850 lines of C++ in the import code's style: no exceptions, no `new`, malloc-backed hash tables. It covers:

- class realization from `class_t` → `class_ro_t` → `method_list_t`, with metaclasses, superclasses realized first, and the 4 categories;
- host classes bound to the imported `_OBJC_CLASS_$_X`/`_OBJC_METACLASS_$_X` objects;
- selector interning and selref canonicalization;
- host overrides, per-class method caches, and the `objc_msgSend`, `objc_msgSend_stret` and `objc_msgSendSuper2` entry points, which enter recompiled IMPs through `mt_dispatch`.

Guest memory and allocation go through a caller-provided allocator. The real runtime uses #42's guest heap and pointer helpers (ADR 0025).

Measured on the armv7 slice (`tests/test_objc_runtime_real.py`):

- All 75 classes realize with no per-class code: 69 host classes (every imported class, bound through the image's own binds), 4 categories, 0 problems.
- Every one of the 2,151 lookups (each class × every selector its image superclass chain defines, plus NSObject's host methods) and the 15 category lookups agrees with `mtport.objc`'s independent parse. Every one of the 1,064 IMPs is reachable, except the two IMPs that the `+sharedChartboost` and `+IsGameCenterAvailable` overrides replace.
- The 951 selrefs are already canonical. The linker coalesced `__objc_methname`, which holds 1,143 distinct selector names.
- No app class starts its ivars inside the SDK sizes above.
- Loading the metadata takes 0.6 ms. A cached lookup takes 7 ns (Apple M4, `-O1`).
- The prototype took about one day, its tests included.

## Options considered

The estimates are working days and come from the per-class table of [objc-shell.md](../re/objc-shell.md#effort-native-rewrite-vs-uikit-emulation) and the prototype. Some work is the same under both options: the ObjC runtime with its host-class bridge (#47), the Foundation classes the engine uses (#49), text and image drawing (`UIFont`, `UIImage`, `drawAtPoint:withFont:`; #51), the GL surface (#117, M5) and compositing over it. The table leaves that work out.

| | (a) Guest runtime + host UIKit | (b) Native shell (hybrid: the same) |
|---|---|---|
| Host framework surface | 183 selectors on 34 classes (+ untyped receivers): Foundation, and a UIKit subset of views, controls, scroll/page and animations | 72 engine selectors on 14 classes, plus a host facade that answers the engine's 109 shell selectors with the shell's exact state |
| Shell behaviour | Runs as recompiled code, unchanged: 563 methods, 145 KB | Re-derived by hand from -O0 code: about 20 screens and panels, every coordinate, image, state and engine call |
| Effort beyond the shared work | UIKit subset ≈ 10–15 + per-class bring-up ≈ 15 → **≈ 25–30** | native screens ≈ 22 + engine facade ≈ 3–4 → **≈ 25–26** |
| Fidelity risk | **Low.** Errors sit in UIKit semantics: hit-testing (alpha < 0.01), button states, animation curves, paging, label layout. Each is a small, documented behaviour, testable on synthetic views, and one fix repairs every screen. | **High.** Errors are per screen and found only by playthrough and reference screenshots (human hand-offs). The engine reads shell state, so a wrong native panel changes game behaviour, not just looks. |
| Controller layer (#67) | Direct. Actions call `Clet_HandleEvent(2/3, code, 0)` `0x5A75` and set the polled `on…Key` state on the guest `TouchKeyPadView`. No synthetic touches, except to drive menus. | Direct C++ calls. Marginally simpler. |
| Later changes (M10 widescreen, HD menus) | Swap one screen at a time through host overrides | Easiest: the code is ours |
| Chartboost | Override `+[Chartboost sharedChartboost]` → nil | Same |

The hybrid (emulate Foundation, rewrite the shell's views) is option (b). Option (b) already needs Foundation and the runtime for the engine, so the two are the same plan.

The effort is a wash. The difference is where the risk sits. (a) keeps the game's own shell logic bit-exact and concentrates the risk in a small framework surface that we can test without the game. (b) re-implements 145 KB of logic that the engine depends on, and only a human can check it. ARCHITECTURE.md's first goal is to keep game logic faithful, and that decides it.

## Decision

We implement option (a): an Objective-C 2 runtime over the guest's own metadata, with the framework classes implemented in the host. Host overrides are the escape hatch for replacing individual methods.

- **Object model.** Objects, classes and selectors are guest values. An object is a guest address whose first word is its isa. A class is its `class_t` in guest memory: for an image class, the image's own structure; for a host class, the imported `_OBJC_CLASS_$_X`/`_OBJC_METACLASS_$_X` data object that the image is bound to (ADR 0019). The runtime writes isa and superclass into those objects. A SEL is the guest address of its name. The image's own copy is canonical, and selrefs are rewritten if not (0 of 951 are today). Inside the runtime, methods are keyed by selector id, so host code never depends on which copy is canonical.
- **Host state** lives in host side tables keyed by the guest object, never in guest bytes. A host class's guest instance size is the SDK size that the image's subclasses were compiled against: `NSObject` 4, `UIView` 46, `UIViewController` 152. The runtime reports any image class whose ivars would start inside its host superclass (libobjc would slide them). Today there are none.
- **Messaging.** `objc_msgSend`, `objc_msgSend_stret` and `objc_msgSendSuper2` are HLE imports. They look up the method from the receiver's isa (for `Super2`, from the superclass of `objc_super.class`) through a per-class cache, then enter it with the sender's registers unchanged. A recompiled IMP goes through `mt_dispatch` and the result is cached. A host method is a host function with the same `void (mt_cpu *)` signature, which reads its arguments through `maplethief/hle/abi.h`. A message to nil returns 0 in r0 and r1. `_stret` to nil writes nothing, like libobjc's armv7 `objc_msgSend_stret`.
- **Undeliverable messages.** An unrecognized selector, or a receiver of no known class, is logged with the class, the selector and the caller. The `MAPLETHIEF_UNIMPLEMENTED_IMPORT` policy then applies (ADR 0019). `stop` traps with the new trap kind `MT_TRAP_OBJC` (12, pc = lr, detail = selector). `continue` and `trace` return 0. #53's report lists these messages next to unimplemented imports.
- **Start-up** (#43, #117):
  1. Link the imports.
  2. Place the data imports. Each host class's `init` hook registers it with the runtime at its import objects.
  3. Apply the binds.
  4. `mt_objc_load_image`, with the ObjC section ranges of the loader manifest: selrefs, then every class with its superclasses first, then the categories.
  5. Install the host overrides.
  6. Run the static constructors and `main` → `UIApplicationMain`.

  Classes are realized eagerly. The app has no `+load` and no `+initialize`, but the runtime still sends `+initialize` before a class's first message, as libobjc does.
- **Host overrides** replace a method of an image class with a host function. They are the only way the port changes shell behaviour. Each one is listed in the runtime with its reason:
  - `+[Chartboost sharedChartboost]` → nil, so ads are off and no Chartboost code runs;
  - `+[GameCenterManager IsGameCenterAvailable]` → NO, unless #52 decides to emulate GameKit;
  - `+[SKPaymentQueue canMakePayments]` (a host class method) → NO, unless #52's StoreKit ADR says otherwise.

  Replacing a whole screen natively later (M10) is a set of overrides on that class, recorded in its own ADR.
- **The framework classes are host classes** registered through `mt_objc_add_host_class`:
  - #47 owns `NSObject` (allocation, retain/release, autorelease, the NSObject protocol methods);
  - #49 owns Foundation, with constant CFStrings as `NSString`s, toll-free with #48;
  - #51 owns `UIImage`, `UIFont` and the drawing selectors;
  - #54 owns the UIKit subset and `MainWindow.nib`;
  - #52 owns the stubs.

  Each implements only the selectors that game code reaches (#11's lists). Any other selector is an unrecognized selector, not a silent no-op.
- **Not built:** Chartboost's classes are realized, but never instantiated or run. Their framework needs are dropped: `UIWebView`, `UITableView`, `UIBezierPath`, CA animations, `NSURLConnection`, `NSUserDefaults`, `NSKeyedArchiver`, `NSTimer`, SystemConfiguration, and GCD apart from the main queue. The same goes for MediaPlayer (dead code), and for StoreKit and GameKit while their availability checks answer NO.

## Consequences

- **#47** grows the prototype into the runtime:
  - registry entries for the three senders;
  - start-up as above;
  - `NSObject` alloc/`allocWithZone:`/`new` on #42's heap, with the class's instance size;
  - retain counts in a side table, `dealloc` freeing guest memory, `NSAutoreleasePool` and a drain every run-loop turn;
  - `+initialize`;
  - the NSObject protocol methods from the runtime's tables (`class`, `superclass`, `isKindOfClass:`, `respondsToSelector:`, …);
  - `objc_getProperty`/`objc_setProperty` (99 + 42 sites);
  - `@synchronized` as a recursive per-object count (single guest thread, ADR 0014), and `objc_enumerationMutation` as a trap.

  The blocks ABI (`_NSConcreteStackBlock`, `_Block_object_assign`/`dispose`) is needed only if #52 keeps `GameCenterManager` alive. The ObjC `@catch` helpers already exist (ADR 0020).
- **#49, #51, #52, #54** implement host classes on #47's bridge and don't start before it. #49 loses `NSUserDefaults`, `NSKeyedArchiver`, `NSTimer` and the host thread for `NSThread` (only `+isMainThread`). They are Chartboost-only, and its save-file test replaces the `NSUserDefaults` one. #54 needs no CA animations (Chartboost-only), but it needs `UIView` begin/commit animations (`MainMenuView`).
- **#117** boots with this start-up order and the overrides. Its "empty unimplemented report" covers undeliverable messages too.
- **#67** gets direct actions without a native shell. The controller layer calls the engine entry points the pad calls and sets the pad state that the engine polls.
- **Tests.**
  - `tests/cpp/objc_test.cpp` (doctest, `objc.*`) covers lookup, inheritance into host classes, categories, overrides, `super`, `_stret`, nil and the undeliverable-message policies, on synthetic metadata.
  - `tests/test_objc_runtime.py` loads the synthetic Mach-O of `objcfixtures` through its own sections and binds, in CI.
  - `tests/test_objc_runtime_real.py` (local) does the same on the slice, as measured above.
- **Performance.** A send costs an isa load and about three hash probes. If profiles ever show it, cache the class index in the guest `class_t.cache` word, which is runtime-private.
- **How we'd know this was wrong:**
  - The UIKit subset keeps growing past the 34 classes and 183 selectors measured here.
  - A shell screen needs UIKit behaviour that can't be matched within a few days.
  - Profiling shows message sending as a real cost.

  The first two can be answered screen by screen with host overrides (a native screen), without changing the runtime.
