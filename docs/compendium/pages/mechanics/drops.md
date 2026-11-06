# Drops and meso

Every kill of a normal monster gives its EXP, counts for hunting quests and rolls its drops: one piece of equipment, up to three other items and a bag of meso, each rolled on its own. The odds on the monster pages are the real odds of the game's random numbers, which for equipment are about three times what the monster table says.

## Random numbers

Every roll in the game comes from one generator, the MSVC `rand()` sequence, which gives a number from 0 to 32,767 (15 bits). Three helpers build on it:

- **a range** from lo to hi takes the number modulo (hi minus lo plus 1) and adds lo, so each value from lo to hi comes up. If hi is below lo, the result is always lo;
- **n chances in d** succeeds when the number modulo d, plus 1, is at most n;
- **a percentage** p is p chances in 100. A p of 0 or less never succeeds; 100 or more always does.

**The 15-bit quirk.** Since the number never exceeds 32,767, a chance with a denominator above 32,768 is not what it looks like. n chances in 100,000 are really n in 32,768, about three times the intended odds. n chances in 1,000 are about n in 993: the remainders 0 to 767 come up 33 times in 32,768 and the rest 32 times. Chances in 100 are within 0.1% of fair.

The game seeds the generator once when it starts; nothing about the rolls depends on the save.

**Example:** a chance of 300 in 100,000 (0.3% on its face) is 300 in 32,768, or 0.92%. A chance of 40 in 1,000 is 1,320 in 32,768, or 4.03%.

## Drops

A boss starts its scene when it dies instead. Any other monster, outside [Monster Carnival](mechanics/monster-carnival.html), rolls in this order:

1. **Equipment**, when the monster has an equipment chance: (the hero's level + the chance) in 100,000, which the 15-bit quirk turns into that many in 32,768. A dropped item always has each stat at the table's minimum and 5 upgrade slots. Dropped equipment never has random stats or the bonus stat.
2. **Other items**, for each of the monster's three item slots with a chance: (the hero's level / 10 + the chance) in 1,000, one item each.
3. **Meso:** an amount from 80% to 120% of the monster's meso value. If it is above 0, it drops 80% of the time. The bag is {{item:etc:0}} below 100 meso, {{item:etc:1}} from 100 to 199 and {{item:etc:2}} from 200, and it holds the amount.

The drops then scatter around where the monster fell. A monster can drop its equipment, all three items and meso at once.

The odds on the monster and item pages are the real odds before the hero's level bonus; a higher level adds a little to each.

**Where rolled equipment comes from.** Only one place makes equipment with random stats: some quests' rewards, given through the script call `makeEquipItem`. Each of the six stats is rolled between the table's minimum and maximum (the minimum when the maximum is lower), the item's bonus is added to its bonus stat, and the item gets 7 upgrade slots and counts as rolled for [scrolling](mechanics/items.html). Shops, drops and the other script gifts give the fixed version.

**Example** (made-up numbers): a level 12 hero kills a monster with a meso value of 30, an equipment chance of 300, and item slots with chances 930 and 40. The equipment drops with 312 in 32,768, about 0.95% (the table suggests 0.3%). The first item drops with 931 in 1,000, really 30,560 in 32,768, about 93%; the second with 41 in 1,000, about 4%. Meso: an amount from 24 to 36, dropped 80% of the time, as the bronze bag.

## Sources

`Rand_Next` `0x225E5`, `Rand_Range` `0x22641`, `Rand_Chance` `0x22685` and `Rand_Percent` `0x226B9` (names from `mtport.timing`); `CMob::Die` `0x73349`, `Quest_CountKill` `0x7B5A9`, `RollMobDrops` `0x5CAD9`, `SpreadDrops` `0x5C201`, `DropEquip` `0x5C8ED`, `DropEtc` `0x5C699`, `DropMeso` `0x5C3C1`; `CItem::InitEquipFixed` `0x69B5D`, `CItem::InitEquipRolled` `0x69BB9` (behind `Quest_MakeEquipItem` `0x7C2D9`) and `CItem::InitEtc` `0x699A1`. The real-asset test `play.mechanics` checked every drop of a 60-kill hunt against these rules.
