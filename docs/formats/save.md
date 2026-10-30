# Save data (`Documents/theif%d`, `quest%d`, ...)

The game saves its state as **raw binary files in the app's `Documents/` directory**, written with `NSFileHandle`. It does not use `NSUserDefaults` or `NSKeyedArchiver` for save data, and no file has a checksum, signature or version field. Each of the three character slots is two files: `theif<slot>` (the character, 1,124 bytes) and `quest<slot>` (its quest state, 4,781 bytes). A few more small files hold options, scores and in-app-purchase receipts.

- **Parser:** [`tools/mtport/savedata.py`](../../tools/mtport/savedata.py): parse and encode every file below, byte-exact. `python3 -m mtport.savedata dump FILE...` prints files as JSON, `new-character` writes the files of a new character, and `names` writes `work/re/names/save.json`. Tests: [`tests/test_savedata.py`](../../tests/test_savedata.py) (CI, synthetic saves) and [`tests/test_savedata_real.py`](../../tests/test_savedata_real.py) (local).
- **Status:** every field `CDataMgr::SaveSaveData` writes is located and typed from the code. Most have a known meaning; the unknown ones are marked. No save from a real device has been checked yet ([pulling one](#pulling-a-save-from-a-device) is a `needs-human` step).

Addresses are armv7 VM addresses; function pointers carry the Thumb bit. `CDataMgr` is the data manager, the object at `*0x11A970` ([data-tables.md](data-tables.md)). The hero object is `CHero` (its methods use `CHero.*` string keys, for example `CHero.setMeso.1` in `0x618E1`).

## Storage mechanism

| Evidence | What it shows |
|---|---|
| `0x6EF1` | path = `[[NSHomeDirectory() stringByAppendingPathComponent:@"Documents"] stringByAppendingPathComponent:[NSString stringWithUTF8String:name]]`. The `Documents` CFString `0x117F34` is referenced only here and in `0x7105`. |
| `0x6F65` (open) | mode 1 `fileHandleForReadingAtPath:`, 2 `fileHandleForWritingAtPath:`, 4 writing + `truncateFileAtOffset:0`, 8 `createFileAtPath:contents:attributes:` if missing, then `fileHandleForUpdatingAtPath:`. |
| `0xB785` → `0xB729` (write) | opens in mode 8 and calls `0x6C41`: `[[NSData alloc] initWithBytes:buf length:n]`, `writeData:`, `synchronizeFile`; then `0x6BF5` `closeFile`. It writes at offset 0 and **never truncates**. |
| `0xB7C9` (read) | `0x71E9` gets the size (`fileHandleForReadingAtPath:` → `availableData` → `length`), then opens the file (mode 8) and `0x6CED` reads that many bytes (`readDataOfLength:` + `getBytes:`) into a `new[]` buffer. |
| `0xB6A9` (exists) | `0x7105`: `[[NSFileManager defaultManager] fileExistsAtPath:]`. |
| `NSUserDefaults` | all 14 code references to the `standardUserDefaults` selector are in Chartboost: `-[Chartboost startSession]`, `cacheInterstitial:`, `showInterstitial:`, `…IdentityTracking…`, `-[CBAPIConnection didFailForRequest:]`, `retryDeliveries`, `+[CBIdentity cbhhna/cbui/cbuia]` and `CBWebImageCache`. |
| `NSKeyedArchiver` | `archivedDataWithRootObject:` / `unarchiveObjectWithData:` are used only by `-[CBAPIConnection didFailForRequest:]` / `retryDeliveries` (Chartboost's request queue). |
| `CC_MD5`, `CC_SHA1` | `_CC_MD5` (stub `0xF0CAC`) has one caller, `-[NSString(MD5Addition) stringFromMD5]` `0xC12F9`. It is used by `-[UIDevice(IdentifierAddition) uniqueDeviceIdentifier]` (MAC address → MD5). The game sends that id in a network packet (`0x98865` → `0x371ED`); it never stores it. `_CC_SHA1` is used only by `+[CBCrypto SHA1DataWithData:]` `0xD3051`, inside Chartboost. Neither touches a save file. |
| `NSSearchPathForDirectoriesInDomains` | Game code calls it once, in `0x9462D`, to **delete** a slot (below). Its other callers are `CBWebImageCache` methods. |

## Files

| File | Bytes | Written by | Read by | Contents |
|---|---|---|---|---|
| `theif0`, `theif1`, `theif2` (sic) | 1,124 (`0x464`) | `CDataMgr::SaveSaveData` `0x38711`; new: `CDataMgr::LoadSaveData` `0x39989` | `CDataMgr::LoadSlots` `0x37A01` → `CHero::LoadSave` `0x677E9` | one character |
| `quest0`, `quest1`, `quest2` | 4,781 (`0x12AD`) | `CDataMgr::SaveQuestData` `0x38F55`; new: `CDataMgr::LoadQuestData` `0x37D7D` | `0x37A01` → `QuestSave_Apply` `0x83631` | that character's quests |
| `config` | 6 | `CDataMgr::LoadConfig` `0x38611` (defaults), `0x46A29`, `0x50131` (options menu) | `0x38611` | options |
| `cupon` | 5,962 (`0x174A`) | `CDataMgr::SaveCupon` `0x38171` | `CDataMgr::LoadCupon` `0x38311` | 3 best scores + 50 string records |
| `CTerms` | 2 | `-[TermsView releaseAgreementButton:]` `0xC1729` | `-[MainMenuView drawRect:]` (only whether it exists) | the Korean terms of service were accepted |
| `crestore`, `riprestore1`, `riprestore2`, `totalrestore` | varies | `0x3ED19`, `0x87501`, `0x8D4C9`, `-[InAppPurchaseManager completeTransaction:]` | the same | in-app purchase receipt lists |

There are three character slots (`0`–`2`). The slot in use is `CDataMgr+0x4E4`, set by `LoadSaveData`/`LoadQuestData`. `NoCharge1` and `NoCharge2` are only tested for existence (`0x823A1`); the game never creates them.

**Integers** are little-endian, written byte by byte (`0x1BCB1` i16, `0x1BD09` i32) and read back by `0x1BC61` (i16) and `0x1BCD5` (i32); `i16` is signed. **Text** fields are fixed-width. `0x1BE31` copies at most `n` bytes and pads with NULs, so a string of `n` bytes or more has no terminator. Readers copy `n` bytes and add one (`0x1BDB1`).

**Uninitialised bytes.** Each writer allocates its buffer with `operator new[]` and does not clear it. `SaveSaveData` never writes `theif` bytes `0x009`–`0x00C` or `0x315`–`0x324`, and `SaveQuestData` never writes `quest` bytes `0x12A4`–`0x12AC`. A saved file holds whatever was on the heap there. The parser keeps these as `unwritten_*` bytes so that a round trip is exact, and nothing reads them.

## When the game saves and loads

- **Startup / slot list:** `CDataMgr::LoadSlots` `0x37A01` (from `0x9462D` and `0x96845`) frees the cached buffers (`CDataMgr::FreeSlot` `0x3757D`). Then, for each slot whose `theif%d` exists, it reads `theif%d` into `CDataMgr+0x4DC[slot]` and `quest%d` into `CDataMgr+0x4E0[slot]`.
- **Choosing a slot:** `LoadSaveData(slot)` `0x39989` and `LoadQuestData(slot)` `0x37D7D` return the cached buffer. For an empty slot they build a new one (below) and **write it to disk at once**.
- **Slot screen:** `0x96845` shows each existing slot from the cached buffer: `CHero::LoadSavePreview` `0x672A9` (job, level, worn items), the level, and the caption at `0x424`.
- **Entering the game:** `0x86C1D` builds the hero with `CHero::LoadSave` `0x677E9`. It applies `quest%d` with `0x83631`, then reads the map (`0x41C`, `0x41D`) and position (`0x41E`, `0x420`) itself. With `chapter == 0` (a new game) it ignores them and starts the intro at map (area, 10), position (−50, 239).
- **Saving:** `SaveSaveData` and `SaveQuestData` are always called as a pair. The callers are `0x3ED19` (after a purchase or restore is granted), `0x54BF9` (a menu action; it refuses on a fixed list of maps), `0x62ADD` (a hero state change), `0x7C2D9` (after a quest reward item is granted) and `0x8D4C9` (two sites in the game state). The game has no autosave timer.
- **Deleting a slot:** `0x9462D` gets `NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, …)[0]`, formats `"%@/%@"` with `quest%d` and `theif%d`, and calls `removeItemAtPath:error:` on both if both exist.

**Quirk:** `0xB6E9` checks for `"thief%d"`, correctly spelled, for slots 0 and 1 (from `0x46A29`). The game only ever writes `"theif%d"`, so that check is always false. A port must keep the misspelled file names to stay compatible with real saves, and should reproduce the always-false probe.

## `theif0`–`theif2`: the character (1,124 bytes)

Written by `CDataMgr::SaveSaveData` `0x38711` from the hero (`this` = data manager, argument = `CHero`). *Hero field* is the `CHero` offset that is saved; *world* is the object at `hero+0x150`. *Read by* is `CHero::LoadSave` `0x677E9`, unless noted.

| Offset | Type | Field | Hero field | Meaning | Read by |
|---|---|---|---|---|---|
| 0x000 | u8 | `branch` | `+0x88` | job branch: 0 Rogue, 1 Assassin, 2 Bandit (`skill.dat` `branch`). Branch 1/2 with tier > 1 loads the `hero` sprite 1/2. | `0x677E9` |
| 0x001 | u8 | `job_tier` | `+0x8C` | job advancement, 0 = beginner. It picks the skill slots (`0x7FD69`); hero `+0xB4` = 2 if 0, else 8. | `0x677E9` |
| 0x002 | u8 | `level` | `+0x80` | level 1–100 (`hero.dat` key) | `0x677E9` |
| 0x003 | i32 | `exp` | `+0x90` | EXP in the current level | `0x677E9` |
| 0x007 | i16 | `sp` | `+0xBC` | unspent skill points (`0x7F99D` subtracts from it) | `0x677E9` |
| 0x009 | 4 bytes | `unwritten_009` | | **not written by `SaveSaveData`**. `LoadSaveData` puts the new character's max HP and max MP here (i16 each); nothing reads them. | none |
| 0x00D | i16 | `hp` | `+0x94` | current HP | `0x677E9` |
| 0x00F | i16 | `mp` | `+0x98` | current MP (an int, stored truncated) | `0x677E9` |
| 0x011 | i32 | `meso` | `+0xD0` | meso (`CHero.setMeso` `0x618E1`) | `0x677E9` |
| 0x015 | 48 × 16 | `equip_inventory` | `+0xD4[48]` | equipment inventory, [equipment records](#equipment-record-16-bytes) | `0x677E9` → `0x64309` |
| 0x315 | 16 bytes | `unwritten_315` | | **never written** (neither by `SaveSaveData` nor by `LoadSaveData`) | none |
| 0x325 | 48 × 3 | `etc_inventory` | `+0xD8[48]` | other items: `u8 id` (`item_etc`, `0xFF` = empty, item `+0x90`), `i16 count` (item `+0x100`) | `0x677E9` → `0x699A1` |
| 0x3B5 | 13 × u8 | `skill_levels` | `[+0x114]+0xC+8i` | skill level per [skill slot](#skill-slots) | `0x677E9` → `0x7F99D` |
| 0x3C2 | 5 × 16 | `equipped` | `+0xDC[5]` | worn items, [equipment records](#equipment-record-16-bytes), indexed by `item_equip.slot` (0 hat, 1 weapon, 2 overall, 3 earrings, 4 shoes) | `0x677E9`, `0x672A9` → `0x64CA1` |
| 0x412 | 5 × 2 | `shortcuts` | `+0xE0+8i`, `+0xE4+8i` | five (kind, id) pairs; kind 0 = empty. A new character has (1, 3) and (1, 6) = its two potions. Other kinds are unknown (probably skills). | `0x677E9` |
| 0x41C | u8 | `area` | world `+0x84` | map area: picks `<area>.mapprj` and `"%d%d.map"` (area, map) | `0x86C1D` |
| 0x41D | u8 | `map` | world `+0x88` | map in the area | `0x86C1D` |
| 0x41E | i16 | `x` | `+0x4` | position; −1 = the map's default | `0x86C1D` |
| 0x420 | i16 | `y` | `+0x8` | position; −1 = the map's default | `0x86C1D` |
| 0x422 | u8 | `flags` | `+0xC` | hero flag bits, low byte (bit 3 is cleared by `0x62ADD`); meaning unknown | `0x677E9`, `0x672A9` |
| 0x423 | u8 | `action` | `+0x10` | the hero's state, restored with `0x60551` (a new character has 2) | `0x677E9`, `0x672A9` |
| 0x424 | char[64] | `caption` | | UTF-8 text shown on the slot: the title of the current quest's section from `CDataMgr::QuestCaption` `0x37095`, or ScriptExtra `CDataMgr.SaveSaveData.1` ("How it All Began", "모험의 시작") if there is none. Read with `stringWithCString:encoding:NSUTF8StringEncoding`. Korean text can be cut mid-character at 64 bytes. | `0x96845` |

The records end exactly at `0x464`.

### Equipment record (16 bytes)

Used by `equip_inventory` and `equipped`. Written from the item object, rebuilt by `0x6A9DD` + `0x69A51` + `0x69145`.

| Offset | Type | Field | Item field | Meaning |
|---|---|---|---|---|
| +0x0 | u8 | `id` | `+0x90` | `item_equip` id; `0xFF` = empty (the rest is then 0) |
| +0x1 | u8 | `rolled` | `+0xD4` | 0: base stats are the table minimums (`0x696A9`). 1: a dropped item (`0x69BB9`); its base stats are 0 and the rolled stats are in the six fields below. It also picks the scroll success table (`0x69511`). |
| +0x2 | u8 | `slots_left` | `+0xCC` | upgrade slots left; a scroll uses one (`0x69511`). Dropped items start with 7; the starting club has 5. |
| +0x3 | u8 | `scrolls_used` | `+0xD0` | successful scrolls; with `rolled`, it indexes the success rates `{100, 75, 50, 25, 10}` / `{100, 80, 60, 40, 30, 20, 10}` % |
| +0x4 | i16 × 6 | `attack` `defense` `accuracy` `avoid` `hp` `mp` | `+0xD8`…`+0xEC` | stat bonuses on top of the table stats: the drop roll (`rolled` = 1) plus every successful scroll |

### Skill slots

`skill_levels[i]` is the level of the skill in slot `i` of the hero's skill list. That list is not stored: `0x7FD69` rebuilds it from `branch` and `job_tier` before the levels are applied. For each tier from `job_tier` down to 1, it takes every `skill.dat` row, in table order, with `type != 0` (not a basic attack), that `tier`, and `branch` 0 or the hero's branch. The unused slots are id −1. So a tier-4 Assassin has slots `[14, 11, 12, 13, 6, 7, 8, 9, 10, 2, 3, 4, 5]`: 13 slots, all filled. When the tier changes, `0x7FD69` carries each level over by skill id. `savedata.skill_slots()` computes the list.

### A new character

`LoadSaveData` `0x39989` builds this for an empty slot and writes it immediately. `hp`/`mp` come from `hero.dat` level 1 (`hp_gain`, `mp_gain` = 10, 10):

- `branch` 0, `job_tier` 0, `level` 1, `exp` 0, `sp` 0; `0x009` = max HP, max MP; `hp`/`mp` the same; `meso` 100;
- all inventory slots empty; `etc_inventory[0]` = 5 × Red Potion (3), `[1]` = 5 × Blue Potion (6);
- `equipped[1]` (weapon) = Wooden Club (`item_equip` 72 = `0x48`), `rolled` 0, `slots_left` 5;
- `shortcuts[1]` = (1, 3), `[2]` = (1, 6); `area` 2, `map` 8 (`28.map`), `x` = `y` = −1, `flags` 0, `action` 2;
- `caption` = ScriptExtra `CDataMgr.LoadSaveData.1` ("How it All Began").

```
0000  00 00 01 00 00 00 00 00 00 0a 00 0a 00 0a 00 0a   branch tier level | exp | sp | 10 10 | hp 10 mp 10
0010  00 64 00 00 00 ff 00 00 00 00 00 00 00 00 00 00   meso 100 | inventory slot 0: empty
0320  00 00 00 00 00 03 05 00 06 05 00 ff 00 00 ff 00   etc: (3, 5) (6, 5) empty...
03D0  00 00 48 00 05 00 00 00 00 00 00 00 00 00 00 00   equipped[1] = 0x48, slots_left 5
0410  00 00 00 00 01 03 01 06 00 00 00 00 02 08 ff ff   shortcuts (0,0) (1,3) (1,6) | area 2 map 8 | x
0420  ff ff 00 02 48 6f 77 20 69 74 20 41 6c 6c 20 42   y | flags action | "How it All B"
```

(`python3 -m mtport.savedata new-character DIR --hp 10 --mp 10` writes these bytes, with zeros where the game leaves heap contents.)

## `quest0`–`quest2`: the quest state (4,781 bytes)

Written by `CDataMgr::SaveQuestData` `0x38F55` from the quest manager (`[hero+0x150]+0x58`), applied by `QuestSave_Apply` `0x83631`. A new slot gets all records empty and everything else 0 (`0x37D7D`).

| Offset | Type | Field | Source | Meaning |
|---|---|---|---|---|
| 0x000 | u8 | `chapter` | questmgr `+0x4` | current chapter; 0 = new game (`0x86C1D` starts the intro) |
| 0x001 | u8 × 100 | `flags_a` | questmgr `+0x8+4i` | one byte per entry; quest progress flags, meaning unknown |
| 0x065 | u8 × 100 | `flags_b` | questmgr `+0x19C+4i` | one byte per entry; meaning unknown |
| 0x0C9 | 10 × 0x1C9 | `records` | questmgr `+0x32C` list | the active quests, [quest records](#quest-record-0x1c9-bytes); loading stops at the first empty one |
| 0x12A4 | 9 bytes | `unwritten_tail` | | **never written** |

### Quest record (0x1C9 bytes)

| Offset | Type | Field | Source | Meaning |
|---|---|---|---|---|
| +0x000 | u8 | `key_a` | quest `+0x4` | with `key_b`, identifies the quest (`0x7B150`); `0xFF` = empty slot |
| +0x001 | u8 | `key_b` | quest `+0x8` | |
| +0x002 | u8 | `extra` | quest `+0xC` | restored through `state+0xAC`; meaning unknown |
| +0x003 | char[64] | `title` | quest `+0x14` | a `Scripts.strings` key (shown via `0x6DD1(key, "Scripts")`) |
| +0x043 | char[256] | `description` | quest `+0x18` | a `Scripts.strings` key |
| +0x143 | 5 × 14 | `requirements` | quest `+0x24` list | see below; kind `0xFF` ends the list |
| +0x189 | 5 × 13 | `rewards` | quest `+0x28` list | see below; kind `0xFF` ends the list |

**Requirement (14 bytes):** `u8 kind; u8 item_table; u8 item_id; i16 item_count; u8 target; i16 needed; i16 progress; i32 value` (offsets +0, 1, 2, 3, 5, 6, 8, 0xA). Kind 0 is "have `item_count` of item `item_id`", where `item_table` is 0 = `item_equip` and 1 = `item_etc` (checked by `0x7B68D`; built by `0x79B1D`). Kind 1 uses `target`, `needed`, `progress` (`0x79B4D`), probably "defeat `needed` of monster `target`". Kind 2 uses `value` (`0x79B7D`). All fields are always written.

**Reward (13 bytes):** `u8 kind; u8 item_table; u8 item_id; i16 item_count; i32 value_1; i32 value_2` (offsets +0, 1, 2, 3, 5, 9). Kind 0 = item (`0x79DCD`), kind 1 = `value_1` (`0x79DFD`), kind 2 = `value_2` (`0x79E19`). Which of EXP and meso is kind 1 or 2 is not settled.

**Overlap:** the requirements start at +0x143 and the rewards end at +0x1CA, so each record writes 0x1CA bytes into a 0x1C9 stride. The last byte of `rewards[4].value_2` is the next record's `key_a`. The game writes the records in order, so `key_a` wins, and reading back gives `rewards[4].value_2` with `key_a` as its top byte. This only matters for a quest with a fifth reward of kind 2. `savedata.link_quest_records()` applies the overlap to a synthetic save.

## `config` (6 bytes)

Loaded (and created with the defaults) by `CDataMgr::LoadConfig` `0x38611` into the buffer at `*0x11A98C` (`g_pConfig`). The options menu writes it back (`0x50131`, `0x46A29`).

| Offset | Default | Field | Meaning |
|---|---|---|---|
| 0 | 3 | `sound` | `0x37C79` (`printf "sound=%d"`): 0–3, bit 0 = effects (OpenAL listener gain 1/0), bit 1 = music (`AVAudioPlayer` volume 0.7/0) ([audio.md](../re/audio.md#settings-pause-and-interruptions)). |
| 1 | 1 | `speed` | game speed: 0 / 1 / 2 select 65 / 50 / 40 from table `0x108C58`, passed to `0x21DA1`. These are the `CMTimer` tick lengths in ms, applied by `CDataMgr::ApplyGameSpeed` `0x37C41` from `LoadConfig` and from the options menu `0x46A29` ([timing-threading.md](../re/timing-threading.md#interval-and-game-speed)). Values above 2 aren't guarded: 3 gives −1 (a tick on every 1 ms poll). |
| 2 | 0 | `unknown_2` | not read by the config loader |
| 3 | 1 | `unknown_3` | vibration: `0x37BD1` calls `CMSoundPlayer::SetVibrate` with 0 when the byte is 0, else with 1, and stores the byte in two player fields nothing reads (`0x1CC11`, `0x1CC39`). When on, `CMSoundPlayer::Vibrate` plays `kSystemSoundID_Vibrate` ([audio.md](../re/audio.md#vibration)) |
| 4 | 1 | `unknown_4` | cleared by `0x3EAA1` in state `0x68` |
| 5 | 1 | `unknown_5` | index into `{0, 1, 3}` (`0x108C4C`), read unchecked by `0x37B85` (passed to `0x785BD`) and `CStateGame::LoadGame` `0x86C1D`. Values above 2 read past the table. |

The game writes only 0–2 to bytes 1 and 5, but never checks them on load. The port reads values above 2 as 2 (`mt_saves_sanitise`, [ADR 0074](../adr/0074-save-files-atomic-writes-sanitising-and-ios-documents.md)), leaving the file itself unchanged; the original's out-of-bounds read is on [the list of original bugs](../re/original-bugs.md#4-unchecked-config-bytes).

## `cupon` (5,962 bytes)

`CDataMgr::SaveCupon` `0x38171` and `LoadCupon` `0x38311`. The latter runs at startup and creates the file with zeros and empty strings.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0x000 | i32 × 3 | `best_scores` | data manager `+0x438`, `+0x43C`, `+0x440`: the best score of each of three modes of a mini-game; raised by its result screen `0x819E5` (`CStateGame.resultMonCa.*`) |
| 0x00C | 50 × 0x77 | `entries` | `char[64] text_a; char[35] text_b; char[10] text_c; char[10] text_d` (data manager `+0x118`, `+0x1E0`, `+0x2A8`, `+0x370` pointer arrays), listed by the menu in `0x9462D`; meaning unknown (the name suggests coupons) |

## `CTerms` (2 bytes)

Two zero bytes, written when the player accepts the Korean terms of service. Only the file's existence is checked, by `-[MainMenuView drawRect:]` when the locale's country code (`objectForKey:` at `0xAD8C6`) is `KR`.

## Receipt lists: `crestore`, `riprestore1`, `riprestore2`, `totalrestore`

```
i16 count
count × { u8 flag; i16 len; u8 receipt[len] }      no terminator, no padding
```

Each list holds App Store receipts (`transactionReceipt.bytes`, or `getPurchasedReceipt`) of restorable purchases. A new entry is appended with `flag` 0 by rewriting the whole file (`-[InAppPurchaseManager completeTransaction:]`, `0x8D4C9`). `riprestore1`/`riprestore2` go with purchase kinds 7 and 8 (`CDataMgr+0x538`). `0x3EAA1` reads only their `count`. Which product each file belongs to ([re/cash-shop.md](../re/cash-shop.md), #156): `crestore` holds cash shop purchases, `riprestore1` the POW! Package revive (`powpackage`), `riprestore2` the Comeback Pack, and `totalrestore` transactions that StoreKit completed outside a purchase the game started (an earlier session's). An entry is added when StoreKit reports the purchase and removed (the list is rewritten with one entry fewer) when the server confirms it with op 3 or op 5; a non-empty list makes the game ask for a restore first. Every writer found sets `flag` 0; what reads it is not established. Because files are updated without truncating, a list rewritten shorter would keep stale bytes after its end; the parser keeps them as `trailing`.

## Porting notes

- Keep the file names and bytes exactly, misspelling included; a real device's `Documents/` can then be copied in as is. The port stores them in `Documents/` under the per-user data directory (`~/Library/Application Support/MapleThief/Documents/` on macOS, `%APPDATA%\MapleThief\Documents\` on Windows, `$XDG_DATA_HOME/MapleThief/Documents/` on Linux; `MAPLETHIEF_DATA_DIR` moves it), and on iOS in the app's own `Documents/`, which the Files app can see ([ADR 0074](../adr/0074-save-files-atomic-writes-sanitising-and-ios-documents.md)). The `save_dir` setting moves `Documents/`, for example to a cloud-synced folder. In portable mode (a `portable.txt` beside the executable) the data directory is `data/` beside it. After each save the port copies the saved character to `backups/<slot>/<time>/` in the data directory. `--export-saves` and `--import-saves` move a set of saves as a folder or a zip ([ADR 0078](../adr/0078-save-export-import-backups-and-checks.md)).
- There is no version field, so the port can't detect an older format; this is the only one that shipped in 1.5.
- The game's writes are not atomic: it overwrites in place and calls `synchronizeFile`. The port writes the same bytes to a temporary file (`<name>.mt-tmp`, flushed to the disk) and renames it over the file at `synchronizeFile`/`closeFile`, so a crash never leaves half a save ([ADR 0074](../adr/0074-save-files-atomic-writes-sanitising-and-ios-documents.md)). Tools that copy saves skip `*.mt-tmp`.
- Treat the `unwritten_*` bytes as don't-care: write zeros, accept anything.
- With the `cash_shop = meso` Assist ([ADR 0072](../adr/0072-offline-cash-shop-and-revives.md)) a receipt list can hold the port's synthetic receipts (the text `maplethief-offline 1 <product id>` and a NUL) for the few frames between a purchase and its grant. Beside the saves the port keeps its own `maplethief-cashshop<slot>` (the cash shop's counters: `key = value` lines, written atomically), which the game never reads. Export, import and the backups ([ADR 0078](../adr/0078-save-export-import-backups-and-checks.md)) carry it with the slot's files. An import of `theif<n>` without it removes the old one (backed up first), and a save copied without it starts its counters afresh.

## Checking a save

The loader trusts everything above: it indexes tables with item ids, loads `<area><map>.map` by number, reads skill and config values as table indexes, and passes quest keys to `NSString`. Before the port imports or restores a file it checks it ([ADR 0078](../adr/0078-save-export-import-backups-and-checks.md)). The checks run in C, `mt_saves_check` in `runtime/src/foundation/saves.cpp`, because players have no Python. `tools/mtport/savecheck.py` has the same rules written on the parsed records of `savedata.py`. CTest `saves.check_parity` runs both on the same synthetic fixtures. A file gets the first rule it breaks, going through it in byte order; a file the game wrote always passes. A broken file is refused, never repaired.

| Verdict | Rule |
|---|---|
| `bad_name` | not one of the file names above, nor the port's `maplethief-cashshop0`–`2` |
| `bad_size` | not the file's size (`theif` 1,124, `quest` 4,781, `config` 6, `cupon` 5,962, `CTerms` 2); a receipt list under 2 bytes or over 1 MiB; `maplethief-cashshop<n>` over 1,024 bytes |
| `bad_job` | `theif`: `branch` above 2 or `job_tier` above 4 (`skill.dat`'s branches and tiers) |
| `bad_level` | `theif`: `level` outside 1–100 (`hero.dat`'s rows) |
| `bad_item` | an equipment id that is neither `0xFF` nor below 74 (`item_equip.dat`'s rows), or another item id that is neither `0xFF` nor below 138 (`item_etc.dat`'s rows). Checked in the inventory, the worn items, and the item requirements and rewards of the active quests |
| `bad_skill` | `theif`: a skill level above 20, the highest `max_level` in `skill.dat` |
| `bad_map` | `theif`: an `area` above 10, or a `map` past the area's last shipped map. Areas 0–10 have 6, 5, 12, 9, 19, 4, 4, 7, 7, 11 and 6 maps |
| `bad_quest` | `quest`: in an active record (one before the first empty record), a `title` or `description` that is not well-formed UTF-8 text without control characters (below `0x20`, or `0x7F`) ending with a NUL inside its field (the keys are ASCII but for one Korean `Scripts.strings` key with Hangul in it), a requirement or reward (one before the list's `0xFF`) of kind above 2, or of kind 0 with an item table other than 0 or 1 |
| `bad_config` | `config`: byte 1 (speed) or byte 5 above 2. Byte 0 (sound) is a set of bits, so any value is accepted |
| `bad_receipts` | a receipt list with a negative count, or an entry that runs past the end. Bytes after the last entry (an old tail) are allowed |
| `bad_counters` | `maplethief-cashshop<n>`, the port's cash shop counters ([ADR 0072](../adr/0072-offline-cash-shop-and-revives.md)): a byte that is not printable ASCII, a tab, `\r` or `\n`, or a line that is not blank, a `#` comment or `key = number` (the key letters, digits and `_`; the number 1–10 digits, at most 4,294,967,295). The game never reads this file; the check keeps a renamed or damaged file out |
| `bad_pair` | (a set) `theif<n>` without `quest<n>`, or the reverse. The game reads both for a slot whose `theif` exists |

The limits are facts about the 1.5 data. `tests/test_savecheck_real.py` checks them against the bundle, and checks that every `Scripts.strings` key passes the key rule. `unwritten_*` bytes, `exp`, `hp`, `mp`, `meso`, positions, flags, `chapter`, quest progress and the other `config` bytes are not checked: the game reads them as numbers and never as indexes.

## Pulling a save from a device

On a jailbroken device the files are in the app's sandbox: `/var/mobile/Applications/<UUID>/Documents/` (iOS ≤ 7) or `/var/mobile/Containers/Data/Application/<UUID>/Documents/` (iOS 8+). Copy all of `theif0`–`theif2`, `quest0`–`quest2`, `config`, `cupon`, `CTerms` and the receipt lists that exist. Then run `MAPLETHIEF_SAVE_DIR=<copy> scripts/test` (`tests/test_savedata_real.py::test_pulled_device_saves_round_trip`) and `python3 -m mtport.savedata dump <copy>/*`.

## Validation

- `tests/test_savedata.py` (CI) builds synthetic saves from this spec. It checks that every field lands at the offset in the tables above, and that a synthetic character, quest state, `config`, `cupon` and receipt list round-trip byte-exactly through parse and encode. It also round-trips random bytes for each fixed-size file, checks the new-character defaults against `LoadSaveData`, checks the quest-record overlap, rejects malformed input, and checks that this spec's `theif` and `quest` tables list the parser's fields in order.
- `tests/test_savecheck.py` (CI) runs [the checks](#checking-a-save) in Python on 50 fixture sets, valid and hostile, and checks that the C side has the same limits, codes and order. CTest `saves.check_parity` runs the same fixtures through `MapleThief --check-saves` as folders and as deflated zips, and compares the verdicts. `saves.cli_round_trip` imports each fixture with `--import-saves`: a refused one changes nothing, and an accepted one ends in `Documents/` byte for byte. It is then exported to a folder and to a zip, and both match.
- `tests/test_savedata_real.py` (local): the new-character save built from the shipped `hero.dat` refers to the Wooden Club, the two potions, `28.map` and `2.mapprj`. The default caption matches `ScriptExtra.strings`. The skill slots of a tier-4 Assassin and Bandit fill exactly 13 distinct skills. The named functions exist in the Ghidra export. A pulled device save round-trips (skipped until one exists).
