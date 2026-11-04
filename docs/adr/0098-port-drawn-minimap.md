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

## Addendum: the corner with the key pad at the edges (2026-11-04)

**Context.** With `widescreen = auto` and `widescreen_hud = edges` the key pad's groups sit at the wide view's edges, but `overlay` stayed on the 3:2 screen image's top edge right of the compass: 90 points in from the view's left on a 568-point field, with the empty left wing beside it. Joris asked for the minimap in the top left like the PC game's. The top left of the field is the game's, though: the compass (the Map button, a `UIButton` of panel 0 at (0, 0, 52, 52), [keypad.md](../controls/keypad.md#panel-0-the-game-pad)) in the corner, the game's messages ("EXP obtained", "… Obtained!") from y ≈ 55 at the view's left edge, and the quest note at the top centre from x ≈ 188 (screen points). The old overlay already covered the compass's "E" and the start of the map-name banner.

**Options.**

1. *Below the compass, at the left edge.* Over the game's messages.
2. *Right of the compass at the top* (as before, tidied). Not the corner, so the ask is unmet.
3. *Move the compass into the left wing, the box right of it.* The corner holds the compass, not the minimap.
4. **The box in the corner, the compass moved right of it. Chosen.** The box is at most 150 × 48 points, so it ends above y = 52, where the messages begin. The compass moves only as far as the box needs and stays short of the quest note.

**Decision.**

- **The anchor** (`minimap_anchor`): `corner` with `widescreen = auto` (as the runtime has it) and `widescreen_hud = edges`; `compass` otherwise. The mode and the anchor combine as the table in `minimap.h` shows: `off` draws nothing; `overlay` goes over the picture at the anchor's place; `border` goes in a bar when one has room, else to the overlay's place for that anchor. `narrow`, and widescreen off, keep the place right of the compass exactly as before.
- **The corner** (`minimap_layout(mode, anchor, image, wide, visible, …)`): the box is a margin in from the top left of the visible view (`minimap_visible`: the part of the wide picture the engine draws, the screen image on a narrow map), clamped into the safe area. It is at most `kMinimapCornerW` × `kMinimapCornerH` (150 × 48) points, ends by y = 52 of the screen image, and is narrowed if needed so that the moved compass ends by `kMinimapCornerRight` (180). All of it is in drawable pixels from the placement, so the window's aspect, the render scale, the scale mode and frame interpolation change nothing in screen points.
- **The compass moves** by `compass_dx` (screen points, ceil(box right + 4)). At each composite `game.cpp` puts a host transform on the button (`minimap_move_compass`, ADR 0086's `mt_uikit_set_host_transform`: drawn there, the guest's frame and hit testing untouched). This happens only while the game pad is the panel in use and the key pad editor is closed. Otherwise the transform is removed.
- **Touches follow it.** `Input::set_redirects` (new): a touch that begins in a redirect's `from` is moved into its `to` for its whole life. The corner gives two (`minimap_touch_redirects`): the moved compass to the button's frame, and the box to the button's frame. The second covers the part of the frame still under the box. So a tap on the moved compass, or on the minimap, opens the map screen, the same way for every point of the box.
- **A frame**: a 1-point edge (`kMinimapEdge`, translucent) just outside the box in every placement (`minimap_frame`). The map's name is not drawn: the frontend has no cheap way to the game's localised map names.

**Consequences.**

- With `overlay` (or `border` without a bar) and widescreen's edges, the minimap is in the visible view's top left corner, the compass right of it, and nothing of the game pad is under it. With the compass moved, shots and captures show it at its new place; the minimap itself is still never in them. With `minimap = off`, `narrow` or widescreen off, nothing changes.
- A tap on the minimap opens the map screen. NPCs under the box can't be tapped there (the box covers them anyway).

**Evidence.** `platform_test`: `minimap: the anchor, and the corner with the key pad at the edges` (the anchor table; the visible view; 16:9 with map 2.3's size; compass and narrow unchanged; border's bar on 21:9 else the corner; 3:2, 16:9, 16:10, 21:9, taller than 3:2, 1280 × 720, a notched phone's safe area, at render scales 1–3, integer, fit and smooth, views 568, 540 and 480, four map shapes: the box at the visible top left inside the safe area, at most 150 × 48 points and above y = 52, the moved compass and the frame never overlapping), `input: redirects move a touch that begins in one into its target for its life`. Real assets, `play.minimap_wide` (local, Debug arm64, 2026-11-04): first_map's way to 2.3 at 568 points with the key pad at the edges, then 600 ticks of the hunt. The box was at the visible view's top left in all 49,715 wide field turns, the compass 60 points right. In 2,486 composites of the game pad alone across the wide view, no pixel lay under the box or its frame. With the compass unmoved, 1,531 pixels did. A tap on the moved compass and one on the box in the left wing each opened the map screen. In the shots the "… Obtained!" message begins just below the frame.
