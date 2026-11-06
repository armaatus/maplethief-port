# Accuracy, avoid and critical hits

A hit's [damage roll](mechanics/damage.html) is only the start: the monster may evade it, the hit may miss, and a lucky roll doubles it. The same kind of checks decide whether a monster's touch hurts the hero, and how much.

## Evade, miss and critical hits

Each hit of the hero passes three checks, in this order.

1. **The monster evades** with a chance in percent of the monster's level minus the hero's ACC. The monster shows MISS. The hit still counts as having reached a target, so it is used up: a star stops and a piercing attack loses one of its targets. A hero whose ACC is at least the monster's level is never evaded.
2. **The hit misses** if its damage is below 1, or with a chance in percent of half the monster's level minus the damage. MISS shows and nothing else happens. Once the damage reaches half the monster's level, a hit never misses.
3. **A critical hit** comes with a chance of the hero's critical rate in percent. It doubles the damage, and the phone vibrates.

Then the monster loses the HP.

A chance of 0% or less never succeeds and one of 100% or more always does.

**Example** (made-up numbers): a level 30 monster, a hero with 22 ACC, a 9-damage hit and an 8% critical rate. The monster evades 30 minus 22 = 8% of the time; the hit misses 30 / 2 minus 9 = 6% of the time; 8% of the hits that land are critical, for 18 damage.

## Monster damage and the hero's avoid

A monster hurts the hero by touching. Each tick, the game checks every monster whose box touches the hero's:

- While {{skill:5}} is on, contact does nothing.
- A {{monster:65}} that touches the hero explodes: it dies and deals its damage roll.
- Otherwise the hero first tries to avoid it, with a chance of (the hero's AVOID minus the monster's level) × 4 in percent. An avoided touch deals 0 and shows MISS.
- If not avoided, the hero takes the monster's damage roll.

**The damage roll.** L is the monster's level and D the hero's DEF.

1. d = L × L / 30 + 2 × L + 20, minus D × 7 / 3.
2. If d is below 0, the roll is from 1 to L / 2.
3. Otherwise the roll is d plus a number from minus (2 + d / 4) to d / 4 + 2.
4. The damage is the roll × 90 / 100.

A high DEF therefore never makes a touch harmless: the damage only falls to between 0 and about 0.45 L.

**Bosses' attacks.** A boss's skill attack deals twice the damage roll and cannot be avoided. Each attack hits at most once.

**Hazard maps.** In the Abandoned Mine ({{area:10|area 10}}; the rule also names an area 11, which no shipped map has), the hero takes 300 damage every 50 world ticks (2.5 seconds), unless wearing {{item:equip:73}}.

**Example** (made-up numbers): a level 20 monster against 15 DEF. d = 400 / 30 + 40 + 20 minus 105 / 3 = 13 + 40 + 20 minus 35 = 38. The roll is 38 plus minus 11 to 11, so 27 to 49, and the damage is 24 to 44. A hero with 24 AVOID avoids the touch (24 minus 20) × 4 = 16% of the time.

## Taking damage

**The hero.** With {{skill:22}} on, the skill tries to pay meso for the blow: (damage / 2) × the skill's value / 100 meso. If the hero has that much, the damage is halved. HP then drops by the damage, and at 0 the hero [dies](mechanics/death.html). Unless on a ladder, the hero is knocked back, away from the hit at 1.5 times the jump's sideways speed and up at half the jump's push. A short invulnerability follows; an avoided touch starts a shorter one.

**A monster.** After the three checks above:

- {{skill:7}} heals the hero by the damage × the skill's value / 100, up to the maximum HP.
- With {{skill:19}} on, the hit drops meso with a chance of the skill's value in percent: an amount from the monster's meso / 18 minus 5 to its meso / 18, which then goes through the usual 80% meso roll ([drops](mechanics/drops.html)).
- HP drops by the damage. At 0 or below the monster dies; otherwise a normal monster is knocked back for a short while and shows its HP bar.

**Example** (made-up numbers): Meso Guard with a value of 80 and a blow of 40 damage. The guard costs 20 × 80 / 100 = 16 meso; a hero with 16 meso or more takes 20 damage instead of 40.

## Sources

`CMob::RollEvade` `0x72639` (from the attack objects `0x766F1` and `0x76AED`), `CMob::TakeHit` `0x73715` (miss, damage, Drain, Pickpocket), `CHero::RollCritical` `0x6039D`, `CHero::CheckMobContact` `0x65F05`, `CHero::RollAvoid` `0x603DD`, `CMob::RollDamage` `0x6F591`, boss attacks `0x6EF6D` checked by `CHero::CheckAttackHit` `0x65DC5`, `CHero::TakeDamage` `0x65B55`, `CSkill::MesoGuard` `0x7EAB1`, `CSkill::Drain` `0x7D4A5`, `CSkill::Pickpocket` `0x7EB35`. The chance functions are `Rand_Percent` `0x226B9` and `Rand_Range` `0x22641`. The real-asset test `play.mechanics` checked the hero's hits and the monsters' contact damage of a 60-kill hunt against these rules.
