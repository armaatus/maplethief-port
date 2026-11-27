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
scripts/regress --native-script-vm shadow  # each script call run as the original and the host VM
```

On Windows: `scripts\regress.ps1 --repeat 10` from a Visual Studio x64 developer shell.
CTest `play.regress` (labels `cpp;real_assets`) plays it twice.

Each play goes to `build/test-output/<preset>/regress/run_NN/`: `checkpoints.txt`, the
program's `stdout.txt` and `stderr.txt`, and its data directory. These hold renders of game art
only when `MT_REGRESS_SHOTS=1` is set (`check_<name>.png`, for looking at). They are never
committed.

A play takes about 60 s of real time in the Debug build on an M-series Mac (78 s of
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

`--native-script-vm on|off|shadow` sets `MAPLETHIEF_NATIVE_SCRIPT_VM` for the plays, which the
driver applies as the game's `--native-script-vm` ([ADR 0226](../adr/0226-the-host-script-vm.md),
#386, #430): the script VM's loop on the host, the recompiled original (the default), or the
shadow runs, which run each outermost `Lua::execute` both ways from one state and fail the play
on any difference (`maplethief/scriptvm.h`). A shadow play's summary line (`scriptvm shadow: N
runs, M native calls replayed, 0 with a difference`) is repeated in the runner's output; each
difference is written to the play's `stderr.txt` with the address, both words and the host VM's
last opcodes. `on`, `off` and `shadow` give the same digest (`8a2f05733c8d4603`, 2026-11-27).
The playthrough below takes the same variable from the environment
(`MAPLETHIEF_NATIVE_SCRIPT_VM=shadow scripts/playthrough`).

Which settings reach the run: those the driver applies as the game does. These are the Assists
(`turbo_attack`, `auto_potion`, `auto_run`, `hold_toggle`, `auto_loot`), `skip_intros`,
`ui_animations`, `menu_repeat`, `confirm_guard` and `game_speed`. The window's presentation
(scale mode, render scale, filters, the picture adjustments, widescreen, the minimap, the HUD
labels, the EXP percentage, the button prompts, the gamepad pointer's drawing) never reaches the
480 × 320 screen the hashes cover. The driver does not apply `flashes` (game.cpp installs its
hooks), `audio_mono` or the volumes (AudioOut sets them on the mixer), so a run with them gives
the defaults' digest too; `pause_on_disconnect` needs a pad to disconnect and `save_states` its
F5 / F9, which the replay never does. With `--preset enhanced` and epic #201's settings on (the
list in [re/qol.md](../re/qol.md)'s suite table), the digest is the Original's (`8a2f05733c8d4603`, 2026-11-05).

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

## The playthrough: the whole chain as a regression suite

`scripts/playthrough` ([#306](https://github.com/armaatus/maplethief-port/issues/306)) turns the
save-chained whole-game runs into a regression suite: one command plays them and tells whether a
build behaves as the last one did. The chain is CTest's, as the chapter pages describe it:
[chapter 1](chapter-1-quests.md)'s parts `play.chapter_1_a` to `_m`,
[chapter 2](chapter-2-quests.md)'s main parts `play.chapter_2_a` to `_f` (from `starts.chapter_2`,
with the raised starts their pages explain) and [chapter 3](chapter-3-quests.md)'s parts, the
ending watched and skipped and the post-game's three processes. Chapter 2's side quests, crafts
and shops branch off the chain and are not in it. Each part goes on from the save of the part
before it; CTest's fixtures order them and bring a part's own start states along.

```sh
scripts/playthrough                                         # builds play_real_test (mac-release), plays the chain once
scripts/playthrough --quick                                 # the first parts only (under 15 minutes)
scripts/playthrough --save-baseline work/regress/chain.txt  # on one build
scripts/playthrough --baseline work/regress/chain.txt       # on the other: names the first difference
scripts/playthrough --quick --baseline work/regress/chain.txt  # a quick run against a full baseline
scripts/playthrough --repeat 2                              # twice: one digest or a failure
```

On Windows: `scripts\playthrough.ps1 --quick` from a Visual Studio x64 developer shell (the
`win-release` preset). It plays in Release (`mac-release`: one architecture, the host's;
`linux-release` on Linux), several times faster than Debug; `--build-preset mac-debug` plays the
Debug build. `--tests REGEX` picks other parts (a CTest regex).

Each part runs with `MT_CHECKPOINTS=60000` (`--every`): `play_real_test` then hashes the audio
from the part's start and takes this page's checkpoints (the screen, the audio, the GL frames and
the state line) every virtual minute and one named `end` at the end of the turn the part is done
in. It writes them to its output directory as `checkpoints_<mode>.txt` and prints their digest.
The runner prints each part's status, real time and digest:

```text
run 1/1:
  PASS    play.chapter_1_a                            …  s  <16 hex>
  …
run 1/1: N of N parts passed in … s, digest <16 hex>
playthrough: digest <16 hex>
```

The chain's log (`build/test-output/<preset>/playthrough/run_NN/chain.txt`) is every part's
checkpoint lines, each prefixed by its test's name; the last line's digest is FNV-1a 64 of it.
`--baseline` compares each part this run played with the baseline's, in order, and names the
first checkpoint that differs (the part, the checkpoint, both lines); a part the baseline does not
have fails, and the baseline's parts this run did not play are left out, so a quick run compares
with a full baseline. `--save-baseline` saves the first run's log, and only when all its parts
passed. Baselines stay in `work/regress/`, never committed.

`--nudge-tick STEP` (`MT_NUDGE_TICK`) is the deliberate change for checking the comparison: at
that step of each part the virtual clock moves one game tick (50 ms) more. A run with it must
fail against a baseline without it.

### Times

Measured on 2026-11-27 on the Mac (Apple silicon, `mac-release`, `--no-build`), with four other
agents' builds and tests running on the same machine, so a quiet machine is faster:

| Run | Parts | Time |
| --- | --- | --- |
| Full (`scripts/playthrough`) | 28 | 3778 s (63 min) |
| Chapter 1, a to m | 13 | 2518 s; d 863 s, i 476 s, f 342 s, b 242 s |
| Chapter 2, a to f | 6 | 918 s |
| Chapter 3 and the postgame | 9 | 342 s |
| Quick (`--quick`, chapter 1 a to c) | 3 | 427 s and 485 s (7 to 8 min) |

The quick time includes `play.bot_world`, which CTest runs first as a fixture of part a. The build
(`play_real_test` in Release) is not counted.
