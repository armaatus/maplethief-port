# Save states: what a snapshot of the running game holds

A save state is a snapshot of the running port, not of the game's save: restore it and the game goes on from that instant, enemies, timers and music included ([#190](https://github.com/armaatus/maplethief-port/issues/190)). This page lists everything a state has to carry, where each part lives, how each part is saved or rebuilt, and when a state may be taken at all. The decision is [ADR 0105](../adr/0105-save-states-in-process-snapshot-at-safe-points.md); the verdict row is in [qol.md](qol.md#save-states).

Addresses are armv7 VM addresses. Counts come from the census of `play_real_test save_state_fork` (see [The prototype](#the-prototype)) at step 46,000 of the regression replay (ADR 0104): map 2.8, the field, mid-tutorial, the hero walking.

## Summary

- **The game itself is easy.** All game code runs on the main thread from one 1 ms run-loop timer ([timing-threading.md](timing-threading.md)); between two turns of the run loop no game function is on the stack, and the game keeps all of its state in guest memory (`__DATA`, the heap). Its random generator (`Rand_Next` `0x225E5`) and its clocks' last values are guest data too.
- **The port's host side is the work.** The guest refers to host state by guest addresses: ObjC and CF objects are guest blocks whose behaviour sits in host-side *states* (ADR 0029, ADR 0032); GL names, OpenAL names and players are keys into host tables. 43 kinds of host state exist; 21 were live at the census point (1,125 objects).
- **A whole-process snapshot at a safe point is complete and exact.** The fork prototype took one between ticks in the field, ran the parent on for 200 ticks, then ran the untouched snapshot the same 200 ticks: all 201 checkpoints (frame hash, audio hash, state) were equal. Nothing the game depends on lives outside the process: no open file, no thread, no wall clock in fixed-step.
- **A shippable state is an in-process serialization, not fork.** fork has no Windows or iOS equivalent, cannot be written to a file, and on macOS CoreFoundation (used by SDL's joystick drivers) refuses to run in a forked child. A state file of the census point would hold about 2.7 MB of guest memory and 8.9 MB of texels, a few MB compressed.

## Inventory: each state and its plan

"Save" means its bytes go into the state file; "rebuild" means the restore re-creates it from saved values; "keep" means it is the same in every process of one binary and is checked, not saved; "reset" means it is dropped and comes back by itself.

### Guest memory and the CPU (ADR 0025)

| State | Where | Census | Plan |
|---|---|---|---|
| `__DATA` (and any writable image page) | 0xF1000–0x11B000 | 172,032 bytes | Save. `__TEXT` and `__LINKEDIT` are read-only and come from the binary (keep, checked by the binary's hash). |
| The guest heap's blocks | 0x01000000 up to the heap's top | top 0x0137AAF0: 3.6 MB handed out, 610 of 891 pages nonzero (2.5 MB) | Save, sparse (zero pages skipped). Reused blocks keep old bytes, which the game reads ([ANALYSIS](../ANALYSIS.md), ADR 0025), so every byte below the top counts, not only live blocks. |
| The heap's bookkeeping | host: `mt::mem::Heap` (`runtime/src/mem/mem_internal.h`): live blocks, free ranges, small free lists, top, committed | 5,654 live blocks | Save. Addresses are deterministic, so after a restore the next `malloc` must hand out what the uninterrupted run hands out. |
| Guest stacks | 0x1F000000– (the main thread's 1 MiB) | 1 | Save the main stack (sp is at the run loop's frame, the bytes below it matter only as stale data the guest could read). |
| The main cpu's registers, VFP, sjlj state | host: `mt_cpu`, `mt_sjlj_state` | — | Save. At a safe point they are the run loop's: the sjlj chain holds no game context. |

### The ObjC runtime and its host states (ADR 0026, ADR 0029)

| State | Where | Census | Plan |
|---|---|---|---|
| Classes, methods, method caches, imported classes | host: `mt_objc` | — | Keep: built at boot from the binary and the host classes; caches are caches. |
| The `+initialize` flag of each class | `Class::initialized` | — | Save: a class first initialized after the state would otherwise not be initialized again after the restore, and its guest statics would be missing. |
| Selectors interned after boot | `sel_names`, `sel_ids` | — | Save the names added after boot, re-intern them. |
| Retain counts, `@synchronized` depths | `retains`, `syncs` | 98 objects retained more than once | Save. |
| Autorelease pools | `pool_objects`, `marks` | 0 at the safe point | Save (empty between turns; the check below asserts it). |
| Host states (`mt_objc_state_type`) | `states`, `state_heads`: per (object, type) a host block | 1,125 in 21 types | Each type gets a `save`/`load` pair (registry by the type's name). References between host objects are saved as guest object ids and linked after every box exists. Per type: below. |

The host states, by kind (43 types; the counts are the census's):

| Types | Census | Holds | Plan |
|---|---|---|---|
| CFString (110), CFNumber (5), CFData, CFURL, CFBundle (1), NSURL (3), NSError, NSArray (2), NSSet, NSDictionary (1) | 122 | Host strings, byte vectors, lists of guest ids | Save the values. CFBundle is the main bundle: rebuild from the bundle directory. |
| CFRunLoop (1), CFRunLoopTimer (1) and the run loop's schedule (`runtime/src/cf/runloop.cpp`) | 2 | Fire date, interval, callout, info (guest), order | Save. The fire date is in CFAbsoluteTime, re-based with the clock (below). |
| UIView (116), CALayer (1), UIViewController (1), UINibDecoder | 118 | Frames, flags, subview lists (guest ids), running UIView animations | Save; a state is refused while an animation runs (safe points). UINibDecoder only exists while a nib loads: never at a safe point. |
| UIImage (73), UIColor (5), UIFont, UIBezierPath, CGColorSpace (648), CGColor (5), CGDataProvider (73), CGImage (73), CGGradient, CGPath, CGContext | 950 | Boxes of host objects: decoded pixels, colours, paths, bitmap contexts | Save the values (pixels as bytes); rebuild the boxes. The 648 colour spaces are values of a few bytes each (none had been released by the census point). |
| EAGLContext (1) | 1 | The current context | Rebuild (there is one). |
| AVAudioPlayer (3), AVAudioSession (1) | 4 | The player's file, volume, loops, playing flag; its `Mp3Stream` | Save the file's path, volume, loop count and the frame position; rebuild the stream and `seek` to it (`Mp3Stream::seek` re-decodes from the start, so no decoder state is saved). |
| NSFileHandle | 0 at the safe point | The whole file in memory, offset, dirty flag | Refuse while one is open (safe points); none was open at the census point. |
| UITouch | 0 at the safe point | A finger's phase and place | Refuse while a touch is down; the frontend releases input at a restore. |
| GameKit (GKLeaderboardViewController (1), GKScore, GKAchievement, GKLeaderboard), StoreKit (5 types), CFSocket and its source (offline stubs) | 1 | Stub records, the cash-shop flow | Save the few values; the cash shop (game mode 0xB) is never a safe point. |

### Graphics (ADR 0040, ADR 0041)

| State | Where | Census | Plan |
|---|---|---|---|
| GL names, bindings, matrices, the fixed-function state, the viewport | host: `mt_gles::State` (`runtime/src/gles/gles_internal.h`) | 549 textures, 1 context | Save. |
| Texture pixels | host: mt_gfx `Textures` keeps every texture's RGBA8 texels on the CPU (`runtime/src/gfx/texture.cpp`) | 8,875,272 bytes | Save the texels (the game's upload buffers are freed after `glTexImage2D`, so the pixels exist nowhere else). With a texture pack, the pack's replacement is applied again at the rebuild, so the original texels are what is saved. |
| GPU textures and pipelines | host: the SDL_GPU backend (`gfx/gpu/gpu_backend.cpp`) | — | Rebuild from the texels (the backend re-uploads on a texture's new `generation`). |
| The renderbuffer's target, the last frame | host | 480 × 320 RGBA8 (× the render scale²) | Save (#203). The game never calls `glClear` and draws each frame over the last one; some of its draws blend with what is there, so the first frames after a restore depend on the frame before it. Without these pixels the restored run's frames were off by one in 2,116 pixels of the first frame and took 167 ticks to agree again. The renderbuffers themselves are the screen's and are kept (checked: same names, sizes and formats). |

### Audio (ADR 0045, ADR 0046)

| State | Where | Census | Plan |
|---|---|---|---|
| OpenAL buffers (PCM) and sources | host: `runtime/src/audio/audio.cpp` `State` | 44 buffers (26 filled), 44 sources | Save the PCM of each filled buffer (the guest's copy is freed after `alBufferData`) and each source's buffer, gain, loop and play position. |
| The mixer's voices | host: `runtime/src/audio/mixer.cpp`, under its lock | — | Save each voice's position and step (32.32 fixed point), bus and gain, under the mixer's lock (the SDL audio thread reads it). |
| ExtAudioFile | host: the whole data chunk in memory, a frame position | — | Only open inside a tick: never at a safe point. |
| The output device, the buses' gains, mute, mono | host and frontend | — | Keep: the player's settings, not the game's. |

### Threads, queues and clocks (ADR 0033, ADR 0044, ADR 0075)

| State | Where | Plan |
|---|---|---|
| GCD queues, timers, groups, workers | host: `runtime/src/gcd/gcd.cpp` | Refuse unless idle: no worker thread alive, empty queues, no timer pending. The game itself creates no thread and uses no queue (only Chartboost did, and it is stubbed: [timing-threading.md](timing-threading.md)), so this always holds in play. |
| The guest's clock | the virtual clock (fixed step) or the launch clock (real time), `platform/src/clock.h` | Save the guest time. At a restore, re-base the clock so the guest's time goes on from the saved value: `GetTimeMs`, `CFAbsoluteTime` and the run-loop timer's fire date stay consistent, and no sub-timer sees a jump. In real time a restore is the same *state*, not the same future: what the player does next is new input. |
| The `CMTimer` globals (lastTick, lastFrame, the sub-timers) | guest `__DATA` (`0x11A478`–`0x11A4F0`) | Saved with guest memory. |

### The port's frontend

| State | Plan |
|---|---|
| Settings, bindings, the window, the presenter, the audio device | Keep: they are the player's, not the game's. |
| Input (held keys, touches, the pad's held buttons in `Actions`) | Reset at a restore (`platform/src/save_state_input.h`): everything held is released before the restore; a key still held physically acts again only when it is pressed again. |
| The Assists' per-turn state (turbo phase, auto-potion's cooldown, the walk latch), scene skip, menu repeat, focus, prompts, minimap | Reset: `Assists::restart`, `SceneSkip::restart` and `MenuInput::restart` at a restore; the focus, prompts and minimap read the guest every turn. |
| Autosave (ADR 0089) | Reset its timer. A state never writes the game's save files; the game's next save after a restore writes whatever is loaded, as after loading an older save. |
| The runtime's hooks (screen shake, flashes, widescreen) | Reset; they follow the guest. |
| `game_speed`, the frame interpolator (#204) | The player's: after a load the frontend writes `game_speed`'s table and interval into guest memory again, and the interpolator forgets its ticks. |
| The clock behind the guest | Re-based to the state's guest time: `VirtualClock::rebase_to`, `LaunchClock::rebase_to` and (#204, the default `pacing = display`) `PacedClock::rebase_to`. |

## Safe points

A state is taken or restored only in the turn hook (between two run-loop turns, after the turn's callouts ran) and only when all of these hold. Otherwise the request is refused with the reason.

| Condition | Why | How it is read |
|---|---|---|
| Between ticks | No game function on the stack; the sjlj chain holds no game context; no tick is half done (a map load blocks inside a tick, so it is never in progress here). | The turn hook. |
| Game mode 0, popup −1, key pad panel 0 | The field with nothing open. Menus, shops, NPC chats and the map screen are UIKit or GL screens with more host state in motion (focus, list drags, text). | `0x11AA00`, `0x108C64`, `mt_input_panel()`. |
| No scene (game modes 9 and 10) | A scene's commands run against the frame count (qol.md's [cutscene skip](qol.md#cutscene-skip)); a boss's entrance is a scene: save before it. | Game mode. |
| No touch down, no held key in the runtime | A touch is host state with a phase; releases must reach the game. A restore does not ask this: the state's input (released) replaces what is held now. | `mt_input`, UITouch states. |
| No UIView animation running | Animations are host-side model/presentation values over a time. | The views' `anims`. |
| GCD idle, no NSFileHandle or ExtAudioFile open, autorelease pools empty | Threads cannot be snapshotted; files are only open inside a tick. | The GCD stats, the states' census. |

At the census point (step 46,000) every condition held. In the replay's tutorial the field with nothing open is most of the play (an NPC chat, mode 4, was open at step 40,000), so a quick-save is refused only while something is open.

## Size

At the census point: writable image 172 KB + heap 2.5 MB (nonzero pages) + main stack ≤ 1 MB + texels 8.9 MB + PCM of 26 buffers + a few hundred KB of host states. About 12–14 MB raw; texels and PCM compress well. The port already pins stb_image_write, whose zlib compressor (with stb_image's inflate) gives deflate without a new dependency (ADR 0003). Later maps load more textures, so a state grows with the textures resident.

Measured (#203, a full state at step 47,377): 23,467,715 bytes stored, 4,659,654 deflated. Stored is more than the estimate because the heap is saved up to its committed end (#202's addendum) and the screen's pixels are in it; the zero pages are left out of both.

## The prototype

#203 replaced it with `play.save_state`, an in-process restore ([below](#the-restore-203)); the fork test is gone. What it showed:

`play_real_test save_state_fork` (CTest `play.save_state_fork`, local, real assets) plays the regression replay (`tests/regress/title_to_combat.replay`, ADR 0104) to step `MT_SNAPSHOT_STEP` (46,000), checks the safe point, prints the census (`OUT_DIR/census.txt`), and forks. The parent runs on 200 game ticks with a checkpoint after each tick: the uninterrupted run. The child is the snapshot: it waits until the parent's whole run is over (its files written, its process torn down), then runs the same 200 ticks: the restored run. The parent compares the two logs with `compare_logs`.

Result (2026-11-05, macOS arm64, Debug): 201 checkpoints, steps 46,000–56,106, every one equal; digest `4d063d80e179a629` both ways; the last audio hash `278e200ee92ec987` after 2,474,274 frames. The window is play: 109 distinct frames, the hero at 44 places (walking, a jump, the slope up). The test takes about 45 s.

What it shows: the safe point is a complete boundary. Everything the next 200 ticks depend on is in the process at that turn, and nothing outside it (the data directory, a wall clock, a thread) leaks in: the snapshot ran *after* the parent had played on and written its files, and still matched. What it does not show: that the host state can be written to a file and read back. That is the in-process serializer of ADR 0105, whose test replaces this one with a restore inside the same process.

Two things it taught:

- At step 40,000 the game was in an NPC chat (mode 4): the check refused the point. The tutorial's field with nothing open came at 44,833.
- On macOS a forked child cannot use CoreFoundation, and SDL's IOKit, HIDAPI and MFi joystick drivers do (the child died in `__THE_PROCESS_HAS_FORKED_AND_YOU_CANNOT_USE_THIS_COREFOUNDATION_FUNCTIONALITY___YOU_MUST_EXEC__`). The test turns them off (`SDL_JOYSTICK_HIDAPI=0`, `SDL_JOYSTICK_MFI=0`, `SDL_JOYSTICK_IOKIT=0`); the virtual pad needs none. It is one more reason fork is a probe, not a feature.

The census uses `mt_objc_each_state` and `mt_objc_ref_counts` (`maplethief/objc/host.h`), which the serializer's registry check will use too.

## The implementation (#202)

Part 1 of [ADR 0105](../adr/0105-save-states-in-process-snapshot-at-safe-points.md) is `maplethief/snapshot.h` (`runtime/src/snapshot/`):

- **The file** (`state_file.h` has the byte layout): a 104-byte header (magic, format version 1, the armv7 slice's SHA-256 from `mt_process_image_sha256`, the port's version, the slot, the flags, the guest time in Unix microseconds, its own CRC-32), then named sections, each with its inflated and stored lengths and the CRC-32 of the stored bytes, deflated through the runtime's stb copy (`runtime/src/cg/zlib.h`) or stored. A reader skips sections it does not know and checks every length against the bytes left and a 768 MiB limit, and every CRC, before it inflates or reads anything.
- **The sections**: `cpu`, `sjlj` (checked equal), `image` (the writable segments), `heap` (pages up to the committed end), `heap.book`, `stacks`, `objc` (retain counts, `@synchronized` depths, pools and their marks, the classes whose `+initialize` ran, by name, the selector table: every name, its canonical SEL, every known guest copy), `objc.states`, `process` (`__cxa_atexit`), and the modules' `cf`, `cf.bundle`, `cf.runloop`, `ns`, `ns.files`. Pages are 4 KiB; a page of zeros is left out.
- **The host states**: one `mt_snap::StateOps` per state type, next to the type (`save`; `load` into a detached zeroed block, before guest memory is touched; `drop`, which frees a state's host memory without calling into the guest; `link` for #203's boxes; a stable name), listed in `runtime/src/snapshot/registry.cpp`. #202 has the value types: CFString, CFNumber, CFData, CFURL, CFBundle, CFRunLoop, CFRunLoopTimer, NSArray, NSSet, NSDictionary, NSURL, NSError. NSFileHandle has none on purpose: a state is refused while one is open.
- **A restore** stages everything first (every section parsed and checked against this process: the stack pointer, the unwinder's chain, the image segments, the stacks, the selector table's common part, every state loaded), and only then drops the live states, writes guest memory, rebuilds the tables, attaches the loaded states, applies the modules, sets the registers, runs the links, clears the method caches and hands the saved guest time to the platform's `rebase_clock` (`VirtualClock::rebase_to`, `LaunchClock::rebase_to`). A refused state changes nothing.
- **Tests**: `objc.snapshot: *` (synthetic: the format's round trip and every refusal, the safe point, the census, a restore of every value type with the next allocations equal to the uninterrupted run's) and `play.save_state_census` (local, real assets).

`play.save_state_census` (2026-11-05, macOS arm64 Debug): from step 46,000 of the regression replay the first safe turn is 47,377 (map 2.8, the field; at 46,000 the hero walks, a held key). Live there without a serializer, all #203's: UIView, UIViewController, CALayer, UIImage, UIColor, CGColorSpace, CGColor, CGDataProvider, CGImage, EAGLContext, AVAudioPlayer, AVAudioSession, GKLeaderboardViewController. The state without them (a partial save, `MT_SNAPSHOT_PARTIAL`, never restorable) is 3,280,920 bytes stored and 820,557 deflated, saved in 239 ms.

## The restore (#203)

Part 2 of [ADR 0105](../adr/0105-save-states-in-process-snapshot-at-safe-points.md) adds the rest of the host state, so that a full save is possible and a restore works in the process that made it:

- **Serializers** next to each remaining type: CGColorSpace, CGColor, CGDataProvider (its bytes), CGImage, CGGradient, CGPath (`cg/objects.cpp`), UIImage, UIColor, UIBezierPath (`cg/uikit.cpp`), UIFont (`cg/text.cpp`), UIView and CALayer (`uikit/view.cpp`: every field, the subviews, layer, actions and images as guest ids), UIViewController (`uikit/app.cpp`), EAGLContext (`gles/eagl.cpp`), AVAudioSession and AVAudioPlayer (`audio/avfoundation.cpp`), the GameKit and StoreKit records and the offline CFSocket (`stubs/`). CGContext has none: a bitmap context lives only inside a call and is never live at a safe point. `cg/cg_snapshot.h` writes the shared values (colours, points, rects, transforms, fonts, paths).
- **Sections** for the tables beside the objects:
  - `gfx`: every texture with its texels and the next handle. A replaced texture (a texture pack) is saved with the replacement's texels.
  - `gles` (`gles/gles_snapshot.cpp`): the GL names, bindings, matrices, fixed-function state and current context. It also holds the screen renderbuffers' pixels; the renderbuffers themselves are kept.
  - `audio`: OpenAL's buffers (PCM) and sources, the session, the players and the mixer's voices. The voices are copied and replaced under the mixer's lock. Each player's MP3 is re-opened (through the audio pack first, as it was opened) and `Mp3Stream::restore` seeks to the saved frame. The output device, bus gains, mute and mono stay.
  - `uikit`: the application, its windows, nib objects and presented controllers.
  - `uikit.touch`: the touch tracking. A restore drops the present's queued touches.
  - `cg.uikit`: named colours and the display.
  - `input`: the key pad's panel, routes, queue and ticks.
  - `stubs`, `gamekit`, `storekit`, `cashshop`.
  - `libc.stdio` and `libc.ntoa`.
- **Kept, not saved** (the same in every process, or the player's): the text and audio packs, the bus gains and mono, the texture filter override, the image hook, and the hooks' flags. The flashes hook's mark and the widescreen menus are only set inside a call.
- **A restore lets go of input.** `mt_snapshot_restore` skips the safe point's touch, held-key and queued-input conditions: a save was made with none of them, and the state's `input` and `uikit.touch` replace what the player holds now. The frontend first releases everything it holds and restarts its per-turn helpers (`platform/src/save_state_input.h`, `release_for_restore`). That covers Input's touches, the pad, every source in Actions, the pad pointer, Assists, the scene skip and the menu repeat. Otherwise they would press or release into the restored game what belongs to the present.

`play.save_state` (local, real assets) replaces the fork prototype. It plays the regression replay with an in-process save and restore at two points, the first turn at or after a point's step where `mt_snapshot_safe` holds and the replay's pad holds nothing:

- At the save, the state is saved into memory (`mt_snapshot_save`). The harness keeps its own counters beside it: turns, ticks, GL frames, the replay's cursor, the audio hash, the last GL frame and the monitor.
- 200 game ticks follow, with a checkpoint after each: the uninterrupted run.
- At the next turn a restore may run, the pad and the frontend let go, `mt_snapshot_restore` puts the state back (`VirtualClock::rebase_to`) along with the harness's counters, and the same 200 ticks run again.
- The 201 checkpoints (composited screen hash, audio hash, state line) must be equal.

The run then goes on, so the second point also checks that the game after a restore plays on as it would have.

Result (2026-11-05, macOS arm64 Debug):

| Point | Saved at (from) | Restored at | Window | Digest (both runs) | Last audio | Play in the window |
|---|---|---|---|---|---|---|
| The tutorial's first map, 2.8 | 47,377 (46,000) | 61,135 | steps 47,377–57,528 | `f970fd2b49c31151` | `c7ea5e9d1a185555` / 2,536,984 frames | 109 distinct frames, the hero at 38 places |
| The hunting field 2.9, the snail fight | 73,628 (73,627) | 83,783 | steps 73,628–83,782 | `819786c3ce7d80e1` | `5f949f79a6ddfad9` / 3,694,786 frames | 85 distinct frames, the hero at 14 places, a snail defeated (kills 0 → 1) |

A full state there is about 4.6 MB deflated (23.5 MB stored at 47,377). It is saved in about 820 ms and restored in about 390 ms (Debug).

`play.save_state_census` now requires that no live type lacks a serializer, and that a full save is made and read back at its safe point (47,377: 4,659,654 bytes deflated, 23,467,715 stored).

Two things it found:

- **The screen is state.** The game never clears and draws each frame over the last. With everything else restored but not the renderbuffer, the first frame after the restore had 2,116 pixels off by one. The GL calls were the same call for call; the difference came from blending over the old frame. It took 167 ticks to agree again on 2.8 and 27 on 2.9. The renderbuffer's pixels are now in the `gles` section.
- **The restore point need not be a save point.** The replay's hero walks or attacks at most turns, so the turn after a window rarely has nothing held. A restore skips the input conditions, and the test lets go of the pad (and `release_for_restore` of the frontend's layers) first. The 2.9 restore ran at the first turn after its window; the 2.8 one waited from step 57,529 to 61,135 for the field with nothing open (the other conditions still hold for a restore).

Debugging aids for the next difference:
- `MT_SAVE_STATE_DEBUG=1` prints each GL frame's hash.
- `MT_SAVE_STATE_DUMP=<step>` writes that step's GL frames.
- `MT_SAVE_STATE_TRACE=<step>` writes every GL call of 60 steps from there, once per run.

## The Assist (#204)

Part 3 of [ADR 0105](../adr/0105-save-states-in-process-snapshot-at-safe-points.md#addendum-what-204-built-the-assist-204-2026-11-05) is the player's side, `platform/src/quick_state.{h,cpp}`: `save_states = on` (an Assist, off in every preset), F5 `quick_save` and F9 `quick_load` ([keyboard.md](../controls/keyboard.md#save-states)).

- One state per character slot: `<data dir>/states/slot<N>.state`, N the slot in play (CDataMgr `+0x4E4`), written with `mt_saves_replace_file`. The game's `Documents/` is never touched.
- In the turn hook at the turn after the press. A save needs the whole safe point; a load skips the held-input conditions, checks the file's header and the port's version first, then lets go of the frontend's input (`release_for_restore`) and restores. Every refusal is a notice in the player's words; the runtime's reason is logged.
- The port's overlays hold the turn while open and take every key, so a load never happens under one.

`play.quick_state` (local, real assets) plays the regression replay with the frontend's path: at step 40,000 (an NPC chat) F5 and F9 are refused ("not in a menu") and write nothing; at 73,628 on 2.9 F5 writes the state (4,609,535 bytes), at 83,783 F9 loads it and the 200 ticks after it give the same 201 checkpoints as the uninterrupted run (digest `58bd3d6ef67677d5`); `theif0` and `quest0` are byte for byte the same before and after each key.
