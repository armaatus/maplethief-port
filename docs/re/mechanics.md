# The game's rules: damage, hits, drops, EXP, items, skills, death

How MapleStory Thief Edition 1.5 decides what happens in a fight and around it ([#214](https://github.com/armaatus/maplethief-port/issues/214)). Each section names the functions, the inputs, the rule in words, every random roll with its range, the edge cases, and a worked example with made-up numbers. The compendium pages ([#222](https://github.com/armaatus/maplethief-port/issues/222), [#224](https://github.com/armaatus/maplethief-port/issues/224)) are written from this note.

Addresses are armv7 VM addresses; functions carry the Thumb bit. The evidence is the Ghidra decompilation of the 1.5 binary (2026-11-06), the table columns of [formats/data-tables.md](../formats/data-tables.md), and the real-asset trace `play.mechanics` (see [Verification](#verification)), which checks every hit, kill, EXP gain and drop of a 60-kill hunt against the rules below. The names come from `python3 -m mtport.mechanics names` (committed, applied by `scripts/re-analyze`).

All arithmetic is integer arithmetic. Division rounds toward zero, as the game's `___divsi3` does; where the game shifts right instead (which rounds toward minus infinity), the rule says so.

## Contents

- [Random numbers](#random-numbers)
- [The hero's stats](#the-heros-stats)
- [Attacking: hit areas, targets and stars](#attacking-hit-areas-targets-and-stars)
- [The hero's damage](#the-heros-damage)
- [Evade, miss and critical hits](#evade-miss-and-critical-hits)
- [Monster damage and the hero's avoid](#monster-damage-and-the-heros-avoid)
- [Taking damage](#taking-damage)
- [Drops](#drops)
- [EXP and level-up](#exp-and-level-up)
- [Monster Carnival (the level-scaling mode)](#monster-carnival-the-level-scaling-mode)
- [Potions, scrolls, return scrolls and special items](#potions-scrolls-return-scrolls-and-special-items)
- [Skills: values, MP, buffs and passives](#skills-values-mp-buffs-and-passives)
- [Movement](#movement)
- [Death](#death)
- [Not found](#not-found)
- [Verification](#verification)

## Random numbers

Every roll goes through four functions (named by `mtport.timing`):

| Function | Address | Gives |
|---|---|---|
| `Rand_Next` | `0x225E5` | the MSVC `rand()` sequence: a number from 0 to 32767 (15 bits) |
| `Rand_Range` | `0x22641` | `(lo, hi)`: lo plus `Rand_Next` modulo (hi − lo + 1), so each value from lo to hi. If hi is below lo, hi is first set to lo, so an inverted range always gives lo. |
| `Rand_Chance` | `0x22685` | `(n, d)`: true when `Rand_Next` modulo d, plus 1, is at most n: n chances in d |
| `Rand_Percent` | `0x226B9` | `(p)`: `Rand_Chance(p, 100)`. p of 0 or less never succeeds; 100 or more always does. |

**The 15-bit quirk.** `Rand_Next` never exceeds 32767, so a chance with a denominator above 32768 is not what it looks like. `Rand_Chance(n, 100000)` is really n in 32768 (about 3 times the intended odds), and `Rand_Chance(n, 1000)` is about n in 993 (the low remainders 0 to 767 come up 33 times in 32768, the rest 32). Denominators of 100 are within 0.1 % of fair. This matters for [equipment drops](#drops).

The game seeds once per process; nothing about the rolls depends on the save.

## The hero's stats

The hero object keeps the stats the rules read (fields of `CHero`):

| Field | Stat | Where it comes from |
|---|---|---|
| `+0x80` | level | 1 to 99 |
| `+0xA4` | attack | `hero.dat` `attack` of the level, plus equipment ATT, Weapon Mastery, Concentration |
| `+0xA8` | defense | `hero.dat` `defense`, plus equipment DEF |
| `+0xAC` | avoidability | `hero.dat` `avoidability`, plus equipment AVOID, Nimble Body |
| `+0xB0` | accuracy | `hero.dat` `accuracy`, plus equipment ACC, Nimble Body, Concentration |
| `+0xB4` | critical rate, % | 2 before the first job advancement, 8 after; plus Critical Throw / Critical Stab |
| `+0x160` | attack interval class | the weapon's `speed` |

`CHero::AddEquipStats` `0x5F89D` adds an item's six stats (base plus bonuses, ATT DEF ACC AVOID HP MP) when it is worn and `CHero::RemoveEquipStats` `0x5F979` takes them off. HP and MP from equipment raise the maximums.

STR, DEX, INT and LUK (`hero.dat` 0x0A to 0x0D) are shown on the status tab (`0x56321`) and used by nothing else: they are display only.

## Attacking: hit areas, targets and stars

The attack key (`CHero::onAttack` `0x605FD`) starts the current skill's action. A new attack can start only when the previous one has run for the weapon's interval: 2, 4 or 6 ticks for weapon `speed` 1, 2 or 3 (table at `0x117140`; 0.10, 0.20 or 0.30 s at the default 20 ticks a second).

`CSkill::SpawnAttack` `0x7D781` creates the attack's hit object for each hero action. Melee boxes are measured in front of the hero, in map pixels.

| Action | Skill | Hit area | Targets | Hits | Stars used |
|---|---|---|---|---|---|
| dagger swing / claw punch | basic attack (0) | box 60 × 24 | 1 | 1 | 0 |
| star throw | basic attack (claw, 1) | a star flying up to 160 px | 1 | 1 | 1, 2 with Shadow Partner |
| Lucky Seven | 2 | 2 stars (4 with Shadow Partner) | 1 each | 1 each | 2 (4) |
| Double Step | 3 | box 60 × 24 | 1 | 2 | 0 |
| Drain | 7 | 1 star | 1 | 1 | 1 |
| Avenger | 10 | a 34 × 34 star that flies through mobs | `@etc` | 1 each | 3 |
| Shadow Web | 13 | box 150 × 24 | up to 6 | none (binds) | 0 |
| Savage Blow | 16 | box 60 × 24 | 1 | `@etc` | 0 |
| Boomerang Stab | 23 | box 150 × 24 | `@etc` | 2 each | 0 |

The skill ids are `skill.dat` rows. `@etc` is the skill's etc value (see [Skills](#skills-values-mp-buffs-and-passives)).

**Weapons.** `CSkill::setActiveSkill` `0x7EFF1` lets a skill be used only with the weapon its branch needs: a claw (`weapon_type` 1) for star skills, a dagger (2) for Bandit skills. The refusal texts are the `CSkill.setActiveSkill.N` strings. With a dagger the basic attack picks one of two swing animations at random (`0x7E9A1`); both hit the same way. With a claw, the basic attack punches instead of throwing when a mob is right next to the hero or when the hero has no stars; a punch always does 1 damage.

**Stars.** A star skill needs as many stars as it uses (`CHero::isSpear` `0x5FF3D` counts all stacks). `CHero::UseStars` `0x67005` takes them from the first stack that holds enough, and that stack's star decides the damage bonus. An empty star stack stays in the inventory with count 0, ready to be recharged at a shop.

**Shadow Web** (`CSkill::ShadowWeb` `0x7EB8D`) does no damage: each mob in the box is bound (mob `+0xD4`) for `@etc` ticks and cannot move.

Example: a claw Assassin with Shadow Partner on throws Lucky Seven: four stars leave, four damage rolls are made against the one target, and four stars are taken from the first stack with at least four.

## The hero's damage

`CHero::RollDamage` `0x64DA1` (hero, skill, star), called only by `CMob::TakeHit` `0x73715`, gives the damage of one hit before evade, miss and critical.

**Inputs:** the hero's attack A (`+0xA4`); the skill and its level; for a star, the star's `item_etc` `attack`.

**Rule:**

1. A claw punch does 1 damage, and the rest is skipped.
2. If the hit comes from a star (star ids 11 to 18), the star's attack is added to A.
3. The base is x = A × (A / 30 + 3) / 5. (A / 30 is rounded down first.)
4. The skill adjusts it. For the attack skills, p = `damage_base` + `damage_per_level` × skill level (the skill's damage percentage), and x gains x × p / 100 plus a constant:
   - basic star throw (1): x minus a tenth of x (no p);
   - Lucky Seven (2): x × p / 100 − 5;
   - Double Step (3): x × p / 100 − 3;
   - Drain (7): x × p / 100 + 2;
   - Avenger (10): x × p / 100 + 4;
   - Savage Blow (16): x × p / 100 + 2;
   - Boomerang Stab (23): x × p / 100 + 3;
   - the basic attack (0, the dagger swing) and every other skill: x unchanged.
5. If x is now negative it becomes 10.
6. The roll: r = `Rand_Range`(−(x / 3), x / 5). The damage is (x + r) × 110 / 100.

So one hit lands anywhere from about 0.73 x to 1.32 x, uniformly.

**Edge cases:**

- p uses the full skill level, while the skill window's `@dmg` (`CSkill::DamageValue` `0x7D551`) uses level − 1. A level 5 skill hits with the percentage the window shows for level 6.
- The percentage is added on top of the base: a skill whose window says 60 % damage does 160 % of the base, plus its constant.
- x of exactly 0 stays 0 and the hit then [misses](#evade-miss-and-critical-hits) (damage below 1). Only a negative x is lifted to 10, which can only happen with a skill's flat penalty on a tiny attack (Lucky Seven's −5, Double Step's −3).
- The star bonus applies to star hits only; a claw's own ATT is already in A.

**Example** (made-up numbers): A = 40 and a basic dagger swing. A / 30 = 1, so x = 40 × 4 / 5 = 32. The roll is from −10 to 6, so the damage is from 22 × 110 / 100 = 24 to 38 × 110 / 100 = 41. With a 4-attack star thrown by a claw (basic throw), A = 44: x = 44 × 4 / 5 = 35, minus 3 is 32, the same range. A level 5 Double Step (p = 60 + 5 × 5 = 85) at A = 40: x = 32 + 32 × 85 / 100 − 3 = 32 + 27 − 3 = 56, so each of its two hits is from (56 − 18) × 110 / 100 = 41 to (56 + 11) × 110 / 100 = 73.

## Evade, miss and critical hits

One hit goes through three checks, in this order.

**1. The mob evades** (`CMob::RollEvade` `0x72639`, from the attack objects `0x766F1` and `0x76AED`): `Rand_Percent`(mob level − hero accuracy). On success the mob shows MISS; the hit still counts as having reached a target, so it is used up (a star stops, a pierce count drops). A hero whose accuracy is at least the mob's level is never evaded.

**2. The hit misses** (in `CMob::TakeHit` `0x73715`): if the damage is below 1, or `Rand_Percent`(mob level / 2 − damage) succeeds, MISS is shown and nothing else happens. Big hits are never missed: once the damage reaches half the mob's level, the chance is 0.

**3. Critical** (`CHero::RollCritical` `0x6039D`): `Rand_Percent`(hero critical rate). On success the damage is doubled and the device vibrates.

Then the mob loses the HP (see [Taking damage](#taking-damage)).

**Example:** a level 30 mob, hero accuracy 22, a 9-damage hit, critical rate 8. Evade: 30 − 22 = 8 %. Miss: 30 / 2 − 9 = 6 %. Critical: 8 %, for 18 damage.

## Monster damage and the hero's avoid

**Contact.** `CHero::CheckMobContact` `0x65F05` runs each tick for each mob whose box touches the hero's.

- While Dark Sight (skill 5) is on, contact does nothing.
- A Malady Bomb (mob movement kind 3) that touches the hero explodes: it dies and deals its damage roll.
- Otherwise the hero first tries to avoid: `CHero::RollAvoid` `0x603DD` succeeds with `Rand_Percent`((hero avoidability − mob level) × 4). Avoided contact deals 0 and shows MISS.
- If not avoided, the hero takes the mob's damage roll.

**The damage roll** (`CMob::RollDamage` `0x6F591`). Inputs: the mob's level L (`mob.dat` `level`) and the hero's defense D.

1. d = L × L / 30 + 2 × L + 20 − D × 7 / 3.
2. If d is negative, the roll is `Rand_Range`(1, L / 2).
3. Otherwise the roll is d + `Rand_Range`(−2 − d / 4, d / 4 + 2).
4. The damage is the roll × 90 / 100.

A high defense therefore never makes contact harmless: the damage falls to between 0 and 0.45 L.

**Boss attacks.** Bosses' skill attacks (`0x6EF6D`, checked by `CHero::CheckAttackHit` `0x65DC5`) deal twice the damage roll and cannot be avoided. Each attack hits at most once.

**Hazard maps.** On areas 10 and 11, every 50 world ticks (2.5 s) the hero takes 300 damage, unless wearing the Fire Suit (overall 73).

**Example:** a level 20 mob against defense 15. d = 400 / 30 + 40 + 20 − 105 / 3 = 13 + 40 + 20 − 35 = 38. The roll is 38 plus −11 to 11, so 27 to 49, and the damage is 24 to 44. The hero's avoidability 24 avoids it (24 − 20) × 4 = 16 % of the time.

## Taking damage

**The hero** (`CHero::TakeDamage` `0x65B55`):

- With Meso Guard (skill 22) on, `CSkill::MesoGuard` `0x7EAB1` tries to pay meso for the blow: (damage / 2) × `@etc` / 100 meso. If the hero can pay, the damage is halved.
- HP drops by the damage. At 0 the hero [dies](#death).
- Unless on a ladder, the hero is knocked back: away from the hit at 1.5 times the jump's horizontal speed (hero `+0x28`), and up at half the jump impulse.
- A short invulnerability timer (`+0x11C`) starts; a missed (avoided) contact starts a shorter one (`+0x120`).

**A mob** (`CMob::TakeHit` `0x73715`, after the checks above):

- Drain (7) heals the hero by damage × `@etc` / 100, up to max HP (`CSkill::Drain` `0x7D4A5`).
- With Pickpocket (19) on, `CSkill::Pickpocket` `0x7EB35` drops meso with `Rand_Percent`(`@etc`): an amount from `mob meso` / 18 − 5 to `mob meso` / 18, then the usual 80 % meso roll (see [Drops](#drops)).
- HP drops by the damage. At 0 or below the mob [dies](#drops); otherwise it is knocked back (non-bosses, for a short while) and shows its HP bar.

## Drops

`CMob::Die` `0x73349` handles a death. Bosses (mob ids 64, 66 to 69) start their scene instead. Any other mob, outside [Monster Carnival](#monster-carnival-the-level-scaling-mode):

1. gives its `exp` to the hero (`CHero::setExp`, see [EXP](#exp-and-level-up));
2. counts toward hunting quests (`Quest_CountKill` `0x7B5A9`);
3. rolls its drops (`RollMobDrops` `0x5CAD9`), which are then spread out where it died (`SpreadDrops` `0x5C201`).

The drops are rolled in this order, each independently, so a mob can drop an equip, all three etc items and meso at once.

**Equipment** (`DropEquip` `0x5C8ED`), when `equip_drop_chance` is above 0: `Rand_Chance`(hero level + `equip_drop_chance`, 100000). By [the 15-bit quirk](#random-numbers) this is (hero level + chance) in 32768, not in 100000. The item is built by `CItem::InitEquipFixed` `0x69B5D`: each stat the table minimum, 5 upgrade slots, not rolled. Dropped equipment never has random stats or the bonus stat.

**Where rolled equipment comes from.** Only one place builds an equip with random stats: `Quest_MakeEquipItem` `0x7C2D9`, behind the script call `makeEquipItem(kind, id)`. It finds the quest, takes its required items and meso, and gives each reward equip built by `CItem::InitEquipRolled` `0x69BB9`: each of the six stats `Rand_Range`(min, max), the row's `bonus_value` added to its `bonus_stat`, 7 upgrade slots, and the item marked rolled; then the quest completes. With an inverted range (min above max) the stat is the min. Shops, drops and the script's `getItem` all use the fixed builder; etc items are built by `CItem::InitEtc` `0x699A1`.

**Etc items** (`DropEtc` `0x5C699`): for each of the three slots whose chance is above 0, `Rand_Chance`(hero level / 10 + chance, 1000), about (level / 10 + chance) in 993. One item each.

**Meso** (`DropMeso` `0x5C3C1`): the amount is `Rand_Range`(meso × 8 / 10, meso × 12 / 10). If it is above 0, `Rand_Percent`(80) decides whether it drops. The bag is `item_etc` 0 below 100 meso, 1 from 100 to 199 and 2 from 200; its count is the amount.

Drops are skipped entirely in Monster Carnival.

**Example:** a level 12 hero kills a mob with meso 30, equip chance 300, etc slots (chance 930, chance 40, none). Equip: 312 in 32768, about 0.95 % (the table suggests 0.3 %). Etc slot 1: 931 in 1000, really 30560 in 32768, about 93 %; slot 2: 41 in 1000, about 4 %. Meso: an amount from 24 to 36, dropped 80 % of the time, as the small bag (`item_etc` 0).

## EXP and level-up

**Gaining EXP** (`CHero::setExp` `0x6654D`): the gain is added to the current EXP. While the total reaches the level's need (`hero.dat` `exp`, through `ExpToLevelUp` `0x65919`), the need is subtracted and the hero levels up, so one big gain can give several levels and the rest carries over. "EXP obtained" or "EXP reduced" is printed. A negative total is clamped to 0 (a loss never costs a level). At level 99 EXP stays 0.

**Level-up** (`CHero::LevelUp` `0x65089`):

- the level rises by 1, to at most 99 (`hero.dat` has 100 rows; row 100 is never reached);
- skill points: none for the new levels 2 to 9, 1 for levels 10 to 29, 2 from level 30;
- max HP is recomputed as the sum of `hero.dat` `hp_gain` over levels 1 to the new level (`CHero::MaxHpFromTable` `0x63C3D`), plus HP Mastery's percentage of that sum, plus equipment HP; max MP the same with `mp_gain` (`CHero::MaxMpFromTable` `0x63BB9`) and MP Mastery;
- attack, defense, accuracy and avoidability each rise by the difference between the new and old levels' `hero.dat` values;
- HP and MP are filled;
- the Game Center score becomes (level × 999000 + EXP) / 1000.

**Example:** a level 9 hero with 30 of a 40 need gains 25 EXP. 55 reaches 40, so the hero becomes level 10 with 15 EXP, gets 1 SP, and HP and MP are full. If level 10 needs only 12, a second level-up follows: level 11 with 3 EXP, another SP.

## Monster Carnival (the level-scaling mode)

The mode where monster levels follow the hero is Spiegelman's Monster Carnival. Its flag is `CStateGame +0x404`.

**Start.** The scripts `common/Spiegelman1.out`, `Spiegelman2.out` and `Spiegelman3.out` take a ticket (`item_etc` 47, 48 or 49), call the native `initMonCa` (`0x99545` to `CStateGame::StartMonsterCarnival` `0x82271`) with mode 0, 1 or 2 (`+0x40C`), and move the hero to map 0.5, 4.18 or 6.3. The round lasts 3 minutes (`+0x430` = 180 s).

**Scaling** (in `CMob::LoadStats` `0x73B51`, while the flag is set). Let H = (hero attack + hero defense), halved by a right shift.

- A mob above H drops halfway toward it: level minus (level − H) / 2 (rounded down by a shift), and its HP is raised to 150 %.
- A mob below H is pulled further down, not up: level plus (level − H) shifted right by 1, which is negative. This looks like a sign slip in the original; the HP is unchanged.

The level is what the evade, miss, avoid and damage rules use, so in Carnival a weak hero meets weaker, tougher-skinned mobs.

**Points.** A kill gives no EXP, no drops and no quest progress. It adds the mob's carnival points (`+0x154`, from the spawn table behind the pointer at `0xF13B4`) to the score (`+0x434`) and counts the wave (`+0x410`). A death costs 70, 90 or 110 points (mode 0, 1, 2).

**Reward** (`CStateGame::resultMonCa` `0x819E5`, paid when the result popup is closed). EXP by the final score, from the table at `0xEAD40`:

| Mode (ticket) | Score bands | EXP |
|---|---|---|
| 0 (47) | up to 1500 / up to 2000 / up to 2500 / above 2500 | 250 / 500 / 1000 / 1500 |
| 1 (48) | up to 800 / up to 1400 / up to 2000 / above 2000 | 2700 / 3500 / 4000 / 4700 |
| 2 (49) | up to 2000 / up to 2500 / up to 3000 / above 3000 | 4500 / 6200 / 7000 / 7700 |

The best score of each mode is kept in the data manager (`+0x438`, `+0x43C`, `+0x440`).

**Example:** hero attack 50 and defense 30, so H = 40. A level 60 mob becomes 60 − 10 = 50 with 150 % HP; a level 30 mob becomes 30 − 5 = 25 with normal HP.

## Potions, scrolls, return scrolls and special items

`CHero::UseItem` `0x66695` uses one item by its `item_etc` `type`. When a stack reaches 0 the item is removed, except throwing stars (they stay at 0).

**Potions (type 1).** HP and MP are restored by the item's flat `hp` and `mp`. Elixir (9) and Power Elixir (10) restore that percentage of the maximum instead. HP stops at the maximum (`CHero::AddHP` `0x5F181`); MP goes through `setMP` `0x601A5`.

**Return scrolls (type 4).** They move the hero to an area's town:

| Item | Goes to |
|---|---|
| 24 | the nearest town: area 9 from 10, area 6 from 7 or 8, area 4 from 5, otherwise the current area's town |
| 25, 26, 27, 28 | area 0, 1, 2, 3 |
| 29, 30, 31 | area 4, 6, 9 |
| 88 | area 10 |

**Upgrade scrolls (type 3, and type 8 for the 100 % scrolls).** `ScrollTargetSlot` `0x691E5` picks the equipped item the scroll is for:

| Scrolls | Slot |
|---|---|
| 19, 134 | weapon |
| 20, 133 | overall |
| 21, 137 | hat |
| 22, 136 | shoes |
| 23, 135 | earrings |

`CItem::ApplyScroll` `0x69511` needs an upgrade slot left and always uses one. The chance comes from the item's history: for a fixed-stat item the n-th successful scroll so far gives 100, 75, 50, 25, 10, 0, 0 %; for a rolled item 100, 80, 60, 40, 30, 20, 10 % (indexed by the scrolls already succeeded). Type 8 always succeeds. On success all six of the scroll's stats are added to the item (`CItem::AddScrollStats` `0x69145`) and the count of successes rises.

So the first scroll on any item always works, and a fixed-stat item can take at most 5 successful scrolls (its sixth and seventh chances are 0 %).

**Special items (type 7).** 128 gives a level-up; 129 resets the skills (every point spent comes back, stats are recomputed); 131 gives 20 % of the current level's need as EXP; 132 gives one skill point. Item 130 ("Reverse") revives the hero from the death popup instead of returning to town ([cash-shop.md](cash-shop.md)).

**Stars.** A star stack refills at a shop for its `unknown_84` × (`max_stack` − count) / 10 meso (`Shop_OnKey` `0x46DB9`); selling a stack pays its price plus `unknown_84` × count / 10. So `unknown_84` is the price of one star in tenths of a meso.

**Example:** a hat with 2 successful scrolls (fixed stats) and 3 slots left: the next scroll has 50 %, and a slot is used either way. A stack of 200 Subi with 40 left and value 5 costs 5 × 160 / 10 = 80 meso to refill.

## Skills: values, MP, buffs and passives

Each skill's value columns in `skill.dat` give `base + (skill level − 1) × per_level`:

- **MP cost** (`CSkill::MpCost` `0x7D251`): `mp_base`, `mp_per_level`.
- **`@etc`** (`CSkill::EtcValue` `0x7D325`): `etc_base`, `etc_per_level`, then divided by 10 when positive. The column is in tenths: an `etc_base` of 50 is 5 targets, 5 %, or 5 ticks.
- **`@dmg`** (`CSkill::DamageValue` `0x7D551`): `damage_base`, `damage_per_level` (but see the level used by [The hero's damage](#the-heros-damage)).
- **Duration** (`CSkill::Duration` `0x7EC3D`): `time_base`, `time_per_level`, in ticks, for buffs (type 2) only. 600 ticks is 30 s at the default speed.

**Buffs** (type 2). `CSkill::CastBuff` `0x7F749` pays the MP and adds the buff to the list at skills `+0xD64` (skill id, ticks left). Casting a buff that is already on only refreshes its timer; the stats are not added twice. `CSkill::TickBuffs` `0x7F669` counts the timers down from `CHero::Flush`, and `CSkill::EndBuff` `0x7F581` removes the buff and its stats. The icon blinks in the last 30 ticks (`CSkill::IsBuffEnding` `0x7ED51`).

- Concentration (12 and 21): attack plus `@dmg`, accuracy plus `@etc`.
- Dark Sight (5): the hero turns translucent and contact does no harm; attacking ends it.
- Meso Guard (22), Pickpocket (19), Shadow Partner (14): see the sections above.

**Passives** (type 1), applied when points are learned (`CSkill::LearnLevels` `0x7F99D`, limited by the free skill points and `max_level`):

- Nimble Body (4): accuracy and avoidability plus `@etc`;
- Critical Throw (6) and Critical Stab (15): critical rate plus `@etc`;
- HP Mastery (8, 17) and MP Mastery (9, 18): max HP or MP plus `@etc` % of the table sum;
- Weapon Mastery (11, 20): attack plus `@etc`.

**Example:** a skill with `mp_base` 6 and `mp_per_level` 2 costs 6 + 4 × 2 = 14 MP at level 5. `etc_base` 50 and `etc_per_level` 10 give (50 + 40) / 10 = 9 at level 5.

## Movement

What is known (`CHero` constructor `0x68A59` and the shared physics globals set in the `CMapleObject` constructor `0x6DBF5`):

- the hero's walking speed (`+0x20`) is 5 and the jump's horizontal speed (`+0x28`) is 7, per tick; `+0x24` is 4 (probably the ladder speed);
- the jump impulse is the global at `0x11A9C8` (`0x100`, reached through the pointer at `0xF13C4`); next to it `0x11A9D0` is `0x4A` (probably gravity) and `0x11A9D4` is `0x10000` (a 16.16 fixed-point one);
- the movement keys are `KeyWalk` `0x60DD9`, `KeyJump` `0x61651` and `KeyUpDown` `0x63665`;
- mobs walk at their `mob.dat` `unknown_23` (mob `+0x20`, `CNpc::Walk` `0x6FCBD`) and move by their movement kind `unknown_24` (1 walks and turns at edges, 2 floats and never lands (the bosses), 3 is the Malady Bomb, which explodes on contact);
- on area 9 the ground is ice and the hero slides, unless wearing Snowshoes (shoes 52), which turn the slide off (`0x64CA1`, world `+0x94`).

The units of the impulse and gravity (how many pixels a jump rises) were not worked out; see [Not found](#not-found).

## Death

`CHero::Die` `0x65979` sets HP to 0 and the dead flag (`+0x0C` = 0x10). From level 10, after the first job advancement, it also sets the EXP penalty (`+0x158`): a quarter of the current level's need, but never more than the EXP the hero has.

The death popup (texts `CStateGame.OnKeyPressed.1` to `6`) offers:

- **Return to town** (`0x8D4C9`): EXP minus the penalty (0 below level 10 or before the advancement), meso minus a tenth, then the hero goes to the nearest town (the same choice as return scroll 24) and is revived (`CHero::Revive` `0x6140D` restores full HP and MP).
- **Revive here**, with Reverse (item 130) or a paid revive; see [cash-shop.md](cash-shop.md).

In Monster Carnival a death costs carnival points instead (see above).

**Example:** a level 20 hero with a need of 1000, 180 EXP and 5000 meso dies. The penalty is 250, capped at 180, so the hero returns with 0 EXP and 4500 meso.

## Not found

- **`buildup.dat`.** The full-program decompile has no reader of data manager `+0x40` (only its writer, `CStateTitle::LoadSettings` `0x980B5`). It is still unknown ([formats/data-tables.md](../formats/data-tables.md#buildupdat)).
- **`hero.dat` `unknown_9`** and **`skill.dat` `unknown_23` / `unknown_24`** are never read.
- **Movement units.** The jump and gravity globals' scale, and the exact jump height in pixels, need a trace of the hero's y position over a jump.

## Verification

`play.mechanics` (CTest, `real_assets`; `tests/cpp/play_real_test.cpp`, mode `mechanics`) plays a new game through the tutorial to Henesys, then hunts on map 2.3 until at least 60 kills. Every field tick it reads guest memory (`guest_state.h`) and checks, against the rules above and the English `hero.dat` and `mob.dat`:

- **damage:** each mob HP loss from the hero's dagger is in the swing's range from [The hero's damage](#the-heros-damage), or twice it (a critical), or a smaller killing blow; criticals are seen and are rare;
- **monster damage:** each HP loss of the hero on a tick with no potion and no level-up is in some nearby mob's contact range from [Monster damage](#monster-damage-and-the-heros-avoid);
- **EXP:** after each kill tick, the level and EXP equal the old ones plus the killed mobs' `exp`, carried through the `hero.dat` needs; level-ups are seen;
- **drops:** each new drop belongs to a mob killed that tick: its equip (count 1, and only if its chance is above 0), one of its etc items with a chance above 0, or a meso bag of the right size (0, 1 or 2) whose count is within 80 % to 120 % of the mob's `meso`.

Run it with `ctest --preset mac-debug -R play.mechanics` after `scripts/bootstrap`; the command and output of the run for #214 are in the issue's comments.
