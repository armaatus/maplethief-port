# 161. The bot's world tables from the knowledge base, and built start states

- **Status:** Accepted (delegated by Joris, 2026-11-26)
- **Date:** 2026-11-26
- **Issue:** [#297](https://github.com/armaatus/maplethief-port/issues/297) (epic [#285](https://github.com/armaatus/maplethief-port/issues/285))
- **Builds on:** [ADR 0124](0124-symbolic-script-walk-for-the-compendium.md) (warps, shops and their conditions in the knowledge base), [ADR 0074](0074-save-files-atomic-writes-sanitising-and-ios-documents.md) and [ADR 0078](0078-save-export-import-backups-and-checks.md) (the save checks), [ADR 0100](0100-public-repository-and-player-releases.md) (nothing derived from the IPA in git)

## Context

The chapter-1 runs (`play.chapter_1_a` to `_m`, [testing/chapter-1-quests.md](../testing/chapter-1-quests.md)) drive a bot over the game's own maps. Its world was written by hand: 53 portal links and 4 quest gates in `tests/cpp/world_map.h` and `quest_bot.h`, the town shop of each of areas 0 to 3, and each `then_stock` and `then_gear` call naming its NPC. Chapters 2 and 3 have other links and other gates, so hand tables would not carry over. The knowledge base already holds every warp with its chapters and conditions, and every shop with the NPC that opens it.

The chain also has to be replayed from the start to reach a later point: chapter 2 begins after 13 parts and about 4,000 s of real time. Saves have no checksum, and `mtport.savedata` already parses and encodes them, so a start state can be written directly. A written state may be one no play reaches, though (a chapter-1 hero on a chapter-3 map stops the game), and a test on such a state finds bugs the game does not have.

## Options considered

**The bot's world.**

1. **Hand tables per chapter.** Simple, but each chapter's links and gates are copied from the scripts by hand, and nothing checks the copy.
2. **Generated tables committed to the repository.** They are derived from the bundle, which the repository never holds (ADR 0100).
3. **Generated at test time into the build tree** (this ADR). CTest `play.bot_world` runs `python3 -m mtport.botworld`, which builds the knowledge base from the bundle and writes the portal warps of every chapter with their conditions and every shop's items into `build/<preset>/tests/bot_world.txt` (`MT_BOT_WORLD_FILE`). Every `play.*` test requires it as a fixture; it keeps a file newer than its inputs, so one test run on its own costs the knowledge base's build (a minute or two) only once.

**A start state.**

1. **Replay the chain every time.** Exact, but a chapter-3 test would cost hours before its first check.
2. **Edit a chained save by hand.** Fast, but an edit is invisible, and nothing says whether the result is a state the game reaches.
3. **Build it from a description and check it** (this ADR). `python3 -m mtport.savedata build` writes the pair a JSON description names (docs/formats/save.md, "Building a save"), and refuses it unless the port's own save checks (`mtport.savecheck`, the rules of `mt_saves_check`) pass. `describe` turns a pair back into a description, so a state the chain reached can be captured.

## Decision

Decided by the agent (delegated by Joris, 2026-11-26); Joris can veto it in the PR.

1. **The bot's world comes from the knowledge base**, generated at test time (option 3). A link is a portal warp of one chapter: source map and portal, target map, and a gate made from the warp's conditions. A term on a quest state is tested in guest memory; any other term (`checkRequire`, the level) shuts its alternative, so the bot never counts on a warp it cannot check. The bot routes over the links of the hero's chapter. A shop row is an item sold by the NPC that opens the shop, on its map, so `then_stock`, `then_gear` and the restocks name an area and an item, not an NPC. The format is in `tools/mtport/botworld.py`. Before the hand tables were removed, the knowledge base's chapter-1 links were checked against them: the same 53 links and 4 gates, and the same first hop for every pair of maps under all 16 combinations of the gates.
2. **Start states are built from a description and checked** (option 3), and one rule keeps them honest ([testing/start-states.md](../testing/start-states.md)): every built start state either equals a state the save-chained run reaches, compared field by field in every byte the game writes, or carries `synthetic` in its test's name. The first is the start of chapter 2 (`tests/starts/chapter_2.json`), compared with part L's save by `tests/test_savedata_build_real.py`.

## Consequences

- Chapters 2 and 3 get their links and gates without new tables. A shop or a warp the knowledge base gets wrong now misleads the bot, so the knowledge base's own checks (ADR 0124) cover the bot too.
- `play.bot_world` adds a minute or two to the first `play.*` run of a build tree. `-FA '.*'` skips it with the other fixtures, which works once the file exists.
- A non-synthetic description fails its real-asset test when the chain's end state changes (a bot change alters meso or drops); it is then captured again with `savedata describe`.
- The builder checks what the port checks (table indexes, maps that exist, quest keys). It does not know whether a map belongs to the chapter; that is what the rule above is for.
