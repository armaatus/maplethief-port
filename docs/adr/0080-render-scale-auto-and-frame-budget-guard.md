# 80. render_scale = auto, changed while the game runs, and a frame-budget guard

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #152
- **Builds on:** [ADR 0039](0039-rendering-backend-software-rasterizer.md) (the render scale, mt_gfx targets), [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (`render_scale`, the placement), [ADR 0060](0060-gpu-rendering-backend.md) (the GPU backend, still not the default), [ADR 0068](0068-gpu-presentation-without-read-back.md) (GPU presentation), [ADR 0070](0070-settings-registry-and-presets.md) (the registry, Enhanced's `auto`), [ADR 0071](0071-frame-telemetry-and-frame-time-overlay.md) (`FrameStats::recent()`, the overlay), ADR 0037's addendum (#134's compositor at the render scale)

## Context

The render scale N draws the game's GL layer at 480N × 320N, and the compositor draws UIKit's views at that size. Both costs grow with N². Nothing stopped a high N from slowing the game:

- Research on 2026-10-30 (before #134): software 4x ran at 10 GL frames a second; GPU 4x kept 20 but spent 27 ms of the 50 ms tick.
- After #134 the compositor is cheap (4x: 3.7 ms GPU, 5.0 ms software). The software rasterizer is not. The real-time fresh run (the device golden list's taps, Release, Apple M4, other builds running, load 5 to 11) measured per window frame:

  | backend, N | ticks a second | mt_gfx | compositor | frontend (take + compose + draw) |
  |---|---|---|---|---|
  | software 1x | 20.36 | 3.68 ms | 0.55 ms | 0.63 ms |
  | software 2x | 20.36 | 11.16 ms | 1.23 ms | 1.50 ms |
  | software 4x | **18.99** | **41.00 ms** | 3.79 ms | 4.90 ms |
  | software 6x | **8.74** | 88.33 ms | 10.97 ms | 13.5 ms |
  | GPU 1x | 20.45 | 0.02 ms | 0.44 ms | 0.47 ms |
  | GPU 4x | 20.19 | 0.01 ms | 4.43 ms | 4.46 ms |
  | GPU 8x | **19.67** | 0.02 ms | 9.35 ms | 9.39 ms |

  The issue's budget, "the frontend under 10 ms per frame", alone would let software climb to 4x and lose the tick rate: on the software backend the cost that grows is mt_gfx's, which runs inside the guest's GL calls, not in the frontend.
- The render targets are made when the game gives its renderbuffer storage, once at launch (`-renderbufferStorage:fromDrawable:`, at `mt_gles_config.render_scale`). The frontend's `Screen`, the Presenter's texture and `GpuScreen`'s textures are sized from N once.
- A useful N also depends on the window: an image of 1440 × 960 pixels (1080p, integer 3x) gains nothing from 4x.

## Options considered

**When N can change.**

1. **At start only, from a short probe or a per-machine cache file.** A probe at boot measures something other than the game (or the first seconds of the game at a guess), and a cache file is state to invalidate (a new machine, a new build, a GPU driver). A resize could only take effect at the next start.
2. **While the game runs.** Every GL draw looks its target up through the renderbuffer (`State::renderbuffers`), so the runtime can swap a renderbuffer's target between two guest calls. The frontend's images are its own. **Chosen.** No restart, no probe, no cache file.

**What the budget counts.**

1. **The frontend only** (take + compose + draw ≤ 10 ms), as the issue says. Software would reach 4x (frontend 4.9 ms) at 19 ticks a second. Rejected alone.
2. **The tick rate** (step down below 19.5 ticks a second). The symptom itself, but noisy: a map load, another process or a single spike moves it, and it says nothing about how much headroom is left before climbing.
3. **The frontend (≤ 10 ms) and the frontend plus mt_gfx (≤ 20 ms, 40% of the 50 ms tick), as means per window frame over the last second.** Both are the host's own work at the render scale, measured every frame (#137). **Chosen.**

**How to find the largest N.** Measuring every N is a visible resolution change each time. The pixel work grows with N², so the cost at N + 1 is predicted as the cost at N × ((N + 1) / N)². That overestimates (part of the cost does not grow), so it errs on the low side. Climb one step at a time on that prediction; drop on a measurement.

## Decision

- **`render_scale = auto`** (`settings.ini`, `--render-scale auto`): the `render_scale` row's value type is `uint_range(1, 8)` with a name for 0, `auto` (`SettingValue::zero`; `Settings::render_scale == kRenderScaleAuto`). The menu's choices are `auto, 1, …, 8`. Columns: Original **1**, Original + controller **1**, Enhanced **auto** (ADR 0070's table). A bad value reads "is not auto or an integer from 1 to 8".
- **The runtime: `mt_gles_set_render_scale(N)`** (`maplethief/gles.h`) remakes each renderbuffer's mt_gfx target at the same logical size and scale N, its pixels resampled from the old one (nearest: the old pixel under each new pixel's centre), because the game never clears and a frame with no draws shows the retained one. Later storage uses N too. GL and EAGL see no change. It is called between guest calls, never inside one.
- **The frontend: `apply_render_scale`** (`game.cpp`), in the turn hook (between two run-loop turns): `mt_gles_set_render_scale`, `Screen::set_scale`, `Presenter::start` at the new size, and with GPU presentation `GpuScreen::resize` (a new overlay texture; the GL frame's copy and the counters stay). If a texture cannot be made, the old scale is put back and auto stops climbing. hd_text (ADR 0066) follows a higher N; a lower one keeps its text scale. The log says `render: render scale 1x -> 2x (frontend 1.71 ms, with mt_gfx 1.74 ms a frame (budget 10 / 20 ms) at 1x: 2x should fit), 0.5 ms`.
- **The policy: `RenderScaleGovernor`** (`platform/src/render_scale.{h,cpp}`, pure, no SDL), after each window frame with `FrameStats::recent()` and the time. It looks once a second, not within 1.25 s of a change (so the second it judges is all at the new N), and not at a second with fewer than 8 window frames or under half a second of them (a pause, a load):
  - **auto starts at 1x** and climbs to N + 1 when the last second, scaled by ((N + 1) / N)², fits both budgets and N + 1 is within the cap.
  - **auto drops** after two seconds in a row over either budget, to the largest smaller N whose scaled cost fits (1 at least), and does not climb again for 30 s. One second over (a spike, a load) does nothing.
  - **The cap**: `render_scale_cap(rect)`, the smallest N whose 480N × 320N image covers the image's place in the drawable (`Placement::rect`), 1 to 8. A resize applies it at once (auto above the cap drops to it), and a larger cap ends the 30 s wait.
  - **A fixed N > 1** is never changed. After three seconds in a row over a budget it **warns once**: the log (`render: Render scale 4x is too slow here (45 ms a frame): try auto or less (frontend 3.79 ms, with mt_gfx 44.79 ms a frame (budget 10 / 20 ms))`), the frame-time overlay's last line (`render 4x: OVER BUDGET`), and, for a player without the overlay, the same sentence as a notice at the window's bottom left for 8 s (`PerfOverlay::draw_notice`: SDL's debug font in the overlay's box, drawn into the window only after every read-back, like the overlay). Under auto the overlay's last line is `render 2x auto (window 4x)`.
  - The exit log adds `game: render scale 2x at the end (auto), 3 changes`.
- **Runs that write images** (`--fixed-step`, `--shot`, `--capture`: `perf_overlay_allowed` is false) have no governor: `auto` is 1x and never changes, so goldens and shots are the same bytes on every machine.
- **The GPU backend stays off by default** (`gfx_backend = software` in every preset column). #131 (D3D12 and Vulkan verified against the reference, a hand-off) is still open. When it is done, Enhanced's `gfx_backend` column becomes `gpu`, with the software rasterizer as the fallback that `GfxBackendHost` already is when no GPU device can be made. Nothing in this ADR has to change for that: the governor reads the same telemetry on both backends and simply climbs higher on the GPU.

## Evidence

- **CI** (`tests/cpp/render_scale_test.cpp`, `platform_test`, `render_scale: …`): the cap at the common sizes (1080p integer 3x, fit 4x, the default window on Retina 4x, 16:10 Retina 5x, at most 8); climbing on GPU-like numbers to the cap; the software backend's numbers stopping at 2x and the main menu's 1x composite not climbing; the drop after two seconds (to 2x from 4x at 43 ms), the 30 s wait, and down to 1x at worst; the resize (down at once, the wait ended by a bigger window); too few frames; the fixed scale's warning once, after three seconds, at 1x never, and for the frontend's budget alone; a failed switch; the overlay's line; `auto` in the file, on the command line, in the presets (Enhanced only; a fixed N under Enhanced is custom).
- **CI** (`present: the render scale changes while the game runs, its frame kept (#152)`): a synthetic guest draws at 1x; `mt_gles_set_render_scale(2)` remakes its one renderbuffer (0 the second time); the next frame with no draws is the 1x frame as 2 × 2 blocks; new draws at 2x match a game started at 2x byte for byte; 2x → 3x → 1x gives back the 1x frame exactly.
- **Real assets, local** (Release, arm64, Apple M4, other builds running): the real-time fresh run (the device golden list's taps, `--seconds 62`, a 960 × 640-point window on a Retina display: 1920 × 1280 pixels, cap 4) with `--render-scale auto`:

  | run | the scales chosen (`render:` lines) | at the end | `game: ticks:` |
  |---|---|---|---|
  | software, `auto` | 1x → 2x at 0.7 s (the menu: 1.74 ms a frame); 2x → 3x at 5 s (Play: 7.95 ms at 2x); 3x → 2x at 7 s (the intro's map: 27.3 ms at 3x); 2x → 1x at 50 s (the first map: 23.8 ms at 2x) | 1x, 4 changes | **1267 GL frames, 20.44 a second**; p99 60.90 ms, 78 over 55 ms |
  | GPU, `auto` | 1x → 2x → 3x → 4x within the first 4 s (1.67, 1.08, 0.46 ms a frame); the cap (the window's 1920 × 1280 image) | 4x, 3 changes | **1268 GL frames, 20.45 a second**; p99 53.69 ms, 9 over 55 ms |
  | software, fixed 4x (the guard) | warned at 3 s on the main menu: `Render scale 4x is too slow here (28 ms a frame): try auto or less (frontend 25.10 ms, with mt_gfx 27.66 ms ...)`, and the notice showed | 4x, warned | 1042 GL frames, **16.77** a second |

  Both `auto` runs keep the tick rate of a 1x run (20.36 to 20.45 here). Software at `auto` reaches 2x on a lightly loaded machine (the headless 2x run above: 11 to 17 ms a frame with mt_gfx) and falls back to 1x on a heavy map when the machine is busy, as here (load 6 to 10 from parallel builds). Each switch took 0.1 to 3.5 ms. A scale change is invisible except for the sharper image (the retained frame is resampled, and the next tick redraws it).

## Consequences

- Enhanced now renders above 1x wherever it fits, and nobody has to know which N their machine manages. A fixed N is the player's choice; the guard only says when it costs the tick rate.
- A scale change costs a few milliseconds once (new targets and textures, a resample) and is logged. The first seconds after launch run at 1x while auto measures.
- On the GPU backend mt_gfx's own time is recording only; the GPU's time is not in the budget (it shows, if anywhere, as the display wait). That is fine on Apple silicon, where the GPU at 4x takes under 2 ms (#120). #131's D3D12 and Vulkan verification should check the GPU time at the scales auto picks there (`MAPLETHIEF_GFX_GPU_TIMING=1`); if it matters, the governor gets that number too.
- #89's settings menu shows `auto, 1, …, 8` from the row and applies the choice through the same `apply_render_scale` (`Applies::kAtOnce` since ADR 0076: `request_render_scale` sets a new governor and the pending scale, taken at the next run-loop turn, when the menu closes).
- How we'd know this is wrong: auto runs that lose ticks (the exit log's `game: ticks:` under 19.5 with `render scale` changes in the log), scale changes back and forth more often than once per 30 s, or a resampled frame that shows for longer than one tick.
