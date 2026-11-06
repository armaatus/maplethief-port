# Skills and buffs

Each skill has four values that grow with its level: an MP cost, a damage figure, a duration and a general value the skill uses for its own purpose (targets, percent or ticks). Each is a base plus a step per level, from the skill table. The [skill pages](skills/index.html) list them level by level.

## Skill values

A value at skill level n is the base + (n minus 1) × the step:

- **MP cost**, what a use costs.
- **The skill's value**: the base and step, then divided by 10 when positive. The table stores it in tenths, so a base of 50 is 5 targets, 5% or 5 ticks.
- **Damage**: the figure the skill window shows. The hit itself uses one level more ([damage](mechanics/damage.html)).
- **Duration**, in ticks, for buffs only. 600 ticks are 30 seconds at the default game speed.

**Example** (made-up numbers): a skill with an MP base of 6 and a step of 2 costs 6 + 4 × 2 = 14 MP at level 5. A value base of 50 with a step of 10 gives (50 + 40) / 10 = 9 at level 5.

## Buffs

Casting a buff pays its MP and starts its timer. Casting it again while it is on only restarts the timer; its stats are not added twice. When the timer runs out the buff and its stats go away. The buff's icon blinks in its last 30 ticks.

- {{skill:12}} and {{skill:21}}: ATT plus the damage figure, ACC plus the skill's value.
- {{skill:5}}: the hero turns see-through and touching monsters does no harm; attacking ends it.
- {{skill:22}}, {{skill:19}} and {{skill:14}}: see [accuracy, avoid and critical hits](mechanics/accuracy.html) and [damage](mechanics/damage.html).

**Example:** {{skill:5}} at level 1 lasts its base duration of 50 ticks, 2.5 seconds at the default speed; each further level adds 30 ticks, 1.5 seconds.

## Passives

A passive applies as soon as points are spent on it. Points are limited by the free skill points and the skill's maximum level.

- {{skill:4}}: ACC and AVOID plus the skill's value.
- {{skill:6}} and {{skill:15}}: critical rate plus the skill's value.
- {{skill:8}} and {{skill:17}}, {{skill:9}} and {{skill:18}}: maximum HP or MP plus the skill's value as a percentage of the level table's sum.
- {{skill:11}} and {{skill:20}}: ATT plus the skill's value.

**Example:** {{skill:11}} has a value base of 30 and a step of 15 (in tenths). At level 3 it adds (30 + 2 × 15) / 10 = 6 ATT.

## Sources

`CSkill::MpCost` `0x7D251`, `CSkill::EtcValue` `0x7D325`, `CSkill::DamageValue` `0x7D551`, `CSkill::Duration` `0x7EC3D`, `CSkill::CastBuff` `0x7F749` (the buff list at skills `+0xD64`), `CSkill::TickBuffs` `0x7F669` (from `CHero::Flush`), `CSkill::EndBuff` `0x7F581`, `CSkill::IsBuffEnding` `0x7ED51`, `CSkill::LearnLevels` `0x7F99D`.
