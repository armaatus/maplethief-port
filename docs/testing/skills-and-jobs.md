# Skills and job advancements

Every job advancement of both branches and every skill, played from built start states
([start-states.md](start-states.md), [#302](https://github.com/armaatus/maplethief-port/issues/302)).
The tests are `play_real_test` modes; CTest builds each start (`starts.<name>`, from
`tests/starts/<name>.json`) and runs the mode on it (`play.<mode>_synthetic`, `real_assets`).
No start equals a state of the save-chained run yet, so every test is named synthetic.

```sh
ctest --preset mac-debug -R "play\.(jobs|skills)_"
```

The rules checked are those of [re/mechanics.md](../re/mechanics.md); `tests/cpp/skill_rules.h`
holds them as numbers, and `skill_rules_test` checks those numbers on synthetic rows and the
note's worked examples in CI.

## Job advancements

The knowledge base has 11 advancement paths (`job_advancement:<chapter>-<tier>-<branch>`), all
given by the Dark Lord (npc 3) on map 0.0 through that chapter's script. Each test loads its
start, walks to the Dark Lord, talks and picks the list row, then checks the hero's branch
(`+0x88`) and tier (`+0x8C`), the reward weapon in the bag, the quest states named below, and
that the saved game holds the new job.

| Path | Test | Start | Checked |
| --- | --- | --- | --- |
| `1-1-0` | `play.jobs_1_1_synthetic` (also `play.chapter_1_a`) | `jobs_ch1_beginner_10` | Rogue (0/1), equip 24, main 21 handed in, main 9 active |
| `1-2-1` | `play.jobs_1_2_assassin_synthetic` | `jobs_ch1_rogue_30` | Assassin (1/2), equip 40 |
| `1-2-2` | `play.jobs_1_2_bandit_synthetic` (also `play.chapter_1_g`) | `jobs_ch1_rogue_30` | Bandit (2/2), equip 28 |
| `2-2-1` | `play.jobs_2_2_assassin_synthetic` | `jobs_ch2_rogue_30` | Assassin (1/2), equip 40 |
| `2-2-2` | `play.jobs_2_2_bandit_synthetic` | `jobs_ch2_rogue_30` | Bandit (2/2), equip 28 |
| `2-3-same` | `play.jobs_2_3_assassin_synthetic`, `play.jobs_2_3_bandit_synthetic` | `jobs_ch2_assassin_60`, `jobs_ch2_bandit_60` | tier 3, branch kept |
| `3-1-0` | `play.jobs_3_1_synthetic` | `jobs_ch3_beginner_10` | Rogue (0/1), equip 36, side quest 1 handed in |
| `3-2-1` | `play.jobs_3_2_assassin_synthetic` | `jobs_ch3_rogue_30` | Assassin (1/2), equip 40, side quest 16 handed in |
| `3-2-2` | `play.jobs_3_2_bandit_synthetic` | `jobs_ch3_rogue_30` | Bandit (2/2), equip 28, side quest 16 handed in |
| `3-3-same` | `play.jobs_3_4_assassin_synthetic`, `play.jobs_3_4_bandit_synthetic` (first talk) | `jobs_ch3_assassin_60`, `jobs_ch3_bandit_60` | tier 3, branch kept, side quest 42 handed in |
| `3-4-same` | the same tests (second talk) | the same | tier 4, branch kept, side quest 25 handed in |

### Against the compendium

Each talk also writes one line to the run's `jobs.txt`: the chapter, level, branch, tier and
quest states the guest held before it, and the job after it. `tests/test_compendium_skills_port.py`
([#222](https://github.com/armaatus/maplethief-port/issues/222), `real_assets`, skipped without
the runs) matches every line with its row of the Skills and jobs section's advancement table
(`skills/index.html`, built from the knowledge base): exactly one row of that chapter has the
tier and job the port reached; the guest before the talk meets each of the row's conditions a
line can show (level, advancement count, quest states; a quest's requirement, an unclaimed
reward and free bag slots are left to the start); the page's row names that job and level. All
11 rows are reached by a run.

```sh
ctest --preset mac-debug -R "play\.jobs_" && pytest tests/test_compendium_skills_port.py
```

## Skills

Two level-50 tier-4 heroes on map 1.3 (mobs `12` and `16`) with free SP and
every skill at 0: an Assassin with claw equip `44` and stars etc item `12`
(`tests/starts/skills_assassin.json`), a Bandit with dagger equip `32`
(`tests/starts/skills_bandit.json`). Each skill is learned to level 2 on the skill tab and its
level read back; an active skill is put on quick slot 3 and used on the mobs. A watcher checks
every field tick:

- **MP:** each MP drop with no potion drunk equals the skill's cost at its level.
- **Damage:** each mob's HP loss in a tick is k hits of the skill in use (k up to the hits it
  lands on one mob), each in one hit's range or a critical's, the star's ATT added for star
  skills; a killing blow only stays under the most. A claw on a mob right next to the hero
  punches for 1 (2 on a critical), whatever key is pressed; those are counted apart.
- **Stars:** never more than a use's worth leave in one tick, and a phase's total is its casts
  times the stars a use takes (give or take one use in flight).
- **Buffs:** the buff list at skills `+0xD64`; a buff's lifetime (its counter's last value plus
  one) equals its duration at its level.

| Skill | Branch / tier | Test | Asserted |
| --- | --- | --- | --- |
| 0 basic attack (dagger) | all | `skills_bandit` | hits in the swing's range |
| 1 basic throw | all | `skills_assassin` | hits in the throw's range with the star's ATT; one star a throw |
| 2 Lucky Seven | 0/1 | `skills_assassin`; refused in `skills_bandit` | MP, hits (2 a use), 2 stars a use; with a dagger no MP is paid |
| 3 Double Step | 0/1 | `skills_bandit`; refused in `skills_assassin` | MP, hits (2 a use); with a claw no MP is paid |
| 4 Nimble Body | 0/1 | both | accuracy and avoid plus `@etc` |
| 5 Dark Sight | 0/1 | both | MP, mobs touch the hero and do no harm, lifetime = duration |
| 6 Critical Throw | 1/2 | `skills_assassin` | critical rate plus `@etc` |
| 7 Drain | 1/2 | `skills_assassin` | MP, hits, 1 star a use, heals `@etc` % of the damage up to max HP |
| 8 HP Mastery | 1/2 | `skills_assassin` | max HP plus `@etc` % of the `hero.dat` sum |
| 9 MP Mastery | 1/2 | `skills_assassin` | max MP plus `@etc` % of the sum |
| 10 Avenger | 1/2 | `skills_assassin` | MP, hits, 3 stars a use |
| 11 Weapon Mastery | 1/3 | `skills_assassin` | attack plus `@etc` |
| 12 Concentration | 1/3 | `skills_assassin` | MP, attack plus `@dmg`, accuracy plus `@etc`, lifetime |
| 13 Shadow Web | 1/3 | `skills_assassin` | MP, mobs bound (`+0xD4`) for `@etc` ticks, no damage |
| 14 Shadow Partner | 1/4 | `skills_assassin` | MP, lifetime; Lucky Seven under it lands up to 4 hits and takes 4 stars |
| 15 Critical Stab | 2/2 | `skills_bandit` | critical rate plus `@etc` |
| 16 Savage Blow | 2/2 | `skills_bandit` | MP, hits (`@etc` a use) |
| 17 HP Mastery | 2/2 | `skills_bandit` | max HP plus `@etc` % of the sum |
| 18 MP Mastery | 2/2 | `skills_bandit` | max MP plus `@etc` % of the sum |
| 19 Pickpocket | 2/2 | `skills_bandit` | MP, lifetime, a hit's meso drop within the mob's `meso` / 18 |
| 20 Weapon Mastery | 2/3 | `skills_bandit` | attack plus `@etc` |
| 21 Concentration | 2/3 | `skills_bandit` | MP, attack plus `@dmg`, accuracy plus `@etc`, lifetime |
| 22 Meso Guard | 2/3 | `skills_bandit` | MP, lifetime, each blow halved and `@etc` % of the half paid in meso |
| 23 Boomerang Stab | 2/4 | `skills_bandit` | MP, hits (2 a use) |

Each run prints one `skills:` line per skill with its counts and an example hit, and fails on
the first value off a rule.

## The weapon rules of passives and buffs

[#307](https://github.com/armaatus/maplethief-port/issues/307): the same two heroes without
their branch's weapon. `play.skills_weapon_assassin_synthetic` loads the Assassin with nothing
worn (`tests/starts/skills_weapon_assassin.json`), `play.skills_weapon_bandit_synthetic` the
Bandit with claw `44` and stars (`tests/starts/skills_weapon_bandit.json`). Each learns its
branch's passives to level 2, the stat rule checked as above, then puts each buff on quick slot 3
and presses it up to three times: cast (on the buff list, its MP paid) or refused (no buff, MP
unchanged).

| Test | Passives (rule held) | Buffs |
| --- | --- | --- |
| `play.skills_weapon_assassin_synthetic` | 4, 6, 8, 9, 11 | 5, 12, 14 cast |
| `play.skills_weapon_bandit_synthetic` | 4, 15, 17, 18, 20 | 5, 19, 21, 22 cast |

So `CSkill::setActiveSkill` `0x7EFF1`'s weapon check holds for the attack skills only
([re/mechanics.md](../re/mechanics.md#attacking-hit-areas-targets-and-stars)). The Assassin run also checks that a
hero with nothing worn and no skill has exactly the level's `hero.dat` attack, defense,
accuracy and avoidability (`ok: level 50: ATT DEF ACC AVOID 64 63 61 52, hero.dat 64 63 61
52`).

```sh
ctest --preset mac-debug -R "play\.skills_weapon"
```
