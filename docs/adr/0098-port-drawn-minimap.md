# 98. A port-drawn minimap from the game's loaded .att grid, in the bar or over the picture

- **Status:** Accepted
- **Date:** 2026-11-02
- **Issue:** #176
- **Builds on:** [ADR 0092](0092-quality-of-life-assists-and-ui-animations.md) (quality of life; [re/qol.md](../re/qol.md)'s verdict: read only, low risk), [ADR 0085](0085-borders-around-the-picture.md) (the present stage's steps), [ADR 0090](0090-widescreen.md) (the wide picture), [ADR 0070](0070-settings-registry-and-presets.md) (the registry)

## Context

The game has a map screen (game mode 3, `Map_PaintSub0` `0x59BB1`) with its own small map, but nothing on the field: the key pad's panel 2 is a "minimap" panel the game never fills in 1.5 ([objc-shell.md](../re/objc-shell.md)). #90's survey found a minimap cheap and safe if the port draws it from data it only reads. The collision grid ([formats/att.md](../formats/att.md)) already shows every platform, ladder and wall, one byte per 8 × 8-pixel cell, and the hero's position is `CHero +0x04` / `+0x08`.

## Options considered

**Where the map comes from.**

1. **The bundle's `.att` file**, decoded in C++ by area and map. It needs a second decoder (the Python one is `tools/mtport/attfile.py`), the bundle's path in the frontend, a file read on every map change, and the current area and map, which change a few ticks before the new map is loaded.
2. **The game's loaded grid in guest memory.** The `.map` loader `0xD6D1` allocates the grid (`CMMap +0x3C`, size `+0x40`, `attW` `+0x28`, `attH` `+0x2C`) and `CMMap::LoadAtt` `0xD5B1` fills it. `CStateGame +0x2C` is the `CMMap` (already used by widescreen). The objects are there too (`+0x48` / `+0x4C`, with their runtime `visible` flag). **Chosen:** it is exactly what the game collides with, it follows every map change by construction, and it needs no file and no new decoder. The real-asset test checks it against the bundle's `.att` (decoded independently by `tests/cpp/world_map.h`).

Guest memory is treated as untrusted input: every read goes through a `GuestSpan` that only returns a pointer when the whole range is inside one image segment or the committed heap; the grid's sides are capped at 1024 cells, `attW · attH` must fit in the buffer's size, the cell size must be 1–64 pixels, and at most 64 objects are read. Anything else: no minimap that frame. A bad object costs only its marker.

**Where it is drawn.**

1. **A step of the screen image** (composited into the 480 × 320 picture). It would show in shots, goldens and captures, change the device goldens, and cover the game's art at its own resolution.
2. **A window overlay after the read-backs** (Presenter step 5, like the frame-time overlay and the gear). **Chosen.** `off` changes nothing at all, and the minimap is drawn at the window's resolution.

**Its place.** `minimap = off | border | overlay` (Video). `border` uses the bar to the left of the (wide) picture, at its top, or the bar below it on a window taller than 3:2, when the bar is at least 48 points wide (24 tall); the gear (#89) has the right bar and the top bar. Where no bar has room (3:2, or widescreen filling a 16:9 window) it falls back to `overlay`. `overlay` sits on the screen image's top edge 46 points from its left, right of the compass (the map button), clear of Bag and Cash at the top right and of the HP bar at the bottom. With widescreen the screen image is the middle 480 points of the wide picture, so the overlay sits at the same place over the HUD at 3:2 and wide.

**Its size and look.** The map is scaled to fit 150 × 64 points (overlay) or the bar's width less margins and half the picture's height (border), at most 0.125 points a map pixel (a tile at most 2 points). When a box pixel covers several cells it takes the most important one: foothold, then ladder, then wall (so one-cell floors never vanish). The grid is uploaded as a texture only when the map or the box changes; the dots are filled rectangles: portals (kinds 0–2, the hidden kind 3 left out as the game's own minimap does), visible NPCs (4–59) and the hero, a cell above their anchors. Mobs are not drawn: the issue asks for "the hero, portals and NPCs if cheap".

## Decision

- **`platform/src/minimap.{h,cpp}`**: `guest_span(cpu)`, `read_minimap(GuestSpan)` → `MinimapView` (shown only for a `CStateGame` in game mode 0), `minimap_layout(mode, placement, wide_rect, map_w, map_h)`, `minimap_dots`, `minimap_grid_pixels` / `render_minimap` (the CPU drawing, for the golden), and `MinimapLayer` (the window's renderer).
- **`game.cpp`**: after the read-back and captures, before the focus highlight, when `minimap` is not off and neither the settings menu nor the remap overlay is open.
- **Row** `minimap = off | border | overlay` (Video, `--minimap`, `off` in every preset column, `Applies::kAtOnce`: read at every present from the live settings).

## Consequences

- With `minimap` on, the field shows the map's platforms, ladders, walls, portals, NPCs and the hero's position; outside the field (game mode 0: the map screen, the popup menu, shops, NPC chats, scenes, the title) there is none. A popup over the field keeps it.
- Off (the default everywhere) draws nothing and reads nothing: the window is byte for byte as before, and shots, goldens and captures never contain the minimap in any mode.
- The minimap reads guest memory once a present (the grid is at most 1024 × 1024 bytes, about 48 KB on the largest shipped map) and never writes it.

## Evidence

- `platform_test` `minimap:` cases (synthetic guest, hostile values, the layouts, the golden `tests/golden/data/minimap_synthetic.png`, off drawing nothing on a software renderer), `presets_test` (the row's columns).
- Real assets: `play.minimap` (first_map's route to 2.3 with `minimap = overlay`): the guest's grid equals `map/23.att`, every marker is in `map/23.obj`, the minimap shows in every field turn and not on the map screen, and the hero's dot is at the hero's position (overlay at 3:2, border at 16:9) and follows him both ways. Local, Debug arm64, 2026-11-02: map 2.3's grid 120 × 60 cells equal to `23.att`, 2 markers; shown in 80,019 of 80,019 field turns (maps 2.8, 2.9, 2.0, 2.3), none of 2,844 map-screen turns; the dot right in all 32,601 turns on 2.3 (in the left bar at 16:9 in all of them), 154 moves (93 left, 61 right), none the wrong way.
