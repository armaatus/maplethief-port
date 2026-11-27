# 242. Host-driven tests for the functions no play can enter

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#432](https://github.com/armaatus/maplethief-port/issues/432) (epic [#285](https://github.com/armaatus/maplethief-port/issues/285))
- **Builds on:** [ADR 0232](0232-coverage-gaps-and-difftest-import-models.md) (the coverage gaps), [ADR 0160](0160-run-coverage-entry-counters-and-hit-recorders.md) (run coverage), [ADR 0105](0105-save-states-in-process-snapshot-at-safe-points.md) (save states)

## Context

ADR 0232 classed 124 game functions `unreachable`: live, but entered only from a state no run produces. About a hundred are destructors and `dealloc`s: the game frees its singletons and states only when the process ends, and iOS ends it without unwinding. The rest are the system's events (a memory warning, an audio-session interruption, a push-notification token), the cash shop's buttons and its restored transactions, Game Center, and a coupon in the save (nothing in 1.5 adds one).

What the binary says about them, checked on 2026-11-27 with the Ghidra export:

- Most of the destructors are the complete-object destructor (the vtable's slot 0). `delete` calls the deleting destructor (slot 1), which the runs do enter and which does not call slot 0; no code calls slot 0 directly.
- `-[GameCenterManager setMyAttainedAchievement:]` is sent by `-[GameCenterManager dealloc]` and two dead functions only.
- `-[EAGLView didReceiveMemoryWarning]`: `EAGLView` is a `UIView`. UIKit sends that message to view controllers only, and the one load of the selector is the method's own send to `super`.
- The nine `-[TouchKeyPadView release{Exp,Meso,GradeRevival}…:]`: no `TouchKeyPadView` button has these actions. The buttons that do belong to `CashShopKeyPadView` and `PopupKeyPadView` and target their own view ([re/objc-shell.md](../re/objc-shell.md)).
- `-[InAppPurchaseManager restoreTransaction:]` handles state 3 (restored). StoreKit delivers that state only after `restoreCompletedTransactions`, and the game never sends it ([re/cash-shop.md](../re/cash-shop.md)).

## Options considered

**Driving the states through play.** A memory warning, an interruption or a push token cannot come from a pad, and teardown never happens in a run. Not possible.

**Calling each function bare, from a unit test.** Without the game's objects the functions would not do their real work, and most destructors follow member pointers. Too far from the game.

**One real-asset run that reaches the field, then enters each function from the host with the runtime's own entry points** (`mt_objc_send*`, `mt_call_guest`, the lifecycle event), checking what each one changes. Teardown is destructive, so a save state of the field is restored after each destructor before the game's next turn. Chosen.

**For the 11 functions with no sender.** Calling them anyway would hide the fact that nothing can reach them. They get `dead` rules with the evidence above.

## Decision

- **`play_real_test host_events`** (CTest `play.host_events`, real assets, about 30 s) writes a `cupon` file with two coupons, plays the new game and the tutorial to the field, then from the turn hook, each step in its own autorelease pool:
  - **push token**: `application:didRegisterForRemoteNotificationsWithDeviceToken:` to the app delegate with 32 bytes; `CDataMgr`+0x58C then holds them as hex;
  - **memory warning**: `didReceiveMemoryWarning` and `viewDidUnload` to the `EAGLViewController` (`0x11A97C`), as UIKit sends them to a controller whose view is in no window;
  - **audio interruption**: the new `mt_audio_session_interruption(cpu, state)` calls the listener the game gave `AudioSessionInitialize` with 1 and then 0. The session goes inactive and then active again. The platforms never call it: [ADR 0045](0045-audio-mixer-and-output.md) stands, and a desktop has no interruptions;
  - **grade revival**: `PopupKeyPadView`'s `releaseGradeRevivalButton:`, which sends `clickGradeRevivalButton:` to the `TouchKeyPadView`; the field comes back with no popup;
  - **coupon**: `CDataMgr::DeleteCoupon(0)` `0x491E1` moves the second coupon up;
  - **teardown**, one step a turn: a save state of the field; `applicationWillTerminate:`; `dealloc` on each of the 14 ObjC classes (the game's object, or a new `alloc` where the game has made none); then each of the 86 destructors on its object. For a C++ destructor the object is a live object of the class found by its vtable words: a heap block, a member of one, or a static object. Where none is found, the test uses a zeroed object with the class's vtables, or, when the destructor follows a member pointer (`CAniManager`), one made by its constructor. Every step but the last restores the field before the game's next turn.
- **`python3 -m mtport.coveragegaps update --coverage <file or directory>`** adds what a run entered to the functions the committed doc already counts as executed, and regenerates the doc. A new host test then does not need the 1.5 h suite again.
- The `unreachable` rules go; three `dead` rules carry the 11 functions above with their evidence.

## Consequences

- [re/coverage-gaps.md](../re/coverage-gaps.md) has no `unreachable` row: of the 124, the run entered 113 and 11 are `dead` (202 dead in all, 81 difftest, 94 missed: those are #433's).
- A destructor run on a zeroed object exercises only its null-member paths: 39 of the 86 had no live object in the field (the abstract bases, the title and menu states, the script loader's objects). The coverage counts them as entered, but less of their code ran than for a live object.
- The save-state restore after each destructor depends on the snapshot covering everything a destructor can touch (GL objects, ObjC objects, the heap). A destructor that broke something the restore misses would show up in the next step or the next turn.
- How we'd know this was wrong: a later run that sends one of the 11 `dead` functions (then its rule needs a root), or a destructor that only returns because its object is zeroed and traps on a live one (seen when a later state makes one).
