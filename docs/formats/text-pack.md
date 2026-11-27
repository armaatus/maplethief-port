# Text packs (`.strings` folders)

A text pack replaces the game's text and the port's own with a translation in UTF-8 ([#189](https://github.com/armaatus/maplethief-port/issues/189), [ADR 0108](../adr/0108-text-packs.md)). It is our format, not the game's: the game's own text is in binary-plist `.strings` and fixed-width tables ([script-out.md](script-out.md#dialogue-keys), [data-tables.md](data-tables.md)); a pack uses Apple's text syntax for the same keys. Where each kind of text goes in the game: [re/text.md](../re/text.md).

- **Loaded by**: `mt_textpack_open` (`runtime/include/maplethief/textpack.h`, `runtime/src/textpack/`), from `text_pack = DIR` or `--text-pack DIR`, before the boot.
- **Parsers**: `runtime/src/textpack/strings_file.cpp` (C++) and `tools/mtport/textpack.py` (Python, the same rules and reasons; `export`, `check`).
- **Tools in the game** ([#371](https://github.com/armaatus/maplethief-port/issues/371), [ADR 0205](../adr/0205-pack-tools-in-the-game.md)): `mt_textpack_export` and `mt_textpack_check` (`maplethief/packcheck.h`), as `MapleThief --export-text DIR` and `--check-pack DIR` and the settings menu's Export text and Check packs; the frontend adds `Port.strings` and checks its keys against the port's own text.
- **Validation**: synthetic files in CI (`tests/cpp/textpack_test.cpp`, `tests/cpp/packcheck_test.cpp`, `tests/test_textpack.py`); locally, the export of the player's copy opens whole (`test_export_of_the_real_bundle`) and `play.text_pack` plays with a pack made at run time.

## The folder

| File (any case) | Required | Keys | Replaces |
|---|---|---|---|
| `pack.strings` | this or `textpack.strings` | ADR 0149's manifest for every pack kind: `format`, `kind` (`text`), `id`, `name`, `language`; `version`, `author`, `description`, `game_version`, `port_min`, `content`, `license` optional (`runtime/include/maplethief/pack_manifest.h`); when present it is the manifest and `textpack.strings` is ignored, logged | nothing: the manifest |
| `textpack.strings` | without `pack.strings` | `format`, `name`, `language`; `version`, `game_version`, `author` optional (packs made before ADR 0149) | nothing: the manifest |
| `Scripts.strings` | no | the game's `Scripts` keys, `<Prefix>.<function>.<n>` | dialogue and quest text |
| `ScriptExtra.strings` | no | the game's `ScriptExtra` keys, `<Class>.<member>.<n>` | the C++ code's sentences |
| `Tables.strings` | no | `<table>.<row>.<column>` | names and descriptions in the data tables |
| `Port.strings` | no | the port's text ids, `setting.<key>.label` / `.help` / `.help_pad`, `action.<key>.label` / `.help` / `.help_pad` / `.button` / `.confirm` | the port's menus |

Other files are ignored (`check` names a `.strings` file it doesn't know). Without the manifest, or with a manifest that is refused, there is no pack. The manifest: `format` must be `"1"`; `name` must not be empty; `language` is a BCP 47 tag (`de`, `pt-BR`: 2 or 3 letters, then `-` and up to 8 letters or digits per part). `pack.strings` also needs `kind = "text"` and an `id`, and is held to ADR 0149's lengths, `game_version` and `port_min` (`runtime/src/textpack/pack_manifest.cpp`). Both exports write `pack.strings`.

`Tables.strings`' row is the record's index in the file (0 first, not its id column). The columns and their room in bytes of UTF-8 (a full field needs no NUL: the game's accessor `0x37999` ends its copy):

| Key | Room |
|---|---|
| `mob.<row>.name` | 16 |
| `npc.<row>.name` (`$` is a line break) | 32 |
| `item_equip.<row>.name` | 32 |
| `item_etc.<row>.name` | 64 |
| `item_etc.<row>.description` (`$` is a line break) | 128 |
| `skill.<row>.name` | 16 |
| `skill.<row>.description` | 128 |
| `skill.<row>.detail` (`@dmg`, `@time`, `@etc`, `@mp` filled in by the game) | 128 |

A longer value is cut at a whole character and logged once. A key that names another column, a row past the table's end or no `<table>.<row>.<column>` at all is logged and ignored. The pack's values go into the table of the language the game runs in (`English`, `Korean` or `Japanese`).

`Port.strings`' values are drawn as they are ([ADR 0206](../adr/0206-the-port-menus-draw-accents-and-hangul.md)): printable ASCII in the overlays' 5x7 font, any other character from the bundled fonts (NanumGothic, else Fira Sans), so Latin with accents, Hangul, Greek and Cyrillic show; a character neither font has shows as `?`. Text is laid out in cells, one per character and two per Hangul syllable or other East Asian wide character; a value longer than its room is cut at a whole character with `...`. The cash shop's price line (SDL's debug font) is the exception: it folds its text to ASCII (accents dropped, typographic quotes and dashes to `'`, `"` and `-`, anything else `?`). A value must keep the English text's places (`{0}`, `{1}`, ...), in any order; one that doesn't is logged and the English stays.

The port ships its own menus in Dutch and Korean as text packs of this format, `platform/lang/<tag>/` (`textpack.strings` and `Port.strings` only), embedded in the frontend ([ADR 0222](../adr/0222-the-port-ships-its-menus-in-dutch-and-korean.md)). A pack's `Port.strings` wins over them key by key; a key the pack lacks shows in the menu language (`menu_language`), else in English. To start a new menu language, copy one of those folders, change `language` and `name`, and translate the values.

## The syntax

The bytes must be valid UTF-8 (no overlong forms, no encoded surrogates, nothing above U+10FFFF), with an optional BOM (`EF BB BF`). Then any number of entries:

```
/* a comment, over lines if need be */
"key" = "value";   // a comment to the end of the line
```

- Space between the parts is spaces, tabs, carriage returns and line feeds; comments count as space.
- Keys and values are quoted with `"`. A string may span lines (the line feed is part of it).
- Escapes: `\"`, `\\`, `\'`, `\n`, `\t`, `\r`, `\Uxxxx` and `\uxxxx` (exactly four hex digits, a UTF-16 unit; a high surrogate must be followed by a `\U` low one, and the pair is one character).
- A key given twice keeps its last value (counted).

### Limits and refusals

| Rule | Reason (`line N: ...`) |
|---|---|
| at most 4 MiB a file | `larger than 4 MiB` (no line) |
| valid UTF-8 | `invalid UTF-8 (byte 0xF6 at offset 20)` |
| an entry starts with a quote | `expected a quoted key` |
| `=` after the key | `expected '=' after the key` |
| a quoted value after `=` | `expected a quoted value after '='` |
| `;` after the value | `expected ';' after the value` |
| strings end | `a string without its closing quote` (the line it starts on) |
| block comments end | `a comment without its end (*/)` (the line it starts on) |
| known escapes only | `an unknown escape \q` |
| `\U` with four hex digits | `\U needs four hex digits` |
| surrogates in pairs | `a lone surrogate (\UD800)` |
| no NUL, raw or escaped | `a NUL character` |
| a key of 1 to 256 bytes | `an empty key`, `a key longer than 256 bytes` (the value's line) |
| a value of at most 4,096 bytes | `a value longer than 4096 bytes` (the value's line) |
| at most 65,536 entries | `more than 65536 entries` |

A file that breaks a rule is refused whole; the runtime logs `textpack: DIR/FILE: line N: reason: refused` and the game's text stays for that file's keys. The other files still load.

## Example

`pack.strings`:

```
"format" = "1";
"kind" = "text";
"id" = "maplethief-de";
"name" = "MapleThief auf Deutsch";
"language" = "de";
"version" = "0.1";
```

`Tables.strings` (the first line as `export` writes it):

```
/* at most 64 bytes */
"item_etc.1.name" = "Pötion";
```
