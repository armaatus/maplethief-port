# 105. Save states: an in-process snapshot at safe points, built in three parts

- **Status:** Accepted (research done; the implementation is #202, #203, #204)
- **Date:** 2026-11-05
- **Issue:** #190 (epic #201)
- **Builds on:** [ADR 0025](0025-guest-memory-and-address-space.md) (guest memory, host-side heap bookkeeping), [ADR 0029](0029-objective-c-runtime-services.md) (host objects and their states), [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (the virtual clock), [ADR 0070](0070-settings-registry-and-presets.md) (Assists), [ADR 0089](0089-autosave-on-quit-and-background.md) (no "save anywhere" in the game's saves), [ADR 0104](0104-input-replay-and-regression-checkpoints.md) (checkpoints)

## Context

Players of emulators and recomp ports expect save states: quick-save before a boss, quick-load after a bad jump. The game's own saving is limited on purpose (it refuses on 16 maps, boss state is not saved), and ADR 0089 rejected "save anywhere" for the game's save files. A save state sidesteps that: it is a snapshot of the running port, kept apart from the game's saves.

#190 asked whether that can be done safely, and how. [docs/re/save-states.md](../re/save-states.md) has the research: every host state the guest refers to with its plan, the safe points, the size, and the prototype. Its findings:

- The game is single-threaded and keeps all of its state in guest memory. Between two run-loop turns no game function is on the stack.
- The port's host side holds the rest: the heap's bookkeeping, the ObjC runtime's tables (retain counts, pools, `+initialize` flags), 43 kinds of per-object host state (21 live, 1,125 objects at the census point), GL names and 8.9 MB of texels, OpenAL buffers and the mixer's voices, players and their MP3 streams, the run-loop timer, the clocks.
- **A whole-process snapshot taken between ticks in the field is complete and exact.** `play.save_state_fork` forked the process at step 46,000 of the regression replay; the parent played 200 ticks on, then the untouched snapshot played the same 200 ticks, and all 201 checkpoints (screen hash, audio hash, state) were equal.

## Options considered

1. **fork() (or an OS process snapshot).** Exact and free, as the prototype shows. But Windows and iOS have no fork, a forked process cannot be written to a file or survive a quit, threads (the audio callback, SDL's) do not survive it, and on macOS CoreFoundation refuses to run in a forked child. A probe, not a feature.
2. **A state = the inputs since a known point, replayed.** ADR 0104 replays exactly, so a "state" could be the inputs since the last game load, fast-forwarded on restore. Only exact in the fixed-step mode (the shipped game runs on real time), and a restore costs time in proportion to the play since the load: a minute of play is about 45 s of Debug replay.
3. **A host arena.** Allocate all of the runtime's host state in a second fixed arena and copy both arenas. It would catch everything, but every container, SDL and the standard library allocate outside it, and host pointers into code and the GPU differ per process. Fragile, and invisible when it breaks.
4. **An explicit, in-process serialization at safe points.** Guest memory (sparse) plus each host subsystem's state through a save/load contract, restored inside the running process at the turn hook, where the host call stack is the same every turn. Work in proportion to the host state's kinds, but explicit, testable per part, and portable.
5. **Reject.** Defensible if the host side were unbounded, but the census bounds it: 43 state kinds and a few subsystems.

## Decision

**Option 4, with limits, built in three parts.**

**Where.** Only in the turn hook, and only at a safe point: between ticks, game mode 0, popup −1, key pad panel 0, no scene, no touch or held key in the runtime, no UIView animation running, GCD idle, no file handle open, autorelease pools empty. Anywhere else the request is refused with a reason ("not in a menu", "not during a scene", …). Restoring at the turn hook means the host call stack is the run loop's, the same in every turn, so only data is replaced.

**What.** The state file holds: a header (magic, format version, the armv7 slice's SHA-256, the port's version, the character slot, the guest time) and tagged sections with lengths and CRCs: writable image pages, the heap below its top (zero pages skipped), the heap's bookkeeping, the main stack, the cpu and sjlj state, the ObjC runtime's mutable tables (retain counts, `@synchronized` depths, pools, `+initialize` flags, selectors interned after boot), every host state through a registry of per-type `save`/`load` functions keyed by the type's name, the GL state and texels, the OpenAL buffers' PCM and sources, the mixer's voices, each player's file and frame. Compressed with deflate through the already pinned stb_image_write / stb_image (no new dependency). The census point's state is about 12–14 MB raw.

**What not.** Class and method tables and caches (the same for one binary: the binary hash is checked instead), the settings, the bindings, the window and the audio device (the player's, not the game's), and the frontend's per-turn helpers (reset at a restore: input released, Assists, scene skip, focus, autosave's timer). Never the game's save files: a state is not read or written by the game, and the game's next save after a restore writes whatever is loaded.

**Clocks.** The state holds the guest time; a restore re-bases the clock behind the guest (the virtual clock, or the launch clock in real time) so the guest's time goes on from it, and the timers' fire dates stay consistent.

**Refusals.** A reader refuses a wrong magic, version or binary hash, a section with a bad CRC or a length past the file (the file is untrusted input), and a state of another character slot. A refused state changes nothing.

**Parts.** The implementation is more than the ~5 days CLAUDE.md allows one issue, so it is split, each part with its own tests:

1. **#202**: the state file, the safe-point check, guest memory, the heap's bookkeeping, the cpu and sjlj, the ObjC tables, the value host states (CF, Foundation, the run loop), the clocks, and a census test that names every live host state type without a serializer.
2. **#203**: the graphics, audio and UIKit host states, and the in-process restore test that replaces the fork prototype: restore at tick N, 200 more ticks give the frames and audio of the uninterrupted run, at two points.
3. **#204**: the Assist: `save_states = off | on` (Assists, off in every preset), one state per character slot in `<data dir>/states/`, `quick_save` (F5) and `quick_load` (F9) unbound on pads, toasts for saved, loaded and refused.

## Evidence

- `play.save_state_fork` (local, real assets, 2026-11-05, macOS arm64 Debug): the safe point at step 46,000 (map 2.8, mode 0, popup −1, panel 0), 201 checkpoints over steps 46,000–56,106 equal between the uninterrupted and the restored run, digest `4d063d80e179a629`, the last audio hash `278e200ee92ec987` over 2,474,274 frames; 109 distinct frames, the hero at 44 places.
- The census at that point (`OUT_DIR/census.txt`): docs/re/save-states.md.

## Consequences

- Every new kind of host state needs a serializer. #202's census test fails by name when a live state type has none, so a forgotten one shows in the first real-asset run, not in a player's restore.
- The fork prototype stays as a cheap check of the boundary (anything new outside the process that the game depends on breaks it) until #203's in-process test supersedes it. It is POSIX only and needs SDL's CoreFoundation joystick drivers off on macOS.
- A state belongs to one binary: a new port build with the same game binary may read it only if its format version matches; an update of the port can make old states unreadable, which the toast says.
- In real time a restore gives the same state, not the same future. Only the fixed-step mode makes the future equal, which is what the tests use.
- `mt_objc_each_state` and `mt_objc_ref_counts` (`maplethief/objc/host.h`) are the census's API and the serializer's entry to the host states.

## Addendum: what #202 built, and four refinements (#202, 2026-11-05)

#202 built part 1 (`maplethief/snapshot.h`, `runtime/src/snapshot/`, [re/save-states.md](../re/save-states.md#the-implementation-202)). It follows the decision, with four refinements found while building it:

- **The heap is saved up to its committed end, not its top.** A free range that reaches the top lowers it, and the bytes above stay; a later `malloc` that grows the top hands them out again unzeroed. So the bytes between the top and the committed end count too (sparse, they cost almost nothing), and a restore zeroes whatever this process committed beyond the state's end, as a fresh commit would be.
- **The sjlj state is checked, not restored.** Each live guest jump buffer has a host `jmp_buf` on the host stack; at the turn hook the chain is the run loop's, the same every turn. The state holds the chain (top, exceptions in flight, the guest jump buffers) and a restore refuses unless it equals this turn's. Likewise the state's stack pointer must be this turn hook's.
- **The modules' own tables are host state too.** Besides the per-object host states, CF keeps its immortal objects, the two booleans, the main bundle and the run loop's schedule, Foundation its singletons and the files created but not yet written, and the process its `__cxa_atexit` list: each is a section (`mt_snap::Module`), so a guest object made after the state (a singleton, the main run loop) is forgotten again at a restore.
- **A held key is not a safe point.** At the fork prototype's step 46,000 the replay's hero is walking (a direction held), which the safe point refuses; `play.save_state_census` takes the first safe turn from there, step 47,377.

The census at that point: 13 live types without a serializer, all #203's (UIView, UIViewController, CALayer, UIImage, UIColor, CGColorSpace, CGColor, CGDataProvider, CGImage, EAGLContext, AVAudioPlayer, AVAudioSession, GKLeaderboardViewController); the state without them is 3.28 MB raw and 0.82 MB deflated, saved in 239 ms (macOS arm64 Debug).
