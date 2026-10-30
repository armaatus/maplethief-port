# The scripted first-map run

`play.first_map` ([#76](https://github.com/armaatus/maplethief-port/issues/76)) plays the real game
for 15 game-minutes on its first hunting map and checks that nothing breaks. A small bot drives
the recompiled game through the frontend's own input layer with a virtual Xbox 360 pad, so the
input goes the way a player's does (Gamepads → Actions → the runtime, ADR 0048). It needs the
real assets, so it runs locally only. Without `work/gen`, the slice or the bundle it exits 77
(skipped), so CI skips it.

## Running it

```sh
scripts/bootstrap                      # work/ipa, work/slices (once per checkout)
python3 -m recomp all                  # work/gen
cmake --preset mac-debug && cmake --build --preset mac-debug --target play_real_test
ctest --preset mac-debug -R play.first_map --output-on-failure
```

(`linux-debug` and `win-debug` the same way.)

Or directly, with the number of game ticks to play (default 18,000 = 15 game-minutes at 50 ms):

```sh
SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
  build/mac-debug/tests/play_real_test first_map OUT_DIR [TICKS]
```

It runs on the virtual clock (ADR 0044), so a run is deterministic. In a Release build
(`CMAKE_BUILD_TYPE=Release`, one architecture) it takes about 70 s of real time on an
M-series Mac; a Debug build is slower.

## What it does

1. Play on a fresh data directory creates the character. The intro, then the tutorial on maps
   2.8 and 2.9 (`then_tutorial`). Then Henesys (2.0), where Stan gives Elder's Advice.
2. Portal200 (x 40) to map 2.3, the first hunting field. There it opens the map screen: BACK
   opens the mini map, BACK again the world map, A the mini map again, B closes it.
3. It hunts on 2.3 until TICKS game ticks have passed since the hero first stood in a field
   (`then_hunt`):
   - it drinks a Red Potion (Y) when HP is under a third, a Blue Potion (B) when MP is under
     half;
   - it walks to the nearest mob on the hero's floor and attacks it (X);
   - with no mob near, it walks onto the nearest drop, which the game picks up on contact;
   - otherwise it wanders, jumping every 6 s;
   - after a fall it presses OK (Return to town) and walks from Henesys back through the
     portal.

## What it checks

Each game-minute: the game ticked (at least 90 % of the ticks a minute should have) and
presented (at least 0.9 GL frames per tick), the screen is not black, and nothing is in the
unimplemented report. Each minute prints a summary line and saves `minute_NN.png`.

At the end (`common_checks`, for every mode):

- the run ended from the script, with no trap or guest crash;
- the play lasted TICKS ticks;
- the unimplemented report is empty;
- no GL misuse, and no unsupported or invalid draw;
- no error line in the runtime's log: words like error, fail, unsupported, exception, fault or
  cannot, outside the trace channels.

Then `first_map_checks`:

- the hero was on maps 2.8, 2.9, 2.0 and 2.3;
- the hero walked, jumped and attacked, and crossed at least 300 px of the field;
- mobs died and damage numbers showed;
- mobs hit the hero (`CHero::TakeDamage`);
- drops lay on the ground and were picked up, and meso and items went up;
- the hero gained a level on the field: `CHero::LevelUp` was entered and the level went up;
- the map screen showed at least two different views.

The output directory (git-ignored, under the test output directory) holds:

- `play.log`, the runtime's log;
- `henesys.png`, `map_view_*.png`, `first_kill.png`, `first_pickup.png`, `minute_01.png` …
  `minute_15.png` and `end.png`, composited shots for looking at by eye;
- `data/`, the save directory.

These are renders of game art. They are never committed.

A full run on 2026-10-30 took the hero from level 5 to 12. It had 111 kills, 197 drops (160
picked up), 13 falls (the potions run out after about five minutes) and 1,200–1,300 GL frames
per 1,200 ticks. See [ANALYSIS.md](../ANALYSIS.md), Program flow.

## Debugging switches

| Variable | Effect |
|---|---|
| `MT_HUNT_TRACE=1` | Twice a game-second: the hero, the map, the popup and the held buttons. |
| `MT_DROP_TRACE=1` | Every kill and every new drop: id, place, state, life. |
| `MT_GL_DUMP=FIRST-LAST` | Every draw of GL frames FIRST..LAST: the caller (`lr`), the texture, the colour and the first vertices. |

## Save, quit, relaunch (#78)

The same program has three more modes:

- `play.save_quit` then `play.save_relaunch` (a CTest fixture) are two processes on one data
  directory, `play_save/data`. The first plays the tutorial, drinks a Red and a Blue Potion, and
  saves on Stan's ledge in Henesys. While the game shows "saved" it records the hero's state
  (`saved_hero.txt`: 119 facts) and copies `theif0` and `quest0` (`saved_*`); then the process
  ends. The second checks that the files are byte for byte as saved, loads slot 0 (Play, the
  first slot, Start) and compares the hero with the record. It then saves again and compares the
  bytes, leaving out the heap bytes the game never writes.
- `play.config_speed` boots on a `config` whose speed byte is 3 and checks that the game ticks
  25 times a game-second (40 ms, speed 2), with the file left as it was
  ([ADR 0074](../adr/0074-save-files-atomic-writes-sanitising-and-ios-documents.md)).

A mode with `relaunch` set in `k_modes` keeps OUT_DIR and its data directory from the run before.
Without saves there it exits 77.

```sh
ctest --preset mac-debug -R "play.save|play.config" --output-on-failure   # about 2 min (Debug)
```

## Extending it (#77, #146, #147, #86)

The code is in two parts:

- `tests/cpp/game_driver.h`: the driver that `menu_nav_real_test` shares. It boots the game and
  runs the `Run`'s task queue, one check per run-loop turn, and drives the virtual pad. Its
  tasks: `then_wait_for`, `then_press`, `then_walk_to`, `then_climb`, `then_portal`,
  `then_talk`, `then_fight`, `then_save`, `then_new_game`, `then_load_slot0`, `then_tutorial`
  and `then_finish`.
- `tests/cpp/guest_state.h`: read-only views of guest memory: the hero, the mobs, the drops, the
  quests and the popups.

To add a run:

1. Write a function `void my_mode(Run &r, u64 goal)` in `play_real_test.cpp` that queues
   tasks. Start from a fresh game (`then_new_game` + `then_tutorial`) or from a save
   (`then_load_slot0` on a data directory the mode prepares). Go where the run should go with
   `then_walk_to`, `then_portal` and `then_talk`. Play there with `then_hunt`, or with a new
   `then_*` built on `then(r, what, timeout, fn)`, where `fn(cpu, elapsed)` returns true when
   done.
2. Write its checks, `void my_mode_checks(Run &, u64 goal)`, over `g_play` and the game events.
3. Add `{"my_mode", my_mode, my_mode_checks}` to `k_modes`, and an `add_test` next to
   `play.first_map` in `tests/CMakeLists.txt`.

Every mode gets the monitor and `common_checks`. The monitor starts counting when the hero first
stands in the field on map 2.8 (the tutorial's first map); a mode that starts elsewhere, from a
save for instance, needs that condition in `monitor` widened. A chapter-1 run (#77) can chain
several hunting maps, each a `then_hunt` with its own `Comeback`: the town map, the x of the
portal back and the hunting map, used after a fall.
