# 42. The frontend runs the game: one thread, presentation at every GL present, the bundle and the generated code

- **Status:** Accepted
- **Date:** 2026-10-29
- **Issue:** #58
- **Builds on:** [ADR 0024](0024-release-contents-and-flow.md) (no generated code in a release), [ADR 0027](0027-process-start-up.md) (loading and running the process), [ADR 0033](0033-gcd-worker-threads-and-main-queue.md) (one guest main thread), [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (`mt_uikit_app_main`, the turn hook, the compositor's GL slot), [ADR 0038](0038-gl-recording-eagl-silent-audio-boot-to-first-frame.md) (EAGL, the present hook), [ADR 0039](0039-rendering-backend-software-rasterizer.md) (`mt_gfx`, the SDL streaming texture), [ADR 0040](0040-gles-fixed-function-pipeline.md) (the renderbuffer's target, `mt_gles_frame.target`, `render_scale`)

## Context

After #56 the game's GL frames exist as `mt_gfx` targets, but nothing shows them: `MapleThief` was still M0's placeholder window, and only the real-asset tests (`runtime.boot_real`, `runtime.launch_real`) booted the game. #58 makes the frontend boot the game and present its frames: the renderbuffer, then the compositor's GL slot (the UIKit views over it), then the window.

What has to be decided:

- **Threads.** `mt_process_run` runs the guest on the calling thread until it ends; UIKit's `UIApplicationMain` never returns and hands control to the host only after each run-loop turn. SDL wants its events pumped on the main thread (macOS requires it).
- **When to present.** The turn hook is the natural place, but the game also presents from inside long calls: while it loads a map its loading screen animates, presenting frame after frame without returning to the run loop. A frontend that presents only between turns shows none of those frames (measured: GL frames 33 to 66 of a launch arrived within one turn).
- **Retained backing.** The game asks for no retained backing and never calls `glClear`.
- **Orientation.** The game is a landscape iPhone OS 3 app on a portrait window. Its `EAGLView` turns itself (transform (0, −1, 1, 0), bounds 480 × 320), so the renderbuffer is 480 × 320 and the projection is `glOrthox(0, 480, 0, 320, −1, 1)`. The original issue expected a portrait 320 × 480 buffer that might need turning.
- **The generated code.** It exists only in local builds with `work/gen`. A release kit must never contain it ([ADR 0024](0024-release-contents-and-flow.md)), CI never has it, and `platform.smoke` must keep working without it.
- **Where the game comes from**: the extracted bundle and the armv7 slice, and a data directory for the saves.

## Options considered

**Threads.**

1. **One thread: the guest runs on the main thread, and the hooks pump SDL.** The present hook and the turn hook are the only places the host runs during a run, and both are on the main thread.
   - Pro: no locking. Every runtime call happens where the runtime expects it (single guest thread, ADR 0033). SDL's events are pumped on the main thread.
   - Con: a guest call that neither presents nor returns to the run loop freezes the window. The game has none longer than a load, and a load presents.
2. **The guest on a second thread, SDL on the main thread, frames handed over.**
   - Pro: the window stays responsive whatever the guest does.
   - Con: every UIKit and input call crosses threads. The compositor reads the view tree, which the guest changes while it runs, so it would need a lock around every turn. iOS (M9) wants the main thread for UIKit-like work as well.

**When to present.**

1. **Only in the turn hook.** Loses the loading screen's frames (above).
2. **At every GL present, from the present hook, plus in the turn hook when something else changed.** The device shows a presented renderbuffer at once, so does this. The turn hook covers UIKit-only changes: an event, a running UIView animation, and a periodic refresh (0.25 s) for a view changed without either.
   - Con: the compositor runs inside the guest's `-presentRenderbuffer:` call, mid-turn, and sees the views as they are at that moment. Core Animation would commit view changes at the end of the turn. The difference is at most one GL frame (40 to 65 ms) late for a view change.

**Linking the game.**

1. **`MapleThief` links `maplethief_gen` when the target exists** (`if(TARGET maplethief_gen)`), and runs the game by default then. Without it, the program stays the placeholder.
2. **A separate `MapleThiefGame` executable.** The build would have two programs and two bundles on macOS, and the "real" one would never be what the kits ship. With option 1 the kit's program is the same one, without the game code.

## Decision

- **One thread (threads option 1).** `main.cpp` creates the SDL window and renderer and calls `run_game` (`platform/src/game.{h,cpp}`). That loads the process (`mt_process_load` with `app_main = mt_uikit_app_main`) and runs it on the main thread.
  - **The present hook** (`mt_gles_config.present`) copies the presented target (`Screen::take_gl_frame`), pumps SDL's events and presents the window.
  - **The turn hook** (`mt_uikit_config.turn`) pumps events, delivers scripted taps, presents if an event arrived, a UIView animation runs, or 0.25 s passed since the last present, and then waits. It waits until the run loop's next wake (`next`) or an event (`SDL_WaitEventTimeout`), for at most 0.25 s, or 1/60 s while an animation runs. A wait of under 2 ms sleeps with `SDL_DelayNS`: on a map the game's `CFRunLoopTimer` fires every millisecond ([ANALYSIS.md → Program flow](../ANALYSIS.md#program-flow)), and without that sleep the loop spun at about 600 turns a second.
  - **The run ends when the window closes** (or `--frames` are done, or a check fails). The hook that sees it returns nonzero. That is `MT_PROCESS_STOPPED` with "UIApplicationMain: the host ended the run loop" or "presentRenderbuffer: the host ended the run", and `mt_process_run` returns, so the frontend frees the process and SDL in order.
  - Exit statuses: 0 clean, 1 SDL failed, 2 usage, 3 a read-back check failed, 4 no game (no generated code, bundle or slice, or the load failed), 5 the guest trapped, stopped by itself or exited non-zero.
- **Presentation (when-to-present option 2)**, `platform/src/present.{h,cpp}`:
  - `Screen::take_gl_frame` copies the frame's `mt_gfx` target with alpha forced to 255. The `CAEAGLLayer` is opaque, so the device shows the renderbuffer's colour and ignores its alpha.
  - `Screen::compose` calls `mt_uikit_composite(MT_UIKIT_COMPOSITE_FULL)` with that copy as the GL slot's frame, at 480 × 320 × render scale.
  - `Presenter` uploads the result into the SDL streaming texture (`SDL_PIXELFORMAT_ABGR8888`, blend none, nearest), moved here from M0's `main.cpp`. It clears the drawable to black and draws the texture at `present_rect()`.
  - The copy at present time is also what keeps a frame whole: draws made after a present cannot reach the window before the next present.
- **The seam for #59: `present_rect(output_w, output_h, image_w, image_h)`** returns a `Placement`, the rectangle in output pixels plus nearest or linear filtering. It is the only place that decides where the image lands and how it is filtered.
  - The output size is the drawable's pixel size (`SDL_GetCurrentRenderOutputSize`), so a HiDPI window gets its full resolution. SDL's logical presentation is no longer used.
  - Today the placement is `fit_rect`: the largest rectangle of the image's aspect ratio that fits, centred, with its size rounded to whole pixels and black bars on the other axis, filtered nearest.
  - #59 replaces this function's policy (scaling modes, integer scaling, the config file) in `platform/src/scaling.{h,cpp}`. Mouse-to-screen mapping is the inverse of the same rectangle.
- **Retained backing.** Nothing clears the target: neither the game nor `mt_gfx`, nor the frontend. Each present shows the target as drawn, with whatever earlier frames left where the current one did not draw. That is retained backing whatever the drawable property says; the game redraws the whole screen every frame, so on the device the property makes no difference either.
- **Orientation: nothing to turn.**
  - `mt_gfx` stores clip-space y = +1 (GL's top, y = 320 in the game's ortho) in its first row. That is the top-row-first, upright image that `mt_uikit_gl_frame` expects.
  - The turned `EAGLView`'s local space is the upright landscape screen, so the compositor draws the frame 1:1 into the screen (its image draw copies exactly when pixel-aligned).
  - No portrait buffer appears anywhere, so there is no detection step. Tests pin this down (Evidence).
- **The game in the build (linking option 1).**
  - `MapleThief` links `maplethief_gen` only `if(TARGET maplethief_gen)`, with `MT_HAVE_GENERATED_CODE` and the dev default bundle `MT_DEV_BUNDLE_DIR` (`<work>/ipa/Payload/MapleThief.app`, next to `MAPLETHIEF_GEN_DIR`).
  - `main.cpp` is the only unit that sees the generated tables (`MT_PROGRAM_GENERATED`) and `MT_MEM_RESERVE_FOR_BUILD` under the generated code's `MT_MEM_CHECKED`. It hands both to `run_game`.
  - The game and presentation code is the static library `maplethief_frontend`, which never sees generated code, so CI builds and tests it.
  - A build without generated code defines a `mt_dispatch` that aborts: the boot that `game.cpp` links refers to it, and `run_game` never loads a process in such a build.
  - With the generated code, the game is the default mode. `--smoke-frames` and `--gfx-prototype` still select the placeholder and the prototype, so `platform.smoke` and `platform.gfx_prototype` are unchanged in every build. Without it, the program is exactly the placeholder shell, and the kits (built from a clean clone, [ADR 0024](0024-release-contents-and-flow.md)) and the release guard see what they saw before.
- **The bundle, the slice and the data directory.**
  - The bundle is `--bundle <dir>`, else `MAPLETHIEF_BUNDLE_DIR`, else the dev default. The first one given decides; a wrong one is an error, never a fallback.
  - The slice is the armv7 entry (cputype 12, subtype 9) of the bundle's fat executable `MapleThief`, read with a bounds-checked fat header walk (`find_armv7_slice`), or a thin armv7 file given with `--slice`.
  - The data directory (`Documents/`, the saves, `gamecenter.json`) is `MAPLETHIEF_DATA_DIR`, else `SDL_GetPrefPath("", "MapleThief")`. That is the stubs' default directory on macOS and Windows; on Linux it is `~/.local/share/MapleThief`, where the stubs alone would say `maplethief`.
  - Configuration otherwise comes from the environment as for the tests (`mt_import_config_from_env`, `mt_diag_configure_from_env`).
- **Options for testing and tuning.**
  - `--render-scale N` sets `mt_gles_config.render_scale` (1 to 8); the screen is composited at that scale.
  - `--frames N` quits after N window frames. The last is read back (`SDL_RenderReadPixels`) and must equal the composited screen: the output pixel under every image pixel's centre, RGB, with black bars. It must not be black either, or the exit status is 3.
  - `--capture DIR` writes read-backs as PNGs (`SDL_SavePNG`): the first frame, the first that is not black, the first that shows a GL frame that is not black, the last, and those of `--capture-at`.
  - `--tap T,X,Y` taps a screen point at T seconds. It is a test aid; mouse and touch input is M7 (#65).

## Evidence

- **CI** (`tests/cpp/platform_test.cpp`, `platform.*`, synthetic; SDL's dummy video driver):
  - The fit rectangle at 3:2, wider, taller and smaller outputs, HiDPI sizes, render scale 2, and empty sizes.
  - `find_armv7_slice` on a fat file like the game's, on thin files, and on eleven hostile headers (no armv7 entry, counts of 0 and 65,536, tables and slices past the end, an offset + size that wraps, an entry that is no Mach-O or of another subtype).
  - The bundle's precedence.
  - The present path on a synthetic guest built as the game builds its own: a 320 × 480 window, a turned GL view whose `CAEAGLLayer` gives a 480 × 320 renderbuffer, and the game's viewport, ortho and `GL_SHORT` strips. An asymmetric scene (grey, red top left, green top right, blue bottom left in GL's y-up space) goes through `-presentRenderbuffer:`, the present hook, the compositor and the SDL texture into a 720 × 640 window. Each marker lands in its corner, in the GL frame, in the composited screen (equal to the frame pixel for pixel) and in the read-back window (720 × 480 at y 80, black bars). A mirrored image fails the read-back check, and a vertical flip in the copy fails 37 assertions.
  - Retained contents: a frame that draws only one marker shows the previous frame's pixels everywhere else (exactly 48 × 32 pixels change). A present with no draws shows the same image. Draws before a present do not change the copy shown.
  - Render scale 2: a 960 × 640 target, composited and drawn at 1200 × 800 in a 1280 × 800 window.
  - A UIKit view over the GL frame, composited above it.
- **Real assets (local, `platform.game_boot`, only with generated code)**: `MapleThief --frames 150 --tap 3,106,165` headless boots the game, taps Play and runs into the first map. The last frame's read-back equals the composited screen (0 mismatched pixels), and the boot slice from the bundle's fat executable is byte-identical to `work/slices/MapleThief.armv7` (`platform.slice: …` test).
- **A real macOS window** (Cocoa, Metal renderer, 960 × 640, vsync on), 240 frames read back to `build/test-output/mac-debug/game_window/` and looked at:
  - Window frame 1 is the main menu at the start of its slide-in, with the promo banner: UIKit views over the black GL frame.
  - Frame 20 is the full main menu: logo, Play, the banner.
  - After the Play tap at 3 s, window frame 31 is the first that shows a GL frame that is not black: GL frame 24, the loading screen (skull pattern, spinner around the pig, "Loading…", a tip), upright and readable.
  - Frame 100 is Henesys with "Peaceful Henesys", sky on top and ground below, the game's own letterbox bars.
  - The last frame's read-back equals the composited screen (0 mismatched).
  - The first GL frame is the fade-in's black quad. The index of the first non-black GL frame depends on when Play is tapped: 24 to 33 in these runs.
- **Speed** (Apple M4). Release, per window frame: 7 to 9 ms compositing, under 0.1 ms to upload and draw. Debug: 42 ms compositing and 13 ms of `mt_gfx` per GL frame. The game presents every 40 to 65 ms, so even Debug keeps up on the main menu.

## Consequences

- **#59** changes `present_rect()` (and adds `scaling.{h,cpp}`, the config file and the mouse mapping). The drawable size comes from `SDL_GetCurrentRenderOutputSize`, and the screen image's size is `Screen::width()`/`height()` (480 × 320 × render scale).
- **#60** can read the composited screen (`Screen::pixels()`) or the window (`--capture`) for its comparisons with device screenshots.
- **M7 (#65)** feeds SDL's mouse and touch events to `mt_uikit_touch` from the same hooks, mapping through the placement's rectangle. `--tap` stays as a scripted test aid.
- **The compositor runs on every GL frame**, 7 to 9 ms in Release, mostly copying the full-screen GL image through CoreGraphics and redrawing the views. If that matters (a GPU backend in M10, slower machines), the overlay mode with a cached overlay is the way out: draw the GL frame directly and composite the views only when they change.
- **How we'd know this was wrong:**
  - The window freezes in some game state: a guest call that runs long without presenting. Then the present hook's event pumping is not enough, and threads option 2 comes back.
  - A view change shows a frame late where the device shows it at once: compositing mid-turn is visible. Then the turn hook presents again after the run-loop turn.
  - The game draws into a portrait buffer somewhere (a renderbuffer of 320 × 480). The orientation tests then need that case and the compositor a rotation.
