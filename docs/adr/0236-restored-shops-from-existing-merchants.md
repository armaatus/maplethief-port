# 236. Shops 0, 1 and 16 open from three existing merchants under restored_content

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#426](https://github.com/armaatus/maplethief-port/issues/426)
- **Builds on:** [ADR 0069](0069-guest-function-hooks.md) (guest function hooks; its #379 addendum: restored content is its own setting), [ADR 0070](0070-settings-registry-and-presets.md) (`restored_content`, off in every preset, #389), [ADR 0108](0108-text-packs.md) and [ADR 0222](0222-the-port-ships-its-menus-in-dutch-and-korean.md) (the port's own text)

## Context

`market.dat` has listings for shops 0, 1 and 16 (61, 30 and 7), and the shop screen `0x4628D` opens any shop number, but no script asks for them: the 14 `showMarket` calls, one per merchant script and each its script's whole `main`, open shops 2–9, 11, 13 and 15. Restoring them under `restored_content` needs three decisions the data only half answers: who sells each shop, how the merchant offers it without changing a shipped file, and what to do with the data's oddities (row 146, shop 8's listing filed under shop 0, and the 18 unused all-zero rows, which the screen counts as shop 0's). The facts are in [re/original-bugs.md](../re/original-bugs.md#shops-0-1-and-16-restored-426).

## Options considered

**Who sells them.**

1. **One new NPC for all three.** The game has no such NPC; adding one is inventing content. Rejected.
2. **The merchants of the first town.** Shop 0 would put the level-65 tier, sold nowhere else, in the first town. Rejected.
3. **The data's best fit.** Shops 0 and 1 are full catalogues of every equipment and every consumable shop (supersets at the same prices), so the last town's merchants, `Equip90` and `Etc90` on 9.0, where the hero has every tier's level. Shop 16 is seven ores and gems, six of them the chapter-3 smith's materials, and the smith stands on 0.0: that map's `Etc00`, on the etc side as its number (odd shops are etc shops up to 15) suggests. Chosen.

**How the merchant offers it.**

1. **An in-memory override of the merchant's script at load** (new bytecode with `regSelList` rows and a callback). Needs a Lua 2.4 bytecode writer and new strings in the VM's table, for a script that is one call. Rejected.
2. **A hook on the shop screen** that opens the other shop instead. Gives no choice: the merchant's usual shop would be gone. Rejected.
3. **Hooks on the native `showMarket` `0x998D1` and on `Lua::runCallback` `0x9CE2D`.** The first builds the list box a script builds (`initMsg`, `regMsgNPC`, `regSelList`, `showMsg(2)`) with the chat box's own functions, the second opens the row's shop when the box's OK runs the callback. Shipped files and the guest's scripts stay as they are. Chosen.

**The data's oddities.**

- Row 146 stays in shop 0, as the file has it: moving it to shop 8 would change a reachable shop, a bug fix rather than restored content.
- The 18 unused rows are left out of shop 0: they are the file's padding, not listings (shop 0 would sell a level-10 hat for nothing 18 times). Only the hook's own shop-0 call sees them changed.
- Shop 16's price (10 meso for items the Sell tab buys back for 200–2,000) is kept: it is the data, and `restored_content` is off in every preset. The catalogue records the risk.

## Decision

- **`mt_hooks_install_restored_shops(enabled, texts)`** (`maplethief/hooks.h`, `runtime/src/hooks/restored_shops.cpp`), with two new hooked functions in `recomp/hooks.py`: `ScriptNative_showMarket` `0x998D1` and `Lua::runCallback` `0x9CE2D`. `mt_restored_shop_for(area, map, shop)` is the table: (0.0, shop 5) → 16, (9.0, 8) → 0, (9.0, 13) → 1. The merchant is known by the shop he opens and the map he stands on, so `Equip60` on 6.0, which also opens shop 8, keeps his shop.
- With the setting on, the merchant's talk shows his line and two rows, his usual shop and the restored one; OK on a row opens that shop, Back ends the talk as in any box. A marker at `L + 8` (an image address, never a script function) tells the callback hook the box is its own; a box closed by Back leaves it there, and the next close puts back the value it replaced.
- For its own call that opens shop 0, the hook sets the unused rows' shop byte to `0xFF` and restores the zeros before it returns.
- The texts are the port's (`port_text.def`: `restored_shop_line`, `restored_shop_usual`, `restored_shop_0`, `restored_shop_1`, `restored_shop_16`; Dutch and Korean in `platform/lang/`), in the menu language, drawn by the game's chat box. `platform/src/restored_content.cpp` passes them; `game.cpp` and the tests' boot (`tests/cpp/game_driver.h`) install the hooks from `restored_content`. Off: both hooks are registered disabled and every merchant opens his shop at once, as on the device.

## Evidence

- `play.systems2_restored_shops_0_synthetic`, `play.systems2_restored_shops_9_synthetic` (real assets, local; built starts on 0.0 and 9.0): each merchant's talk shows a two-row list box; the restored row opens shop 16 / 0 / 1 with 7 / 61 / 30 Buy rows and the usual row shop 5 / 8 / 13 with 7 / 14 / 11, each `market.dat`'s listings of that shop in order at their prices (read from the file at run time).
- `play.systems2_restored_shops_0_off_synthetic`, `play.systems2_restored_shops_9_off_synthetic`: the setting off, each merchant opens his shop with no chat box at any turn and no list box built.
- `hooks_test` (CI): the merchant table, and both hooks on or off with the setting.

## Consequences

- The economy changes for a player who turns `restored_content` on: shop 0 sells the level-65 tier, shop 1 four throwing stars and a scroll no other shop has, and shop 16 is a meso source (buy at 10, sell for up to 2,000). Off is the original, and no preset turns it on.
- A merchant's script that changed (a text pack cannot change scripts; a future script override could) would still be recognised only by its shop number and map.
- Shop 8's slip (row 146) stays a slip in every mode; if it is ever fixed, it belongs under `fix_original_bugs` with its own decision.
