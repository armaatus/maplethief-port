# Adding a selector on a host class

The game's Objective-C code sends messages to framework classes: `NSString`, `UIView`, `AVAudioPlayer` and the rest. In the port these are **host classes**: C++ tables of methods that guest code sees as ordinary Objective-C classes ([ADR 0026](../../adr/0026-objective-c-strategy.md), [ADR 0029](../../adr/0029-objective-c-runtime-services.md)). A selector a host class doesn't implement is an *unrecognized selector*: it is logged, goes into the unimplemented report, and the `MAPLETHIEF_UNIMPLEMENTED_IMPORT` policy applies. This page adds one. For a plain C function (`CFStringGetLength`, `memcpy`) see [Adding an HLE function](hle-function.md).

## 1. Find the class and the selector

- **From a run.** The unrecognized selector stops the guest with `MT_TRAP_OBJC`, and the crash report names the class and selector ([Debugging a trap](trap.md)). `MAPLETHIEF_UNIMPLEMENTED_IMPORT=continue MAPLETHIEF_REPORT=-` lists every one of a run at exit.
- **From the analysis.** [docs/re/host-surface.md](../../re/host-surface.md) lists every (class, selector) that game code sends to a framework class, with its call sites. The test `objc.host_surface` (`tests/cpp/host_surface_test.cpp`) checks that the registered host classes answer every row, so a row is a selector the port must have. The page is generated from the analysis (`python3 -m mtport.hostsurface generate`): don't edit it by hand. [docs/re/objc-shell.md](../../re/objc-shell.md) says what each of the game's own classes does with them.

Implement exactly what game code sends, and only that.

## 2. Find the class's file

`runtime/src/objc/classes.cpp` lists every host class, grouped by framework, with the file each comes from: Foundation in `runtime/src/foundation/`, the bridged CoreFoundation classes in `runtime/src/cf/`, UIKit's views and controls in `runtime/src/uikit/`, the drawing classes in `runtime/src/cg/`, AVFoundation in `runtime/src/audio/`, EAGL in `runtime/src/gles/`, the offline services (GameKit, StoreKit) in `runtime/src/stubs/`. A class is an `mt_objc_host_class`: its name, its superclass, its guest instance size, and two method tables, instance methods and class methods.

A class that isn't there yet gets a new `mt_objc_host_class` and one line in `k_host_classes`, after its superclass. The worked example at the top of `runtime/include/maplethief/objc/host.h` is a whole class.

## 3. Write the method

A host method is `void method(mt_cpu *cpu)`, entered like any method: r0 is `self` (the class object for a class method), r1 is `_cmd`, the arguments follow under the iOS armv7 rules, and the result goes in r0 (r0:r1 for 64 bits):

```cpp
// +isMultiThreaded: NO, as no game code creates a thread.
void thread_is_multi_threaded(mt_cpu *cpu) { cpu->r[0] = 0u; }

const mt_objc_host_method k_thread_class[] = {{"isMainThread", thread_is_main},
                                              {"isMultiThreaded", thread_is_multi_threaded}};
```

- Arguments: `mt_args a = mt_objc_args(cpu);` starts after `self` and `_cmd`, then `mt_arg_u32`, `mt_arg_f32` and the rest of `maplethief/hle/abi.h`. A method that returns a structure larger than 4 bytes (`-frame`, a `CGRect`) is sent with `objc_msgSend_stret`: use `mt_objc_args_stret` and `mt_ret_struct`.
- **Read every argument first.** Sending a message or calling guest code clobbers r0–r3.
- **Keep host state in the side table** (`mt_objc_state` with an `mt_objc_state_type`), never in the object's guest bytes: the instance size is the SDK's, and the game's subclasses put their own ivars right after it.
- **Retain and release through messages** (`mt_objc_retain`, `mt_objc_release`, `mt_objc_autorelease`), so classes that override them keep working. A method that returns a new object autoreleases it, as Cocoa's naming rules say.
- The selector string is the method's name without the `+` or `-`: `"errorWithDomain:code:userInfo:"`.

The rules are in full at the top of `runtime/include/maplethief/objc/host.h`.

## 4. Test it

- **The behaviour**, in CI: a doctest case that sends the message through the runtime, as the game does. `tests/cpp/foundation_test.cpp` (`w.send(w.cls("NSThread"), "isMainThread")`), `uikit_test.cpp`, `objc_services_test.cpp` and `stubs_test.cpp` have the fixtures for their areas.
- **The surface**: `objc.host_surface` passes for every area it checks. When a whole framework's area is done, it joins `kCheckedAreas` in `tests/cpp/host_surface_test.cpp`.
- **In the game**: `MAPLETHIEF_REPORT=-` no longer lists the selector, and `MAPLETHIEF_TRACE_HLE='_objc_msgSend*'` logs each send with its selector name.

Then [the quick check](../testing.md#before-you-push): `scripts/lint` and `scripts/test`.

## Worked example

Followed for [#333](https://github.com/armaatus/maplethief-port/issues/333) on a scratch branch: `+[NSThread isMultiThreaded]` as above, added to `k_thread_class` in `runtime/src/foundation/misc.cpp`, with one `CHECK` beside the `isMainThread` checks in `foundation_test.cpp`. Two files and one build; `objc.Foundation: NSURL, NSError, NSThread and NSClassFromString` passed, and so did the whole `cmake --workflow --preset mac-debug`. The game never sends it, so the example was not merged.
