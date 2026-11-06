# Damage

Damage in MapleStory Thief Edition depends on one number, the hero's ATT, adjusted by the skill used and, for a thrown star, by the star's own attack. Accuracy, critical hits and the target's level come in afterwards ([accuracy, avoid and critical hits](mechanics/accuracy.html)).

## The hero's stats

The hero keeps six numbers that the fight rules read:

| Stat | Where it comes from |
|---|---|
| Level | 1 to 99 |
| ATT | the level's attack in the level table, plus equipment ATT, {{skill:11}} and {{skill:12}} |
| DEF | the level's defense, plus equipment DEF |
| AVOID | the level's avoidability, plus equipment AVOID and {{skill:4}} |
| ACC | the level's accuracy, plus equipment ACC, {{skill:4}} and Concentration |
| Critical rate | 2% before the first job advancement, 8% after, plus {{skill:6}} or {{skill:15}} |

Wearing an item adds all six of its stats (ATT, DEF, ACC, AVOID, HP and MP); taking it off removes them. HP and MP from equipment raise the maximums. STR, DEX, INT and LUK appear on the status tab and nothing else reads them: in this game they are for display only.

The level table's attack, defense, accuracy and avoidability columns are read by the code; what each column means was worked out from that code, so the [level pages](skills/index.html) mark them probable.

**Example:** a level 10 hero (level table ATT 14) wears a dagger with 8 ATT and has nothing else. The hero's ATT is 22.

## Attacks, targets and stars

The attack key starts the current skill. A new attack can start once the previous one has run for the weapon's interval: 2, 4 or 6 game ticks for weapon speed 1, 2 or 3, which is 0.1, 0.2 or 0.3 seconds at the default game speed.

| Skill | Hit area | Targets | Hits | Stars used |
|---|---|---|---|---|
| {{skill:0}} (dagger swing or claw punch) | a box 60 by 24 pixels in front | 1 | 1 | 0 |
| {{skill:1}} (claw) | a star flying up to 160 pixels | 1 | 1 | 1, or 2 with {{skill:14}} |
| {{skill:2}} | two stars, four with Shadow Partner | 1 each | 1 each | 2 (4) |
| {{skill:3}} | a box 60 by 24 | 1 | 2 | 0 |
| {{skill:7}} | one star | 1 | 1 | 1 |
| {{skill:10}} | a large star that flies through monsters | the skill's value | 1 each | 3 |
| {{skill:13}} | a box 150 by 24 | up to 6 | none (it binds) | 0 |
| {{skill:16}} | a box 60 by 24 | 1 | the skill's value | 0 |
| {{skill:23}} | a box 150 by 24 | the skill's value | 2 each | 0 |

"The skill's value" is the per-level value the skill window shows for it ([skills and buffs](mechanics/skills.html)).

- **Weapons.** Star skills need a claw and Bandit skills need a dagger; the game refuses the skill otherwise. A dagger's basic attack picks one of two swing animations at random, and both hit the same way.
- **The claw punch.** With a claw, the basic attack punches instead of throwing when a monster is right next to the hero or the hero has no stars. A punch always does 1 damage.
- **Stars.** A star skill needs as many stars as it throws. They come from the first stack that holds enough, and that stack's star sets the damage bonus. An empty stack stays in the inventory with a count of 0, ready to be recharged at a shop.
- **Shadow Web** does no damage: each monster in its box is bound and cannot move for the skill's value in ticks.

**Example:** a claw Assassin with Shadow Partner on uses Lucky Seven. Four stars leave, four damage rolls are made against the one target, and four stars come off the first stack that has at least four.

## The damage roll

One hit's damage, before evade, miss and critical, follows six steps. A is the hero's ATT.

1. A claw punch does 1 damage; nothing else applies.
2. If the hit is a thrown star ({{item:etc:11}} to {{item:etc:18}}), the star's attack is added to A.
3. The base is x = A × (A / 30 + 3) / 5, with A / 30 rounded down first.
4. The skill adjusts x, as in the table below. For an attack skill, p is its damage percentage at its level: the base value plus the per-level value times the skill level.
5. If x is now below 0, it becomes 10.
6. A roll r from minus x / 3 to x / 5 is added, and the result is multiplied by 110 / 100.

| Skill | x becomes |
|---|---|
| {{skill:0}} with a dagger, and every skill not listed | x, unchanged |
| {{skill:1}} | x minus a tenth of x (no p) |
| {{skill:2}} | x + x × p / 100, minus 5 |
| {{skill:3}} | x + x × p / 100, minus 3 |
| {{skill:7}} | x + x × p / 100, plus 2 |
| {{skill:10}} | x + x × p / 100, plus 4 |
| {{skill:16}} | x + x × p / 100, plus 2 |
| {{skill:23}} | x + x × p / 100, plus 3 |

So one hit lands anywhere from about 0.73 x to 1.32 x, every value equally likely.

Edge cases:

- **Skills hit one level higher than the window shows.** The damage uses the skill's full level, while the skill window's damage figure uses the level minus one. A level 5 skill hits with the percentage the window lists for level 6.
- **The percentage is added on top.** A skill whose window says 60% damage does 160% of the base, plus its constant.
- **x of exactly 0** stays 0, and the hit then misses (its damage is below 1). Only a negative x is lifted to 10, which takes a flat penalty (Lucky Seven's 5, Double Step's 3) on a very small ATT.
- **The star bonus** counts for star hits only; a claw's own ATT is already part of A.

**Example** (made-up numbers): A is 40 and the hero swings a dagger. A / 30 is 1, so x = 40 × 4 / 5 = 32. The roll runs from minus 10 to 6, so the damage is from 22 × 110 / 100 = 24 to 38 × 110 / 100 = 41.

**Example:** the same hero throws a 4-attack star with a claw (the basic throw), so A is 44. x = 44 × 4 / 5 = 35, less a tenth (3) is 32: the same 24 to 41.

**Example:** a level 5 Double Step (p = 60 + 5 × 5 = 85) at A = 40. x = 32 + 32 × 85 / 100 minus 3 = 32 + 27 minus 3 = 56, so each of its two hits does (56 minus 18) × 110 / 100 = 41 to (56 + 11) × 110 / 100 = 73.

## Calculator

The calculator applies the rules on this page and on [accuracy, avoid and critical hits](mechanics/accuracy.html). Choosing a level fills in that level's ATT, ACC, DEF and AVOID from the level table, and choosing a weapon fills in its fixed ATT (the stat a bought or dropped weapon has); change any number to match a rolled or scrolled item. Other ATT is for the rest of the equipment and for Weapon Mastery and Concentration. The mean damage per attempt counts the monster's evade, the miss chance and critical hits.

## Sources

`CHero::RollDamage` `0x64DA1` (the damage roll, called only by `CMob::TakeHit` `0x73715`), `CSkill::DamageValue` `0x7D551` (the window's damage figure), `CHero::onAttack` `0x605FD` and the interval table at `0x117140`, `CSkill::SpawnAttack` `0x7D781` (hit areas), `CSkill::setActiveSkill` `0x7EFF1` (weapon checks), `0x7E9A1` (the two swings), `CHero::isSpear` `0x5FF3D` and `CHero::UseStars` `0x67005` (stars), `CSkill::ShadowWeb` `0x7EB8D`, `CHero::AddEquipStats` `0x5F89D` and `CHero::RemoveEquipStats` `0x5F979` (equipment), the status tab `0x56321`. The real-asset test `play.mechanics` checked every hit of a 60-kill hunt against these rules.
