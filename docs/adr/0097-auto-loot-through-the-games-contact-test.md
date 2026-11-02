# 97. Auto-loot: the game's own pick-up, with its contact test widened by a guest hook

- **Status:** Accepted
- **Date:** 2026-11-02
- **Issue:** #175
- **Builds on:** [ADR 0069](0069-guest-function-hooks.md) (guest function hooks), [ADR 0070](0070-settings-registry-and-presets.md) (the registry, Assists), [ADR 0092](0092-quality-of-life-assists-and-ui-animations.md) (the other Assists, `MAPLETHIEF_SETTINGS`)

## Context

#90 left auto-loot as a follow-up ([re/qol.md](../re/qol.md)): the game picks drops up on contact, and moving drops or calling the pick-up from the host would change the drop list the game walks each tick. #175 asks for the research first, and for an Assist only if it can go through the game's own pick-up.

The decompile of `CDropItems::Update` `0x5BC75` ([re/qol.md](../re/qol.md#auto-loot)) shows one place where contact is decided: for each lying drop, the call at `0x5BCEE` to `CMapleObject_Touches` `0x6CF01` (the hero, then the drop's box as left, top, right and bottom, the last on the stack). Everything after a yes is the game's: meso is always taken, an item only if `CHero::isAddInven` `0x6003D` finds room (else the drop is marked refused and stays), then the drop flies to the hero and `CHero::addInven` `0x64309` takes it (stacks, the quest check, the sound). `0x6CF01` has five other callers (the hero's and the mobs' touches).

## Options considered

1. **Move the drops to the hero** (host writes to `x`, `y`). The drops' boxes and positions are the game's state, drawn and saved; a drop moved through a wall or onto another floor looks wrong, and the write races the update. Rejected.
2. **Call the pick-up from the host** (`isAddInven` + set the state, or `addInven`). This copies the update's branches (meso, refusal, the 3-tick wait, the life count) or skips them, and changes the list while the game walks it. Rejected; #90's verdict.
3. **A hook on `CDropItems::Update`** that enlarges each drop's width and height (`+0x38`, `+0x3C`) for the call and restores them. The box is also used for drawing and the flight; the writes and restores have to cover every exit of a 1 kB function. Rejected.
4. **A hook on `0x6CF01`** that, for the drop update's call only (its return address `0x5BCF3`), widens the rect it is asked about and then runs the original test. **Chosen.** No guest state changes: only the rect argument of one call (two registers and the outgoing stack word, restored after the call). The game decides everything else exactly as when the hero walks onto the drop.

**The reach.** 160 points left and right (a third of the 480-point screen) and 32 up and down (two map tiles): drops on the hero's floor around it, not the platform above or below. The drop then flies to the hero by the game's own code (at least 8 points a tick vertically, the horizontal distance spread over the same ticks).

**Where.** Only in the field, as the other Assists: `CStateGame`, game mode 0, no popup, the key pad's panel 0, the hero alive. The game itself runs the drop update in game mode 0 only and skips it under some popups (`CStateGame::Paint` `0x87501`); with any popup, auto-loot leaves the game's test as it is.

## Decision

- **`recomp/hooks.py`**: `0x6CF01` `CMapleObject_Touches` is hookable (`MT_HOOK_FN_OBJECT_TOUCHES`).
- **`platform/src/assists.{h,cpp}`**: `install_auto_loot(on)` registers the handler (on) or removes it (off). The handler passes every call whose return address is not `kDropContactReturn`, and every call outside the field. Otherwise it widens the rect by `kAutoLootReachX` / `kAutoLootReachY`, calls the original, restores the stack word, and counts the tests that met only because of the reach (`auto_loot_stats()`).
- **Row** `auto_loot = off | on` (Assists, `--auto-loot`, `off` in every preset column, `Applies::kAtOnce`): `game.cpp` installs it at start, `apply_live_setting` when the settings menu changes it, and the test driver from its settings (so `MAPLETHIEF_SETTINGS="auto_loot = on"` runs a suite with it on).

## Consequences

- With `auto_loot = on`, a drop that lands near the hero flies to it as if the hero had walked over it. Meso, stacks, the inventory-full refusal (and its message, once per drop coming into reach), quest items and the pick-up sound are the game's.
- A refused drop stays on the ground as before. With the reach, its refused mark is cleared only when the hero moves out of reach, not out of contact, so the "inventory full" line shows less often.
- Off, no handler is registered: the hook's entry check is the only difference, and a run is the same byte for byte (`play.first_map` before and after, below).
- Every other caller of `0x6CF01` runs the original test: the hero's and the mobs' touches do not change.

## Evidence

- `platform_test` `assists: auto_loot …` cases (synthetic guest): the reach and the half-open box test, only the drop update's return address widened, the stack argument restored, the guards (popup, popup menu, another state, dead hero, a menu's panel), off is the original.
- Real assets, Release arm64: `play.auto_loot` (after the tutorial, on 2.3): with it off every pick-up was in contact; turned on with the hero standing, the drop 27 points from the hero's box was taken (x unchanged, meso and items up); on, 10 pick-ups, 6 across a gap, none wider than the reach; with both inventories full, drops in reach refused and left on the ground, no item taken, meso still taken.
- `play.first_map` with the setting off on the epic tip (`23d23ca`) and on this branch: the same 1,331 log lines (18,000 ticks, 197 drops, 160 picked up, meso +1,128, items +93) and all 34 output files (screenshots, saves, `play.log`) byte-identical.
- `ctest -L real_assets` with `MAPLETHIEF_SETTINGS="auto_loot = on"` and off ([re/qol.md](../re/qol.md#verification) has the result).
