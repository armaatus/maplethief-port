# Systems sweep 2

The cash shop's tiles, the four restores, the Reverse, the ten upgrade scrolls, the Pigmy dialog
and `portal620`, played from built start states ([start-states.md](start-states.md),
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

`input.menu_shops_save_auto_potion` runs `input.menu_shops_save` with `auto_potion = 50`: the
stall [qol.md](../re/qol.md) recorded on 2026-11-02 no longer happens.

## The item tab by the pad

Yes on the item tab's questions (an upgrade scroll's `0x65`, a return scroll's 100, a special
item's `0x18`–`0x1B`) uses the item the grid widget has selected (`+0x14`), which only a touch
sets; by the keys it stayed -1 and Yes used nothing. The port's item grid hook now selects the
keys' cursor cell on that OK when no touch selected one (`platform/src/list_keys.cpp`;
`platform_test` "list keys: OK on an item question *"). The scroll runs above go through it.
