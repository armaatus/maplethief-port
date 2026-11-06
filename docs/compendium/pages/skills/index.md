The hero of the game is a thief with three possible jobs. Every new game starts without a job. From level 10 the first job advancement, given by {{npc:3}} in {{map:0-0}}, makes the hero a [Rogue](skills/rogue.html). From level 30 the second advancement turns the Rogue into an [Assassin](skills/assassin.html), who fights with claws and throwing stars, or a [Bandit](skills/bandit.html), who fights with daggers. The third advancement (level 60) and the fourth keep the job and open the higher tiers of its skills.

## How skills work

- The skill window holds every skill of tier 1 up to the hero's number of job advancements, from the Rogue branch and from the hero's own branch. The two basic attacks are never in it ([save format](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/save.md#skill-slots)).
- Each skill level costs one skill point. A level-up gives none up to level 9, one from level 10 to 29 and two from level 30, so the points never cover every skill of a job; see [Skill builds](skills/builds.html).
- A skill's values at level n are the table's base value plus n minus 1 times its per-level value. The damage roll is the exception: it uses one level more than the skill window shows.
- The skill-specific value (the window's targets, hits, percentages and bind time) is kept in tenths and rounded down.
- Buff durations are counted in ticks, 20 to a second at the default game speed.

The rules are described in full, with addresses, in [the mechanics notes](https://github.com/armaatus/maplethief-port/blob/main/docs/re/mechanics.md#skills-values-mp-buffs-and-passives).
