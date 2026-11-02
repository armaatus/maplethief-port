# 96. Scene skip: every frame of a scene, faster, with its speech boxes tapped

- **Status:** Accepted
- **Date:** 2026-11-02
- **Issue:** #174
- **Builds on:** [ADR 0087](0087-game-speed-and-fast-forward.md) (the guest clock's rate and its present gate), [ADR 0092](0092-quality-of-life-assists-and-ui-animations.md) (frontend presses of the game's actions, `MAPLETHIEF_SETTINGS`), [ADR 0070](0070-settings-registry-and-presets.md) (the registry; `skip_intros` was a planned key), [ADR 0069](0069-guest-function-hooks.md) (why no hook is added), [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (the fixed-step clock)

## Context

#90 left the cutscene skip as a high-risk follow-up. A scene is a `CAniManager::makeAni*` command list run by the scene runner `0x83C71`, and its commands set quest flags, move the hero, create and remove NPCs, start bosses and change maps. [docs/re/qol.md](../re/qol.md#cutscene-skip) classifies the runner's 64 commands, and the per-scene table there lists each scene's state steps. Three facts decide the design:

- The runner runs **one step per `CStateGame::Paint`** (`Scene_NextStep` `0x26A11`) while no speech box waits.
- **Between steps, `Paint` moves the hero, the NPCs and the scene's sprites by their speeds every frame.** The hero's place after `makeAniChapter1Perion` is 38 frames × speed 3 from where command 5 put it.
- **Only a key closes a speech box** (`CStateGame::OnKeyPressed`: two lines a press, then `+0x1E9` cleared).

## Options considered

1. **Jump to the scene's end command** (or run the end command at once). This loses the frames: the hero ends up 114–129 points short in four scenes that end in the field. It also skips the script runs (54) and boss placings (60) before the end. Rejected.
2. **Run all the remaining steps in one frame**, from a wrapper on the runner (`mt_hook_wrap`, next to #158's handler). Every command runs, but the walks between them take no frames: same result as 1. Rejected.
3. **Run only the state commands, drop the picture and sound commands.** The frames stay, so nothing is gained but the boxes' waits, and the scene's music commands would no longer leave the music they leave. Rejected.
4. **Run every frame, faster, and tap each box.** **Chosen.** The game runs the same code on the same ticks. Only the host's wait between ticks shrinks, and the boxes get the key a player would give.

**How faster.** Write a shorter tick into CMTimer's interval (as `game_speed`), or raise the guest clock's rate (as fast-forward)?

- **The interval:** the game would see a different tick length. Its real-time timers (the sub-timers, `GetTimeMs` users) would see less game time per tick than in a watched scene.
- **The rate (chosen):** `GetTimeMs` and the sub-timers advance by the game's own interval per tick, exactly as watched. The present gate, the wall plan × rate and the scaled waits of ADR 0087 already exist.

**How the box is tapped.** By writing `+0x1E9` and `+0x4C8` from the host (a copy of `OnKeyPressed`'s branch), or by pressing OK through `mt_input_press` (ADR 0092's way)?

- **Writing the fields** would bypass the game's key path.
- **The press (chosen)** goes through SkyCanvas's slots and `OnKeyPressed`, as the cut-scene panel's button does: a skipped scene is one a player could have watched. No guest memory is written, and no hook or regenerated code is needed.

## Decision

`skip_intros = off | on` (Gameplay, off in every preset, `--skip-intros`, applies at once, the settings menu's "Skip scenes"). `platform/src/scene_skip.{h,cpp}`'s `SceneSkip` runs every turn, in the game's turn hook and in the test driver. While the engine's `CStateGame` (vtable `0x119B78`) is in game mode 9 with no popup:

- `rate()` is `kSceneSkipRate` (8). `set_fast_forward` takes the larger of it and the fast-forward key's rate, so the paced clock gets "the wall clock ×8" and the present gate shows at most one frame a refresh. The real-time clocks only: the fixed-step clock has no rate (ADR 0087).
- With a speech box up (`CStateGame +0x1E9`), the key pad on panel 3 and nothing queued, OK is pressed, then released 2 game ticks later. The next tap waits until the release has been delivered.

The skip logs each scene: its start step, its ticks, its taps and its wall time.

## Consequences

- **The state after a scene is the one a watched scene leaves.** The only difference is the frames each box waits, which in the original depend on the player's taps too. They move only scene NPCs that the scene removes, or that a map change drops (qol.md). Measured on real assets: the intro (64 fields after the map load and after MacArthur's dialog) and Balog's entrance (32 fields, Balrog's attack countdown included) are equal skipped and watched, and the game's `rand()` state is the same.
- **Wall time.** The intro's 444 steps (447 ticks) took 4.2 s in a real-time run on the dummy video driver, where software GL costs about 11 ms a frame. Watched, the same steps take 22.4 s plus the time the boxes wait. A faster renderer gets closer to ×8.
- **Audio.** The scene's music commands still run in order; the music itself plays at its own speed. Vibrations become rumble ([ADR 0053](0053-observed-functions-game-events-and-rumble.md)) and fire as in the scene, closer together.
- **The fixed-step tests see only the taps.** `ctest -L real_assets` with `MAPLETHIEF_SETTINGS="skip_intros = on"` exercises them on every flow that plays a scene.
- **Nothing changes with the setting off.** `SceneSkip::turn` returns at once, and the rate is 1.

## Evidence

- `platform_test`, `scene_skip:` cases: the view (game mode 9 in `CStateGame`, the box byte above `+0x1E8`, the step, no scene in modes 0, 8, 10, in a popup or another state); the rate only while skipping; a tap of OK released after 2 ticks, none in the release's turn, none off panel 3, while queued, without a box or with the setting off; a held tap released after the scene; the row.
- `tests/test_scenes.py`: the classification covers the switch's 64 numbers; the table and checks on synthetic commands; the names file; with the slice, every builder's commands classified, one end on each scene's last step, and qol.md's generated block up to date.
- `play.skip_intros_{watched,skipped}`, `play.skip_intros_boss_{watched,skipped}` (real assets, local, Release arm64): see [qol.md, Verification](../re/qol.md#verification).
