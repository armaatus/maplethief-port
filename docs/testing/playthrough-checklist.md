# Playthrough checklist

The chapter by chapter checklist of [#79](https://github.com/armaatus/maplethief-port/issues/79) and [#92](https://github.com/armaatus/maplethief-port/issues/92) ([#296](https://github.com/armaatus/maplethief-port/issues/296)): every quest, boss, job advancement, climb and scene in the order a player meets them. It is generated from the compendium's knowledge base ([kb.md](../compendium/kb.md), ADR 0124), so it cannot drift from the game. It holds the game's names, so it is built from your own copy into git-ignored `build/checklists/` and never committed; the repository holds only the generator, `tools/mtport/compendium/playthrough.py`.

## Making it

```sh
scripts/bootstrap                                          # work/ipa/ from your IPA
PYTHONPATH=tools python3 -m mtport.compendium.kb build     # build/compendium/kb/
PYTHONPATH=tools python3 -m mtport.compendium.playthrough  # build/checklists/
```

It prints the counts: 138 quests, 5 bosses, 11 job advancements and 3 climbs in version 1.5. `--kb DIR` and `--out DIR` pick other directories.

## What it holds

**`build/checklists/playthrough.md`** (#79), one tick box per row:

- **Tutorial**, then **chapters 1 to 3** part by part, with the parts and the order of the [walkthrough](https://armaatus.github.io/maplethief-port/walkthrough/): each part's story quests, then the side quests that open there. A boss follows the quest that asks for its kill; a job advancement, a climb or a scene follows the latest quest its conditions (for a climb, the conditions of the way in) need accepted or done. One that needs no quest goes under "Any time in chapter N".
- **The endings and the post-game**: the tutorial's end, the two chapter ends, the last boss's fight and its two scenes, and playing on after the ending.
- **Both job branches**: each branch's advancements and skills, for a second pass from the save before the second advancement.
- **The three climbs** and **save and load** (the menu save, the import, save states, the autosave, and the maps where the game refuses to save).

Each row ends with *Automated*: the CTest that already plays it to the end and checks it in guest memory, or `none yet`. The map is `RUNS` in the generator, from [chapter-1-quests.md](chapter-1-quests.md); [#295](https://github.com/armaatus/maplethief-port/issues/295)'s run coverage report can extend it.

**`build/checklists/platforms.md`** (#92): a sign-off table with one row per platform (the Switch row points at the [Switch checklist](switch-checklist.md) and #245), then per platform the shorter pass (the bosses, job advancements, climbs, scenes, endings and save and load) with a column per preset (Original, Original + controller, Enhanced), and a controller-first section.

## Tests

`tests/test_compendium_playthrough.py` checks the generator on a synthetic knowledge base in CI. `tests/test_compendium_playthrough_real.py` (local, needs `work/ipa/`) counts the quests, bosses, job advancements and climbs against the real knowledge base and checks that every entity *Automated* names exists.
