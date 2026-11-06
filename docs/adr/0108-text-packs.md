# 108. Text packs: the game's text and the port's in UTF-8 from a folder

- **Status:** Accepted
- **Date:** 2026-11-05
- **Issue:** #189
- **Builds on:** [ADR 0032](0032-corefoundation-object-model.md) (CF strings and encodings), [ADR 0035](0035-foundation-subset.md) (NSBundle's `.strings`), [ADR 0036](0036-coregraphics-and-uikit-drawing.md) (text drawing, NanumGothic), [ADR 0053](0053-observed-functions-game-events-and-rumble.md) (guest function hooks), [ADR 0070](0070-settings-registry-and-presets.md) (the registry), [ADR 0081](0081-texture-packs-and-nearest-texture-filter.md) and [ADR 0106](0106-audio-packs.md) (packs: the pattern)

## Context

#189 asks for text packs: community translations, in UTF-8, without touching the game's files. The research is in [re/text.md](../re/text.md). What it found:

- **Three sources.** Dialogue and the C++ code's sentences come from `Scripts.strings` and `ScriptExtra.strings` through `LocalizedCString` `0x6DD1`, which already hands the game UTF-8. Names and descriptions come from fixed-width fields of the data tables, read raw by `0x37999` (a full field needs no NUL). The port's own menus are C++ literals in `platform/src/`.
- **EUC-KR first.** `TextCache_DrawText` `0x9031` converts each text trying EUC-KR before UTF-8, and many accented Latin sequences ("ö" is `C3 B6`) are valid EUC-KR syllables: a pack's "Pötion" would draw as Hangul.
- **Three bytes a character.** `Text_Wrap` `0x22EC5` steps through text with `Text_CharCount` `0x22989` and `Text_SubChars` `0x22A1D`, which count 3 bytes for any byte ≥ 0x80 (Hangul's UTF-8 length): two-byte characters are cut in half at line ends.
- **No glyphs.** NanumGothic has 38 of Latin-1's 96 characters and no Greek or Cyrillic.
- **Fixed fields.** A table name has 16 to 64 bytes; a UTF-8 translation can be longer than the original.
- **The port's fonts are ASCII**, and its layout code counts bytes.
- **Packs are untrusted**: downloaded files (ADR 0081's rule).

## Options considered

**The file format.**

1. **Apple's `.strings` text syntax, strictly UTF-8**, one file per source (`Scripts`, `ScriptExtra`, `Tables`, `Port`) plus a manifest. Chosen: it is the format the game's own text has (as a binary plist), translators' tools read it, keys are the game's own, and comments carry each field's room.
2. JSON or PO files. Either works, but neither matches the game's keys and tables one to one, and PO's plural and context machinery has nothing to attach to here.
3. Binary plists like the bundle's. Not editable by hand.

**UTF-8 against EUC-KR first.**

1. **While a pack is open, the Korean encodings read valid UTF-8 as UTF-8 and write UTF-8.** Chosen: no game text is valid UTF-8 outside ASCII (0 of the 526 Korean table strings; English is ASCII), so the game's own text converts exactly as before, and every path that names EUC-KR, MacKorean or CP949 (`0x9031`, `0x3243D`, `0x59D08`) is covered at once.
2. Hooking `TextCache_DrawText` to try UTF-8 first. It misses `drawRequire` and the measure, and duplicates their logic.
3. Converting the pack to EUC-KR. EUC-KR has no "ö".

**The wrap's step.** Hooks on `Text_CharCount` and `Text_SubChars` that step by the UTF-8 lead byte (installed only while a pack is open). Their step equals the original's for every text the game ships (ASCII 1, Hangul 3). Patching `Text_Wrap` itself would mean re-implementing its layout.

**The glyphs.**

1. **A pinned fallback font, Fira Sans Regular (OFL 1.1), glyph by glyph** for characters NanumGothic lacks. Chosen: Latin, Greek and Cyrillic in one 457 KB file; text NanumGothic covers draws as before.
2. A font from the pack. Declined: stb_truetype is not hardened against malformed fonts.
3. Noto Sans. Similar coverage at a larger size; Fira Sans's look sits closer to the game's.

**Table fields.** The pack's value is written into the table file's bytes as the game reads it (`CFURLCreateDataAndPropertiesFromResource`), up to the field's width, cut at a whole character and logged once. Growing the fields would change the record layout the game indexes by offset.

**The port's menus.** One table (`port_text.def`, X-macro `MT_TEXT(id, "English")`), looked up by `tr(Txt::id)`; the settings registry's labels and help and the menu actions keep their English in their rows and translate by derived keys. A translation is folded to ASCII for the port's fonts. Positional places `{0}`, `{1}` rather than printf formats, so a translation cannot crash the port with a wrong `%s`; a translation whose places differ from the English is not used.

## Decision

- **The pack** is a folder; files match in any case:
  - `textpack.strings` (required): `format` = `"1"`, `name`, `language` (a BCP 47 tag); optional `version`, `game_version`, `author`.
  - `Scripts.strings`, `ScriptExtra.strings`: the game's keys.
  - `Tables.strings`: `<table>.<row>.<column>` for the eight text columns (`mob.name`, `npc.name`, `item_equip.name`, `item_etc.name`, `item_etc.description`, `skill.name`, `skill.description`, `skill.detail`), row = the record's index.
  - `Port.strings`: `port_text.def`'s ids, `setting.<key>.label|help`, `action.<key>.label|help|button|confirm`.
- **The syntax** ([formats/text-pack.md](../formats/text-pack.md)): strict UTF-8 (an optional BOM), `"key" = "value";`, `/* */` and `//` comments, the escapes `\" \\ \' \n \t \r \Uxxxx \uxxxx` (surrogate pairs joined), no NUL. Limits: 4 MiB a file, 256 bytes a key, 4,096 a value, 65,536 entries. A file that breaks a rule is refused whole (`line N: reason`, logged); a key given twice keeps its last value.
- **Runtime** (`maplethief/textpack.h`, `runtime/src/textpack/`): `mt_textpack_open(DIR)`, `close`, `lookup(table, key)`, `manifest(key)`, `get_stats`, `set_draw_observer` (for tests).
  - `-[NSBundle localizedStringForKey:value:table:]` asks the pack first.
  - `CFURLCreateDataAndPropertiesFromResource` writes the pack's `Tables.strings` into `<Language>/data/<table>.dat` when its header has the game's layout; a key for a row past the table, a column that is not text or a bad key is logged and ignored.
  - The Korean encodings' UTF-8 rule and the two wrap hooks are on while the pack is open.
  - A single-line text wider than its bitmap is logged once (`textpack: overflow: ...`).
- **Fira Sans Regular** is pinned (`maplethief_use_fallback_font`, `THIRD_PARTY.md`) and embedded; the kit carries its licence.
- **Setting** `text_pack = DIR` (`--text-pack DIR`; Other, not in the presets, from the next start). The frontend opens it before the boot, beside `audio_pack`, and logs the counts at the end. The first start's preset picker runs before it and stays English.
- **The port's text table** (`platform/src/port_text.{def,h,cpp}`): `tr`, `tr(id, {args})`, `tr_key`, `ascii_fold`, `reload_port_text`. Without a pack every text is its English literal.
- **`python3 -m mtport.textpack export DIR [--bundle APP] [--language en|ko]`** writes every text of the player's copy and the port's, each table value with its room; **`check DIR`** applies the runtime's rules (refused files, ignored keys, cut values, places). The export holds the game's text: it is the player's and stays out of the repository.

## Evidence

- **CI (synthetic)**:
  - `tests/cpp/textpack_test.cpp` (`objc_test`): the parser (escapes, comments, BOM, duplicates) and its refusals, each with its reason; a pack that overrides one table row and one script string, a missing key falling back to the game's; invalid UTF-8 refused with the byte and offset; the manifest; the encodings with and without a pack; the hooks' steps; the overflow log; the draw observer.
  - `tests/cpp/cf_test.cpp`: the table file the game reads carries the pack's text. `tests/cpp/foundation_test.cpp`: `NSBundle` answers from the pack first, the bundle's text for a key it lacks. `tests/cpp/cg_test.cpp`: the fallback's glyphs and widths.
  - `tests/cpp/port_text_test.cpp` (`platform_test`): every text is its English literal without a pack; a pack's `Port.strings` folded to ASCII, missing keys and wrong places keeping the English, derived keys, closing.
  - Every overlay's drawing test (settings menu, controls card, presets, remap, disconnect, key pad editor, price line, Saves tab) passes unchanged: the English is byte for byte what it was.
  - `tests/test_textpack.py`: the Python parser against the same rules and reasons, `check`, `export` from a synthetic bundle.
- **Local, real assets**:
  - `play.text_pack`: a pack made at run time in the test output directory from the player's copy; through the intro and the tutorial every box line is whole UTF-8 and the boxes show "Hénésys"; the Red Potions' item page draws "Red Pötion"; no drawn text has a Hangul syllable.
  - `tests/test_textpack.py::test_export_of_the_real_bundle`: the English export opens whole and uncut (its longest names fill their fields exactly); Korean as UTF-8 has names that would be cut.
  - The goldens and the real-asset suites without a pack: unchanged.

## Consequences

- A translation can be shared as a folder of text files, made from `export`, checked with `check`.
- Names keep the game's field widths: a German or French name longer than the original is cut, at a whole letter, logged once. Korean re-encoded as UTF-8 is longer than its CP949 (3 bytes a syllable against 2).
- The port's menus show translations without accents ("Einstellungen für" shows "Einstellungen fur"); scripts beyond Latin show as "?" there.
- The game's own text draws exactly as before, with or without the new font: the fallback is only used for characters NanumGothic lacks.
- How we'd know this is wrong: a pack's line that ends inside a character (the wrap hooks), Hangul where a pack has accents (the encoding rule), Korean text that changes with a pack open (the rule's assumption that no game text is valid UTF-8), or a crash on a pack from the wild (the parser's limits).
