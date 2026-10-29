# 29. Objective-C runtime services: start-up, NSObject, autorelease, blocks, host classes

- **Status:** Accepted; the `+[SKPaymentQueue canMakePayments]` → NO override is superseded by [0034](0034-offline-services.md)
- **Date:** 2026-10-27
- **Issue:** #47

## Context

[ADR 0026](0026-objective-c-strategy.md) chose a runtime over the guest's own metadata and left the services around message sending to #47: allocation, retain counts, autorelease, `+initialize`, properties, `@synchronized`, blocks, start-up, and an API for the framework classes of #49, #51, #52 and #54. Three facts shaped the details:

- **Start-up order.** ADR 0026 had each host class's data-import `init` hook register the class. An `init` hook only gets the import's id and address. The hooks run in import id order, which is `nlist` order, but a host class needs its superclass registered first. A class and its metaclass are also two separate imports, and the image binds only one of the two for some classes (`NSString`: the class, not the metaclass).
- **Classes no one implements yet.** The image imports 79 classes. Until #49, #51, #52 and #54 exist, most have no host class. An image class whose superclass is unknown cannot be realized: 17 app classes subclass `UIView` and 2 subclass `UIViewController`.
- **Game Center.** The epic decided that GameKit is emulated offline (#52), so `GameCenterManager` runs. It is the game's only user of blocks (completion handlers, `__block` variables, `dispatch_async` to the main queue). Blocks are therefore needed in full: `_NSConcreteStackBlock`/`GlobalBlock`, `_Block_copy`/`_release`, `_Block_object_assign`/`dispose` and byrefs.

The game is pre-ARC: retain and release are explicit messages, and two classes override them (`EAGLView_Instance`, a singleton, and the block classes).

## Options considered

1. **Start-up through data-import `init` hooks (ADR 0026).** Pros: no new start-up step. Cons: hooks run in id order, not superclass order. A hook gets no name, so every class would need its own registry line, and the two half-imported classes would need special cases.
2. **Record the placed class objects, then add host classes from a registry in superclass order.** Pros: one generic pass over the import descriptors; the order is ours; classes without an import get an allocated object. Cons: one more start-up call, which #43's boot makes.
3. **Missing imported classes: leave them out.** The image classes on top of them fail to realize, so even `MapleThiefAppDelegate`'s neighbours (`EAGLView : UIView`) would be "no object of a known class" until #54.
4. **Missing imported classes: placeholders.** An empty `NSObject` subclass with the SDK instance size. Image classes realize, `alloc`/`init` work, and every other message is an unrecognized selector that names the class. That is exactly what #53's report needs.

## Decision

- **Start-up** is `mt_objc_start`, which runs after `mt_imports_place` and `mt_imports_apply_binds`. The boot (ADR 0027, `runtime/src/boot/objc_boot.c`) makes the same calls, split over its milestones: steps 1–4 are `objc-image` and step 5 is `objc-overrides`. The steps, in order:
  1. `mt_objc_bind_imports` records the placed `_OBJC_CLASS_$_X`/`_OBJC_METACLASS_$_X` objects. It also records `__NSConcreteStackBlock`/`__NSConcreteGlobalBlock` as the class objects of `__NSStackBlock__`/`__NSGlobalBlock__`.
  2. `mt_objc_add_host_classes` adds the host class registry (`runtime/src/objc/classes.cpp`, one line per class, superclass first) at those objects.
  3. `mt_objc_add_placeholders` adds a placeholder for every recorded class that is still missing. A placeholder subclasses `NSObject` and has no methods. Its size is `UIView` 46, `UIViewController` 152, otherwise 4. It is logged in one summary line, and messages to it are logged with "(placeholder)".
  4. `mt_objc_load_image` realizes the image's classes.
  5. `mt_objc_install_overrides` installs `+[Chartboost sharedChartboost]` → nil and `+[SKPaymentQueue canMakePayments]` → NO. **`+[GameCenterManager IsGameCenterAvailable]` is not overridden**: GameKit is emulated (#52). This supersedes that one bullet of ADR 0026.

  Data-import `init` hooks are not used for classes.
- **Memory.** `mt_objc_create` takes the allocator (`mt_mem_guest_alloc` on the guest heap, ADR 0025), and `mt_objc_set_free` takes its free (`mt_mem_guest_free`). Objects get the class's instance size, 16-byte aligned and zeroed. `-[NSObject dealloc]` runs the host-state destructors, drops every side-table entry and clears the isa. It then frees the block, so a later message to the object is one to no known class. An invalid free is logged. The runtime calls the allocator only through these function pointers, so the recompiler's C test harness links it without the heap's C++ code.
- **Retain counts** live in a side table keyed by the guest object. An absent entry means count 1. `-release` at count 1 marks the object as deallocating and *sends* `-dealloc`, so the subclasses' `-dealloc` runs and ends in `[super dealloc]`. Class objects are never retained or freed (`retainCount` is `UINT_MAX`). `+alloc` sends `+allocWithZone:`, so overriding classes see every allocation, and `+new` sends `+alloc` and then `-init`.
- **Autorelease.** Pools are marks on one stack of autoreleased objects. An `NSAutoreleasePool` object pushes a mark at `-init` and pops it (with the marks inside it) at `-drain`/`-release`. The run loop pushes a pool before each turn and pops it after (`mt_objc_pool_push`/`pop`). A pop releases the objects newest first. Objects autoreleased while the pop runs, from a `-dealloc` for example, go into the same pool and are released in the same pop.
- **`+initialize`** is sent once per class before its first message, superclass first, by every sender. It is sent only if some class in the metaclass chain implements it. An inherited one runs with the subclass as self, as in libobjc, and the triggering message keeps its registers.
- **Properties, `@synchronized`, enumeration.** `objc_setProperty` retains, copies (`copyWithZone:`) or mutable-copies the new value and releases the old one. An atomic `objc_getProperty` returns `[[v retain] autorelease]`. One guest thread means no locks (ADR 0014). `@synchronized` is a recursive count per object. An unmatched `objc_sync_exit` returns −1 and is logged. `objc_enumerationMutation` logs and traps (`MT_TRAP_OBJC`, detail = the collection).
- **Blocks** follow iOS 4's libclosure. Flags carry the reference count in the low 16 bits, with `BLOCK_NEEDS_FREE`, `HAS_COPY_DISPOSE` and `IS_GLOBAL`.
  - `_Block_copy` moves a stack block to the guest heap (isa `__NSMallocBlock__`) and runs its copy helper.
  - `_Block_object_assign`/`dispose` handle objects (retain/release), blocks (copy/release) and byrefs. A byref moves to the heap once, and both its copy and the stack copy forward to it. `BYREF_CALLER` means a plain assignment.
  - The block classes (`NSBlock` and its three subclasses) answer `copy`, `retain`, `release` and `retainCount` from the flags.
  - Host code keeps a block with `mt_block_copy`, calls it with `mt_block_invoke` and drops it with `mt_block_release`.
- **Host classes** are written against `maplethief/objc/host.h`:
  - arguments: `mt_objc_args` (after self and `_cmd`), `mt_objc_args_stret`, then `maplethief/hle/abi.h`;
  - sending from host code: `mt_objc_msg`, `_super`, `_stret` and `mt_objc_sendN`. Arguments past r3 go on the guest stack. lr and sp are preserved, and a message that cannot be delivered reports the guest caller of the host code;
  - object helpers: `mt_objc_retain`/`release`/`autorelease`/`create_instance`/`is_kind_of`;
  - per-object host state: `mt_objc_state(rt, obj, &type)` is created zeroed on first use, and its destructor runs at dealloc. Each class of a host hierarchy has its own state type;
  - blocks: the three `mt_block_*` functions above.
- **Undeliverable messages** are recorded once per (class, selector), with a count, the first receiver and the first guest caller. They are readable through `mt_objc_undeliverable_count`/`_at` and are pushed to an optional hook. #53's report uses this.

## Consequences

- The boot (ADR 0027) creates the runtime with `mt_objc_create` and `mt_objc_set_free` before `place`, runs the start-up steps after the binds, then calls `mt_objc_set_current`. The run loop (#54) pushes and pops a pool around every turn. On the real slice, `runtime.boot_real` reaches `UIApplicationMain` with 75 image classes, 67 placeholders, 0 problems, the two overrides, main's `NSAutoreleasePool` in place and no undeliverable message.
- #49, #51, #52 and #54 add a class by writing an `mt_objc_host_class` and adding one line to `classes.cpp`. That class's placeholder then disappears by itself. Each class must be listed after its superclass, so for example `UIView` must come before `UIControl`.
- The ADR 0026 Game Center override is gone. `GameCenterManager` runs, and #52 must provide `GKLocalPlayer`, `GKAchievement` and the rest, or their selectors show up as undeliverable.
- `-[NSObject description]` is not provided, because no `NSString` exists yet. #49 adds it with `NSString`, and no selref of the image names it today.
- Tests:
  - `tests/cpp/objc_services_test.cpp` (doctest `objc.*`) runs on a real `mt_mem` heap and covers NSObject, `+initialize`, pools, protocol methods, properties, `@synchronized`, blocks, the example host class, undeliverable records and start-up;
  - `tests/cpp/objc_real_test.cpp` (`objc.real_image_app_delegate`, local only) runs the recompiled `MapleThiefAppDelegate` and `EAGLView_Instance` methods on the real image;
  - the #46 tests stay as they were.
- **How we'd know this was wrong:**
  - a placeholder hides a missing superclass relationship that game code tests (`isKindOfClass:` against `UIView` from a `UIButton`). Until the real class exists, a placeholder inherits only from `NSObject`;
  - the run loop's per-turn pool releases an object the game still uses, because the game relied on a pool lifetime iOS gave it. The fix would be in the pool push/pop placement, not in this design.
