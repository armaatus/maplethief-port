# Potions and scrolls

Using an item from the inventory runs one of a few rules, picked by the item's type in the item table. A stack that reaches 0 is removed, except throwing stars, which stay at 0 until recharged.

## Potions

A potion restores its flat HP and MP. {{item:etc:9}} and {{item:etc:10}} restore that percentage of the maximum instead. HP stops at the maximum.

**Example:** a hero with 30 of 110 HP drinks {{item:etc:3}}, which restores 50 HP, and has 80 HP. An Elixir at the same point restores half the maximum (55) and gives 85 HP.

## Return scrolls

A return scroll moves the hero to a town:

| Scroll | Goes to |
|---|---|
| {{item:etc:24}} | the nearest town: area 9 from area 10, area 6 from 7 or 8, area 4 from 5, otherwise the current area's town |
| {{item:etc:25}} to {{item:etc:28}} | areas 0, 1, 2 and 3 |
| {{item:etc:29}} to {{item:etc:31}} | areas 4, 6 and 9 |
| {{item:etc:88}} | area 10 |

**Example:** a hero in {{area:7}} reads {{item:etc:24}} and arrives in the town of area 6, {{map:6-0}}.

## Upgrade scrolls

An upgrade scroll works on the equipped item of its slot: weapon ({{item:etc:19}}, {{item:etc:134}}), overall ({{item:etc:20}}, {{item:etc:133}}), hat ({{item:etc:21}}, {{item:etc:137}}), shoes ({{item:etc:22}}, {{item:etc:136}}) or earrings ({{item:etc:23}}, {{item:etc:135}}).

It needs an upgrade slot left on the item and uses one whether it works or not. The chance depends on how many scrolls have already worked on the item:

| Scrolls that worked so far | 0 | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| Fixed-stat item (shop, drop) | 100% | 75% | 50% | 25% | 10% | 0% | 0% |
| Rolled item (a quest reward) | 100% | 80% | 60% | 40% | 30% | 20% | 10% |

The second scroll named for each slot (a 100% scroll) always works. On success all six of the scroll's stats are added to the item. So the first scroll on any item always works, and a fixed-stat item takes at most 5 successful scrolls.

**Example:** a hat with 2 successful scrolls and 3 slots left. The next ordinary scroll works with 50%, and a slot is used either way.

## Special items

- {{item:etc:128}} gives a level-up.
- {{item:etc:129}} resets the skills: every point spent comes back and the stats are recomputed.
- {{item:etc:131}} gives 20% of the current level's need as EXP.
- {{item:etc:132}} gives one skill point.
- {{item:etc:130}} revives the hero from the death popup instead of a return to town ([death and revives](mechanics/death.html)).

Initialize SP and SP Marble came from the [cash shop](mechanics/cash-shop.html); no product grants the other two cash items.

**Example:** at a level whose need is 1,000 EXP, XP 20% Increase gives 200 EXP.

## Throwing stars

A star stack refills at a shop for one star's price × the missing stars, and selling a stack pays its sell price plus one star's price × the stars in it. The item table stores one star's price in tenths of a meso.

**Example:** a stack of 200 {{item:etc:11}} with 40 left, at 5 tenths of a meso a star, costs 5 × 160 / 10 = 80 meso to refill.

## Sources

`CHero::UseItem` `0x66695`, `CHero::AddHP` `0x5F181`, `setMP` `0x601A5`, `ScrollTargetSlot` `0x691E5`, `CItem::ApplyScroll` `0x69511`, `CItem::AddScrollStats` `0x69145`, and the shop's `Shop_OnKey` `0x46DB9` (refills and sales). The star price is the item table's column at `0x54` ([data tables](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/data-tables.md)).
