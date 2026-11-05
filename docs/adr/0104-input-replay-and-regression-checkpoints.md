# 104. Input replay and regression checkpoints

- **Status:** Accepted
- **Date:** 2026-11-05
- **Issue:** #91 (pulled into epic #201: #190's save states compare a restored run with an uninterrupted one through it)
- **Builds on:** [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (the virtual clock, the fixed-step mode), [ADR 0045](0045-audio-mixer-and-output.md) (the audio capture), [ADR 0048](0048-gamepad-actions-and-delivery.md) (gamepads → actions), the game driver of the real-asset play tests (`tests/cpp/game_driver.h`, #69, #76, #127)

## Context

#91 asks for a deterministic local test that boots the game, plays a recorded input sequence and checks key states and frames: input recording and replay with timestamps in game time, a deterministic mode, checkpoints (frame hashes) at chosen times, and a `scripts/regress` runner. Its acceptance criterion: a replay from the title to the first map and a fight passes 10 times in a row. Epic #201 also needs two more things from it:

- **A comparison between builds and settings.** "The Original preset plays exactly like the baseline" means a stable digest of a run that two builds compare, for a chosen preset (`MAPLETHIEF_SETTINGS`).
- **An API for #190's save states.** "Run N fixed steps from a state, give me frame hashes and an audio hash", so a restored run can be compared with an uninterrupted one.

What exists already:

- The **fixed-step mode** (ADR 0044) makes a run deterministic. One virtual clock is behind every clock the guest reads, it advances one step per run-loop turn, and the game's random seed is the clock at boot. ADR 0044 showed scripted runs identical across runs, Debug and Release, arm64 and x86_64.
- The **game driver** boots the real game in-process on that clock. It drives the frontend's own input layer with an SDL virtual Xbox 360 pad (Gamepads → Actions → Focus / MenuInput → the runtime). Its bot (reactive tasks that read guest memory) already plays the title, the intro and the tutorial to the snail of quest 5 (`then_tutorial`, `play.first_map`).
- **`--shot` / `Screen::compose`** gives the composited 480 × 320 screen. **`AudioCapture`** renders the mixer up to the virtual time into 16-bit samples, the same bytes every run.

## Options considered

**What a replay records.**

1. **SDL events in `MapleThief`** (`--record` / `--replay` in the frontend): keys, mouse and pad events at their turn. Pro: the shipped binary, any device. Con: nobody can record by hand in the fixed-step mode, which is not paced: it runs as fast as the host can. A recording in real time does not replay, because the guest reads real time. The committed replay would have to be written by hand, timed to the millisecond through a tutorial of about 80 s.
2. **The driver's virtual pad, recorded from the bot**: every button and axis change the bot makes, with its turn, and played back through the same pad and the same input layer. Pro: the bot already reaches the fight. A replay of its recording is exact by construction (the same pipeline, the same order inside a turn), and nothing reads the game to decide what to press. Con: the replay plays in the driver, not in `MapleThief`. The driver honours the settings that reach the game (below); the window's presentation does not reach the 480 × 320 screen anyway.
3. **The runtime's actions** (`mt_input_press`): below Focus and MenuInput, so the menus' focus and the confirm guard would not be replayed.

**What a checkpoint holds.** Shots as PNGs (golden images) are game art and large: they live in `work/` and are compared pixel by pixel. A **hash** of the composited screen, of the audio and of a few state facts is small and exact. It names *where* two runs part, which is all a regression or a save state needs. A shot is still there for looking (`MT_REGRESS_SHOTS=1`).

**Where the format and the hashes live.** In the frontend library (`platform/src/replay.{h,cpp}`, `AudioHash` in `audio_out`), with no SDL and no guest in them: unit-tested in CI, usable by the driver now and by `MapleThief` or a save-state tool later.

## Decision

- **The replay file** (`replay.h`, our own data). `maplethief-replay 1`, `step_us`, `every N` (a checkpoint every N steps), `data empty` (a fresh install). Then one event per line at a step (run-loop turns from the first; at 1 ms a step the game's 50 ms tick is 50 steps): `pad attach xbox360`, `pad button <SDL name> down|up`, `pad axis <SDL name> <raw>`, `check NAME`, `end`. Last come `expect NAME key=value|key>=N|key<=N` lines on a checkpoint's state. Events stay in step order (the order inside a step is kept). Every malformed line is refused with its number (the file is untrusted). `ReplayCursor` hands out the input events due at a step.
- **Recording and playback** (`game_driver.h`). `virtual_pad::Pad::on_change` reports every button and axis set. With `Run::record` set, the attach and each change go into the replay at the turn's step. With `Run::replay` set, `play_replay` runs between the timed steps and the tasks, where a recording run changes the pad. It plays the attach then pumps, plays the buttons and axes, then pumps once the pad is there, as `run_tasks` does every turn. The bot's own run and every replay of it therefore give the same checkpoints, byte for byte.
- **Checkpoints** (`replay.h`). `CheckpointPlan` lists the steps that take one: the replay's checks, every N steps, or any a caller adds. At the end of such a turn, `take_checkpoints` records:
  - `frame`: `frame_hash`, FNV-1a 64 of the width, the height and the pixels of `Screen::compose`'s screen;
  - `audio`: `AudioHash`, which renders the mixer after every turn exactly as `--audio-capture` does (the same frames and the same 16-bit bytes, so its hash is the hash of the capture's data chunk), and its frame count;
  - `gl`: the GL frame count;
  - the state line: map, position, HP, MP, level, EXP, meso, panel, game mode, popup, mobs defeated, ticks.
  `CheckpointLog::text()` is a line per checkpoint. Its `digest()` is FNV-1a 64 of that text as 16 hex digits. `compare_logs` names the first checkpoint that differs.
- **The modes** (`play_real_test`). `regress_record` has the bot play the title, Play, the intro, the first map and the tutorial to the snail (`then_tutorial_to_snail`, split out of `then_tutorial`, which is unchanged) and writes the replay. `regress` plays `MT_REPLAY` back, writes `checkpoints.txt` and prints its digest. Both apply `MAPLETHIEF_SETTINGS` as the driver does (the Assists, `skip_intros`, `ui_animations`, `auto_loot`), plus `menu_repeat`, `confirm_guard` and `game_speed` as `game.cpp` applies them.
- **The committed replay** `tests/regress/title_to_combat.replay`: 163 events, checkpoints `title`, `play`, `intro`, `first_map` (2.8, the field), `map_2_9`, `combat` (the snail defeated: `kills>=1`) and `end`, and one every 1,000 steps, 85 in all. It runs 78.4 virtual seconds.
- **The runner** `scripts/regress` (`scripts/regress.ps1`; `python3 -m mtport.regress`). It builds `play_real_test`, plays the replay `--repeat` times in fresh directories and fails unless every play passed and all gave one digest. It checks that the program's printed digest is its log's, and names the first differing checkpoint. `--preset` / `--settings` set `MAPLETHIEF_SETTINGS`. `--save-baseline` / `--baseline` keep and compare a run's checkpoints (in `work/regress/`: hashes of our renders, never committed). The last line is `regress: digest <hex>`. CTest `play.regress` (real assets) plays it twice.
- **For #190.** A save-state tool restores a state, sets a `CheckpointPlan` (`set_every(K)`, `set_end(N)` or named steps), runs the turns with `take_checkpoints` and an `AudioHash`, and compares the resulting `CheckpointLog` with an uninterrupted run's through `compare_logs` or the digest. The step is the turn count, so a restored run counts from its own first turn. `add` names the steps to compare.

## Evidence

- **The acceptance criterion** (local, real assets, Debug build, Apple M4, 2026-11-05): `scripts/regress --repeat 10`: 10 of 10 runs passed, every expect held (`first_map`: map 2.8 in the field; `combat`: map 2.9, 1 kill), one digest over the 10 runs (`8a2f05733c8d4603`), about 45 to 60 s each with three other builds running.
- **The replay is exact.** The bot's recording run (`regress_record`) and the replay give the same `checkpoints.txt`, 85 lines, digest `8a2f05733c8d4603`.
- **CI (synthetic).** `platform_test` "replay: …": the file's round trip, 27 malformed lines refused with their line, SDL's pad names, the cursor, the frame hash and FNV-1a's reference values, checkpoint lines, digests and the first difference, the plan, the expects. "audio_out: AudioHash hashes exactly the bytes the capture writes". `tests/test_regress.py`: the digest (the same reference values), first differences, the settings list, the committed replay's shape, and whole runs against a stand-in program (repeats, a run that differs, a shorter log, failures, a digest that is not its log's, skips, the settings reaching the run, baselines).

## Consequences

- Any change that moves a frame, a sample or a fact of the first 78 s of a new game fails `play.regress` on the second play (non-determinism) or against a baseline (a regression), with the first checkpoint that differs. A deliberate rendering change only moves hashes: save new baselines. A change in the game's timing (how the runtime runs the guest) can make the replay diverge, so that its expects fail: record it again with `regress_record`.
- The replay runs in the driver, not in `MapleThief`. The frontend's own turn hook (`game.cpp`) is not covered by it. `platform.device_golden` covers that path. `--replay` in `MapleThief` stays possible with the same file.
- The state facts come from the driver's guest readers (`guest_state.h`), so they stay test code.
- How we'd know this was wrong: a replay that passes while the game visibly broke (then the checkpoints are too sparse or the facts too few: add checks); a digest that differs between two plays of the same build (something outside the virtual clock reaches the guest, as ADR 0044 warns); or a recorded replay that does not reproduce its recording (then the order of input inside a turn differs between `run_tasks` and `play_replay`).
