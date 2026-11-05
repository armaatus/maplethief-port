# 109. HUD extras: the EXP percentage only; the game already draws mob HP bars and a pick-up log

- **Status:** Accepted
- **Date:** 2026-11-05
- **Issue:** [#191](https://github.com/armaatus/maplethief-port/issues/191) (epic #201)
- **Builds on:** [ADR 0098](0098-port-drawn-minimap.md) (a read-only window overlay from guest memory, `GuestSpan`), [ADR 0101](0101-button-labels-on-the-field-key-pad.md) (`HudLabelLayer`, the label unit), [ADR 0103](0103-ui-scaling-window-text-veil-and-minimum-window.md) (window text), [ADR 0114](0114-paging-lists-quantity-repeat-crisp-prompts-price-line.md) (the price line), [ADR 0070](0070-settings-registry-and-presets.md) (the registry), [ADR 0104](0104-input-replay-and-regression-checkpoints.md) (the regression replay)

## Context

#191 asked for three HUD extras that "the original lacks": a mob's HP bar for a few seconds after it is hit, the EXP as a percentage beside the bar, and a short log of pick-ups and meso gained. It asked for research first: where each value is read and whether the game already shows it. The research is [re/hud-extras.md](../re/hud-extras.md).

- **Mob HP bars.** The game draws them. `CMob::TakeHit` `0x73715` starts a counter (`CMob +0xD0`) on every hit the mob lives on, `CMob::Flush` `0x7458D` counts it to 20, and while it runs `CMob::Paint` `0x72685` draws the mob's name and a 40 × 2 point HP gauge over it. Bosses have a bar at the top of the screen (`HUD_Paint` `0x5A465`). On real assets 16 of 16 closed bars ran 19 field ticks after their hit (`play.hud_extras`).
- **Pick-up log.** The game prints one. `CHero::addInven` `0x64309` and `CHero::setMeso` `0x618E1` add "<item> Obtained!" and "Meso obtained.(+N)" lines to the notify list under the compass (`CNotifyTextList::Add` `0x78339`), shown for 5 s and fading at the end (`Expire` `0x77B39`, `Paint` `0x77C19`). On real assets each Red Potion and meso gain had its line.
- **EXP percentage.** Missing in the field. The HUD has only a 2-point EXP line (`Gauge_Width(153, need, EXP)` at (163, H − 3)). The figure is only in the popup menu's status tab, as a whole percent.

## Options considered

1. **Build all three.** A port mob bar would be drawn over the game's for its first second. A port log would repeat each of the game's lines. Neither adds information. Making the game's own bar last 3 s means writing `+0xD0`, which #191 rules out (read-only).
2. **Build the EXP percentage, drop the other two with the evidence.** **Chosen.** The issue says to drop a candidate that doesn't hold up. The two dropped ones are no follow-up issues, because the screen already has them.

**Where the figure goes.** Above the HUD plate (which starts at y 304), left of the level badge (x 215 … 265 from y 285): a box right-aligned at x 213, bottom at y 303, within x 150 … 213. It sits over the field, clear of:
- the HP / MP gauges and the plate (covered by none);
- the minimap overlay at both of its anchors;
- the key pad's labels (ADR 0101);
- the price line (only in the meso cash shop; the figure is hidden there);
- the window text of ADR 0103 (in the window's corners).

The HUD is drawn at fixed 480-point places whatever the width (ANALYSIS.md), so its point position follows the screen image in every mode: whole, wide or narrow. With the wide view it moves with the screen image, not the wings. The text is the HUD labels' size (`hud_label_unit`, about 9 points high at most, never under 12 pixels) with a dark backing, "EXP" in the game's EXP green `0xB6DA00`.

**The figure.** Two decimals, floored: "EXP 42.17%". It never shows 100 %, because the game levels up at the need (`CHero::setExp` `0x6654D`). It is clamped to 0 … 99.99 for odd memory. A whole percent would repeat the status tab and often stand still for several kills. Two decimals move on every gain at the early levels' needs (10 … 430).

## Decision

- **`platform/src/exp_percent.{h,cpp}`**:
  - `read_exp(GuestSpan)` reads, only in the field (`CStateGame` vtable `0x119B78`, game mode 0), the hero (`CStateGame +0x30`): level `+0x80` (1 … 255) and EXP `+0x90`.
  - It finds the level's need as `CHero::ExpToLevelUp` `0x65919` does: hero.dat at `*0x11A9A8`, the first row whose u8 key is the level, then the i32 at column offset 1. Every read is range-checked, the row count is capped at 1,024 and the stride at 5 … 256. Anything odd shows nothing.
  - `exp_percent_text` formats the text; `exp_percent_rect` places the box on whole pixels; `render_exp_percent` draws it (Canvas, premultiplied).
  - `ExpPercentLayer` is a streaming texture, redrawn only when the text or the unit changes.
- **`game.cpp`** draws it after the minimap: after the read-backs, in the window only. It is not drawn while the settings menu or remap overlay is open, or while the meso cash shop's price line shows.
- **Row** `exp_percent = off | on` (Video, "EXP percentage", `--exp-percent`, live): presets **off / off / on** (Original, Faithful, Enhanced). It is a small figure beside the game's own bar, so it doesn't clutter Enhanced.
- **No guest write and no hook.** The names (`Gauge_Width`, `CDataMgr::FindRow` / `ReadI32`, `CMob::Paint` / `TakeHit` / `Die`, `CNotifyTextList::*`, the camera getters) are in `tools/mtport/hudextras.py`, run by `scripts/re-analyze`.

## Consequences

- With `exp_percent = on`, a field player sees the hero's EXP to the next level as "EXP 42.17%" above the HUD's bar. Off, the window is as before.
- It is never in a shot, golden, `--capture` or regression hash. The replay's digest is the same off and on (below).
- A mob's HP after a hit and each pick-up stay as the game shows them: a 1 s bar and a 5 s line. Someone who wants a longer bar would need a guest write. That is out of #191's read-only rule and has no issue.

## Evidence

- `platform_test`'s `exp percent:` cases:
  - the read-back against a synthetic guest (need = 15 n²), with no writes;
  - hostile memory (no state, another mode, level 0 / 256, a missing row, a short table, a huge stride or count);
  - the text (floors, 0, 99.99, the clamp, need 0);
  - the box at 1–8 pixels a point and in fit mode;
  - the drawing's pixels;
  - the layer on a software renderer, redrawn only on a change;
  - the settings row.
- `ui_scaling_test` (2,304 configurations): the box is whole, inside the screen image, between x 150 and 213.5 points with its bottom at 303.5 or above, at most 9.5 points high, clear of the minimap overlay at both anchors and of every HUD key and label box (edges and narrow). `presets_test`: the row's columns.
- Real assets, local: `play.hud_extras` (148,521 field turns; levels 1–7; 22 figures from 0.00 % to 95.00 %, each the hero's EXP over hero.dat's need; the game's mob bar on 65 surviving hits and 16 closed after 19 ticks; 1 Red Potion and 11 meso gains, each with the game's line, none longer than 5.03 s). `platform.device_golden` passes with `exp_percent = on`.
- `scripts/regress`: digest `8a2f05733c8d4603` with `exp_percent = off`, with `on` and with `--preset enhanced`.
