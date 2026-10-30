# 53. Observed guest functions: game events at a function's entry, and gamepad rumble

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #72
- **Builds on:** [ADR 0014](0014-recompiler-design.md) (the lifter's function shape, `MT_FN_ENTER`), [ADR 0030](0030-runtime-diagnostics.md) (entry coverage, the trace channels), [ADR 0045](0045-audio-mixer-and-output.md) (the #64 addendum's vibrate hook), [ADR 0048](0048-gamepad-actions-and-delivery.md) (`Gamepads::active()`), [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (`settings.ini`)

## Context

#72 wants the gamepad to rumble when the hero is hit, when it gains a level and when the game calls the phone's vibration. The game's code says ([docs/re/audio.md](../re/audio.md#vibration), the decompiler):

- **A hit** goes through one function, `CHero::TakeDamage(hero, damage, from_left)` `0x65B55`. It does nothing for a dead hero (flags `+0x0C` bit `0x10`) and shows "miss" for `damage <= 0`. Otherwise it calls `CMSoundPlayer::Vibrate(100)` first and then takes the damage off HP `+0x94`, halved by a guard skill. `CHero::Die` `0x65979` runs at 0 HP. Two functions call it: the mob-contact check `CHero::CheckMobContact` `0x65F05` and the attack-hit check `CHero::CheckAttackHit` `0x65DC5`. One hazard takes 300 HP directly (`0x65F05` in areas 10 and 11) and never reaches it.
- **A level-up** is `CHero::LevelUp(hero)` `0x65089`: level `+0x80` + 1 while it is below 99, new max HP and MP, both refilled. `CHero::setExp` `0x6654D` calls it once per level gained (with `CHero::ExpToLevelUp` `0x65919` as the step), and so does an item effect `0x66695`. The game doesn't vibrate on a level-up.
- **The vibration**: `CMSoundPlayer::Vibrate` plays `kSystemSoundID_Vibrate` while the vibration option (config byte 3, on by default) is set. It runs on every hit, in the options menu's preview and in 22 scene steps. Since #64 the runtime calls `mt_audio_set_vibrate_hook`'s function for it.

So the vibration covers hits, but only while the game's option is on, and it says nothing about level-ups. The runtime has no way to hear about a guest function being called. Generated functions call each other directly in C, and `mt_dispatch` only sees indirect calls. The entry coverage of ADR 0030 exists only in trace builds and only reports the first entry.

What had to be decided:

- **How the host learns of a hit and a level-up** without changing what the guest does.
- **Where the SDL part lives**, so that the runtime stays SDL-free.
- **What rumbles, how strongly, and how it is switched off.**

## Options considered

For the hook:

1. **Poll the hero's state every turn**: an HP drop is a hit, a level rise a level-up. This needs no recompiler change. But it guesses: unequipping clamps HP to a lower maximum, loading a character can reuse the same hero address with a lower HP, and the 300 HP hazard counts as a hit. It also sees nothing of what the function was called with.
2. **Replace the functions with host code that calls the original.** The recompiler has no such mechanism. It would be a second way to reach guest functions, beside the direct calls, just for two functions.
3. **Observed functions. Chosen.** The recompiler emits a call to the runtime at the entry of a listed function, before its first instruction: `MT_FN_OBSERVE(0x00065B55u)` after `MT_FN_ENTER()`. The runtime's observer reads the arguments in `r0`–`r3` and guest memory, applies the function's own guard (not dead, `damage > 0`; level below 99) and reports an event. It never writes, so the guest runs byte for byte as without it. The list is data in the recompiler, and observing another function later costs one line there and one case in the runtime.

For the platform side:

1. **Rumble at once, from the hook.** A hit with the game's vibration on is two requests within one guest call (the vibration, then the damage event). Two `SDL_RumbleGamepad` calls in a row: the second replaces the first.
2. **Merge per turn. Chosen.** The requests of one frontend turn merge: the strongest amplitude per motor and the longest duration. They go out as one `SDL_RumbleGamepad` at the next turn, at most one turn later (16 ms at 60 Hz).

## Decision

**The recompiler** (`recomp/observe.py`): `OBSERVED` maps function pointers to names, today `CHero::TakeDamage` `0x65B55` and `CHero::LevelUp` `0x65089`. `LiftOptions.observe` holds the pointers whose entry the lifter reports. It is empty by default, so snippets and the differential test (ADR 0017) lift as before. The whole-image lift (`recomp.prototype.lift`, used by `lift`, `all`, `coverage` and `census`) passes `OBSERVED`. `maplethief/recomp/calls.h` declares `void mt_observe_entry(mt_cpu *, uint32_t fn)` and the macro `MT_FN_OBSERVE(fn)`, part of the generated code's ABI. The call comes after the flag locals and before the first guest instruction.

**The runtime** (`maplethief/events.h`, `runtime/src/events/events.cpp`) has no SDL:

- `mt_observe_entry` switches on `fn` (`MT_EVENTS_FN_TAKE_DAMAGE`, `MT_EVENTS_FN_LEVEL_UP`; `tests/test_recomp_observe.py` checks that they match `OBSERVED`). It reads the hero from `r0` only where guest memory exists (an image segment or the committed heap, as the input module reads). It reports a `mt_game_event`: `HERO_DAMAGE` with the damage asked for, or `LEVEL_UP` with the new level, plus the hero and its HP and max HP. Entries the guard turns away are counted as `ignored`.
- `mt_game_events_set_hook(fn, user)` installs the host's function (called on the guest thread, inside the guest's call; it must not call back into the guest). `mt_game_events_get_stats` counts entries, hits, level-ups and ignored entries. The trace channel is `events`.
- The game's vibration stays the audio module's hook (#64). Neither knows of the other.

**The frontend** (`platform/src/rumble.{h,cpp}`, `Rumble`):

- `install()` takes both hooks: the vibration requests `kRumbleVibrate` (0.70 / 0.70, 400 ms: one fixed buzz like the phone's, which ignores the game's milliseconds too), a hit `kRumbleDamage` (0.60 / 0.30, 180 ms) and a level-up `kRumbleLevelUp` (0.35 / 0.85, 700 ms). The two numbers are the amplitudes of SDL's low- and high-frequency motors.
- `turn()`, once per frontend turn after `Actions::turn()`, sends the merged request to `Gamepads::active()` with each amplitude × `rumble_strength` / 100. Requests are ignored while rumble is off or at strength 0. A turn with no active pad drops them. A pad without motors answers false, which is counted and logged once. `stop()` silences the pad that rumbled last, looked up again by its id since it may have been unplugged. Losing focus and shutting down call it.
- **Settings:** `settings.ini` has `rumble = on | off` (default on) and `rumble_strength = 0..100` (default 100), and the command line has `--rumble on|off` and `--rumble-strength N` (the #64 pattern: `apply_setting`, then `--save-settings` writes them). `rumble` and the game's vibration option are separate: the game's option gates its own vibration, the port's setting gates every rumble.
- The exit log counts hits, level-ups and what Rumble sent.

## Consequences

- A hit rumbles whatever the game's vibration option says. With the option on (the default), the vibration and the hit arrive in one turn and give one rumble. The scenes' vibrations rumble as on the phone. Level-ups rumble, which the phone never did.
- The generated code gains two calls, one at each listed function's entry. The real game behaves the same: `input.gamepad_play` and `input.gamepad_hold` still pass, and the hero takes the same HP and gains the same levels in `input.rumble`.
- Observing another function (a mob's death, a quest's completion) takes one entry in `OBSERVED`, one case in `mt_observe_entry` and one event kind.
- **CI:** `platform_test`'s `rumble_test.cpp` covers the observer on a synthetic hero (each guard, a bad pointer, no write to the guest) and `Rumble` on SDL3 virtual joysticks whose `Rumble` callback records SDL's calls. Covered: each event at 100, 50 and 1 %; a merge; off and strength 0 never rumble; no pad, a pad not yet active, a pad unplugged, a pad without motors; the vibrate import and the observer through the installed hooks. The settings keys are tested too. `tests/test_recomp_observe.py` checks the lifter: the call goes only where asked and before the first instruction.
- **Real assets:** `input.rumble` boots to the first map with a virtual Xbox 360 pad with motors. It hits the hero through `CHero::TakeDamage` (7 HP, as a monster's touch does) and levels it up through `CHero::setExp` with the level's EXP, first with rumble off (HP and level change, no call to the motors) and then on at 60 % (one call each, at the expected amplitudes; the hit's call merges the game's own vibration with the damage).
- Real pads: rumble on an Xbox and a PlayStation pad is a needs-human check on #72.
