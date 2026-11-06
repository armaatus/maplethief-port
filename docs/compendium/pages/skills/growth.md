The hero grows from level 1 to level 99. The level table (`hero.dat`, one row per level) gives the EXP each level needs, the HP and MP it adds, and the hero's base stats. The tables below list every row; the table's row for level 100 is never reached.

## EXP

- EXP from a kill or a quest is added to the current EXP. While it reaches the level's need, the need is taken off and the hero levels up, so one large gain can give several levels and the rest carries over.
- A loss of EXP never costs a level: the EXP stops at 0.
- At level 99 the EXP stays 0.
- Dying and returning to town costs a tenth of the meso. From level 10, once the first job advancement is made, it also costs a quarter of the current level's need in EXP, but never more EXP than the hero has.

## How stats are gained

- On each level-up, maximum HP is worked out again as the sum of the HP gained up to the new level (the Maximum HP column), plus the percentage of {{skill:8}} or {{skill:17}}, plus the HP of the equipment worn. Maximum MP works the same way with the MP gained, {{skill:9}} or {{skill:18}}, and the equipment's MP.
- ATT, DEF, ACC and AVOID each rise by the difference between the new level's row and the old one's. Equipment and passive skills add to them on top.
- HP and MP are filled on every level-up.
- STR, DEX, INT and LUK are shown on the status tab and used by no rule: they are for display only.
- Levels 1 to 4 have an AVOID of 50 in the table and level 5 has 7, so the fifth level lowers AVOID by 43.
- The critical rate is 2% before the first job advancement and 8% after it, before {{skill:6}} or {{skill:15}}.

## Skill points

A level-up gives no skill point for levels 2 to 9, one for levels 10 to 29 and two from level 30, so a level 99 hero has had 160 points. The rule is fixed in the level-up code. The table's column at offset 9 holds 1 in every row and is never read, so it does not decide the points.

Four items change levels and points: {{item:etc:128}} gives one level, {{item:etc:131}} gives a fifth of the current level's need as EXP, {{item:etc:132}} gives one skill point, and {{item:etc:129}} returns every point spent.
