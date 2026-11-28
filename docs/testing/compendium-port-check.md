# The compendium in the running port

Five NPCs, two shops and two crafting recipes of the compendium's NPCs and shops section,
checked by play in the recompiled game ([#220](https://github.com/armaatus/maplethief-port/issues/220)).
The bot starts from the built start of chapter 2 ([start-states.md](start-states.md):
`tests/starts/chapter_2.json`, equal to part L's save, so the test is not synthetic) and checks
what it sees in guest memory against the knowledge base's own records, the ones the compendium's
pages are built from. Nothing is written by hand: `python3 -m mtport.botworld` writes the KB's
NPC placements (`npc`), shops (`shop`) and recipes (`recipe`) into `bot_world.txt`
([ADR 0161](../adr/0161-bot-world-tables-from-the-knowledge-base-and-built-start-states.md)),
and `play_real_test compendium_npcs` reads them.

```sh
ctest --preset mac-debug -R "play\.compendium_npcs" --output-on-failure
```

## What it checks

- **An NPC**: the hero goes to the map the compendium names for it. Its object is visible there,
  at the tile the compendium gives (the live object's cell, as the game holds it), and up in
  front of it opens its chat or its shop.
- **A shop**: opened from its NPC, the Buy tab's rows (item and price, read from the shop
  object) are the compendium's stock of that shop, the same number of rows, and the shop sells
  the kind of item (equipment or other items) the compendium lists.
- **A recipe**: crafted at its crafter through his lists; the meso fee and each material the
  compendium gives leave the bag, and the result enters it.

| Checked | Where (compendium) | Result on 2026-11-27 |
| --- | --- | --- |
| NPC: Pirate (`Pirate_Orbis`) | Orbis Dock 4.1, tile (6, 7) | at (104, 127), chat opens, list 1 sails to Henesys |
| NPC: Chief Stan (`Stan`) | Henesys 2.0, tile (34, 10) | at (552, 175), chat opens |
| NPC: Mr. Wang (`Etc20`) | Henesys 2.0, tile (27, 10) | at (440, 175), shop opens |
| NPC: Blackbull (`pig`) | Perion 1.0, tile (18, 5) | at (296, 95), chat opens |
| NPC: Mr. Smith (`smith`) | Kerning City 0.0, tile (6, 6) | at (104, 111), chat opens |
| Shop 3, Mr. Wang's shop in Henesys | `Etc20` on 2.0, chapters 1 to 3 | 6 rows, every item and price as listed |
| Shop 2, Mr. Lee's shop in Henesys (`Equip20`) | `Equip20` on 2.0, chapters 1 to 3 | 10 rows, every item and price as listed |
| Trade: Blackbull's third (`pig`, list 3) | Perion 1.0, chapter 2 | 50 of item 112 out, 3 of item 99 in (2026-11-28) |
| Recipe: Lappy Boots (`2-equip:51`) | Mr. Smith, 0.0, chapter 2; shoes list | 2 Topaz, 7 Bronze and 700 meso out, one pair in |
| Recipe: Lightning Earrings (`2-equip:63`) | Mr. Smith, 0.0, chapter 2; earrings list | 7 Topaz, 6 Bronze and 550 meso out, one pair in |

Every run also passes `play_real_test`'s common checks (no trap, no unimplemented call, no
error line in the runtime's log).

The smith's first list crafts whatever row is chosen, so the bot never closes his chat with
"any row": the first recipe's talk takes over the chat the NPC check opened.

Part L's save (`tests/starts/chapter_2.json`) holds 10 of item 99 since its capture after
#306, and the two recipes take 7 and 6. The bot makes up the difference with Blackbull's third
trade before it goes to the smith, as a player would (#441).
