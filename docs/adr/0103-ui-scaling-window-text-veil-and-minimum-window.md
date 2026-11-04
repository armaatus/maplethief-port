# 103. UI scaling: window text at the picture's scale, a veil over widescreen's wings, a minimum window

- **Status:** Accepted
- **Date:** 2026-11-04
- **Issue:** Joris's report "scaling UI in menus is not perfect" (an audit, no issue number)
- **Builds on:** [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (the placement, the mouse mapping), [ADR 0055](0055-ios-safe-area-landscape-orientations-and-input.md) (the safe area), [ADR 0071](0071-frame-telemetry-and-frame-time-overlay.md) (the frame-time overlay), [ADR 0072](0072-offline-cash-shop-and-revives.md) (the price line), [ADR 0076](0076-settings-menu.md) (the settings menu), [ADR 0080](0080-render-scale-auto-and-frame-budget-guard.md) (the notice), [ADR 0084](0084-offline-polish-hidden-online-ui-local-game-center.md) (the local leaderboard and toast), [ADR 0090](0090-widescreen.md) (the wide picture)

## Context

An audit of the UI across window sizes (480 × 320 to 3840 × 2160 points; 3:2, 4:3, 16:10, 16:9, 21:9), densities 1 to 3, every scale mode, widescreen off and on, render scale 1 to 3 and a phone's safe area. Real-asset runs (Henesys from `play.save_quit`'s save, the cash shop, the settings menu) were read back after the window overlays.

What holds already:

- **The picture and the mapping.** `place()` puts the picture on whole pixels, centred, inside the safe area; the mouse and touch map through the same placement, so the game's menus, the settings menu and the remap overlay (drawn into the 480 × 320 screen at the render scale) hit-test exactly where they are drawn. The settings menu scales with the picture, so it is as crisp and as large as the game's own menus at every size.

What did not:

1. **The window-only text was a fixed 8 pixels a line.** The frame-time overlay, render_scale's notice, the cash shop's price line, the local leaderboard and the achievement toast use SDL's 8 × 8 debug font at the pixel density's scale (1 on most Windows and Linux displays). In a 1080p window the picture is 3x and the game's text about 30 pixels tall, the price line 8: unreadable beside the shop it explains. At 4K it is a sixth of the game's text. They were also placed from the window's corners, so on a phone the price line and the notice sat under the Dynamic Island or the home indicator.
2. **Widescreen's wings stayed bright under the port's overlays.** The settings menu and the remap overlay darken the 480 × 320 screen image (their backdrop, black at 170), but with widescreen the wings beside it showed the field undimmed: a bright frame around a dark modal.
3. **A window could shrink below 480 × 320 points.** Integer mode then falls back to fit, which scales down nearest and drops whole rows and columns of the picture and of every menu's text.
4. **Edge rounding.** A window point at the image's top or left edge could map a hair outside (25 pixels / 1.5 x 1.5 = 24.999998), and a text box at a fractional position drew its glyphs between pixels.

## Options considered

**The window text's scale.**

1. **Keep the density's scale.** Rejected: issue 1.
2. **Draw it into the screen image** (Canvas, as the settings menu). It would then be in shots, goldens and captures, which these overlays must never be in (ADR 0071, ADR 0072).
3. **Scale with the picture: the larger of the density and half the picture's pixels per screen point, rounded, at most 8.** 8 window points a line at least (as before on the default window), and about 4 screen points a line beside the game's text as the window grows: 16 pixels at 1080p, 24 at 4K. A box that would not fit the safe area at that scale takes the largest that fits. **Chosen.**

**The wings under an overlay.** Draw the overlay over the wide picture (the menus' layouts are 480 × 320 points; a wider layout is a redesign), or **darken the wings with the backdrop's own black at 170 in the present stage**, after the picture (and the key pad's strips), while an overlay is open. **Chosen**: one constant (`kOverlayBackdropAlpha`) for both, and the screen image is never touched.

## Decision

- **`text_frame(placement, density, output)`** (`scaling.h`, pure): the window text's scale, `max(round(density), round(picture pixels per point / 2))` clamped to 1..8, and its area, the safe area in drawable pixels (the whole output before a placement exists). **`place_text_box(frame, w, h, corner, margin)`**: the scale (the frame's, less until the box and its margins fit), and the box in whole font pixels inside the area, at the top left, top right, bottom left or centre. The frame-time overlay and the notice (`PerfOverlay::draw`, `draw_notice`), the price line (`draw_price_line`) and the leaderboard and toast (`LocalGameCenter::draw`) take a `TextFrame`; `game.cpp` makes one per present (`text_frame_of`).
- **The veil**: `Presenter::set_veil(alpha)` darkens `wing_rects(wide_rect, image)` (the wide picture's parts left and right of the screen image) as step 5 of `draw()`. `run_overlays` sets it to `kOverlayBackdropAlpha` (170, `canvas.h`, which the settings menu's and the remap overlay's backdrops now use) while the menu or the remap overlay is open, and back to 0 after.
- **The minimum window**: a player's window of the game on the desktop (an interactive run, not iOS) has `SDL_SetWindowMinimumSize(480, 320)` points. Scripted runs keep the size they ask for.
- **Edges**: `drawable_to_screen` counts a point less than 1e-4 before the image's first row or column as on it.

## Consequences

- The window text is legible at every size and stays inside the safe area; at 3x and above it is larger than before (16 pixels at 1080p). The frame-time overlay, a debug read-out, covers more of a 1080p window (about 800 × 90 pixels).
- The veil is window-only furniture like the overlays themselves; nothing reads it back (the overlay loop runs no `check_readback`).
- Unchanged: the game's own menus, the settings menu, the remap overlay and the key pad editor scale with the picture; the button prompts' layer, the focus frame, the gear and the minimap keep their own rules.
- How we'd know this is wrong: text cut off at a window's edge, a box under a notch, a seam or a bright strip beside a dimmed menu, or a click on a menu's edge that misses.

## Evidence

- **CI** (`tests/cpp/ui_scaling_test.cpp`, `platform_test`): the matrix of 12 window sizes × densities 1, 1.5, 2, 3 × the four modes × widescreen off and on × render scale 1 to 3 × with and without an iPhone's safe area (2304 configurations): whole-pixel rectangles inside the safe area, centred, integer multiples, fit filling an axis; the wings flush with the screen image; every edge pixel of the image inside, the pixel beyond it outside; screen → window → screen round trips; the wings' edges at −44 and 524 points; the gear inside the safe area and off the screen image; the text inside the safe area at the density's scale at least. The text scale's table (1080p 2, 4K 3, 5K 5, at most 8), a box's shrinking and corners, the notice and the price line drawn headless at 1080p (2x) and inside a safe area, and the veil read back (the wings 255 → 85, the image and the bars untouched, off again).
- **Real assets, local** (Debug, dummy video driver, read-backs after the window overlays): at 1920 × 1080 the cash shop's price line is 16 pixels a line (8 before) and the frame-time overlay with it; with `widescreen = auto` in Henesys the settings menu's backdrop and the wings are one even dim (the wings were full brightness before).
