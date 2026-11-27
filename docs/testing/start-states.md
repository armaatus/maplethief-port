# Built start states

A play test can start from any point of the game without replaying the chapters before it: it
loads a save written by `python3 -m mtport.savedata build` from a description
([formats/save.md](../formats/save.md#building-a-save), [ADR 0161](../adr/0161-bot-world-tables-from-the-knowledge-base-and-built-start-states.md),
[#297](https://github.com/armaatus/maplethief-port/issues/297)).

```sh
PYTHONPATH=tools python3 -m mtport.savedata build tests/starts/chapter_2.json \
    OUT_DIR/data/saves --app work/ipa/Payload/MapleThief.app
MT_FROM_SAVE=OUT_DIR SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy MAPLETHIEF_LANGUAGE=en \
    build/mac-debug/tests/play_real_test <part> <output dir>
```

Part M of the chapter-1 chain passes from `tests/starts/chapter_2.json` built this way, with
no other file in `saves/` (2026-11-26).

`build` refuses a description the port's own save checks refuse (an item, skill or map past the
game's tables, a quest key that is not text). `--app` lets a description name skills by id and
the slot's caption by its strings key.

## The rule

A save can hold a state no real play produces, and a test on it finds bugs the game does not
have: a chapter-1 hero placed on map 10.5 stops the game. So **every built start state either
equals a state the save-chained run reaches, compared field by field, or carries `synthetic` in
its test's name.**

- **Equal to a chained state.** The description is captured from a save of the chain
  ([chapter-1-quests.md](chapter-1-quests.md#starting-from-a-later-point)) with
  `python3 -m mtport.savedata describe theif0 quest0 --app work/ipa/Payload/MapleThief.app`,
  and a real-asset test builds it and compares it with that save in every byte the game writes
  (`savedata.written_diff`: the bytes the game leaves unwritten hold heap garbage and are left
  out). When the chain's end state changes, the test fails until the description is captured
  again.
- **Synthetic.** A start state written for one check (a full inventory, a level the chain never
  reaches) is allowed, and its test's name says `synthetic`, so a failure there is read as a
  possible artefact of the state before it is read as a bug of the port.

## The checked starts

| Description | Equals | Test |
| --- | --- | --- |
| `tests/starts/chapter_2.json`: the start of chapter 2, a level-35 Bandit on 4.1 with main 40 active | part L's save (`play.chapter_1_l`) | `tests/test_savedata_build_real.py` (local; skipped without the bundle or the chain's output, which `MT_CHAIN_OUTPUT` can name) |

CTest builds it as `starts.chapter_2` for `play.compendium_npcs` ([compendium-port-check.md](compendium-port-check.md)), which is not synthetic for that reason.

The synthetic starts of [skills-and-jobs.md](skills-and-jobs.md) (`tests/starts/jobs_*.json`,
`tests/starts/skills_*.json`) and [systems-sweep-2.md](systems-sweep-2.md)
(`tests/starts/systems2_*.json`) are built by CTest itself: `starts.<name>` writes the save into
the test output directory (skipped without the bundle) and `play.<mode>_synthetic` loads it.
So are systems sweep 1's ([systems-sweep-1.md](systems-sweep-1.md)): `korean_ch3.json` and
`korean_ch3_key.json` (built with `--language ko`), `chapter_2.json` again as
`starts.korean_chapter_2`, and `systems_slot0.json`, `systems_inventory_full.json`,
`systems_level_99.json` and `systems_carry.json`. `speech_notices.json` (a level-10 Beginner with
a knife on 2.3) starts `play.speech_notices_synthetic` and its base run (#392). The
`map_page_*.json` starts put a hero on each of the five compendium map pages checked in the
running port (#221): `play.map_page_<start>_<kind>_synthetic` takes one portal and
`tests/test_compendium_world_port.py` compares where the game put the hero with that page's
Connections row. The 2.0 and 2.4 starts come twice, with the chapter-1 gate's quest done
(`_open`) and not (`_shut`); `map_page_9_3_top.json` stands the hero beside 9.3's hidden portal.

## Raised starts: a chained save with a few fields changed

A chain can skip what play would take game-hours for, such as levels, without leaving the chain:
`build --base DIR` lays the description over the `describe` of the pair in `DIR` (a play test's
output directory, its `data/saves` or `data/Documents`), so everything the description leaves
out is the save of the part before. A raised `level` levels the hero up by the game's rules
(`CHero::LevelUp`): the skill points of each level gained (none to level 9, 1 to 29, 2 from 30),
EXP 0, and HP and MP the sums of `hero.dat`'s gains (needs `--app`).

```sh
PYTHONPATH=tools python3 -m mtport.savedata build tests/starts/raise/chapter_2_b.json \
    OUT_DIR/data/saves --base build/test-output/mac-debug/play_chapter_2_a \
    --app work/ipa/Payload/MapleThief.app
```

The overlays live in `tests/starts/raise/` (they hold only the changed keys, so they are not
whole descriptions). A raised start equals no chained state, so the part that loads it is
synthetic: CTest names it `play.chapter_2_<part>_synthetic` and builds its start in
`starts.chapter_2_<part>` ([chapter-2-quests.md](chapter-2-quests.md)).

## The bot's world

The bot finds its way with tables generated from the knowledge base, not written by hand
([ADR 0161](../adr/0161-bot-world-tables-from-the-knowledge-base-and-built-start-states.md)):
the portal links of every chapter with their quest gates, and the shops. CTest
`play.bot_world` writes them into the build tree (`build/<preset>/tests/bot_world.txt`) with
`python3 -m mtport.botworld`, and every `play.*` test requires it. `MT_BOT_WORLD` makes
`play_real_test` read another file. A chapter-2 or chapter-3 run started from a built save
routes over that chapter's own links.
