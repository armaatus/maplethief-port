# 203. `render_scale = auto` budgets the GPU's time

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#365](https://github.com/armaatus/maplethief-port/issues/365) (epic [#293](https://github.com/armaatus/maplethief-port/issues/293))
- **Builds on:** [ADR 0080](0080-render-scale-auto-and-frame-budget-guard.md) (`render_scale = auto` and the frame-budget guard), [ADR 0060](0060-gpu-rendering-backend.md) (the GPU backend and its fence timing), [ADR 0070](0070-settings-registry-and-presets.md) (Enhanced is `auto`)

## Context

ADR 0080's governor judges a scale by CPU time: the frontend's take, compose and draw, plus mt_gfx's time in `mt_gfx_submit`. On the software backend that is the whole cost. On the GPU backend mt_gfx only records draws on the CPU and the pixel work moves to the GPU, which the governor did not see: a GPU too slow for 4x showed, at best, as the display wait, and auto would climb to the window's cap anyway. ADR 0080's consequences and ADR 0070 (the Switch's GPU) noted this.

The GPU backend can already time a frame: with timing on, every flush with draws waits for its fence and adds the time from submit to the signal (`mt_gfx_gpu_set_timing`, `mt_gfx_gpu_stats::last_frame_gpu_ns`). That is an upper bound (queueing and the completion notice are in it), and the wait stalls the CPU for the GPU's time, which is why it is off by default. SDL GPU has no timestamp queries to time without waiting.

## Decision

**The frontend times one GL frame in four on the GPU backend.** `GpuFrameTimer` (`platform/src/render_scale.h`) is pure logic. After each window frame that shows a new GL frame (a tick), `time_gpu_frame` in `game.cpp` hands the timed frame's `last_frame_gpu_ns` to the timer and turns timing off; every fourth tick it turns timing on for the next GL frame. At 20 ticks a second that is about five samples a second, and only those frames' flushes wait. `ms()` is the median of the samples of the last second, so one stalled frame does not move it; with fewer than two samples it is 0, unknown. With `MAPLETHIEF_GFX_GPU_TIMING=1` the backend times every frame already, and every tick's time is a sample. Only while the governor runs (auto, or a fixed N above 1, never in a run that writes images).

**The render budget holds the larger of the CPU's and the GPU's time.** `RecentStats::gpu_ms` carries the timer's median to `RenderScaleGovernor::update`, which judges `max(frontend + mt_gfx, GPU)` against the 20 ms render budget (`kRenderBudgetMs`); the frontend's own 10 ms budget is unchanged. The CPU and the GPU work side by side, so a frame costs the slower of the two, not their sum. The GPU's time scales with N squared like the rest (fill rate), so the climb, the drop to the largest scale that fits and the fixed scale's warning all work on it unchanged. Without a GPU time (the software backend, too few samples) the CPU's alone counts, as before. The reason and the warning name the GPU's time when there is one: `frontend 1.50 ms, with mt_gfx 1.80 ms, GPU 30.00 ms a frame (budget 10 / 20 ms) at 4x: down to 3x`.

## Evidence

- **CI** (`tests/cpp/render_scale_test.cpp`): `render_scale: the GPU timer times one GL frame in four and keeps a second's median` (the cadence, no re-arming while a timed frame is to come, the median, a stalled frame, samples expiring, unknown with one sample); `render_scale: auto steps down on a slow injected GPU clock (#365)` (the frontend's loop with made-up times: a GPU taking 30 ms a GL frame at 4x with cheap CPU numbers steps auto down to 3x after two seconds; the same CPU numbers without a GPU time stay at 4x; 70 ms goes to 2x); `render_scale: the GPU's time keeps auto from climbing, and a fixed scale warns`.
- **Real assets, local** (Release, universal, Apple M4, a 1920 × 1080 display at density 1, load 13 to 17 from four other builds): the device golden list's fresh-run taps, `--quit 62`, `--gfx-backend gpu`, `--window 1920x1280` (the screen limits it to a 1920 × 1018 drawable: cap 3x):

  | run | the `render:` lines | GPU time (exit log, timed frames) | `game: ticks:` |
  |---|---|---|---|
  | `auto` | 1x → 2x (frontend 1.60 ms, GPU 0.66 ms); 2x → 3x (frontend 1.02 ms, GPU 0.57 ms); the cap | 0.849 ms median over 318 frames | 1278 GL frames, 20.61 a second; p99 51.66 ms, 1 over 55 ms |
  | fixed 4x | warned: `Render scale 4x is too slow here (23 ms a frame)` (frontend 22.54 ms, GPU 0.70 ms: a second of the main menu's compositing under load) | 0.971 ms median over 318 frames | 1278 GL frames, 20.62 a second; p99 52.01 ms, 1 over 55 ms |

  318 timed frames of 1278 is one in four. The timed frames' waits cost no ticks (20.61 to 20.62 a second, as ADR 0080's 1x runs). On Apple silicon the GPU at 4x takes under 1 ms a GL frame, so the GPU never limits auto here; the CPU's frontend does, as before. The slow-GPU case is the CI test's.

## Consequences

- On a GPU that cannot hold 4x (an integrated or old GPU, D3D12 or Vulkan on a weak card, the Switch's), auto settles where the GPU fits instead of at the window's cap.
- One GL frame in four waits for the GPU on the CPU. On Apple silicon at 4x that is under 2 ms (#120); the wait is guest time in the telemetry, not frontend time, so it does not feed back into the budget.
- The fence time is an upper bound: under load (#344) it can read high, and auto then drops one scale more than it needs to, and climbs again after ADR 0080's 30 s. The median over a second keeps single spikes out.
- SDL's own GPU work for the window (the present passes, the overlay) is not in the sample: it is small next to the game's draws at the scales that matter, and it does not grow with the render scale's N squared the same way.
- How we'd know this is wrong: auto on the GPU backend dropping on a machine whose GPU time is far under the budget (the `render:` reason names it), or ticks lost to the timed frames' waits (the exit log's `game: ticks:` under a 1x run's).
