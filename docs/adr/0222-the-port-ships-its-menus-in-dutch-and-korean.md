# 222. The port ships its menus in Dutch and Korean

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#373](https://github.com/armaatus/maplethief-port/issues/373) (epic [#294](https://github.com/armaatus/maplethief-port/issues/294))
- **Builds on:** [ADR 0077](0077-desktop-conveniences.md) (the `language` setting), [ADR 0108](0108-text-packs.md) (text packs, `Port.strings`, the port's text table), [ADR 0206](0206-the-port-menus-draw-accents-and-hangul.md) (the menus draw accents and Hangul)

## Context

Text packs can translate the port's own menus (ADR 0108), and since ADR 0206 the menus draw accented Latin and Hangul, but the port shipped no translation: every player saw English menus unless they made or found a pack. The game itself has English and Korean, so a Korean player got Korean game text around English port menus. The first start's screens (the start question, the import screen, the preset picker and its controls card) ran before any pack was opened and were always English.

#373 asks for the port's strings in Dutch and Korean as packs of our own text only, the first start's screens following the system language before a settings file exists, a completeness test in CI, the Dutch reviewed and the Korean marked as a draft until a speaker reviews it.

## Options considered

**Where the translations live.**

1. **Embedded in the frontend, from `platform/lang/<tag>/` in the repository** (a text pack folder of our own text: `textpack.strings` and `Port.strings`). Chosen: every platform (desktop, iOS, Switch, Android's APK without assets) has them with no kit files to install, find or lose; the files are the text-pack format, so `check` reads them and a translator copies a folder to start a new language.
2. Installed as pack folders in each kit, opened as text packs. Six packaging paths (macOS bundle, Windows zip, Linux AppImage/Flatpak/tarball, iOS bundle, Switch romfs, Android APK) to change and test, and opening a pack turns on the game's UTF-8 encoding rule and wrap hooks (ADR 0108) for a pack that changes no game text.
3. C++ tables per language (`port_text_nl.def`). Translators would edit C++; the registry's and actions' derived keys have no id to hang a row on.

**How a shipped language relates to a text pack.** A layer under the pack: a text is the open pack's `Port.strings` value, else the menu language's, else the English. A pack with a `Port.strings` still wins key by key, so a community translation of the menus works as before, and a pack without one (a translation of the game's text only) leaves the menus in the player's menu language.

**Which language the menus use.**

1. **A setting, `menu_language = auto | en | ko | nl`, default `auto`: the first of the system's preferred languages the port ships, else English.** Chosen: the first start is in the player's language with no question asked; a player who wants English menus on a Dutch system, or Korean menus with an English game, sets it. It applies at once from the settings menu.
2. Follow the game's `language` setting (en | ko). Gives Korean menus to a Korean-game player only, and nothing for Dutch.
3. Follow the system only, no setting. No way out for a player who wants another language.

**The system's language.** SDL's `SDL_GetPreferredLocales`, which reads the system's ordered preferences (macOS's and iOS's preferred languages, Windows's user languages, Android's locales, Linux's `LANGUAGE`/`LC_*`/`LANG`) and which the hint `SDL_PREFERRED_LOCALES` overrides (also as an environment variable). Matched by the language subtag: `nl_BE` is Dutch. The first preference the port has wins, so a system ordered "English, Dutch" stays English.

**Lifetimes.** `tr()` returns pointers the overlays may hold across a frame. A menu language's texts are parsed once and never freed, so changing the language from the settings menu never leaves a dangling text; the pack's cache keeps ADR 0108's rule (valid until `reload_port_text`).

## Decision

- **`platform/lang/<tag>/`** holds `textpack.strings` (format 1, `name` as the language calls itself, `language` = the folder's tag, `version`, `description`) and `Port.strings` with every key of the port's text. `platform/CMakeLists.txt` embeds both as byte arrays (MSVC caps string literals at 64 KiB) in a generated `port_languages.cpp`; adding a folder adds a language, and `kMenuLanguageNames` (settings.h) names it.
- **Runtime**: `mt_textpack_parse(bytes, n, fn, user, why, why_size)` (`maplethief/textpack.h`), the pack parser on bytes, so the embedded files are read by a pack's rules (strict UTF-8, limits, syntax).
- **Frontend** (`port_text.h`): `shipped_port_languages()` (each parsed once, checked against the table: keys the port lacks and keys whose places `{0}, {1}, ...` differ from the English's are dropped and listed), `port_text_rows()` (every `Port.strings` key with its English; `pack_tools` exports from it), `menu_languages()`, `set_menu_language(tag)`, `menu_language()`, `system_menu_language()`, `apply_menu_language(setting)`, `menu_language_name(tag)`. `main.cpp` applies the setting right after settings.ini, the command line and `MAPLETHIEF_SETTINGS` are read, before the start question and the import screen. `live_settings.cpp` applies a change at once. The settings menu shows each value as the language names itself ("Nederlands", "auto (English)").
- **Setting** `menu_language` (`--menu-language`; Other tab; not in the presets; applies at once).
- **Review**: the Dutch is reviewed (this ADR's delegated call; its manifest says so). The Korean's manifest `description` starts with "Draft" and its `name` is "한국어 (초안)" ("draft"), until a Korean speaker reviews it; a test holds the marking so that removing it is a deliberate change.
- **Fixed in English**: keyboard key names (SDL's), setting values (`on`, `integer`, `auto`: they are what settings.ini takes), flags and file names, and the job names in Dutch (the game's own English names; the Korean uses the Korean game's).

## Evidence

- `tests/cpp/port_languages_test.cpp` (`platform_test`, CI on every OS):
  - `menu languages: every port text has a translation in each shipped language, with the English's places (#373)`: every key of `port_text_rows()` in each language, non-empty, same places, no unknown key, the files not refused; the setting's names are `auto`, `en` and the folders; the Korean marked as a draft and the Dutch not. A missing text is listed in `.strings` syntax with its English, ready to translate.
  - `menu languages: auto follows the system's preferred languages; a set one stays`: `SDL_PREFERRED_LOCALES` = `nl_NL`, `fr_FR,ko_KR,nl`, `en_US,nl_NL`, `nl_BE`, `fr_CA,de`.
  - `menu languages: a text drawn before a change of language stays valid; ...`.
  - `menu languages: with the system in Dutch, the first start is Dutch from the import screen on (#373)`: no settings.ini, `menu_language` auto, the system in Dutch: the import screen, the preset picker and the controls card draw their Dutch texts and none of the English ones (a keyboard key's name aside).
- `tests/cpp/menu_glyphs_test.cpp`: `menu glyphs: each shipped menu language draws every overlay and settings row without '?' or clipping (#373)`: ADR 0206's overlay sweep (every overlay at UI scales 1 to 4: no "?", every text inside its panel and the screen, none overlapping) in each shipped language, plus every row of every settings tab selected so its help line draws.
- `tests/test_textpack.py::test_shipped_menu_languages_pass_the_python_rules`: the shipped files parse by the Python parser's rules too.
- English unchanged: every overlay drawing test that checks by hash passes with its old hash where the menu language is English (the default in-process).

## Consequences

- A Dutch or Korean system gets Dutch or Korean menus from the first screen; an English, French or German one gets English.
- Every new port text must be translated in each shipped language in the same change, or CI fails with the rows to translate. A text can be left in English on purpose by giving the English as its translation.
- The speech voice still follows the game's language (ADR 0174): with Dutch menus and an English game, the menus' own spoken lines (the voice sample, popup answers) are Dutch text in an English voice.
- Logs, settings.ini comments, command-line help and the cash shop's price line (SDL's debug font, ADR 0206) stay English.
- A long translation is cut with "..." where ADR 0206's layout has no room; the sweep above shows none for the shipped two.
- How we'd know this is wrong: players on a Dutch system who want English menus by default (then `auto` should require a choice in the picker), a Korean speaker's review finding the draft's tone or terms wrong, or a text whose translation drifts from a changed English (the test checks places, not meaning: a changed English text should change its key).
