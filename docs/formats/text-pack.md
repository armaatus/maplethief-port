# Text packs (`.strings` folders)

A text pack replaces the game's text and the port's own with a translation in UTF-8 ([#189](https://github.com/armaatus/maplethief-port/issues/189), [ADR 0108](../adr/0108-text-packs.md)). It is our format, not the game's: the game's own text is in binary-plist `.strings` and fixed-width tables ([script-out.md](script-out.md#dialogue-keys), [data-tables.md](data-tables.md)); a pack uses Apple's text syntax for the same keys. Where each kind of text goes in the game: [re/text.md](../re/text.md).

- **Loaded by**: `mt_textpack_open` (`runtime/include/maplethief/textpack.h`, `runtime/src/textpack/`), from `text_pack = DIR` or `--text-pack DIR`, before the boot.
- **Parsers**: `runtime/src/textpack/strings_file.cpp` (C++) and `tools/mtport/textpack.py` (Python, the same rules and reasons; `export`, `check`).
- **Validation**: synthetic files in CI (`tests/cpp/textpack_test.cpp`, `tests/test_textpack.py`); locally, the export of the player's copy opens whole (`test_export_of_the_real_bundle`) and `play.text_pack` plays with a pack made at run time.

## The folder

| File (any case) | Required | Keys | Replaces |
|---|---|---|---|
| `textpack.strings` | yes | `format`, `name`, `language`; `version`, `game_version`, `author` optional | nothing: the manifest |
| `Scripts.strings` | no | the game's `Scripts` keys, `<Prefix>.<function>.<n>` | dialogue and quest text |
| `ScriptExtra.strings` | no | the game's `ScriptExtra` keys, `<Class>.<member>.<n>` | the C++ code's sentences |
| `Tables.strings` | no | `<table>.<row>.<column>` | names and descriptions in the data tables |
| `Port.strings` | no | the port's text ids, `setting.<key>.label` / `.help` / `.help_pad`, `action.<key>.label` / `.help` / `.help_pad` / `.button` / `.confirm` | the port's menus |

Other files are ignored (`check` names a `.strings` file it doesn't know). Without the manifest, or with a manifest that is refused, there is no pack. The manifest: `format` must be `"1"`; `name` must not be empty; `language` is a BCP 47 tag (`de`, `pt-BR`: 2 or 3 letters, then `-` and up to 8 letters or digits per part).

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

`Port.strings`' values are shown in the port's ASCII fonts: accented Latin letters lose their accents, typographic quotes and dashes become `'`, `"` and `-`, anything else `?`. A value must keep the English text's places (`{0}`, `{1}`, ...), in any order; one that doesn't is logged and the English stays.

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

`textpack.strings`:

```
"format" = "1";
"name" = "MapleThief auf Deutsch";
"language" = "de";
"version" = "0.1";
```

`Tables.strings` (the first line as `export` writes it):

```
/* at most 64 bytes */
"item_etc.1.name" = "Pötion";
```
