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
| `tests/starts/chapter_2.json`: the start of chapter 2, a level-34 Bandit on 4.1 with main 40 active | part L's save (`play.chapter_1_l`) | `tests/test_savedata_build_real.py` (local; skipped without the bundle or the chain's output, which `MT_CHAIN_OUTPUT` can name) |

The synthetic starts of [skills-and-jobs.md](skills-and-jobs.md) (`tests/starts/jobs_*.json`,
`tests/starts/skills_*.json`) and [systems-sweep-2.md](systems-sweep-2.md)
(`tests/starts/systems2_*.json`) are built by CTest itself: `starts.<name>` writes the save into
the test output directory (skipped without the bundle) and `play.<mode>_synthetic` loads it.
So are systems sweep 1's ([systems-sweep-1.md](systems-sweep-1.md)): `korean_ch3.json` and
`korean_ch3_key.json` (built with `--language ko`), `chapter_2.json` again as
`starts.korean_chapter_2`, and `systems_slot0.json`, `systems_inventory_full.json`,
`systems_level_99.json` and `systems_carry.json`.

## The bot's world

The bot finds its way with tables generated from the knowledge base, not written by hand
([ADR 0161](../adr/0161-bot-world-tables-from-the-knowledge-base-and-built-start-states.md)):
the portal links of every chapter with their quest gates, and the shops. CTest
`play.bot_world` writes them into the build tree (`build/<preset>/tests/bot_world.txt`) with
`python3 -m mtport.botworld`, and every `play.*` test requires it. `MT_BOT_WORLD` makes
`play_real_test` read another file. A chapter-2 or chapter-3 run started from a built save
routes over that chapter's own links.
