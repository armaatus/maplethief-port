# Systems sweep 2

The cash shop's tiles, the four restores, the Reverse, the ten upgrade scrolls, the Pigmy dialog
`portal620` and a boss killed with the hero knocked back, played from built start states ([start-states.md](start-states.md),
[#304](https://github.com/armaatus/maplethief-port/issues/304)). The tests are `play_real_test`
modes; CTest builds each start (`starts.<name>`, from `tests/starts/<name>.json`) and runs the
mode on it (`play.<mode>_synthetic`, `real_assets`). Every run also passes the common checks of
`play_real_test` (ticks, frames, no trap, no unimplemented call, no error line).

```sh
ctest --preset mac-debug -R "play\.systems2_"
```

| Test | Start | Checked |
| --- | --- | --- |
| `play.systems2_cash_tiles_synthetic` | `systems2_cash`: level-30 Rogue, 3,000,000 meso, 50 Red Potions, one Reverse | `cash_shop = meso` ([ADR 0072](../adr/0072-offline-cash-shop-and-revives.md)): each of the 8 tiles tapped once by the focus. Initialize SP and SP Marble each give their item for their meso price; EXP Explosion 40 % and 80 % add 40 % and 80 % of the level's need (80 % carries into the next level); the four meso packs are not sold and cancel with no change. Then a fall and Comeback Pack on the death popup: revived in place, 30 Power Elixirs, +40 % EXP, its price paid. Then a fall with the Reverse and Return to town: popups `0x68`, `0x69`, a tenth of the meso gone, the Reverse kept ([mechanics.md](../re/mechanics.md#death)) |
| `play.systems2_cash_restores_synthetic` | `systems2_cash`, plus one synthetic receipt in each receipt list | `cash_shop = off`. `totalrestore` (`exppack1`) in the field: `0x267`, op 4, `0x269`, the list empty, no EXP, the shop opens ([original bug 18](../re/original-bugs.md#18-totalrestore-grants-no-exp-pack)). `crestore` (`InitializeSP`) on the Cash button: `0x25D`, the receipt sent twice, two Initialize SP ([original bug 17](../re/original-bugs.md#17-a-crestore-receipt-is-granted-twice)). `riprestore1` (`powpackage`) and `riprestore2` (`ComebackPack`) on the death popup: revived in place, 30 Power Elixirs, +40 % EXP for the second, free. The fake server: 5 confirmations for 4 receipts |
| `play.systems2_scrolls_success_synthetic` | `systems2_scrolls_success`: level-30 Bandit wearing hat 4, dagger 28, overall 16, earrings 64, shoes 53, each with 2 slots and no success; each scroll once | All ten scrolls from the item tab by the pad: each target slot, the 100 % chance, popups `0x65` and `0x66`, one slot used, the scroll's six stats added to the item and to the hero, one scroll gone |
| `play.systems2_scrolls_failure_synthetic` | `systems2_scrolls_failure`: the same items with 5 successes; each scroll twice | The five type-3 scrolls fail (0 %): a slot used, no stats; the five type-8 ones still succeed. Then with no slot left scrolls 19 and 133 are refused with popup `0x67`, nothing used; a scroll's sub-list has two rows, so no quick slot can hold it |
| `play.systems2_pigmy_synthetic` | `systems2_pigmy`: level-30 Rogue with a Pink Pigmy Egg | No map places the Pigmy NPCs, so the run calls `CStateGame::SetPigmeState` itself. State 1, row 1 (hatch) leads to 6, whose Accept closes as Refuse (the box's row is still 1; [original-bugs.md](../re/original-bugs.md#unreachable-content)): no connect. From state 7: 0xF, one connect refused by the network stub, no frame, 0x11, closed. The egg stays |
| `play.systems2_portal620_synthetic` | `systems2_portal620`: level-60 Bandit of chapter 2 on map 6.2 | Up at `portal620` leads to 6.1 with the hero still at its 6.2 coordinates, (41, 751), not at `portal611` ([original bug 14](../re/original-bugs.md#14-commonportal620-warps-to-a-portal-that-does-not-exist)); alive in the field |
| `play.systems2_boss_knockback_synthetic` | `systems2_boss_knockback`: level-70 Bandit of chapter 2 on map 9.5 (mob 68's map) | Mob 68 held at 1 HP and killed by the bot; once its end scene is queued (game mode 10) the run sets `CHero +0x119` and `+0x108`. With `fix_original_bugs` off, after 60 s of A every 0.6 s still game mode 9 with `+0x119` set ([original bug 19](../re/original-bugs.md#19-a-boss-killed-while-the-hero-is-knocked-back-locks-its-end-scene)) |
| `play.systems2_boss_knockback_fixed_synthetic` | `systems2_boss_knockback` | The same with `fix_original_bugs` on ([#420](https://github.com/armaatus/maplethief-port/issues/420)): the scene starts with `+0x119` clear and A plays it through to the field (3.4 s) |
| `play.systems2_restored_shops_0_synthetic` | `restored_shops_0_0`: level-30 Rogue of chapter 3 on 0.0 | `restored_content` on ([#426](https://github.com/armaatus/maplethief-port/issues/426), [original-bugs.md](../re/original-bugs.md#shops-0-1-and-16-restored-426)): talking to `Etc00` shows a list box of two rows (shops 5 and 16); the restored row opens shop 16 with its 7 `market.dat` listings in order at their prices, the usual row shop 5 with its 7 and, after them, Monster Carnival's ticket A (`item_etc` 47, 300 meso; [#427](https://github.com/armaatus/maplethief-port/issues/427)) |
| `play.systems2_restored_shops_0_off_synthetic` | `restored_shops_0_0` | `restored_content` off: `Etc00` opens shop 5 at once, no chat box at any turn, no list box built |
| `play.systems2_restored_shops_9_synthetic` | `restored_shops_9_0`: level-60 Assassin of chapter 2 on 9.0 | `restored_content` on: `Equip90`'s list box opens shop 0 (61 listings; the file's 18 unused all-zero rows left out) or shop 8 (14); `Etc90`'s shop 1 (30) or shop 13 (11) |
| `play.systems2_restored_shops_9_off_synthetic` | `restored_shops_9_0` | `restored_content` off: `Equip90` and `Etc90` open shops 8 and 13 at once, no chat box, no list box |
| `play.systems2_restored_carnival_synthetic` | `restored_carnival_0_0`: level-25 Rogue of chapter 1 on 0.0 with `item_etc` 47 | `restored_content` on ([#427](https://github.com/armaatus/maplethief-port/issues/427), [original-bugs.md](../re/original-bugs.md#monster-carnival-restored-427)): 0.0's live list is `00.obj`'s plus kind 42 `Spiegelman1` at tile (14, 19); shop 5 lists the ticket last; the NPC's first row and Accept start mode 0 in room 0.5 with the ticket taken; monsters in the room (33 at most), the score from 0, the round runs out, the result box closes, back on 0.0 with the band's 250 EXP; no Hangul in the NPC's pages |
| `play.systems2_restored_carnival_off_synthetic` | `restored_carnival_0_0` | `restored_content` off: 0.0's live list is `00.obj`'s record for record, shop 5 has its 7 listings, the wrapper wrote nothing |
| `play.systems2_restored_carnival_4_synthetic` | `restored_carnival_4_0`: level-40 Rogue of chapter 2 on 4.0 with `item_etc` 48 | `Spiegelman2` at (20, 21), ticket B in shop 11; mode 1 in room 4.18 (no objects: the hero keeps the town's x, on the floor), 2,700 EXP for a score of 0 |
| `play.systems2_restored_carnival_6_synthetic` | `restored_carnival_6_0`: level-55 Rogue of chapter 3 on 6.0 with `item_etc` 49 | `Spiegelman3` at (31, 6), ticket C in shop 15; mode 2 in room 6.3, 4,500 EXP for a score of 0 |
| `play.systems2_restored_maps_synthetic` | `restored_maps_4_3`: level-60 Rogue of chapter 3 on 4.3 | `restored_content` on ([#428](https://github.com/armaatus/maplethief-port/issues/428), [original-bugs.md](../re/original-bugs.md#the-unreached-maps-428)): 4.3's live list starts with kind 2 `portal480` at tile (18, 7), then `43.obj`'s records; by walking, the added portal to 4.5, 4.5's portal 1 to 4.8, 4.8's portal 0 back to 4.5, 4.5's portal 0 back to 4.3 (on 4.3's own portal 2), then on to 4.8 again and its portal 1 to 4.10; each landing at the target portal's arrival point |
| `play.systems2_restored_maps_off_synthetic` | `restored_maps_4_3` | `restored_content` off: 4.3's live list is `43.obj`'s record for record, its portal 2 leads to 4.6, the wrapper added nothing |

`input.menu_shops_save_auto_potion` runs `input.menu_shops_save` with `auto_potion = 50`: the
stall [qol.md](../re/qol.md) recorded on 2026-11-02 no longer happens.

## The item tab by the pad

Yes on the item tab's questions (an upgrade scroll's `0x65`, a return scroll's 100, a special
item's `0x18`–`0x1B`) uses the item the grid widget has selected (`+0x14`), which only a touch
sets; by the keys it stayed -1 and Yes used nothing. The port's item grid hook now selects the
keys' cursor cell on that OK when no touch selected one (`platform/src/list_keys.cpp`;
`platform_test` "list keys: OK on an item question *"). The scroll runs above go through it.
