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
| The renderbuffer's target, the last frame | host | — | Reset: the next tick draws a whole frame. |

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
| Input (held keys, touches, the pad's held buttons in `Actions`) | Reset at a restore: everything held is released; what is still held physically is pressed again on the next turn. |
| The Assists' per-turn state (turbo phase, auto-potion's cooldown), scene skip, focus, prompts, minimap | Reset: each reads the guest every turn. |
| Autosave (ADR 0089) | Reset its timer. A state never writes the game's save files; the game's next save after a restore writes whatever is loaded, as after loading an older save. |
| The runtime's hooks (screen shake, flashes, widescreen) | Reset; they follow the guest. |

## Safe points

A state is taken or restored only in the turn hook (between two run-loop turns, after the turn's callouts ran) and only when all of these hold. Otherwise the request is refused with the reason.

| Condition | Why | How it is read |
|---|---|---|
| Between ticks | No game function on the stack; the sjlj chain holds no game context; no tick is half done (a map load blocks inside a tick, so it is never in progress here). | The turn hook. |
| Game mode 0, popup −1, key pad panel 0 | The field with nothing open. Menus, shops, NPC chats and the map screen are UIKit or GL screens with more host state in motion (focus, list drags, text). | `0x11AA00`, `0x108C64`, `mt_input_panel()`. |
| No scene (game modes 9 and 10) | A scene's commands run against the frame count (qol.md's [cutscene skip](qol.md#cutscene-skip)); a boss's entrance is a scene: save before it. | Game mode. |
| No touch down, no held key in the runtime | A touch is host state with a phase; releases must reach the game. | `mt_input`, UITouch states. |
| No UIView animation running | Animations are host-side model/presentation values over a time. | The views' `anims`. |
| GCD idle, no NSFileHandle or ExtAudioFile open, autorelease pools empty | Threads cannot be snapshotted; files are only open inside a tick. | The GCD stats, the states' census. |

At the census point (step 46,000) every condition held. In the replay's tutorial the field with nothing open is most of the play (an NPC chat, mode 4, was open at step 40,000), so a quick-save is refused only while something is open.

## Size

At the census point: writable image 172 KB + heap 2.5 MB (nonzero pages) + main stack ≤ 1 MB + texels 8.9 MB + PCM of 26 buffers + a few hundred KB of host states. About 12–14 MB raw; texels and PCM compress well. The port already pins stb_image_write, whose zlib compressor (with stb_image's inflate) gives deflate without a new dependency (ADR 0003). Later maps load more textures, so a state grows with the textures resident.

## The prototype

`play_real_test save_state_fork` (CTest `play.save_state_fork`, local, real assets) plays the regression replay (`tests/regress/title_to_combat.replay`, ADR 0104) to step `MT_SNAPSHOT_STEP` (46,000), checks the safe point, prints the census (`OUT_DIR/census.txt`), and forks. The parent runs on 200 game ticks with a checkpoint after each tick: the uninterrupted run. The child is the snapshot: it waits until the parent's whole run is over (its files written, its process torn down), then runs the same 200 ticks: the restored run. The parent compares the two logs with `compare_logs`.

Result (2026-11-05, macOS arm64, Debug): 201 checkpoints, steps 46,000–56,106, every one equal; digest `4d063d80e179a629` both ways; the last audio hash `278e200ee92ec987` after 2,474,274 frames. The window is play: 109 distinct frames, the hero at 44 places (walking, a jump, the slope up). The test takes about 45 s.

What it shows: the safe point is a complete boundary. Everything the next 200 ticks depend on is in the process at that turn, and nothing outside it (the data directory, a wall clock, a thread) leaks in: the snapshot ran *after* the parent had played on and written its files, and still matched. What it does not show: that the host state can be written to a file and read back. That is the in-process serializer of ADR 0105, whose test replaces this one with a restore inside the same process.

Two things it taught:

- At step 40,000 the game was in an NPC chat (mode 4): the check refused the point. The tutorial's field with nothing open came at 44,833.
- On macOS a forked child cannot use CoreFoundation, and SDL's IOKit, HIDAPI and MFi joystick drivers do (the child died in `__THE_PROCESS_HAS_FORKED_AND_YOU_CANNOT_USE_THIS_COREFOUNDATION_FUNCTIONALITY___YOU_MUST_EXEC__`). The test turns them off (`SDL_JOYSTICK_HIDAPI=0`, `SDL_JOYSTICK_MFI=0`, `SDL_JOYSTICK_IOKIT=0`); the virtual pad needs none. It is one more reason fork is a probe, not a feature.

The census uses `mt_objc_each_state` and `mt_objc_ref_counts` (`maplethief/objc/host.h`), which the serializer's registry check will use too.
