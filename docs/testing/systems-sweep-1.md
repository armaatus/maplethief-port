# Systems sweep 1

[#303](https://github.com/armaatus/maplethief-port/issues/303) covers the systems no chapter run exercises: the Korean game, the second and third save slots, a full bag, the level cap and its Game Center score, and the quick slots and trash by pick up and drop. Each is a mode of `play_real_test` (`tests/cpp/play_real_test.cpp`) with a CTest test. They are real-asset tests, so they run locally only and skip (77) without your own copy. Most start from a save built from a description in `tests/starts/` ([start-states.md](start-states.md)); those tests are named `synthetic`.

```sh
ctest --preset mac-debug -R "^(starts\.(korean|systems)|play\.(korean|inventory_full|level_cap|quick_slots|slots_))"
```

| Test | Start | What it checks |
|---|---|---|
| `play.korean_chapter_1_a` | a new game | Korean: the terms agreed by the pad, the intro, the tutorial, Mr. Wang's shop and chapter 1's first quests (part a) |
| `play.korean_offer_2_synthetic` | `chapter_2.json`, built in Korean | Korean: chapter 2's first quest offer (Heracle on 4.0) |
| `play.korean_offer_3_synthetic` | `korean_ch3.json` | Korean: chapter 3's first quest offer (the Zenumist President on 7.0) |
| `play.korean_missing_key_synthetic` | `korean_ch3_key.json` | Korean: the line `ko.lproj` lacks ([original bug 15](../re/original-bugs.md#15-a-dialogue-key-missing-in-korean)), shown in English with `fix_original_bugs` on |
| `play.slots_create_synthetic` | `systems_slot0.json` | Slots 2 and 3: created with New, played through the tutorial, saved, left by the option tab's exit; slot 1 untouched |
| `play.slots_reload_synthetic` | the run before (a fresh process) | Slots 2 and 3 loaded as saved (map, x, Red Potions), then deleted; slot 1 untouched |
| `play.inventory_full_synthetic` | `systems_inventory_full.json` | A 48 + 48 bag: a drop is refused and stays; a sale empties a cell, pays the price, and the bag keeps its order |
| `play.level_cap_synthetic` | `systems_level_99.json` | Level 99: EXP at the need resets to 0 with no level-up; a map change writes the score to `gamecenter.json` |
| `play.quick_slots_trash_synthetic` | `systems_carry.json` | A skill onto a quick slot and used from it; the trash popup declined, then accepted |

## Korean

The Korean runs set `MAPLETHIEF_LANGUAGE=ko` and remove the copied save's `CTerms`, so each run agrees to the terms first. Every text the game draws goes through `-[NSString drawAtPoint:withFont:]` (`TextCache_DrawText` `0x9031`). After drawing, the runtime hands each one to a test observer (`mt_cg_set_text_observer`, `maplethief/cg.h`). A run fails on any of these:

- **A missing glyph.** A character neither NanumGothic nor the fallback Fira Sans has a glyph for, or U+FFFD from a failed decode. The device draws a box there and the port draws nothing.
- **A clipped line.** A single line whose right edge passes its bitmap's width (TextCache's 512 points).
- **A broken box line.** A dialogue line that is not whole UTF-8, meaning the wrap cut a character in two.

**Checkpoints.** The runs take a checkpoint after the terms, on the first field, on each NPC chat's first and last page (a quest offer's Accept is on its last), and on each shop. Each checkpoint writes `OUT/ko_<n>_<name>.png` and appends to `OUT/checkpoints.txt`: the name, the screen's FNV-1a, and the texts drawn since the previous checkpoint.

**Recorded renders.** CTest points `MT_KOREAN_BASE` at `korean_base/<mode>` in the test output directory. When that directory is empty, the run records its `checkpoints.txt` there, but only if its own checks held. Later runs must take the same checkpoints with the same screen hashes. The runs are deterministic on the virtual clock, so a difference means the rendering changed. The base holds hashes of our own renders and the game's text, so it stays in the build tree and is never committed. To record again, delete the directory.

The chapter 3 offer: the Zenumist President gives main quest 0 once main 40 (from chapter 2) is active and its reward is due (`korean_ch3.json`).

The missing key: his fallback line plays only when main 0 is done, main 1 to 3 are not active, main 40 is not active, and side quests 5, 6, 8, 9, 13, 14 and 25 are all done (`korean_ch3_key.json`, read from the knowledge base).

The run reads the English line's first word from `en.lproj` at run time. It checks that a box shows it and that no box shows the key.

**With the setting off,** the port dies on that line. `Text_Wrap` reads past guest memory on the unbroken key, a SIGBUS ([#419](https://github.com/armaatus/maplethief-port/issues/419)). So the run sets `fix_original_bugs` itself.

## Slots

The character select's New (with the engine's popup) starts a new slot in the first free place. The tutorial's maps are on the list of maps the game cannot save on ([autosave.md](../re/autosave.md)), so each new slot plays the tutorial to Stan in Henesys and saves there. Slot 2 drinks one Red Potion first and slot 3 drinks two, so the two saves differ.

Each save writes its map, x and potion count to `OUT/slots.txt`. The second test runs in a new process on the same data directory: it loads each slot, checks it against `slots.txt` and `CDataMgr +0x4E4` (the slot in use), then deletes both through Delete and its alert. `slots_reload` checks that their files are gone and that slot 1's files are byte for byte as built.

## A full bag

The start has 48 equipment items and 48 etc stacks, none of them a Red Snail's drop, and hunts on 2.3. A drop the hero touches with no room is refused: the drop's byte `+0x10C` is set, it stays on the ground, and the bag is unchanged.

Then Mr. Wang's Sell tab sells bag cell 10 (item 45, one of it). The test checks four things:

- The item is gone.
- Meso rose by item_etc's price for it (7).
- The equipment is unchanged.
- The other 47 stacks are in their old order, with the gap closed and the empty cell last.

**Sorting.** The game has no sort command: no string, no menu item and no function for it. The bag's order is the order items came in. Removing an item closes the gap, and a new item goes into the first empty cell. That compaction is what the test checks as "sorting works".

## The level cap

`CHero::setExp` `0x6654D` calls `CHero::LevelUp` `0x65089` when EXP reaches the level's need. Above level 98, `LevelUp` returns at once. `setExp` then sets EXP to 0. So at level 99, EXP climbs to hero.dat's need (71,740), drops to 0, and the level never changes.

The start is level 99 with 71,735 EXP. The test checks:

- The first kill resets EXP to 0, once.
- The level stays 99, and EXP counts up again.

The map change to Henesys then reports the total ranking, (level × 999,000 + EXP) / 1,000, from `CMapleWorld::changeMap` `0x81369`. The test reads it back from `gamecenter.json` (`mt_gamekit_entries`).

## Quick slots and the trash

The test uses the pad only, through the port's pick up and drop ([ADR 0129](../adr/0129-pick-up-and-drop-focus-mode.md)):

1. On the skill tab, X picks up Double Step (skill 3).
2. Up and down move the carry along the quick-slot column (up raises the slot number), and A drops the skill on slot 3.
3. In the field, LB uses it, and its MP is paid.
4. A stack of Snail Shells goes to the trash. On the game's popup 20, B declines, and the shells stay.
5. Again, and A accepts: the shells are gone.

`input.carry_keys` ([#140](https://github.com/armaatus/maplethief-port/issues/140)) already covers the keyboard's way to the trash.
