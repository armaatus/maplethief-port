# Compendium knowledge base

The knowledge base (KB) is one cross-linked dataset of everything the bundle says about the game: monsters, items, NPCs, shops and their listings, skills, levels, areas and maps, with every reference between them resolved. The compendium site is generated from it. It is derived from each builder's own IPA, so it lives in `build/` and is never committed.

- **Code:** [`tools/mtport/compendium/kb.py`](../../tools/mtport/compendium/kb.py) (builder, writer, reader, checks, CLI), with one module per *stage*: [`tables.py`](../../tools/mtport/compendium/tables.py) (the data tables), [`world.py`](../../tools/mtport/compendium/world.py) (areas, maps, names, portals) and [`quests.py`](../../tools/mtport/compendium/quests.py) (what the scripts do), which walks the scripts with [`scriptwalk.py`](../../tools/mtport/compendium/scriptwalk.py).
- **Tests:** `tests/test_compendium_kb.py` and `tests/test_compendium_scriptwalk.py` (synthetic bundles and bytecode, CI), `tests/test_compendium_kb_real.py` and `tests/test_compendium_quests_real.py` (every shipped file, local only).
- **Issues:** [#213](https://github.com/armaatus/maplethief-port/issues/213) and [#217](https://github.com/armaatus/maplethief-port/issues/217), part of [#210](https://github.com/armaatus/maplethief-port/issues/210).

```sh
PYTHONPATH=tools python3 -m mtport.compendium.kb build [--app DIR] [--out build/compendium/kb]
PYTHONPATH=tools python3 -m mtport.compendium.kb check [--app DIR] [--out build/compendium/kb]
```

`--app` defaults to `work/ipa/Payload/MapleThief.app`. `build` writes the KB; `check` rebuilds it in memory, compares it byte for byte with the files on disk and checks every reference. Both print a `note:` line for each map whose name is a fallback (with the reason), each unused script (with the reason), each dialogue key that lacks a language and each quest no reachable path starts, an `error:` line for each problem, and exit non-zero on any problem. Two builds from the same bundle are byte-identical.

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
| `script` | the file's directory and name, joined by `-` | `"chapter1-Alex"` for `script/chapter1/Alex.out` | `{{script:chapter1-Alex}}` |
| `dialogue` | the script's id | `"chapter1-Alex"` | `{{dialogue:chapter1-Alex}}` |
| `quest` | `<chapter>-<main or sub>-<number>` | `"1-main-7"` | `{{quest:1-main-7}}` |
| `recipe` | `<chapter>-<result item>`, `-2` and up for a second recipe of the same item | `"1-equip:1"` | `{{recipe:1-equip:1}}` |
| `trade` | `<chapter>-<first cost item>-<first result item>` (`meso` for a trade with no cost item) | `"2-etc:42-etc:93"` | `{{trade:2-etc:42-etc:93}}` |
| `job_advancement` | `<chapter>-<advancement>-<job>`, `same` when the job stays | `"1-2-1"`, `"2-3-same"` | `{{job_advancement:1-2-1}}` |
| `warp` | `<script id>-<n>`, the script's `changeMap` calls numbered from 1 (main first, then the functions in slot order, then by offset) | `"common-portal000-1"` | `{{warp:common-portal000-1}}` |
| `event` | `<script id>-<type>`, then `-<argument>` for each argument | `"chapter1-MelodyMap-boss"`, `"chapter1-PerionMap-scene-15"` | `{{event:chapter1-MelodyMap-boss}}` |

`kb.ref_text(kind, id)` writes the prose form.

### References and relations

A reference is `{"kind": ..., "id": ..., ...attrs}`. A relation is stored on both sides: when entity A lists B in field `f` with some attributes, B lists A in the inverse field with the same attributes. `index.json` lists every pair. Lists are sorted by kind, natural id, then attributes.

| Field | Target | Inverse | Attributes |
|---|---|---|---|
| `monster.drops` | item | `item.dropped_by` | `slot` (0 equip, 1 to 3 etc), `chance`, `per` (100000 for equipment, 1000 for the others), `percent` (before the level bonus, [data-tables.md](../formats/data-tables.md#drop-odds)) |
| `monster.meso_drops` | item | `item.meso_dropped_by` | `min`, `max` (the amount's range), `percent` (80): the meso items (`etc:0` to `etc:2`) the amount can turn into |
| `monster.spawns_on` | map | `map.spawns` | `count`: spawn points of that monster on the map |
| `npc.stands_on` | map | `map.npcs` | `script` (the object's script name, null for decoration), `tile` (`[x, y]`); one reference per object |
| `shop.sells` | item | `item.sold_by` | `price`, `listing` |
| `shop.listings` | listing | `listing.shop` | `price` |
| `listing.item` | item | `item.listings` | `price` |
| `map.area` | area | `area.maps` | |
| `map.exits` | map | `map.entrances` | `portal` (the source portal's number, null for an NPC or map-script warp), `target_portal`, `script` (`common/portal000`, `chapter1/portal201`), `gated` |
| `script.maps` | map | `map.scripts` | `role` (`npc`, `portal`, `map`), `npc`, `portal`: the objects that name the script on maps where the game runs it |
| `dialogue.script` | script | `script.dialogue` | |
| `dialogue.npcs` | npc | `npc.dialogues` | `map` |
| `quest.given_by` | npc | `npc.gives_quests` | `map`, `script` |
| `quest.completed_by` | npc | `npc.completes_quests` | `map`, `script` |
| `quest.script` | script | `script.quests` | |
| `quest.needs_items` | item | `item.needed_by_quests` | `count` |
| `quest.needs_kills` | monster | `monster.hunted_by_quests` | `count` |
| `quest.reward_items` | item | `item.rewarded_by_quests` | `count` |
| `quest.requires` | quest | `quest.unlocks` | `op`, `value`: the other quest's state a prerequisite compares |
| `recipe.crafted_by` | npc | `npc.crafts` | `map` |
| `recipe.result` | item | `item.crafted_by` | `count` |
| `recipe.materials` | item | `item.material_of` | `count` |
| `trade.offered_by` | npc | `npc.trades` | `map` |
| `trade.cost` | item | `item.traded_away_in` | `count` |
| `trade.result` | item | `item.traded_for_in` | `count` |
| `job_advancement.given_by` | npc | `npc.advancements` | `map` |
| `warp.script` | script | `script.warps` | |
| `warp.source` | map | `map.warps_out` | `role`, `portal` |
| `warp.target` | map | `map.warps_in` | `portal` |
| `event.script` | script | `script.events` | |
| `event.maps` | map | `map.events` | |
| `npc.opens_shop` | shop | `shop.opened_by` | `map`, `script`, `chapters` (where the game runs that script on that map) |

A portal's exit is the constant `changeMap(area, map, portal)` of its script, read from every variant the game may run (`script/chapter<n>/<name>.out` first, then `script/common/<name>.out`). `gated` is true when the script does anything else (a condition, another call). The quests stage adds the exits of NPC and map scripts that warp (`portal` null, `gated` when the warp has a condition) and the conditions themselves (`warp.conditions`).

### Kinds and their fields

**monster** (69, `mob.dat`): `hp`, `level` (probable), `exp`, `meso` (`{amount, min, max, percent}` or null), `class` (`normal`, `boss`, `bomb`; probable, from `unknown_24`), `boss_bar` (ids 0x40 and up get the HUD's boss bar), `sprite` (`sprite/mob/<n>.sprprj`), `name_fills_field` (the English name fills its 16 bytes, so the game shows it cut), `unknown_23`, `unknown_24`.

**item** (212 = 74 equipment + 138 other): `table` (`equip` or `etc`), `table_id`, `category`, `price`, `icon` (index in `png/item/equip.dat` or `etc.dat`), `description` (`{en, ko}`, null for equipment). Equipment adds `slot`, `weapon_type` (`claw`, `dagger` or null), `job`, `req_level`, `speed` (unknown), `stats` (`{attack, defense, accuracy, avoid, hp, mp}`, each `{min, max}`), `bonus` (`{stat, value}` or null) and `look`. Other items add `type` (0 to 8), `hp`, `mp`, `attack`, `defense`, `accuracy`, `avoidability`, `max_stack` and `unknown_84`.

**npc** (39, `npc.dat`): `sprite` (`sprite/mob/<id + 4>.sprprj`). The quests stage adds the shops it opens, the quests it gives and completes, its recipes, trades, advancements and dialogues (the relations above).

**shop** (one per shop number used by a listing): the name is a label (`Shop 8`). The quests stage adds `unopened_reason`, null when an NPC opens the shop (`opened_by`) and otherwise why it is never seen.

**Finding a shop, recipe or trade.** `shop.opened_by`, `recipe.crafted_by` and `trade.offered_by` name the NPC and the map, `npc.stands_on` places the NPC on that map, and `map.area` gives the area. In 1.5 shops 0, 1 and 16 have listings but no opener: `showMarket` is the only way to the shop screen (`0x4628D`, called only from `0x99900`), and no script passes their numbers.

**listing** (202, `market.dat` rows 0 to 201; rows 202 to 219 are all zero and left out): `row`, `price`, `note` (row 146, a probable data slip, says so). Its name is the item's.

**skill** (24, `skill.dat`): `icon`, `branch` (`Rogue`, `Assassin`, `Bandit`), `tier`, `type` (`basic attack`, `passive`, `buff`, `active attack`), `max_level`, `learnable`, `base` and `per_level` (`{damage, time, etc, mp}`), `description` and `detail` (`{en, ko}`, `detail` with its `@` placeholders), and `levels`: one entry per level 1 to `max_level`, `{level, damage, time, etc, mp, detail}`, each value `base + (level - 1) * per_level` and `detail` with `@dmg`, `@time`, `@etc`, `@mp` filled in.

**level** (100, `hero.dat`): `level`, `exp` (EXP to finish the level), `exp_total` (EXP gained before reaching it), `hp_gain`, `mp_gain`, `max_hp` and `max_mp` (the sums up to the level), `stats` (`{strength, dexterity, intelligence, luck}`), `attack`, `defense`, `accuracy`, `avoidability`, `unknown_9`.

**area** (11, `map/<n>.mapprj`): `world_spot` (0 to 6), `tilesets`, `backdrop` (`map/back/<n>.png`), `town_bgm`, `field_bgm` (sound ids, [re/audio.md](../re/audio.md)). The name is the world spot's ([re/map-names.md](../re/map-names.md)); areas that share a world spot share a name.

**map** (90): `area_no`, `map_no`, `file` (the stem, `"103"`), `width` and `height` (tiles), `name_source`, `name_note`, `name_spot` (`{group, index, key}` of `m_strMiddleSpotName`), `banner` (the game shows the name on entering), `map_script` (the `.obj` map-script record, or null), `bgm` (`{sound, resume}`), `boss_map`, `no_save` (`tutorial`, `boss`, `climb`, `Monster Carnival` or null, [re/autosave.md](../re/autosave.md)) and `portals`: `{number, hidden, script, tile, targets}` per portal object, `targets` being `{script, map, portal, gated}` per known destination.

**script** (307, one per `.out` file): `file`, `dir` (`common`, `chapter1` to `chapter3`), `base_name` (the name objects use), `used`, `unused_reason` (null when used), `chapters` (where the game runs it), `roles`, `functions` (`main` and the functions main defines), `unreached_functions` (walked on their own, below), `paths` (per entry point, the number of paths), `approximate_entries` (entry points walked with approximate merges), `unused_quests` (`{kind, number, title_key}` an unused script prepares) and `notes` (an unused script's own slips: a value the walk cannot resolve). `used` is `code` when no object names the script and `probable` otherwise, since reachability is an upper bound.

**dialogue** (one per used NPC or map script): `chapters`, `roles`, `tree` (the root node's id), `nodes`, `state` (the NPC-state pass), `text` (`{key: {en, ko}}` for every key the script uses) and `missing_text` (keys without both languages). Its name is the NPCs' and the directory (`Mr. Lee (chapter 1)`). A node is `{function, answer, answers, paths}`: `answer` is the box answer the paths read (null at the root), `answers` the values it can take (`[0, 1]` for Accept and Refuse, the rows' values for a list). Each path is `{if, function, do, box, next, loop}`: `if` its conditions (below), `do` what it says and does in order (`{"npc": key}`, `{"hero": key, "face": n}`, `{"choice": key, "value": n}` or `{"call": native, "args": [...]}`), `box` the mode it opens the box in, `next` the node after the answer and `loop` the callback's name when the answer leads back to an earlier point of the conversation. Identical nodes are stored once. `state` lists `{if, notify, hide}`: the quest icon and visibility the pass sets.

**quest** (138): `chapter`, `quest_kind` (`main` or `sub`), `kind_no` (0 or 1), `number`, `main_story`, `title_key`, `description_key`, `description` (`{en, ko}`), `scripts`, `functions`, `requirements` (`{type: item, item, count}`, `{type: mob, monster, count}` or `{type: meso, amount}`), `rewards` (`{type: item, item, count}`, `{type: exp, amount}` or `{type: meso, amount}`), `variants` (when scripts offer it with different requirements or rewards: each `{requirements, rewards}`, the first being the fields above), `prerequisites` (conditions on the `startQuest` paths, without the quest's own state), `started`, `reachable` and `completion_scripts`. The name is the title's text; a quest without one is `Quest <id>` (`data`).

**recipe** (48): `chapter`, `quest` (the transient quest's `[kind, number]`, 77 for the smiths), `title_key`, `title`, `function`, `script`, `material_items` (`{type, item, count}`), `meso`, `result_item`, `result_count`, `other_rewards`. The name is the result item's.

**trade** (20): `chapter`, `quest` (`[1, 78]`), `title_key`, `title`, `function`, `script`, `cost_items`, `cost_meso`, `result_items`, `result_other`. The name is the first result item's.

**job_advancement** (11): `chapter`, `advancement` (the count after it, 1 to 4), `job` (the `setClassUp` job, null when it keeps the job), `job_name` (probable: 0 Rogue, 1 Assassin, 2 Bandit), `keeps_job`, `script`, `conditions`.

**warp** (173): `used`, `chapters`, `function`, `offset`, `target_map`, `target_portal` (`probable` when the target map has no portal of that number), `conditions` (box answers included), `conditional`, `chapter_end` (the path also calls `clearAll`) and `from` (`{map, role, portal, npc}` of the objects that run the script).

**event** (14): `type` (`boss`: `startBoss`, `scene`: `runAnimation(n)`, `carnival`: `initMonCa(n)`, `chapter_end`: `clearAll`), `args`, `used`, `chapters`, `conditions`, `trigger` (the roles of the objects that run the script).

### Conditions

A list of conditions is a list of alternatives, each a list of conditions that hold together: `[[]]` means always, `[]` never (no path gets there). A condition is `{term, op, value, text}` plus, for a native's result, `native` and `args`; for a quest state, `quest` (`[kind, number]`); for a box answer, `answer` (the number of the box). `text` reads as English for the common terms: `main quest 21 is done`, `level >= 30`, `advancements == 1`, and as the term itself otherwise (`checkRequire(0, 21) == 1`). Quest states are 0 not started, 1 in progress, 2 done (`getQuestState` `0x799B1`). A condition with `approximate: true` marks paths that an approximate merge joined: the other conditions of that alternative may be incomplete. In the dialogue tree a condition is the short `{term, op, value}`.

## The quests stage

The stage walks every script ([formats/script-out.md](../formats/script-out.md), [re/script-vm.md](../re/script-vm.md)) on a symbolic copy of the VM and turns the native calls on its paths into entities.

**Walking.** The shipped scripts have no loops, locals or tables, and every jump goes forward, so a function has finitely many paths. A native's result is a symbol (the call and its arguments), and a branch on it splits the path into the cases its comparisons allow, so a path that needs a quest to be both started and not started is never produced. A native that changes the game (anything but the getters and the message box) makes later reads new symbols. Paths that meet again with the same calls made and the same stack merge when their conditions on one value together allow every value of it, so a merged path's conditions still say exactly when it runs. A global the merged paths disagree on keeps both values with their conditions and splits again where it is read. A conversation follows `regCallBack` and `showMsg` into the callback with `getEvtIndex` bound to the answer, whose values depend on the box's mode ([re/script-vm.md](../re/script-vm.md#entry-points)). An answer that leads back to an earlier point of the conversation is a loop. A function no entry point reaches is walked on its own, with the globals it reads as unknowns. A conversation with more than 100,000 exact paths (20,000 in one function) is walked again with approximate merges: paths that do not cover every case merge anyway, with the `approximate` condition. In 1.5 only one NPC-state pass needs that.

**Chapters.** The game runs `script/chapter<n>/<name>.out` over `script/common/<name>.out` in chapter `n` (`0x63191`). Chapter 0 is a new game, from the intro's map 2-10 and the tutorial's 2-8 until the tutorial's `clearAll`. A chapter starts on the maps the warps of the previous chapter's `clearAll` paths lead to. From there every warp of every script an object names on a reached map is followed, whatever its conditions (an upper bound). A script runs in chapter `n` when an object on a map reached in chapter `n` names it.

**Facts.** `setQuest(kind, number, title, description)` prepares a quest and the `regRequire*` and `regReward*` calls after it fill it in. A quest is its prepared form wherever a script that runs in its chapter makes it. The givers are the NPCs whose scripts prepare it (for a map script, the NPC `setNPC` shows), the completers those whose paths call `getReward` for it, and the prerequisites the conditions of the paths that call `startQuest` for it. A quest a script prepares and drops with `removeQuest` in the same conversation is a transaction: a `recipe` when the script also calls `makeEquipItem` for it, a `trade` otherwise. `setClassUp(job, n)` is a job advancement, `showMarket(n)` links the NPC to shop `n`, and every `changeMap` is a warp.

**Text.** `regMsgNPC`, `regMsgHero` and `regSelList` take keys into `Scripts.strings` ([formats/script-out.md](../formats/script-out.md)). The dialogue holds the English and Korean text of every key; a string that is not key-shaped is text as written (the game shows an unknown key unchanged), as for the one `"..."` line. The text is in the KB output only, in `build/`.

**Unused scripts.** A script the game never runs is still a `script` entity, with `used` false and the reason: no map object names it, the game runs a chapter file of the same name instead in every chapter where a map naming it is reached, or no map that names it is reached in any chapter. The quests only such scripts prepare are its `unused_quests`, not quests.

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
