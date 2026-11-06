# EXP and levelling

The hero levels from 1 to 99 by EXP from kills and quests. Each level needs the amount the level table lists, and a level-up refills HP and MP and raises the stats by the table's steps.

## Gaining EXP

A gain is added to the hero's current EXP. While the total reaches the level's need, the need is taken off and the hero levels up, so one large gain can give several levels and the rest carries over. A message says how much EXP came or went. A total below 0 becomes 0, so a loss never costs a level. At level 99 the EXP stays at 0.

## Level-up

At each level-up:

- the level rises by 1, to at most 99 (the level table has 100 rows; row 100 is never reached);
- skill points: none for the new levels 2 to 9, 1 for levels 10 to 29, and 2 from level 30;
- the maximum HP becomes the sum of the table's HP gains from level 1 to the new level, plus {{skill:8}}'s percentage of that sum, plus equipment HP; the maximum MP works the same way with the MP gains and {{skill:9}};
- ATT, DEF, ACC and AVOID each rise by the difference between the new and the old level's table values;
- HP and MP are filled;
- the Game Center score becomes (level × 999,000 + EXP) / 1,000.

The full table of needs and gains is on the [level pages](skills/index.html).

**Example** (made-up numbers): a level 9 hero with 30 EXP of a 40 need gains 25 EXP. 55 reaches 40, so the hero becomes level 10 with 15 EXP, gets 1 skill point, and HP and MP are full. If level 10 needed only 12, a second level-up would follow: level 11 with 3 EXP and another skill point.

## Sources

`CHero::setExp` `0x6654D`, `ExpToLevelUp` `0x65919`, `CHero::LevelUp` `0x65089`, `CHero::MaxHpFromTable` `0x63C3D`, `CHero::MaxMpFromTable` `0x63BB9`. The real-asset test `play.mechanics` checked the level and EXP after each kill of a 60-kill hunt against these rules.
