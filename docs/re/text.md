# Text: where the game's words come from, how they are encoded, wrapped and drawn

Research for text packs ([#189](https://github.com/armaatus/maplethief-port/issues/189), [ADR 0108](../adr/0108-text-packs.md)): every path by which text reaches the screen, what encoding each one assumes, and what changes when that text is UTF-8 outside ASCII ("Pötion", "Hénésys"). Addresses are armv7 VM addresses; function pointers carry the Thumb bit.

## The paths

| Text | Source | Read by | Encoding the game assumes |
|---|---|---|---|
| Dialogue, quest text, scene speech | `<lang>.lproj/Scripts.strings` (2,219 keys, `<Prefix>.<function>.<n>`, used by the scripts) | `LocalizedCString` `0x6DD1` | UTF-8 C string out |
| UI sentences of the C++ code | `<lang>.lproj/ScriptExtra.strings` (527 keys, `<Class>.<member>.<n>`) | `LocalizedCString` `0x6DD1` | UTF-8 C string out |
| Names and descriptions of items, mobs, NPCs, skills | `<Language>/data/*.dat` (fixed records, [formats/data-tables.md](../formats/data-tables.md)) | the data manager's string accessor `0x37999` | the file's bytes as they are (English ASCII, Korean CP949) |
| The quest log's requirements | the same tables, copied | `CChatBox::drawRequire` `0x3243D` | MacKorean (`kCFStringEncodingMacKorean`, `0x80000003`) |
| Numbers, meso, counts | `sprintf` in the C++ code | (inline) | ASCII |
| The port's own menus | `platform/src/port_text.def` | the frontend's overlays | ASCII (the port's 5x7 font, SDL's debug font) |

### LocalizedCString `0x6DD1`

Takes a C string key and a table name. It makes an `NSString` from the key with `stringWithCString:encoding:` 4 (UTF-8), falling back to EUC-KR when that fails, asks `[[NSBundle mainBundle] localizedStringForKey:value:table:]`, and returns the answer's `UTF8String`. Every script line and every `ScriptExtra` sentence therefore reaches the game as a **UTF-8 C string**, whatever the `.strings` file's own encoding. The tables it names are `"Scripts"` and `"ScriptExtra"`. A text pack's values for these two tables need no conversion: the runtime's `NSBundle` answers from the pack first (`runtime/src/foundation/ns_bundle.cpp`).

### The data tables

The data manager reads each `<Language>/data/<name>.dat` whole through `CFURLCreateDataAndPropertiesFromResource` (the path built by `0x6A15` from the language folder, `English`, `Korean` or `Japanese`), keeps the raw bytes, and reads columns by offset. The string accessor `0x37999` copies `width` bytes of a field into a new buffer of `width + 1` and ends it with a NUL: **a field may be full**, with no NUL of its own. The game's English data does that: three names fill their 16-byte fields exactly (mob rows 31 and 41, skill row 1). The text columns:

| Table | Columns | Stride | Text columns (offset, width) |
|---|---|---|---|
| `mob` | 17 | 50 | name (1, 16) |
| `npc` | 2 | 33 | name (1, 32) |
| `item_equip` | 24 | 73 | name (1, 32) |
| `item_etc` | 14 | 218 | name (1, 64), description (90, 128) |
| `skill` | 19 | 297 | name (1, 16), description (41, 128), detail (169, 128) |

`hero`, `market` and `buildup` carry no text. The bytes go to the drawing code as they are; the conversion happens there (below). A UTF-8 value takes more bytes than CP949 for Hangul (3 against 2), so a Korean name re-encoded as UTF-8 may not fit its field; an English name gains one byte per accented letter.

Of the 526 non-ASCII strings in the Korean tables, **none is valid UTF-8** (`tools/mtport/datatables.py` over `Korean/data`, 2026-11-05): every Hangul syllable in CP949 starts with a lead byte 0xB0–0xC8 followed by 0xA1–0xFE, and no such pair is a UTF-8 sequence. The English tables are pure ASCII.

### Drawing: TextCache_DrawText `0x9031` and the measure `0x72F1`

`TextCache_DrawText` makes the `NSString` it draws by trying, in order, `stringWithCString:encoding:` with EUC-KR (`0x906A`, `0x9082`), with UTF-8 (4), then `stringWithUTF8String:`; it draws the first that converts into a 512 × 32 bitmap ([ANALYSIS.md](../ANALYSIS.md#graphics)). The measure `0x72F1` takes a mode in r3: mode 1 tries UTF-8 first, then encoding 4, then EUC-KR; mode 2 EUC-KR first; any other mode `UTF8String` alone. `0x7505` calls it with mode 1.

**The trap**: EUC-KR first. A two-byte UTF-8 sequence for Latin-1 is `C3 xx` or `C2 xx`, and `C3 B6` ("ö") is also a valid EUC-KR syllable (U+CCA0). EUC-KR succeeds on most accented Latin text and draws Hangul. With a text pack open the runtime's Korean encodings (EUC-KR, MacKorean and CP949: `runtime/src/cf/encoding.cpp`) first read a byte string that is valid UTF-8 as UTF-8, and write UTF-8. That changes nothing for the game's own text: its Korean strings are never valid UTF-8 (above) and its English ones are ASCII, which reads the same both ways. Without a pack the encodings are as before.

`CChatBox::drawRequire` `0x3243D` builds its strings with MacKorean and copies them with `strcpy`; the same switch covers it. `0x59D08` also names encoding `0x940` (EUC-KR); its role is not known, and the switch covers it too.

### Wrapping: Text_Wrap `0x22EC5`

Dialogue pages and speech boxes are cut into lines by `Text_Wrap` `0x22EC5` (named in `tools/mtport/scenes.py`), which walks the text one character at a time through two helpers:

- `Text_CharCount` `0x22989`: the length in characters, from `strlen`, counting **3 bytes for any byte ≥ 0x80** and 1 otherwise. That is UTF-8's length for Hangul (U+AC00–U+D7A3 are 3 bytes), so it is right for the Korean `.strings` text, which `LocalizedCString` hands over as UTF-8.
- `Text_SubChars` `0x22A1D`: the characters [from, to) as a new string. It builds the piece in a static 255-byte buffer at a PC-relative global with the same 3-byte step, then copies it to a `new[]` (`0xF0E30`) buffer of `len + 1`; the caller frees it with `delete[]` (`0xF0E28`).

`0x22B6D` and the wrap's other callers use only these two. For "ö" (2 bytes) the 3-byte step swallows the next byte: a line can end inside a character, the next starts with a stray continuation byte, and both draw as garbage or fail to convert. With a pack open both functions are replaced by hooks (`MT_HOOK_FN_TEXT_CHAR_COUNT`, `MT_HOOK_FN_TEXT_SUB_CHARS`, `runtime/src/textpack/textpack.cpp`) that step by the UTF-8 lead byte's length (2, 3 or 4), which is the same step for every text the game ships. The hooked `Text_SubChars` allocates through the guest heap like the original. No other loop of the game's that steps through text by bytes was found; the drawing and measuring calls above take whole strings.

### The font

The port draws all game text with NanumGothic, which has a glyph for every code point the game ships (`tests/test_hle_cg_real.py`). It lacks most accented Latin and all Greek and Cyrillic: of Latin-1's 96 letters and signs it has 38, of Latin Extended-A 16 of 128, of Greek and Cyrillic none; no "ö", no "é". The runtime adds **Fira Sans Regular** (SIL OFL 1.1, from Google Fonts, pinned by SHA-256 in `cmake/Dependencies.cmake`, `THIRD_PARTY.md`) as a fallback, glyph by glyph: a character NanumGothic lacks is drawn from Fira Sans (all of Latin-1 and Latin Extended-A/B, 121 of Greek's 144, 240 of Cyrillic's 256), each at its own font's scale, with kerning only between glyphs of the same font. Text NanumGothic covers draws exactly as before: the CG goldens are unchanged. A pack cannot bring its own font: stb_truetype is not safe on untrusted font files.

### The port's own text

The port's overlays (settings menu, preset picker, controls card, remap screen, key pad editor, disconnect pause, price line, Saves tab) draw with the port's 5x7 font and SDL's debug font, which have ASCII only, and their width and wrap code counts bytes. Their text moved into one table (`platform/src/port_text.def`, `tr()`), which a pack's `Port.strings` translates; the translation is folded to ASCII (`ascii_fold`: "ö" to "o", "ß" to "ss", typographic quotes and dashes to ASCII, anything else to "?"). Logs stay English.

## Evidence

- `tests/cpp/textpack_test.cpp` (CI, synthetic): the hooks' steps on "Pötion" and Hangul, the encodings with and without a pack, a table field cut at a whole character.
- `tests/cpp/cg_test.cpp` (CI): the fallback's glyphs and widths; text without such characters unchanged.
- `play.text_pack` (local, real assets): a pack made at run time from the player's copy; through the intro and the tutorial 36 boxes, 143 lines, all whole UTF-8, 31 showing "Hénésys"; the item page of the tutorial's Red Potions draws "Red Pötion"; no drawn text has a Hangul syllable. Its shot stays in the test output directory.
- `tests/test_textpack.py::test_export_of_the_real_bundle` (local): every English text exports to a pack that opens whole and uncut; Korean re-encoded as UTF-8 has names that would be cut.
