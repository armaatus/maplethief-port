# Compendium knowledge base

The knowledge base (KB) is one cross-linked dataset of everything the bundle says about the game: monsters, items, NPCs, shops and their listings, skills, levels, areas and maps, with every reference between them resolved. The compendium site is generated from it. It is derived from each builder's own IPA, so it lives in `build/` and is never committed.

- **Code:** [`tools/mtport/compendium/kb.py`](../../tools/mtport/compendium/kb.py) (builder, writer, reader, checks, CLI), with one module per *stage*: [`tables.py`](../../tools/mtport/compendium/tables.py) (the data tables) and [`world.py`](../../tools/mtport/compendium/world.py) (areas, maps, names, portals).
- **Tests:** `tests/test_compendium_kb.py` (synthetic bundle, CI) and `tests/test_compendium_kb_real.py` (every shipped file, local only).
- **Issue:** [#213](https://github.com/armaatus/maplethief-port/issues/213), part of [#210](https://github.com/armaatus/maplethief-port/issues/210).

```sh
PYTHONPATH=tools python3 -m mtport.compendium.kb build [--app DIR] [--out build/compendium/kb]
PYTHONPATH=tools python3 -m mtport.compendium.kb check [--app DIR] [--out build/compendium/kb]
```

`--app` defaults to `work/ipa/Payload/MapleThief.app`. `build` writes the KB; `check` rebuilds it in memory, compares it byte for byte with the files on disk and checks every reference. Both print a `note:` line for each map whose name is a fallback (with the reason), an `error:` line for each problem, and exit non-zero on any problem. Two builds from the same bundle are byte-identical.

## Files

`build/compendium/kb/` holds one file per kind plus an index. The directory belongs to the command: `build` removes any other `*.json` in it.

| File | Content |
|---|---|
| `<kind>.json` | `{"schema": 1, "kind": "<kind>", "entities": [...]}`, the entities sorted by id in natural order (digit runs compare as numbers: `"2"` before `"10"`, `"etc:3"` before `"etc:12"`) |
| `index.json` | `{"schema": 1, "kinds": {"<kind>": <count>, ...}, "relations": [{"kind", "field", "target", "inverse", "confidence"}, ...]}` |

Every file is UTF-8 JSON with sorted keys, `ensure_ascii` off (Korean is stored as text), an indent of 1 and a trailing newline.

Read it with `mtport.compendium.kb.load_kb(path)`, which returns `{kind: {id: entity}}`; `load_relations(path)` returns the relation pairs of `index.json`, and `check_kb(entities, relations)` lists every problem.

## Entities

Every entity has:

| Field | Value |
|---|---|
| `id` | a string, unique within the kind (the forms are below) |
| `kind` | the kind, as in the file name |
| `name` | `{"en": ..., "ko": ...}`: English and Korean, both non-empty |
| other fields | listed per kind below |
| relation fields | lists of references (below), present even when empty |
| `confidence` | `{field: level}` for every field except `id`, `kind` and `confidence` |

**Confidence levels** follow the evidence column of the specs: `code` (the use was seen in the code), `data` (inferred from the shipped values), `probable` (inferred, but not certain), `unknown` (nothing reads it, or the meaning is not known). Labels the KB makes up itself, such as "Shop 3" or "Level 12", are `data`.

**Text.** In names, the game's `$` line break becomes a space ("Mrs. Ming Ming"). In descriptions it becomes `\n`, as does a literal backslash-n.

### Ids and references in prose

| Kind | Id | Example | Prose reference |
|---|---|---|---|
| `monster` | `mob.dat` id | `"2"` | `{{monster:2}}` |
| `npc` | `npc.dat` id | `"12"` | `{{npc:12}}` |
| `item` | `equip:<item_equip id>` or `etc:<item_etc id>` | `"etc:3"` | `{{item:etc:3}}` |
| `shop` | the `market.dat` shop number | `"8"` | `{{shop:8}}` |
| `listing` | the `market.dat` row | `"146"` | `{{listing:146}}` |
| `skill` | `skill.dat` id | `"2"` | `{{skill:2}}` |
| `level` | the level | `"10"` | `{{level:10}}` |
| `area` | the area number (`map/<area>.mapprj`) | `"4"` | `{{area:4}}` |
| `map` | `<area>-<map>`, the two numbers of the file name `"%d%d"` | `"10-3"` for `103.map` | `{{map:10-3}}` |

`kb.ref_text(kind, id)` writes the prose form.

### References and relations

A reference is `{"kind": ..., "id": ..., ...attrs}`. A relation is stored on both sides: when entity A lists B in field `f` with some attributes, B lists A in the inverse field with the same attributes. `index.json` lists every pair. Lists are sorted by kind, natural id, then attributes.

| Field | Target | Inverse | Attributes |
|---|---|---|---|
| `monster.drops` | item | `item.dropped_by` | `slot` (0 equip, 1 to 3 etc), `chance`, `per` (100000 for equipment, 1000 for the others), `percent` (the real odds before the level bonus: the game's random numbers are 15-bit, so an equipment chance is really in 32768, [mechanics](../re/mechanics.md#random-numbers)), `nominal_percent` (the table's face value) |
| `monster.meso_drops` | item | `item.meso_dropped_by` | `min`, `max` (the amount's range), `percent` (80): the meso items (`etc:0` to `etc:2`) the amount can turn into |
| `monster.spawns_on` | map | `map.spawns` | `count`: spawn points of that monster on the map |
| `npc.stands_on` | map | `map.npcs` | `script` (the object's script name, null for decoration), `tile` (`[x, y]`); one reference per object |
| `shop.sells` | item | `item.sold_by` | `price`, `listing` |
| `shop.listings` | listing | `listing.shop` | `price` |
| `listing.item` | item | `item.listings` | `price` |
| `map.area` | area | `area.maps` | |
| `map.exits` | map | `map.entrances` | `portal` (the source portal's number), `target_portal`, `script` (`common/portal000`, `chapter1/portal201`), `gated` |

A portal's exit is the constant `changeMap(area, map, portal)` of its script, read from every variant the game may run (`script/chapter<n>/<name>.out` first, then `script/common/<name>.out`). `gated` is true when the script does anything else (a condition, another call): the conditions themselves are the quests stage's ([#217](https://github.com/armaatus/maplethief-port/issues/217)).

### Kinds and their fields

**monster** (69, `mob.dat`): `hp`, `level`, `exp`, `meso` (`{amount, min, max, percent}` or null), `class` (`normal`, `boss`, `bomb`, from the movement kind), `boss_bar` (ids 0x40 and up get the HUD's boss bar), `sprite` (`sprite/mob/<n>.sprprj`), `name_fills_field` (the English name fills its 16 bytes, so the game shows it cut), `walk_speed` (`unknown_23`), `movement_kind` (`unknown_24`: 1 walks, 2 floats, 3 explodes on contact).

**item** (212 = 74 equipment + 138 other): `table` (`equip` or `etc`), `table_id`, `category`, `price`, `icon` (index in `png/item/equip.dat` or `etc.dat`), `description` (`{en, ko}`, null for equipment). Equipment adds `slot`, `weapon_type` (`claw`, `dagger` or null), `job`, `req_level`, `speed` (unknown), `stats` (`{attack, defense, accuracy, avoid, hp, mp}`, each `{min, max}`), `bonus` (`{stat, value}` or null) and `look`. Other items add `type` (0 to 8), `hp`, `mp`, `attack`, `defense`, `accuracy`, `avoidability`, `max_stack` and `unknown_84`.

**npc** (39, `npc.dat`): `sprite` (`sprite/mob/<id + 4>.sprprj`). Which NPC opens which shop and gives which quest comes from the scripts (#217).

**shop** (one per shop number used by a listing): no fields of its own; the name is a label (`Shop 8`).

**listing** (202, `market.dat` rows 0 to 201; rows 202 to 219 are all zero and left out): `row`, `price`, `note` (row 146, a probable data slip, says so). Its name is the item's.

**skill** (24, `skill.dat`): `icon`, `branch` (`Rogue`, `Assassin`, `Bandit`), `tier`, `type` (`basic attack`, `passive`, `buff`, `active attack`), `max_level`, `learnable`, `base` and `per_level` (`{damage, time, etc, mp}`), `description` and `detail` (`{en, ko}`, `detail` with its `@` placeholders), and `levels`: one entry per level 1 to `max_level`, `{level, damage, time, etc, mp, detail}`, each value `base + (level - 1) * per_level` and `detail` with `@dmg`, `@time`, `@etc`, `@mp` filled in.

**level** (100, `hero.dat`): `level`, `exp` (EXP to finish the level), `exp_total` (EXP gained before reaching it), `hp_gain`, `mp_gain`, `max_hp` and `max_mp` (the sums up to the level), `stats` (`{strength, dexterity, intelligence, luck}`), `attack`, `defense`, `accuracy`, `avoidability`, `unknown_9`.

**area** (11, `map/<n>.mapprj`): `world_spot` (0 to 6), `tilesets`, `backdrop` (`map/back/<n>.png`), `town_bgm`, `field_bgm` (sound ids, [re/audio.md](../re/audio.md)). The name is the world spot's ([re/map-names.md](../re/map-names.md)); areas that share a world spot share a name.

**map** (90): `area_no`, `map_no`, `file` (the stem, `"103"`), `width` and `height` (tiles), `name_source`, `name_note`, `name_spot` (`{group, index, key}` of `m_strMiddleSpotName`), `banner` (the game shows the name on entering), `map_script` (the `.obj` map-script record, or null), `bgm` (`{sound, resume}`), `boss_map`, `no_save` (`tutorial`, `boss`, `climb`, `Monster Carnival` or null, [re/autosave.md](../re/autosave.md)) and `portals`: `{number, hidden, script, tile, targets}` per portal object, `targets` being `{script, map, portal, gated}` per known destination.

### Map names

Every map's `name` is the one the game uses ([re/map-names.md](../re/map-names.md)). `name_source` says how sure that is:

| `name_source` | Meaning | `name` confidence |
|---|---|---|
| `spot` | the map has its own entry in the name table | `code` |
| `default slot` | the map lies past the end of its area's row, so the game falls back to the group's first name (a town); `name_note` says so | `probable` |
| `none` | the area has no name table (no shipped map) | `unknown` |

The 15 `default slot` maps of 1.5 are 0-5, 2-6 to 2-11, 4-18, 6-1 to 6-3, 7-6, 8-6, 9-9 and 9-10. `build` and `check` print them as `note:` lines.

## Adding a stage

A stage is a function `stage(kb, src)`. It reads the bundle through `src` (`kb.Sources`: `src.app`, `src.tables`, `src.script_extra`, `src.script_variants(name)`), adds entities with `kb.add(kind, id, name, {field: (value, confidence)})` and links them with `kb.link(kind, id, field, target_kind, target_id, **attrs)`. Register each relation once, at import, with `kb.relation(kind, field, target_kind, inverse, confidence)`. `kb.set(kind, id, field, value, confidence)` adds a field to an entity of another stage. A link to a missing entity is not stored; it becomes a build problem. Append the stage to `kb.stages()`; the CLI, the files, `index.json` and the checks pick it up with no other change.
