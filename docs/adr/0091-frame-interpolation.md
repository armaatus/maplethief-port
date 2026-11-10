# 91. Frame interpolation: the game's draws replayed between two ticks, off in every preset

- **Status:** Accepted
- **Date:** 2026-11-02
- **Issue:** #87
- **Builds on:** [ADR 0039](0039-rendering-backend-software-rasterizer.md) and [ADR 0060](0060-gpu-rendering-backend.md) (mt_gfx and its GPU backend), [ADR 0068](0068-gpu-presentation-without-read-back.md) (GPU presentation), [ADR 0070](0070-settings-registry-and-presets.md) (the `frame_rate` row), [ADR 0071](0071-frame-telemetry-and-frame-time-overlay.md) (the telemetry it is measured with), [ADR 0075](0075-display-locked-frame-pacing.md) (the paced clock and its refresh grid), [ADR 0087](0087-game-speed-and-fast-forward.md) (game speed and fast-forward)

## Context

The game's logic is fixed-step at 50 ms, and it draws inside its tick: `CStateGame::Paint` `0x87501` runs the simulation and then paints, 1:1 ([timing-threading.md](../re/timing-threading.md)). So the original shows 20 pictures a second, and at 60 Hz each is held for three refreshes. Walking and the camera's scroll step 5 px at a time.

There is no point between two ticks where the engine could draw a state of its own. Splitting logic from paint would mean rewriting the game's state functions, and "present faster" alone only repeats frames. #87 is a spike: find out whether a smoother picture is possible without changing the game, and what it costs.

Fixed-step runs (goldens, shots, `play.*`) must stay byte-identical with the toggle off, and the game's speed must not change with it on.

## Options considered

1. **Interpolate the game's state** (positions in guest memory, rendered twice). The game has no paint-only entry point: `Paint` also steps the world. Calling it between ticks would advance the game. Rejected.
2. **Present more often** (each tick's frame at every refresh). Nothing moves between ticks. Rejected.
3. **Interpolate the draw lists in mt_gfx**, as Ship of Harkinian's frame interpolation does. Record each tick's draws, match them with the previous tick's, and at each refresh replay the tick with the matched draws' vertices blended between where they were and where they are. The game is untouched. The picture runs one tick behind. **Chosen.**

How to match draws (the game draws everything as quads in one 480 × 320 viewport):

- **By texture, texture coordinates, primitive, vertex count, viewport and blending (exact).** A tile or an NPC's frame keeps its key from tick to tick. Repeated tiles share a key, so the nearest one is chosen after the camera's scroll: a repeated tile 16 px apart from its neighbours is matched right even at a 6 px scroll.
- **The camera's scroll** is the commonest move, in quarter pixels, of the draws whose key is unique in both ticks. Standing still is a move too (the HUD). The scroll counts when at least 3 draws vote for it and it has at least a quarter as many votes as standing still.
- **Animation frames** change their texture coordinates every few ticks. A second, loose pass matches what is left by primitive, count and viewport (the same texture preferred) within 24 px of where it is predicted. It is moved by the blend of its centre. Its pose still changes at the tick.
- **The rest** (a mob that appears) moves with the camera's scroll, or is drawn as it is.
- **Snap** (the tick's own frame, no interpolation) when the ticks are of another target or size, more than 2.5 intervals apart (a load, a pause, the settings menu), or fewer than 30 % of the draws match exactly (a map change, a teleport).

## Decision

- **mt_gfx** gets a submit observer (`mt_gfx_set_submit_observer`), NULL by default: with `frame_rate = original` nothing is installed, and `mt_gfx_submit` does what it did.
- **`platform/src/frame_interp.{h,cpp}`** (no SDL): `DrawList` (a tick's draws, copied), `match_draws`, `blend_draw`, `frame_alpha`. `FrameInterpolator` records through the observer and keeps the draws to the presented renderbuffer at each GL present (`end_tick`). It replays the tick at a fraction into its own mt_gfx target (`render`), on the software rasterizer or the GPU backend, without recording its own replays. `MAPLETHIEF_TRACE=interp` logs each tick's matches and each present's fraction.
- **The fraction.** A present whose picture is seen at guest time *t* shows the last tick at `alpha = (t − the tick's guest time) / the tick interval`, so it follows `game_speed` and fast-forward (the interval and the rate are the guest clock's). Locked to the display, *t* is the refresh boundary the present is held to, and the tick's time is its wake's refresh. At 60 Hz the three refreshes of a tick show 1/3, 2/3 and 1. On the wall clock *t* is one refresh period on. In the fixed-step mode it is the next 60 Hz present. At 1 the window shows the tick's own frame, taken again from the game's renderbuffer.
- **The presents** (`game.cpp`). Locked, the turn hook makes one present a refresh until the guest's next timer: `present_smooth_locked` holds each to the first boundary its recent work fits before (the longest of the last 30, at most a refresh), and never to one past the timer. The tick's own first present follows the same rule instead of PresentHold's point, because every refresh now has a present and none can queue ahead of another. With GPU presentation the presents between ticks compose the screen from the last UIKit overlay (`GpuScreen::recompose`), unless a UIKit animation runs. On the wall clock (144 Hz, VRR, no vsync, fast-forward) a present is due one refresh period after the last. In the fixed-step mode one is due every 1/60 of a virtual second. UIKit's views stay at the ticks' rate.
- **The software rasterizer is not used for it in real time.** With `gfx_backend = software`, `frame_rate = smooth` logs that it needs `gpu` and shows the original frames. The fixed-step mode interpolates on either backend: it has no real time to keep, and its shots and tests use it.
- **The setting**: `frame_rate = original | smooth` (Video, `--frame-rate`), `Applies::kAtOnce` (the turn hook reads it from `g.settings`; a change starts or stops the recording). `original` in all three presets. A new render scale (`render_scale = auto`) makes the next tick snap.
- **The exit log** (smooth): the ticks interpolated and snapped (and why), the draws per tick by match, and the latency: the tick's first picture and its full picture after its GL present, and their difference.

## Evidence

- **CI** (`tests/cpp/frame_interp_test.cpp`, `platform.frame_interp: …`, synthetic): the camera's scroll and the HUD, repeated tiles matched to the predicted neighbour, an animation frame matched loosely and a new draw following the camera, a map change snapping, colours blending. On the software rasterizer: the replay at 1 is the tick's frame byte for byte and at 0.5 the quad is halfway; the replays are not recorded; a gap snaps; freed textures are skipped; stopping removes the observer. Locked at 60 Hz with random 1–9 ms works over 600 ticks: the tick's present at the first refresh with 1/3, then 2/3 and 1 at the next two, none past the next tick's wake.
- **The tick count, fixed-step** (`platform.frame_rate`, real assets, local): the same 60 s run (`--fixed-step 16.667`, the save loaded by taps, the key pad's left arm held 8 s then its right 8 s) with `original` and `smooth` has **1253 ticks either way and the same audio capture, byte for byte**. Smooth interpolated 1197 of the 1253 ticks and presented 3650 window frames against 1348. The 56 snaps are the first tick and the loading screen's frames (same guest time) before the map.
- **What matched, on the first map** (`MAPLETHIEF_TRACE=interp`, the same walk): while the camera scrolls (5 px a tick when walking, 19 px easing down at the start), each tick has about 51–65 exact matches, 8–16 loose ones (the hero's and the NPCs' animation frames, the portal), 0–4 that follow the camera and none drawn as they are. Over the 60 s, per interpolated tick: 140.8 exact, 2.6 loose, 0.1 with the camera, 0.0 as drawn.
- **A real window** (Release arm64, Apple M4, Metal, 60 Hz, `--no-audio`, the machine shared with other builds at load 6–10): 40 s runs, the same walk, back to back. The map window is 13–38 s, from `--perf-csv`.

  | Backend | `frame_rate` | Ticks in the window | Ticks over 55 ms | Window frames | Held 1 refresh | Ticks in the run |
  |---|---|---|---|---|---|---|
  | GPU 1× | smooth | 500 (20.000 a s) | 0 | 1500 | **100.0 %** | 844 |
  | GPU 1× | original | 500 (20.000 a s) | 0 | 503 | 0.6 % | 844 |
  | GPU 4× | smooth | 500 (20.000 a s) | 0 | 1500 | **100.0 %** | 835 |
  | GPU 4× | original | 499 (19.960 a s) | 1 | 499 | 0 % | 841 |
  | GPU 1×, `pacing = wall` | smooth | 498 (19.920 a s) | 0 | 1495 | 94.8 % | 839 |
  | GPU 1×, `pacing = wall` | original | 496 (19.840 a s) | 1 | 499 | 0.6 % | 837 |
  | software 1× (before the fallback) | smooth | 492 (19.680 a s) | 113 | 988 | 56.8 % | 832 |
  | software 1× | original | 500 (20.000 a s) | 87 | 500 | 0 % | 845 |

  GPU 4× smooth lost its six ticks on the title screen (0.2–4.4 s). UIKit animations run there, so each present composes UIKit on the CPU at 4× (about 8 ms). On the map none ran late.
- **The latency** (the exit line, the same GPU 1× run): the tick's first picture **15.77 ms** after its GL present (as with `original`: the first refresh boundary), its full picture **49.34 ms** (max 49.94 ms). Smooth **adds 33.6 ms**, two refreshes, (n − 1)/n of a tick at 60 Hz. On the wall clock: first 1.86 ms, full 36.37 ms, +34.5 ms. GPU 4×: +28.7 ms (its first picture was later, 20.6 ms).
- **Artefacts on the first map** (fixed-step shots with `--shot-gl`, local only, looked at side by side and zoomed; not committed):
  - Positions and the scroll move every refresh. A sprite's pose still changes only at the tick (a loose match moves the new pose from where the old one was).
  - A draw that appears in a tick (a mob, an effect) appears at the tick's refresh and moves with the camera.
  - The replay draws over opaque black, not over the frame before as the game did. The game's first layer lets about 1/255 of what is under it through, so the in-between pictures' colours can be 1 off: the sky reads (136, 168, 254) against (137, 169, 255). The tick's own picture (at 1) is the game's, exact.
  - Between ticks the tiles stand at fractional positions, filtered by the game's own filters. There are no seams (adjacent tiles move together and share edges under the fill rule).
  - A texture uploaded twice within one tick would replay with its last image for both draws. This was not seen on the first map. A texture freed since the tick is skipped (0 times in these runs).
  - Map changes, teleports and loads snap: one tick's frame, then interpolation again.
- **Off is the original** (local, real assets): `platform.device_golden` and the fixed-step tests pass with the default (`original`), whose path installs no observer and makes no extra present.

## Verdict

On the GPU backend it is good. Every refresh gets a new picture (100 % of window frames held one refresh at 1× and 4×), the game's speed and tick count are unchanged, and no tick runs late on the map. The price is 33 ms more latency at 60 Hz (two refreshes), poses and UIKit still at 20 Hz, and in-between colours up to 1 off.

On the software rasterizer it is not good. Every refresh redraws the whole frame (5–9 ms at 1×, four times that at 2×), which a busy machine cannot keep up with. Only 57 % of refreshes got a new picture, ticks ran late, and the game lost 1.6 % of its speed on the map. So real time with `gfx_backend = software` falls back to the original frames.

`frame_rate` stays `original` in every preset. Enhanced draws on the software rasterizer (`gfx_backend = software`, `render_scale = auto`), where smooth would do nothing. The extra latency also changes the feel of an action game that the Original columns keep. Smooth is an opt-in for players who also choose `gfx_backend = gpu`.

## Consequences

- With `frame_rate = original` nothing changes: no observer, no extra present, the same frames and goldens.
- With smooth on the GPU, the window presents at every refresh (about three times as many presents). PresentHold's point is not used for the ticks then. Each present goes to the first refresh boundary its recent work fits before.
- A machine starved of CPU still breaks the cadence (a present misses its refresh and the picture is held two). Ticks are protected: no present is held past the guest's next timer.
- At 144 Hz, VRR and with no vsync the paced clock is on the wall clock (ADR 0075), and the presents follow the refresh period without knowing the display's phase (94.8 % held one refresh at 60 Hz on the wall clock). Real vblank times (`CAMetalDisplayLink`, DXGI frame statistics) would fix that, as ADR 0075 notes.
- Windows (D3D12), Linux (Vulkan), real 120/144 Hz displays and the feel of the added latency are judged by hand (`needs-human`, #87).
- The engine's poses could be made smoother only by changing the game (more animation frames). That is out of scope.

## Addendum: with #86's widescreen and #158's colour-blind filter (2026-11-02)

- **Widescreen.** With `widescreen = auto` the GL layer is 568 points, the engine's view width changes per map and turn, the field's backdrop is stretched by a host x transform (`DrawImage` `0x24DC9`), and the Presenter draws the GL frame's wings beside the screen image ([ADR 0090](0090-widescreen.md)). The interpolator's replay has not been checked against any of that, so `frame_rate = smooth` falls back to the original frames for a run whose GL layer is wide (`smooth_wide_blocked` in `game.cpp`, logged once: "frame_rate = smooth is not used with widescreen = auto"), as it does on the software rasterizer in real time. Making the two work together is a follow-up. (Done: see the addendum on #177 below.)
- **The colour-blind filter.** An interpolated frame is taken into the screen like the tick's own (`take_gl_frame`) and presented through the same `present()`, so it goes through every Presenter step: the grade (step 0, [ADR 0093](0093-accessibility-colour-filters-screen-shake-and-text-height.md)), the border and the picture. `frame_rate = original` adds no step and leaves every path as before.

## Addendum: smooth frames over the wide view (#177, 2026-11-10)

The first addendum above left `frame_rate = smooth` off for a run whose GL layer is wide. `smooth_wide_blocked` is gone: smooth now interpolates with `widescreen = auto`.

**Why the replay needs no wide path of its own.** A recorded draw's vertices are clip space after `fixed.cpp`, with the backdrop's stretch, the HUD's move and the centred screens' offset (`mt_gles_set_host_x_transform`, [ADR 0090](0090-widescreen.md)) already in them. The interpolator's target has the tick's size, so 568 points wide. An interpolated frame is taken like the tick's own (`take_gl_frame`) and goes through the same `present()`: the centre 480 points into the screen image (`gl_screen_columns`), the first E points into the wings, the wide picture through the grade and the pass (#170).

**What had to be added is refusing to blend where widescreen moved.**

- `FrameInterpolator::end_tick` takes the view width E the tick was drawn at (`mt_widescreen_view()` at the GL present). A tick at another E snaps.
- Each recorded draw keeps the host x transform it was drawn under (read in the submit observer, which runs inside the guest's draw). The transform is part of the draw's shape and key, so a draw never matches one under another transform. A tick whose set of transforms differs from the last tick's snaps too: the popup menu's paint beginning or ending. The exit line counts these as "at a view change".
- E can also change after the tick: the turn hook applies the next E before the guest's frame. A present between that turn and the next GL present would show the tick at the new E's columns, wings and key pad groups. `smooth_frame` compares `mt_widescreen_view()` with the tick's E and on a difference keeps the tick's own frame, as for a failed take. `read_gpu_gl` hands the Screen the GPU frame's own view, not the current one, so the end check agrees with what was shown.
- Between ticks the key pad's wing strips (#168) stay as composited last, like UIKit's screen overlay (`recompose`), under the same conditions (no UIKit animation, no key pad fade). So the three times as many presents do not composite panel 0 on the CPU.

**Evidence.**

- CI (synthetic, `platform.frame_interp`): `the wide frame replays over its 568 points, in the wings too (#177)` (alpha 1 is the tick's frame byte for byte, alpha 0.5 halfway at column 510) and `a tick at another view width or host x transform snaps (#177)` (E changes, the popup opens and closes, `forget`, and a draw not matching across a transform).
- Real assets (local, Debug arm64, Apple M4, Metal, 60 Hz, `--gfx-backend gpu --widescreen auto --frame-rate smooth --no-audio --window 852x480`, from play.save_quit's save):

  | Run | Window frames | Ticks | Interpolated | With a camera scroll | Snapped (at a view change) | Interpolated frames drawn | Read-back | GPU against the CPU's composite |
  |---|---|---|---|---|---|---|---|---|
  | key pad at the edges (the default), standing | 2400 | 838 | 809 | 0 | 29 (2) | 1588 | 0 mismatched | 0 pixels beyond ±2 |
  | `--widescreen-hud narrow`, the hero walks; the last frame during the walk | 1520 | 547 | 518 | 110 | 29 (2) | 1002 | 0 mismatched | 0 pixels beyond ±2 |

  The read-back covers the whole 568-point wide picture and its bars. The other snaps are the load's gaps and the first tick. The walking run's window frames 1518 to 1520 differ in 3532 to 4492 of the 14,080 pixels of each wing and in about 54,000 of the screen image's (captures local, not committed): the wings move at every refresh with the picture. At `--window 852x480` a point is 1 pixel.
- `platform.frame_rate`, `play.widescreen_on` and `play.widescreen_off` pass (real assets, local). Nothing changes with `frame_rate = original`: no observer, and `set_pad_wings` runs every present as before.

**What is unchanged.** The latency (a tick behind), the poses and UIKit at 20 Hz. Software in real time still falls back to the original frames. The first frame after a view or popup change shows the tick itself, unblended, one tick. Judging it on a real 16:9 screen by hand stays with #87's `needs-human` list.
