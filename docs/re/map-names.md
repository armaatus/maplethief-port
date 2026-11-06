# Map and area names

The game names its areas and maps with the `CDataMgr.m_strWorldSpotName.<n>` (7) and `CDataMgr.m_strMiddleSpotName.<n>` (67) keys of `ScriptExtra.strings`. Nothing in the map files names a map: three small functions and one table in the binary pick the key from the area and map numbers. This page gives that mapping with its addresses. The compendium knowledge base ([compendium/kb.md](../compendium/kb.md), [#213](https://github.com/armaatus/maplethief-port/issues/213)) uses it for the `name` of every `area` and `map`.

Addresses are armv7 VM addresses; function pointers carry the Thumb bit. The names below come from `python3 -m mtport.compendium.world names` (`work/re/names/mapnames.json`, applied by `scripts/re-analyze`).

## Where the names are loaded

`CDataMgr::CDataMgr` (`0x5AE39`) fills fixed `char[50]` arrays from `ScriptExtra.strings` (through `LocalizedCString` `0x6DD1`, so the language follows the device):

| Array | Size | Keys | Filled by the loop at |
|---|---|---|---|
| `g_worldSpotNames` `0x116DA0` | 7 x 50 | `m_strWorldSpotName.1` to `.7` | `cfstring` `0x1180C4`, read at `0x5B5C0` |
| `g_middleSpotNames` `0x111A50` | 3 groups x 50 x 50 (`0x9C4` bytes per group) | `m_strMiddleSpotName.1` to `.67`, numbered on across the groups | `cfstring` `0x1180D4`, read at `0x5B67E`, `0x5B742`, `0x5B80E` |

The three middle-spot loops fill 24, 32 and 11 names, so group 0 holds keys 1 to 24 (Kerning City to Cursed Shrine), group 1 keys 25 to 56 (Orbis Village to Hellfire Altar) and group 2 keys 57 to 67 (Magatia Village to Laboratory). These are the three chapters' worlds.

## From area and map to a name

| Function | Name | What it returns |
|---|---|---|
| `0x36E65` | `CDataMgr::AreaWorldSpot(area)` | the world spot: area 0 to 0, 1 to 1, 2 to 3, 3 to 2, 4 and 5 to 4, 6 to 8 to 6, 9 and 10 to 5, else -1 (a `switch`) |
| `0x36EE5` | `CDataMgr::WorldSpotGroup(spot)` | the name group: spots 0 to 3 give 0, 4 and 5 give 1, 6 gives 2, else -1 |
| `0x36F3D` | `CDataMgr::MapSpotIndex(area, map)` | `g_mapSpotIndex[area][map]`, an `int[11][30]` at `0x1077E0` (the literal at `0x36F70` plus `0x36F44`) |

A map's name is `g_middleSpotNames[WorldSpotGroup(AreaWorldSpot(area))][MapSpotIndex(area, map)]`, that is key `m_strMiddleSpotName.<first + index>` with `first` 1, 25 or 57 for groups 0, 1 and 2. An area's name is `g_worldSpotNames[AreaWorldSpot(area)]`, key `m_strWorldSpotName.<spot + 1>`. Areas share world spots: 4 and 5 are Orbis, 6, 7 and 8 are Magatia, 9 and 10 are El Nath.

The table's rows, with trailing zeros left out (`mtport.compendium.world.SPOT_TABLE`; `tests/test_compendium_kb_real.py` reads it back from the slice):

| Area | World spot | Group | Indices for maps 0, 1, 2, ... |
|---|---|---|---|
| 0 | 0 Kerning City | 0 | 0, 1, 2, 14, 13 |
| 1 | 1 Perion | 0 | 5, 3, 4, 6, 7, 7 |
| 2 | 3 Henesys | 0 | 10, 8, 9, 11, 12, 12 |
| 3 | 2 Ant Tunnel | 0 | 17, 15, 16, 18, 19, 20, 21, 22, 23 |
| 4 | 4 Orbis | 1 | 0, 1, 2, 3, 4, 3, 9, 5, 6, 10, 6, 7, 11, 12, 13, 14, 12, 8 |
| 5 | 4 Orbis | 1 | 15, 16, 17, 18 |
| 6 | 6 Magatia | 2 | (all 0) |
| 7 | 6 Magatia | 2 | 1, 4, 2, 3, 5, 3 |
| 8 | 6 Magatia | 2 | 6, 7, 9, 8, 10, 8 |
| 9 | 5 El Nath | 1 | 20, 19, 21, 22, 23, 24, 25, 26, 22 |
| 10 | 5 El Nath | 1 | 27, 28, 29, 30, 30, 31 |

Every one of the 67 middle-spot names is used by at least one map. Several maps share a name (Henesys's maps 2.4 and 2.5 are both "3-Way Road-Split").

### Maps past the end of their row

15 maps lie past the last non-zero entry of their area's row: 0.5, 2.6 to 2.11, 4.18, 6.1 to 6.3, 7.6, 8.6, 9.9 and 9.10. Their slot is 0, so the code gives them entry 0 of the group, a town (Kerning City Village, Orbis Village or Magatia Village). That is what the game uses, but it is probably not the map's own name: most of them are the tutorial, the climbs and the Monster Carnival rooms of [autosave.md](autosave.md); the others are 2.10, 2.11, 7.6 and 8.6. The knowledge base keeps the name the game uses, marks it `probable` and lists the reason (`name_source: "default slot"`). Area 6 has no non-zero entry at all; its map 0 is taken to be Magatia Village itself.

## Where the names are shown

- **Entering a map.** `CStateGame::Paint` (`0x87501`), after loading the backdrop and running the map script, calls the three functions (`0x87DB0`, `0x87DCA`, `0x87DF0`) and adds the name to the text list at `CStateGame +0x54` with `CNotifyTextList::Add` (`0x78339`, call at `0x87E28`). It skips 17 maps (the comparisons on `CStateGame +0x84`/`+0x88` just before `0x87DB0`): 2.5 to 2.9, 3.8, 4.8, 4.17, 6.1, 6.2, 7.4, 7.5, 9.5, 9.8 to 9.10 and 10.5. Most of them are the boss maps, the tutorial and the climbs. The knowledge base has this as the map's `banner` flag.
- **The world map.** `Map_PaintSub1` (`0x59119`) calls the same three functions for the current area and map (`0x5958A`, `0x59594`, `0x595A8`) and draws the name from `g_middleSpotNames` (a PC-relative address plus `group x 0x9C4 + index x 0x32`). `Map_OnKeySub0` (`0x48E39`) and `CStateGame::OnKeyPressed` (`0x8DD46`) call them too. `Map_PaintSub2` (`0x58AE9`, the world map) uses `AreaWorldSpot` to place the "you are here" marker among the 7 world spots.

## Evidence

- Decompiled with `tools/ghidra/run-script decompile.py OUT 0x5AE39 0x87501 0x36E65 0x36EE5 0x36F3D 0x59119 0x58AE9 0x48E39` on the project of `tools/ghidra/import` (the output is derived data and stays out of git).
- `tests/test_compendium_kb_real.py::test_name_tables_match_the_binary` reads `g_mapSpotIndex` from `work/slices/MapleThief.armv7` and compares it with the transcription.
- `tests/test_compendium_kb_real.py::test_every_map_has_a_name_or_a_reason` and `test_spot_names_are_all_used` check the names the knowledge base gives all 90 maps.
