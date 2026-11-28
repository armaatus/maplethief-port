# 238. The unreached maps 4.5 and 4.8 restored from a portal on 4.3 under restored_content

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#428](https://github.com/armaatus/maplethief-port/issues/428)
- **Builds on:** [ADR 0069](0069-guest-function-hooks.md) (guest function hooks; its #379 addendum: restored content is its own setting, and a map that is unfinished stays unreachable), [ADR 0070](0070-settings-registry-and-presets.md) (`restored_content`, off in every preset, #389), [ADR 0237](0237-restored-monster-carnival-in-three-towns.md) (the `CMMap::LoadObj` wrapper)

## Context

90 maps ship; chapter 3 reaches 69 of them, all chapters together 79. Of the 21 chapter 3 does not reach, 10 are reached in an earlier chapter, 3 are Monster Carnival's rooms (restored by ADR 0237) and 8 are reached by nothing ([re/original-bugs.md](../re/original-bugs.md#the-unreached-maps-428) lists each). Two of those 8, 4.5 and 4.8, are finished maps: monsters, portals, and portal scripts (`common/portal450`, `451`, `480`, `481`) that lead to each other and out to 4.3 and 4.10, both reached. They form a third road from 4.3 to 4.10, beside the two the game reaches (4.4–4.7 and 4.6–4.9). Only the way in is missing: no reached map has a portal whose script leads to them. The other six have no portal at all, and no script names them.

Restoring 4.5 and 4.8 needs three decisions: where the way in is, what it runs, and how it is placed without changing a shipped file or the warps the game already has.

## Options considered

**Where.**

1. **Change an existing portal's target**, for example 4.3's portal 2 (to 4.6) to 4.5. Takes a reached road away. Rejected.
2. **A portal on 4.10 into 4.8.** 4.10 is where the road ends; the scripts make 4.5 the road's start (its portal 0 returns to 4.3). Rejected.
3. **A portal on 4.3**, where 4.5's own way back leads. 4.3's upper middle platform (tile row 8's floor, columns 16–22) carries a portal statue with no portal; every other such statue on 4.3 and 4.5 stands two or three tiles right of a portal. The portal goes at tile (18, 7), on that floor, three tiles left of the statue and clear of the rope at column 21. Chosen. The tile is the port's call; the data names none.

**What it runs.** `common/portal480` warps to 4.5's portal 0, the arrival point of 4.5's own way back. Naming the added object `portal480` runs that shipped script: no new script, no new target. Chosen over a script of the port's own.

**How it is placed.**

1. **Appended** (as the carnival's NPC is). It would need a portal number: 4.3 uses 0, 1 and 2, and 3 is hidden (not drawn, no hint, not on the minimap). A drawn 0–2 appended would become the arrival point of that number, because the warp's arrival (`0x868D5`) takes the last object of the target portal's kind: the warps to 4.3's portal 2 (from 4.6 and 4.5) would land on the new portal. Rejected.
2. **Kind 3 (hidden), appended.** Nothing targets 4.3's portal 3, but a player could not find it. Rejected.
3. **Kind 2, first in the list.** Drawn, with the "up" hint and a minimap dot, and every warp to 4.3's portal 2 still lands on 4.3's own (the last match). 4.3 has no NPC and no map script, so nothing refers to its objects by index. Chosen.

The wrapper is a second wrapper on `CMMap::LoadObj` `0xD259` (`mt_hook_wrap`, beside Monster Carnival's handler); the list is rebuilt one longer with the guest's `new[]`, as ADR 0237 does, through one shared helper (`runtime/src/hooks/map_objects.cpp`).

**The other maps stay unreachable** (ADR 0069's #379 addendum: a map that is unfinished stays unreachable). 4.16 and 8.5 have no objects; 2.11 holds 19 NPCs side by side with no portal or spawn; 7.6, 8.6 and 10.4 have spawns (and on 7.6, 8.6 a nameless NPC) but no portal; no script or warp names any of them. A way in and out would be a layout of the port's own. The 10 maps reached in an earlier chapter are reached by the game and are not unreachable content.

## Decision

- **`mt_hooks_install_restored_maps(enabled)`** (`maplethief/hooks.h`, `runtime/src/hooks/restored_maps.cpp`), a wrapper on `CMMap::LoadObj`. `mt_restored_portal_on(area, map)` is the table: 4.3, cell `7 · 38 + 18`, kind 2, `portal480`, to (4, 5, 0).
- `platform/src/restored_content.cpp`'s `install_restored_maps` sets it from `restored_content`, from `game.cpp` and the tests' boot. Off: the wrapper is registered disabled; every list is its file's.

## Evidence

- `play.systems2_restored_maps_synthetic` (real assets, local; a level-60 hero of chapter 3 on 4.3): 4.3's list is the added portal, then `43.obj`'s records in order; by the d-pad the hero takes the added portal to 4.5, 4.5's portal 1 to 4.8, 4.8's portal 0 back to 4.5, 4.5's portal 0 back to 4.3 (landing on 4.3's own portal 2), then 4.3 to 4.5 to 4.8 again and 4.8's portal 1 out to 4.10; each landing is the target portal's arrival point.
- `play.systems2_restored_maps_off_synthetic`: the setting off, 4.3's list is `43.obj`'s record for record, its portal 2 still leads to 4.6, and the wrapper added nothing.
- `hooks_test` (CI): the table, and that the wrapper's switch leaves Monster Carnival's handler as it is.

## Consequences

- With `restored_content` on, area 4 has a third road from 4.3 to 4.10, with its own monsters. It is a road like the other two: nothing else in the game refers to it, and no quest needs it.
- The four portal scripts are no longer unused with the setting on; the compendium and ANALYSIS.md describe the shipped game and keep listing them as unused.
- A restore of any of the other six maps would need a layout the data does not give; it is not planned.
