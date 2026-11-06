# Language data tables (`<Language>/data/*.dat`)

The game's static content (character levels, monsters, NPC names, items, skills, shop stock) lives in seven fixed-record tables per language, plus a one-value `buildup.dat`.

- **Files:** `English/data/`, `Korean/data/` and `Japanese/data/` in `MapleThief.app/`, each with `hero`, `mob`, `npc`, `item_equip`, `item_etc`, `skill`, `market` and `buildup` `.dat`: 24 files.
- **Parser:** [`tools/mtport/datatables.py`](../../tools/mtport/datatables.py) (one typed dataclass per table; field order = byte order). Tests: [`tests/test_datatables.py`](../../tests/test_datatables.py) (synthetic) and [`tests/test_datatables_real.py`](../../tests/test_datatables_real.py) (every shipped file).
- **Status:** all 24 files parse with zero leftover bytes (21 tables + 3 `buildup.dat`). Every column's position and type is known from code; the names of a few columns are inferred or unknown (marked below).

## Where the game loads them

Addresses are armv7 VM addresses (function pointers with the Thumb bit).

**Language choice:** `0x6A15` builds the path. It compares `[NSLocale preferredLanguages][0]` with `ko` and `ja` and formats `/Korean%s`, `/Japanese%s` or `/English%s` around `/data/<name>.dat`. Any other language gets English.

**Loading:** `0xB689` reads the whole file into a `malloc`ed buffer (through `0xB5DD` → CFBundle/CFURL). Each table has a small loader that stores the buffer pointer in a global. Nothing is parsed at load time: the game keeps the raw bytes and reads columns on demand.

| Table | Loader | Buffer pointer (global) | Called from |
|---|---|---|---|
| `hero` | `0x67181` | `0x11A9A8` | `0x39989` (new game), `0x68A59` |
| `mob` | `0x77225` | `0x11A9E8` | `0x775DD` |
| `npc` | `0x39895` | `0x11A988` | `0x86C1D` |
| `item_equip`, `item_etc` | `0x86215` (both) | `0x11A9AC`, `0x11A9B0` | `0x86C1D` |
| `skill` | `0x800A9` | `0x11A9F8` | `0x80541` |
| `market` | `0x861D1` | `0x11A994` | `0x86C1D` |
| `buildup` | `0x980B5` (title scene, state 3) | value stored at `+0x40` of the object at `*0x11A970` | `0x981B9`, `0x98751` |

The tables are not read through the `DataReader` class (its RTTI name `10DataReader` is in the binary). That class serves other formats.

**Column accessors.** These are methods of the data manager (the object at `*0x11A970`) with the signature `(this, table, byte_offset, row)`. The callers pass the column's byte offset as a constant, which is where every layout below comes from:

| Accessor | Reads at `table + 6 + row * stride + offset` | Helper |
|---|---|---|
| `0x398D5` | `u8` | `0x1BD99` |
| `0x3793D` | `i16` LE, sign-extended (`ldrsh`) | `0x1BC61` |
| `0x385D1` | `i32` LE | `0x1BCD5` |
| `0x37999` | string: copies `len` bytes (stack argument) into a new `len + 1` buffer and NUL-terminates it | `0x1BDB1` |
| `0x3990D` | first row whose `u8` at `offset` equals a key, or `-1` (the lookup used with offset 0) | |
| `0x37975` / `0x37919` | `rows` (header `+2`) / `stride` (header `+4`) | |

Text is turned into `NSString`s with `stringWithCString:encoding:` and `CFStringConvertEncodingToNSStringEncoding(0x0940)`, i.e. `kCFStringEncodingEUC_KR`, for every language (for example at `0x6DD1` and `0x72F1`). English text is plain ASCII, so it is unaffected.

## Common layout

```
u16 columns      number of columns in a record
u16 rows
u16 stride       bytes per record
rows × record    no padding, no trailer: file size = 6 + rows × stride
```

Every value is little-endian. A column is one of:

| Type | Size | Notes |
|---|---|---|
| `u8` | 1 | unsigned |
| `i16` | 2 | signed (some columns hold `-1` or negative per-level steps) |
| `i32` | 4 | read as `int`; no shipped value is negative |
| `char[n]` | n | NUL-padded; text runs to the first NUL. A string that fills the field has no NUL (the game's copy adds one). Padding after the NUL is all zero in every shipped file. |

The header's `columns` and `stride` are checked against the layouts below, and the parser rejects any file whose size is not exactly `6 + rows × stride`.

The first `u8` of the six keyed tables is the lookup key. The game finds rows with `0x3990D` (first match), and the keys are unique in every shipped file. `item_equip`, `item_etc`, `npc` and `skill` keys equal the row index, while `hero` and `mob` keys are the row index + 1.

| File | columns | rows | stride | size |
|---|---|---|---|---|
| `hero.dat` | 13 | 100 | 18 | 1,806 |
| `mob.dat` | 17 | 69 | 50 | 3,456 |
| `npc.dat` | 2 | 39 | 33 | 1,293 |
| `item_equip.dat` | 24 | 74 | 73 | 5,408 |
| `item_etc.dat` | 14 | 138 | 218 | 30,090 |
| `skill.dat` | 19 | 24 | 297 | 7,134 |
| `market.dat` | 4 | 220 | 7 | 1,546 |
| `buildup.dat` | (not a table) | | | 1 |

**Languages:** `Japanese/data` is byte-identical to `English/data`. `Korean/data` has the same headers and the same numeric columns in every row, and differs only in text. `hero`, `market` and `buildup` have no text and are byte-identical across all three languages.

In the field tables below, *Evidence* is one of: **code**, meaning the use was seen in the code (address given); **data**, meaning the name was inferred from the shipped values and texts; **unknown**.

## `hero.dat`

One row per character level, 1–100 (`HeroLevel`).

| Offset | Type | Field | Meaning | Range | Evidence |
|---|---|---|---|---|---|
| 0x00 | u8 | `level` | level, lookup key | 1–100 | code |
| 0x01 | i32 | `exp` | EXP needed to finish this level | 10–73,260 | code: returned for the current level by `0x65919` (EXP bar, level-up); name inferred |
| 0x05 | i16 | `hp_gain` | max HP gained at this level; max HP = Σ over levels 1..L (`0x63C3D`) | 10–100 | code; HP vs MP order inferred |
| 0x07 | i16 | `mp_gain` | max MP gained at this level; Σ in `0x63BB9` | 10–100 | code; order inferred |
| 0x09 | u8 | `unknown_9` | never read through the accessors; 1 in every row (maybe skill points per level) | 1 | unknown |
| 0x0A | u8 | `strength` | status-window stat 1 (read at offset `0x0A + i` by `0x56321`) | 5–39 | code + data: probably STR |
| 0x0B | u8 | `dexterity` | status-window stat 2 | 4–54 | probably DEX |
| 0x0C | u8 | `intelligence` | status-window stat 3 | 4–20 | probably INT |
| 0x0D | u8 | `luck` | status-window stat 4 | 8–108 | probably LUK (the thief's main stat, highest) |
| 0x0E | u8 | `attack` | base attack → hero `+0xA4` (`0x642BD`, level-up `0x66695`) | 1–126 | probable: a passive skill (probably Weapon Mastery) adds its `@etc` to `+0xA4` (`0x7F99D`) |
| 0x0F | u8 | `defense` | → hero `+0xA8` (`0x64271`) | 6–124 | probable |
| 0x10 | u8 | `accuracy` | → hero `+0xB0` (`0x641D9`) | 1–122 | code: Nimble Body adds to `+0xB0` and `+0xAC`; which one is accuracy is inferred |
| 0x11 | u8 | `avoidability` | → hero `+0xAC` (`0x64225`) | 7–102; 50 for levels 1–4 | probable |

The mean of `attack` and `defense` is also used to scale monsters in one game mode (see `mob.level`).

Example (level 1, English = Korean): `01 | 0a 00 00 00 | 0a 00 | 0a 00 | 01 | 05 04 04 08 | 01 06 01 32`.

## `mob.dat`

One row per monster (`Mob`). The monster constructor is at `0x73B51`.

| Offset | Type | Field | Meaning | Range | Evidence |
|---|---|---|---|---|---|
| 0x00 | u8 | `id` | monster id, lookup key | 1–69 | code |
| 0x01 | char[16] | `name` | display name | | code (`0x73B51`, length 16) |
| 0x11 | i32 | `hp` | max HP → mob `+0x88`/`+0x90` | 1–80,000 | code |
| 0x15 | i16 | `level` | → mob `+0x98`. When a game-mode flag (`[[mob+0x15C]+0x404]`) is set, it is pulled halfway toward the hero's `(attack + defense) / 2` and HP is raised to 150 %. | 8–150 | code; the name is a guess |
| 0x17 | u8 | `unknown_23` | → mob `+0x20` | 0–5 | unknown (maybe speed) |
| 0x18 | u8 | `unknown_24` | → mob `+0xAC`: 1 normal, 2 boss (Malady, Balrog), 3 Malady Bomb | 1–3 | unknown (kind/AI?) |
| 0x19 | i16 | `exp` | EXP given on kill (`0x73349` → `0x6654D`) | 0–18,000 | code |
| 0x1B | i16 | `meso` | meso drop: 80 % chance of an amount uniform in [`meso` × 8 / 10, `meso` × 12 / 10] (`0x5C3C1`, see [Drop odds](#drop-odds)) | 0–30,000 | code |
| 0x1D | u8 | `equip_drop` | `item_equip` id dropped (`0x5C8ED` → `0x69B5D`) | 0–68 | code |
| 0x1E | i32 | `equip_drop_chance` | chance in 1/100,000, plus the hero's level (0 = no drop; [Drop odds](#drop-odds)) | 0–600 | code |
| 0x22 | u8 | `etc_drop_1` | `item_etc` id; slots 1–3 are rolled in a loop at `0x5C699` (offset `0x22 + 5i` / `0x23 + 5i`) | 0–102 | code |
| 0x23 | i32 | `etc_drop_1_chance` | chance in 1/1000, plus the hero's level / 10 (0 = no drop) | 0–60 | code |
| 0x27 | u8 | `etc_drop_2` | `item_etc` id | 0–123 | code |
| 0x28 | i32 | `etc_drop_2_chance` | chance in 1/1000, plus the hero's level / 10 | 0–930 | code |
| 0x2C | u8 | `etc_drop_3` | `item_etc` id | 0–127 | code |
| 0x2D | i32 | `etc_drop_3_chance` | chance in 1/1000, plus the hero's level / 10 | 0–100 | code |
| 0x31 | u8 | `sprite` | sprite project: `/sprite/mob/<n>.sprprj` (`0x74EC5` → `0x20005`) | 60–128 | code |

Example, Blue Snail (English): `01 "Blue Snail" 00×6 | 14 00 00 00 (hp 20) | 08 00 | 02 | 01 | 07 00 | 0f 00 | 00 | 00 00 00 00 | 00 00000000 | 20 a2030000 (32, 930) | 06 28000000 (6, 40) | 3c`.
In Korean the name is `c6 c4 b6 f5 b4 de c6 d8 c0 cc` = "파란달팽이" and the other 34 bytes are identical.

### Drop odds

A kill rolls each drop slot on its own (the rolls go through `Rand_Chance(n, d)` `0x22685`, true when `Rand_Next() % d + 1 <= n`). The level is read from `+0x80` of the hero object reached through the mob (`getLevel`, [script-vm.md](../re/script-vm.md), reads the same field), so it is probably the hero's level.

- **Equipment** (`0x5C8ED`): only when `equip_drop_chance` is 1 or more, `Rand_Chance(equip_drop_chance + level, 100000)`. A chance of 600 is 0.6 % before the level bonus, not 60 %. The id is not checked: `equip_drop` 0 with a chance drops item 0 (the Red Snail's Red Woolen Hat).
- **Other items** (`0x5C699`, slots 1 to 3): only when the slot's chance is 1 or more, `Rand_Chance(level / 10 + chance, 1000)`. An id of 0 with a chance drops `item_etc` 0.
- **Meso** (`0x5C3C1`): an amount `Rand_Range(meso × 8 / 10, meso × 12 / 10)` (integer division), dropped when it is 1 or more and `Rand_Percent(80)` passes. The drop is the `item_etc` meso item 0 for 1 to 99, 1 for 100 to 199 and 2 for 200 and up. A flag argument of the routine switches to a second formula (`meso / k − 5` to `meso / 18`, with a divisor not yet read); where it is set is not known.

The compendium knowledge base gives each drop its chance, its divisor (`per`) and its percentage before the level bonus ([compendium/kb.md](../compendium/kb.md)).

Five English names are exactly 16 bytes with no NUL (for example `Reinforced Iron ` from "Reinforced Iron Mutae"), so the game shows them truncated.

## `npc.dat`

One row per NPC (`Npc`).

| Offset | Type | Field | Meaning | Range | Evidence |
|---|---|---|---|---|---|
| 0x00 | u8 | `id` | NPC id, lookup key | 0–38 | code |
| 0x01 | char[32] | `name` | display name; `$` marks a line break (`Mrs. $Ming Ming`) | | code (`0x32BBD`, `0x35189`) |

One caller (`0x8572D`, at `0x8604E`) copies 64 bytes from the 32-byte name. For the last row that reads 32 bytes past the end of the file buffer; the text still ends at the name's NUL. A native runtime should give table buffers slack, or clamp the copy.

Example: `00 "Tombstone" 00×23 | 01 "Mr. Lee" …`.

## `item_equip.dat`

One row per piece of equipment (`ItemEquip`). Item objects are built at `0x696A9` (fixed stats: the minimum of each pair) and `0x69BB9` (dropped items: each stat rolled uniformly in [min, max] by `0x22641`, plus the bonus). The six stats are in the order **ATT, DEF, ACC, AVOID, HP, MP**, the same order scrolls add them in (`0x69145` ← `0x69511`). They land in the item at `+0xB4 … +0xC8`.

| Offset | Type | Field | Meaning | Range | Evidence |
|---|---|---|---|---|---|
| 0x00 | u8 | `id` | item id, lookup key | 0–73 | code |
| 0x01 | char[32] | `name` | display name | | code |
| 0x21 | u8 | `slot` | 0 hat, 1 weapon, 2 overall, 3 earrings, 4 shoes → item `+0x88` | 0–4 | code + data |
| 0x22 | u8 | `weapon_type` | 0 none, 1 claw, 2 dagger → item `+0x8C` | 0–2 | code + data |
| 0x23 | u8 | `job` | required job → item `+0xA8`; 4 (thief) everywhere | 4 | data |
| 0x24 | u8 | `req_level` | required level → item `+0xAC` | 1–65 | data |
| 0x25 | u8 | `speed` | → item `+0xB0`; weapons only | 0–3 | unknown (probably attack speed) |
| 0x26 | i16 | `attack_min` | ATT, low end of the roll | 0–68 | code |
| 0x28 | i16 | `attack_max` | ATT, high end | 0–78 | code |
| 0x2A | i16 | `defense_min` | DEF | 0–51 | code |
| 0x2C | i16 | `defense_max` | | 0–64 | code |
| 0x2E | i16 | `accuracy_min` | ACC (0 in every row) | 0 | code |
| 0x30 | i16 | `accuracy_max` | | 0 | code |
| 0x32 | i16 | `avoid_min` | AVOID (0 in every row) | 0 | code |
| 0x34 | i16 | `avoid_max` | | 0 | code |
| 0x36 | i16 | `hp_min` | max HP bonus | 0–285 | code |
| 0x38 | i16 | `hp_max` | | 0–303 | code |
| 0x3A | i16 | `mp_min` | max MP bonus | 0–355 | code |
| 0x3C | i16 | `mp_max` | | 0–367 | code |
| 0x3E | u8 | `bonus_stat` | dropped items: stat 1–6 (ATT…MP, `tbb` at `0x69D2A`) that gets `bonus_value`; 0 none. 3 (ACC) on weapons/earrings, 4 (AVOID) on hats/overalls/shoes. | 0, 3, 4 | code |
| 0x3F | i16 | `bonus_value` | amount added to that stat | 0–14 | code |
| 0x41 | i32 | `price` | price in meso → item `+0x9C` | 100–72,760 | code + data |
| 0x45 | i16 | `icon` | inventory icon index (`0x31EC1`, `0x69A51`) | 0–72 | code |
| 0x47 | i16 | `look` | avatar sprite index when worn (`0x694C1`); -1 for shoes and earrings | -1–24 | code + data |

Several hats and overalls have an HP or MP bonus with `min > max` (for example `hp_min = 5, hp_max = 0`). The fixed-stat path takes the minimum. What the roll does with an inverted range depends on `0x22641`.

Example: `00 "Red Woolen Hat" … | 00 00 04 0a 00 | 0000 0000 0200 0400 0000…`.

## `item_etc.dat`

One row per non-equipment item: meso, potions, throwing stars, scrolls, materials, quest and cash items (`ItemEtc`). Built at `0x696A9` (second half) and applied as a scroll at `0x69511`.

| Offset | Type | Field | Meaning | Range | Evidence |
|---|---|---|---|---|---|
| 0x00 | u8 | `id` | item id, lookup key | 0–137 | code |
| 0x01 | char[64] | `name` | display name | | code |
| 0x41 | u8 | `type` | 0 meso, 1 potion, 2 throwing star, 3 scroll, 4 return scroll, 5 etc/material, 6 quest item, 7 special (cash) item, 8 100 % scroll → item `+0x8C` | 0–8 | code + data |
| 0x42 | i16 | `hp` | HP restored (potions; percent for Elixir/Power Elixir) or max HP added (scrolls) → item `+0xC4` | 0–1,000 | code + data |
| 0x44 | i16 | `mp` | MP restored or added → item `+0xC8` | 0–800 | code + data |
| 0x46 | i16 | `attack` | ATT added by a scroll; a star's attack | 0–25 | code (`0x69511`) + data |
| 0x48 | i16 | `defense` | DEF added | 0–2 | code + data |
| 0x4A | i16 | `accuracy` | ACC added | 0–2 | code + data |
| 0x4C | i16 | `avoidability` | AVOID added | 0–2 | code + data |
| 0x4E | i16 | `max_stack` | stack size → item `+0xFC`: 100, stars 200–800, 0 for meso | 0–800 | code + data |
| 0x50 | i32 | `price` | price in meso → item `+0x9C` | 0–10,000 | code + data |
| 0x54 | i32 | `unknown_84` | → item `+0xA0` and `+0xA4`; non-zero only for throwing stars | 0–30 | unknown |
| 0x58 | i16 | `icon` | inventory icon index (`0x31EC1`, `0x4CAED`) | 0–108 | code |
| 0x5A | char[128] | `description` | description; `$` marks a line break, `\n` also occurs | | code (length 128) |

Example: `03 "Red Potion" … | 01 | 3200 0000 0000 0000 0000 0000 | 6400 | 05000000 | 00000000 | 0300 | "Recover HP+50"`.

## `skill.dat`

One row per skill (`Skill`). Each value pair gives `base + (skill_level − 1) × per_level` (MP cost at `0x7D251`, others at `0x7D325`, `0x7D551`, `0x7EC3D`). It fills the `@dmg`, `@time`, `@etc` and `@mp` placeholders of `detail`.

| Offset | Type | Field | Meaning | Range | Evidence |
|---|---|---|---|---|---|
| 0x00 | u8 | `id` | skill id, lookup key | 0–23 | code |
| 0x01 | char[16] | `name` | display name | | code |
| 0x11 | i16 | `icon` | icon index; the sprite entry is `icon × 0x24` (`0x7D1AD`) | 0–21 | code |
| 0x13 | u8 | `branch` | 0 Rogue (1st job), 1 Assassin (claw), 2 Bandit (dagger) | 0–2 | code (`0x7FD69`) + data |
| 0x14 | u8 | `tier` | skill-window tier | 1–4 | code (`0x7FD69`) + data |
| 0x15 | u8 | `type` | 0 basic attack, 1 passive, 2 buff, 3 active attack (`0x7EEBD` tests 1) | 0–3 | code + data |
| 0x16 | u8 | `max_level` | maximum skill level; 0 = not learnable | 0–20 | code + data |
| 0x17 | u8 | `unknown_23` | never read through the accessors; 0 in every row | 0 | unknown |
| 0x18 | u8 | `unknown_24` | never read through the accessors; 0 in every row | 0 | unknown |
| 0x19 | i16 | `damage_base` | `@dmg` (damage %) at level 1 | 0–65 | code + data |
| 0x1B | i16 | `damage_per_level` | added per level | 0–31 | code |
| 0x1D | i16 | `time_base` | `@time` (duration) at level 1 | 0–500 | code + data |
| 0x1F | i16 | `time_per_level` | added per level | 0–50 | code |
| 0x21 | i16 | `etc_base` | `@etc`: skill-specific value (accuracy, crit %, max HP %, pierce count…) | 0–900 | code + data |
| 0x23 | i16 | `etc_per_level` | added per level | -10–100 | code |
| 0x25 | i16 | `mp_base` | `@mp` (MP cost) at level 1 | 0–220 | code (`0x7D251`) |
| 0x27 | i16 | `mp_per_level` | added per level | -5–25 | code |
| 0x29 | char[128] | `description` | description | | code (`0x4C2C1`, length 128) |
| 0xA9 | char[128] | `detail` | effect text with `@mp`, `@dmg`, `@time`, `@etc` | | code (`0x4C2C1`, length 128) |

Example: `02 "Lucky Seven" … | 0300 | 00 01 03 0f 00 00 | 1e00 0b00 (30 % + 11/level) | 0000 0000 | 0000 0000 | 0600 0200 (6 MP + 2/level) | …`.

## `market.dat`

Shop stock: one row per listing (`MarketItem`), read by the shop screen `0x4628D`. It scans all rows and keeps those whose `shop` equals the open shop. Rows 202–219 are all zero (unused). One listing in the middle of shop 8 (row 146) has `shop` 0, which looks like a data slip.

| Offset | Type | Field | Meaning | Range | Evidence |
|---|---|---|---|---|---|
| 0x00 | u8 | `shop` | shop id | 0–16 | code |
| 0x01 | u8 | `table` | 0 = `item_equip` (`0x69B5D`), otherwise `item_etc` (`0x699A1`) | 0–1 | code |
| 0x02 | u8 | `item` | item id in that table | 0–102 | code |
| 0x03 | i32 | `price` | buying price in meso → item `+0x98` | 0–99,000 | code |

Example: `00 00 00 | aa000000` (shop 0 sells equip 0, Red Woolen Hat, for 170).

## `buildup.dat`

**`buildup.dat` is not a table.** It is one ASCII byte, `"1"` (`0x31`), with no newline, and it is identical in all three languages.

`0x980B5` handles one state of the title/logo scene (state 3, just before `SetState(TITLE_MAIN)`). In that state it loads the file through `0xB5DD` and copies its bytes into a NUL-terminated string (`0x1BDB1`). It converts the string with `atoi` and stores the result at `+0x40` of the object at `*0x11A970`. The parser (`parse_buildup`) returns that integer.

No reader of that field was found. The search covered the 93 field reads through the `0x11A970` pointer, the `this`-relative reads in the data-manager methods, and `SaveSaveData` (`0x38711`) / `LoadSaveData` (`0x39989`). So what it controls is still unknown. The name suggests a build or content revision ("build-up"). Re-check this with Ghidra cross-references once #7 lands.

## Validation

- `tests/test_datatables_real.py` (local, `real_assets`; needs `scripts/bootstrap`):
  - all 24 files parse with zero leftover bytes;
  - Japanese is byte-identical to English;
  - English and Korean have the same row counts and identical numeric columns;
  - every Korean string decodes strictly (CP949, and re-encodes as EUC-KR), has no U+FFFD and contains Hangul;
  - English text is ASCII;
  - lookup keys are unique;
  - mob drops and market listings point at existing items.
- The data-table browser (`PYTHONPATH=tools python3 -m mtport.tablebrowser build`, issue #28) renders all 8 files as pages in `assets-out/viewers/tables/`: English and Korean text side by side, enum values labelled, drop and shop item ids linked to their rows, and the inventory icons of `item_equip`, `item_etc` and `skill` from the `png/item/equip.dat`, `png/item/etc.dat` and `png/skill/skillico.dat` packs (image index = `icon`, [png-dat.md](png-dat.md)). Its `check` command and `tests/test_tablebrowser_real.py` confirm each page's row count equals the parser's record count.
- `tests/test_datatables.py` (CI): layouts against the shipped header shapes, hand-written byte fixtures, encode/parse round trips, malformed-input rejection, and a check that this spec lists every parser field in order.
