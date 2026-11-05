# The regression replay

`scripts/regress` ([#91](https://github.com/armaatus/maplethief-port/issues/91),
[ADR 0104](../adr/0104-input-replay-and-regression-checkpoints.md)) plays a recorded input
replay on the real game and checks that it plays the same way every time: the same screens,
the same audio, the same game state at every checkpoint. It needs the real assets, so it runs
locally only. Without `work/gen`, the slice or the bundle it exits 77 (skipped), so CI skips it.

## Running it

```sh
scripts/bootstrap                      # work/ipa, work/slices (once per checkout)
python3 -m recomp all                  # work/gen
scripts/regress                        # builds play_real_test (mac-debug), plays the replay once
scripts/regress --repeat 10            # ten plays: each passes, one digest
scripts/regress --preset original      # MAPLETHIEF_SETTINGS=preset=original
scripts/regress --settings turbo_attack=on --preset enhanced
```

On Windows: `scripts\regress.ps1 --repeat 10` from a Visual Studio x64 developer shell.
CTest `play.regress` (labels `cpp;real_assets`) plays it twice.

Each play goes to `build/test-output/<preset>/regress/run_NN/`: `checkpoints.txt`, the
program's `stdout.txt` and `stderr.txt`, and its data directory. These hold renders of game art
only when `MT_REGRESS_SHOTS=1` is set (`check_<name>.png`, for looking at). They are never
committed.

A play takes about 45 to 60 s of real time in the Debug build on an M-series Mac (78 s of
virtual time).

## What it plays

`tests/regress/title_to_combat.replay`, recorded from the scripted bot
(`play_real_test regress_record`, the tutorial's own steps in `tests/cpp/game_driver.h`): a
fresh install, the title (the main menu with its promo banner), B to close it and A on Play,
the intro (map 2.10, A through its speech boxes and MacArthur's dialog), the first map (2.8:
MacArthur's quests, a jump, a rope), the portal to 2.9, MacArthur's next quest, the ladder and
the first fight: the snail of quest 5, until it is defeated. Then 2 s more.

The file is our own data: Xbox 360 pad events at steps of the virtual clock (one run-loop turn,
1 ms; the game's 50 ms tick is 50 steps), named checkpoints, and the facts each must show:

| Checkpoint | Step | Expects |
|---|---|---|
| `title` | 1,501 | no hero yet (`map=-1.-1`) |
| `play` | 4,006 | (Play pressed) |
| `intro` | 4,008 | `map=2.10` |
| `first_map` | 33,557 | `map=2.8 panel=0 popup=-1`: the field, after the intro |
| `map_2_9` | 63,925 | `map=2.9` |
| `combat` | 76,424 | `map=2.9 kills>=1`: the snail defeated |
| `end` | 78,426 (the run ends) | `map=2.9 kills>=1` |

and a checkpoint every 1,000 steps (`s1000`, `s2000`, …).

## What it checks

A play (`play_real_test regress`) replays the pad events through the frontend's own input layer
(the virtual pad → Gamepads → Actions → the runtime, ADR 0048) with no bot: nothing reads the
game to decide what to press. At each checkpoint, at the end of its run-loop turn, it records:

- `frame`: FNV-1a 64 of the composited 480 × 320 screen (what `--shot` writes);
- `audio`: FNV-1a 64 of the mixer's output so far, exactly the bytes `--audio-capture` would
  write, and its frame count;
- `gl`: the GL frames the game presented;
- the state: the hero's map, position, HP, MP, level, EXP and meso, the key pad's panel, the
  game mode, the popup, the mobs defeated, the game's ticks.

It fails unless the replay ran to its end, every checkpoint was taken and every expect holds,
plus `play_real_test`'s checks for every mode: no trap, nothing unimplemented, no GL misuse, no
error line in the log.

`scripts/regress` (`python3 -m mtport.regress`) fails unless every play passed and every play's
`checkpoints.txt` has the same digest (FNV-1a 64 of the file). If two differ it names the first
checkpoint that differs and prints both lines. Its last line is `regress: digest <16 hex>`.

## Comparing two builds, or two presets

```sh
scripts/regress --preset original --save-baseline work/regress/original.txt   # on one build
scripts/regress --preset original --baseline work/regress/original.txt        # on the other
```

The second fails if any checkpoint differs from the baseline and names the first one. Baselines
hold hashes of our renders and stay in `work/` (never committed). The digests of two runs can
also be compared by eye: the last line of each.

Which settings reach the run: those the driver applies as the game does. These are the Assists
(`turbo_attack`, `auto_potion`, `auto_run`, `hold_toggle`, `auto_loot`), `skip_intros`,
`ui_animations`, `menu_repeat`, `confirm_guard` and `game_speed`. The window's presentation
(scale mode, render scale, filters, widescreen, the minimap, the HUD labels, the button prompts)
never reaches the 480 × 320 screen the hashes cover.

## Recording a new replay

```sh
SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy MAPLETHIEF_LANGUAGE=en \
  MT_REPLAY=tests/regress/title_to_combat.replay \
  build/mac-debug/tests/play_real_test regress_record build/test-output/mac-debug/regress_record
```

The bot plays the same way and writes the replay with its checkpoints and expects. The bot's own
run gives the same `checkpoints.txt` as every replay of it (the replay is exact). Record again
when a deliberate change moves the game's timing, for example a change in how the runtime runs
the guest that shifts its ticks. A rendering change only changes the hashes: save new baselines.
