# Playtests and checklists

Runs that check the real game: some scripted, which run as tests on your machine with your own copy of the game, and some by hand, on a device. The test suites themselves, CI and the checks before a push are in [Testing and CI](../build/testing.md).

## Scripted runs

| Page | What |
|---|---|
| [The scripted first-map run](first-map-run.md) | `play.first_map`: the real game played to its first hunting map for 15 game-minutes |
| [Chapter 1's quests](chapter-1-quests.md) | The checks of chapter 1's NPCs, quests and scripts, played by a bot |
| [Chapter 2's quests](chapter-2-quests.md) | Chapter 2's main quests, both bosses, the 3rd job and the change to chapter 3, played by the bot |
| [Chapter 3's quests](chapter-3-quests.md) | Chapter 3's main quests, both bosses, the 4th job, the two ending scenes (watched and skipped), the post-game save, the 33 side quests, the climb of 6.1 and 6.2, and the recipes and trades, played by the bot |
| [The regression replay](regression-replay.md) | `scripts/regress`: recorded input replayed against recorded checkpoints; `scripts/playthrough`: the whole save-chained chain with a digest per part |
| [Built start states](start-states.md) | Play tests that start mid-game from a save built from a description, equal to the chained run's state |
| [Run coverage](run-coverage.md) | Which guest functions, scripts, natives, maps, imports and selectors the scripted runs executed, and the baseline numbers |
| [Skills and job advancements](skills-and-jobs.md) | Every job advancement path and all 24 skills, played from built start states and checked against the rules |
| [Systems sweep 1](systems-sweep-1.md) | Korean, the second and third slots, a full bag, the level cap and its score, the quick slots and the trash |
| [Systems sweep 2](systems-sweep-2.md) | The cash shop's tiles and restores, the Reverse, the ten upgrade scrolls, the Pigmy dialog and `portal620`, from built start states |
| [The compendium in the running port](compendium-port-check.md) | Five NPCs, two shops and two crafting recipes of the compendium, played from the start of chapter 2 and checked against the knowledge base |

## By hand

| Page | What |
|---|---|
| [Playthrough checklist](playthrough-checklist.md) | Chapter by chapter: every quest, boss and job advancement, generated from the knowledge base |
| [Switch sign-off checklist](switch-checklist.md) | The hardware sign-off of the Nintendo Switch build |
| [Switch performance](switch-performance.md) | The logged performance session on the console |
| [Hand-off checklists](handoffs/README.md) | Everything that waits on a device or a person, one page per device |
