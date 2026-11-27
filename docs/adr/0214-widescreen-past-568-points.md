# 214. Widescreen past 568 points: a `fill` value up to 768, the backdrop tiled

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** #387 (research); the work is #425
- **Builds on:** [ADR 0090](0090-widescreen.md) (the per-frame view width and its addenda), [ADR 0147](0147-what-v1-is-and-what-gates-it.md) (#382's finding that 568 is not the engine's limit), [ADR 0098](0098-port-drawn-minimap.md) (the minimap's corner), [ADR 0085](0085-borders-around-the-picture.md) (the borders beside the picture). The facts are in [re/widescreen.md, Past 568 points](../re/widescreen.md#past-568-points-387).

## Context

ADR 0090 draws the field up to 568 × 320 points, the iPhone 5's 16:9. Phones since the iPhone X are 19.5:9 (693 points at 320 high), most Android phones 20:9 (711), ultrawide monitors 21:9 (747); on all of them the port shows the 568-point picture with bars beside it. #387 asks whether the field can go wider, and what limits it.

What #387 measured:

- **The code has no limit at 568.** Everything reads the configured width. A local build at 768 played `play.widescreen_on`'s route with the field 768 wide on Henesys 2.0 and 2.3, and every one of 2,985 ticks equalled the 3:2 run's; `play.widescreen_pad_on` held the keys by touch with the pad at the 768-point picture's edges, the hero equal at every tick.
- **The maps bound it.** 74 of the 90 maps fill 568 points, 52 fill 693, 51 fill 711, 32 fill 747 and 768, 21 fill 776 or more. A narrower map is drawn as wide as it is, centred, with black wings: at 568 that happens on 2 maps, at 768 on 42.
- **The backdrop.** Nine of the eleven backdrops repeat exactly every 160 points, so a copy drawn at x = 480 continues them with no seam; today's stretch makes them 18 % wider at 568 and 60 % at 768. Backdrops 11 and 12 repeat except in a middle band.

## Options considered

The width:

1. **Keep 568.** Safe, and right for 16:9 screens. But on the phones the port targets (iOS, Android) and on 21:9 monitors a third of the screen stays bars, on 51 maps that could fill it.
2. **Raise the fixed width** (`auto` at 693 or 768). Rejected: on a 16:9 window a wider fixed picture is letterboxed top and bottom, smaller than today.
3. **A new value, `fill`. Chosen.** The renderbuffer is 768 points wide (48 tiles: past it only 21 maps would fill the view, and 21:9 is the widest common screen). Each frame E = the map's width clamped to 480 … min(768, the window's width at 320 points high). The window's shape picks the width, the map caps it, and `auto` keeps meaning 568.
4. **A per-area cap** (wide only where the map fills it). Not needed: the per-map cap already does this, and a map narrower than the window is centred like maps 36 and 37 today.

The backdrop:

1. **Stretch** (today). Kept for the backdrops that don't repeat (11, 12).
2. **Tile by the image's 160-point period. Chosen** for the nine that repeat exactly, at every width, `auto`'s 568 included: no distortion, no seam, the pixels the game drew.
3. **Mirror** at the image's edges. Rejected: an obvious symmetry axis in clouds and cave roots.
4. **Clamp or blur** the edge column. Rejected: horizontal streaks.

## Decision

- `widescreen` gains a third value, `fill`: a 768-point renderbuffer, and the view width E each frame the map's width clamped to 480 … the window's width at 320 points high (at most 768). `auto` (568) and `off` keep their meaning; the default doesn't change.
- The backdrop hook (`0x24DC9`) tiles a backdrop that repeats every 160 points (a copy at x = 480, clipped to E) and stretches one that doesn't. Which backdrops repeat is measured at load from the image (the columns at x and x + 160 equal), not from a list, so a pack's replacement backdrop gets the right treatment.
- Everything else already takes the width as a value: the screens over the field stay at 480 in the middle, the key pad's groups move by (E − 480) / 2, the minimap's corner has more room, the scripted scenes and loading stay 3:2.
- The work is #425. It replaces the tests' literal 44-point offsets with (E − 480) / 2 and adds `play.widescreen_fill_on` (768, tick-equal to the off run) and a window-shot check that the tiled backdrop has no seam.

## Consequences

- On 19.5:9, 20:9 and 21:9 screens the field fills the screen on 51 to 52 maps (32 at the full 768), and the picture width changes from map to map on the others: black wings beside a narrow map, as maps 36 and 37 have under `auto`.
- On the 11 maps exactly 48 tiles wide the whole map is on screen at 768, and the camera stops scrolling sideways. The game is unchanged (only drawing reads the width), but the player sees more of the map at once.
- Bag, Cash and the compass are `UIButton`s in the 480-point screen image and stay at their 3:2 places, 144 points in from the picture's edges at 768.
- Under `auto` the nine repeating backdrops stop being stretched: a small visible change at 568, the backdrop at its own proportions.
- Judgement call (delegated): `fill` is opt-in, and the cap is 768. Joris can veto either here or in #425's PR.
