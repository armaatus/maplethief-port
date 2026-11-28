# Translating the port's menus

The port ships its own menus (the settings menu, the first start's screens, the controls card, the popups) in English, Dutch and Korean (a draft). Each language is a folder of two text files in [`platform/lang/`](../../../platform/lang/), which the build embeds in the game ([ADR 0222](../../adr/0222-the-port-ships-its-menus-in-dutch-and-korean.md)). This page is how to fix a text in one of them, or add a language, and get it into the port.

Only the port's own text is translated here: `Port.strings`, the menus the port added around the game. The game's own text (dialogue, quests, item names) is Nexon's, and the project neither hosts nor links translations of it ([ADR 0149](../../adr/0149-pack-manifest-several-packs-and-what-the-project-hosts.md#what-the-project-hosts-links-and-leaves-alone)). A translation that only you will use can also be a [text pack](../../formats/text-pack.md) in your own folder, with no pull request at all.

You need the [setup from CONTRIBUTING](../../../CONTRIBUTING.md#setting-up) up to the first build (`cmake --workflow --preset mac-debug`, or `linux-debug` or `win-debug`). The game's files are not needed: everything below runs on the build CI makes.

## The files

`platform/lang/<tag>/` holds:

- **`textpack.strings`**, the manifest: `format` (`"1"`), `name` (the language's name in itself: `Nederlands`, `Deutsch`), `language` (the folder's tag), `version`, and `description`, which starts with `Draft` until the language is reviewed ([below](#who-reviews)).
- **`Port.strings`**: every text of the port, one line each, `"key" = "translation";`. The key says where the text is: `setting.<key>.label` and `.help` are a setting's row and help line in the settings menu, `action.<key>.*` a menu action, and the rest are named in [`port_text.def`](../../../platform/src/port_text.def), which has the English and a comment on where it shows.

The syntax is Apple's `.strings` in UTF-8 ([the format](../../formats/text-pack.md#the-syntax)): `\"` for a quote, `\n` for a line break, `/* comments */`. Rules for a translation:

- **Keep the places.** `{0}`, `{1}`, ... are filled in by the port (a name, a number, a key); keep each one, in any order. A translation without them is dropped and the English shows.
- **Keep it about as long as the English.** The menus have fixed room; a longer text is cut with `...`. The Dutch and Korean show what fits.
- **Leave alone** what the game or the file reads back: setting values (`on`, `off`, `auto`, `integer`), key names (`Tab`, `Up`), flags (`--fullscreen`) and file names (`settings.ini`). To keep a text in English on purpose, give the English as its translation.
- **What draws**: Latin with accents, Greek, Cyrillic and Hangul. Right-to-left scripts, scripts that need shaping (Devanagari, Thai), Chinese and Japanese characters, and emoji show as `?` ([ADR 0108, addendum #372](../../adr/0108-text-packs.md#addendum-372)). Write each accented letter as one character (Unicode NFC, what keyboards type).

## Fixing a text

1. Branch from `dev`: `git switch -c 123-dutch-menu-fixes origin/dev`.
2. Edit the line in `platform/lang/<tag>/Port.strings`.
3. [Check](#checking) and [open a pull request](#opening-the-pull-request).

## Adding a language

1. **Pick the tag**: the language's ISO 639 code in lower case, two letters where there are two (`de`, `fr`, `pt`), without a region. The port picks the menu language by comparing this tag with the system's preferred languages' language part, so `pt` serves Portugal and Brazil alike; a region in the tag would never match the system.
2. **Copy a folder**: `cp -R platform/lang/nl platform/lang/de`. In `textpack.strings`, set `name`, `language` and `version` (`"1"`), and start `description` with `Draft: `. In `Port.strings`, keep only a comment at the top saying what the file is, and delete every line after it.
3. **Name it in the setting.** `menu_language`'s values are `auto`, `en` and the folders in alphabetical order: add the tag to `kMenuLanguageNames` and its value to `enum class MenuLanguage` at the same place (`platform/src/settings.h`), and to the lists of values in the comments that name them: the `settings.ini` example at the top of `settings.h`, the `menu_language` field in `struct Settings`, and the registry row's `.comment` in `platform/src/settings.cpp`.
4. **Get the list of texts**: build and run [the completeness test](#checking). It fails, listing every text of the port as a line of `Port.strings` with the English as its value. Paste those lines into your `Port.strings`.
5. **Translate** the values, line by line. Start with the first start's screens (`import_*`, `picker_*`, `card_*`), which every player sees, then the settings menu. Run the test again whenever you like: until every value is translated it passes with English left in, so search your file for English, or diff it against another language's.
6. [Check](#checking) and [open a pull request](#opening-the-pull-request).

Translating takes a few evenings: there are about 700 texts, most of them short.

## Checking

**The completeness test** runs in CI for every language, and lists what is missing:

```sh
cmake --build --preset mac-debug
ctest --preset mac-debug -R 'menu languages' --output-on-failure
```

A language that lacks texts fails with each missing one in `.strings` syntax, with its English as the value, ready to paste into `Port.strings` and translate:

```
platform/lang/de/Port.strings lacks:
"hint_page" = "page";
"menu_title" = "SETTINGS";
```

The same test fails for a translation whose places (`{0}`, ...) differ from the English's, for a key the port has no text for (a typo, or a text the port no longer has), and for a `description` that doesn't start with `Draft` in a language not yet reviewed. When the port gains a text, this test fails for every language until each has it: the change that adds a text adds its translations, or the English as a placeholder.

**The drawing test** draws every menu screen in each language at every UI scale and fails on a `?` (a character the fonts lack) or a text outside its panel:

```sh
ctest --preset mac-debug -R 'menu glyphs' --output-on-failure
```

**The file check** reads the folder by a text pack's rules (syntax, manifest, places) without a build: `MapleThief --check-pack platform/lang/de --pack-kind text`, or with [uv](https://docs.astral.sh/uv/) from `tools/`, `uv run --project .. python -m mtport.textpack check ../platform/lang/de`. `tests/test_textpack.py` runs it on every folder.

**The setting** takes the new tag at once: `MapleThief --menu-language xx` is refused with the list of values, which now names your language.

**See it** in a playable build ([with the game](../../../CONTRIBUTING.md#setting-up), `just build`): run it with `--menu-language de` (or set the Menu language row in the settings menu's Other tab, which applies at once) and walk the menus. With `auto`, a system whose first preferred language is German shows it from the first screen. A build without the game's files cannot run the game; the drawing test above is then what shows the menus.

Before pushing, run [the quick check](../testing.md#before-you-push).

## Opening the pull request

Push your branch and open a pull request into `dev`. Say which language, what you translated or fixed, and that the two tests above pass. The template's checklist applies as for any change; screenshots of the menus help the reviewer and are fine (they show the port's menus, not the game's art; keep the game's screens out of them).

## Who reviews

- **The maintainer** reviews every translation for the rules on this page (the files, the tests, nothing of the game's text) and merges it when CI is green.
- **A second speaker** reviews the language itself. Until one has, the language ships as a draft: its `description` starts with `Draft`, and its `name` says so in the language ("한국어 (초안)"). The test above holds the marking: a language leaves the draft list only when it is added to the `reviewed` set in `tests/cpp/port_languages_test.cpp`, in the pull request where a speaker approves it.
- **Fixes** to a reviewed language wait for a speaker's approval in the pull request; fixes to a draft are merged on the maintainer's review.

To find a reviewer, or to ask whether a language is already being translated, ask in [Discussions](https://github.com/armaatus/maplethief-port/discussions).

## Worked example

Followed for [#375](https://github.com/armaatus/maplethief-port/issues/375) on a scratch branch, with a pseudo-language: tag `qps`, the English with its vowels accented (`"hint_choose" = "çhóósé";`), places and escapes kept. Copying `nl/`, editing the manifest (`Draft: ...`), emptying `Port.strings` to its comment and adding `kQps` / `"qps"` and the three comments in `settings.h` and `settings.cpp` built cleanly. `menu languages: every port text ...` then failed for `qps` alone, listing all 680 texts with their English; those lines, put through a script that accents vowels, became the new `Port.strings`. After that the four `menu languages` tests and the five `menu glyphs` tests passed, the glyph sweep included (every overlay at UI scales 1 to 4 with no `?` and nothing outside its panel). `MapleThief --check-pack platform/lang/qps --pack-kind text` said `text pack: opens, 680 usable`, and `python -m mtport.textpack check` said `680 entries, 0 refused`. `--menu-language xx` was refused with `'xx' is not auto, en, ko, nl or qps`. Two deliberate breaks failed the completeness test as described above: a `description` without `Draft` (the draft check), and a value without its `{1}` (listed under places, and its key listed as missing, since the port drops it). The build had no game, so the menus were not seen in a running game. The walk also found that the shipped languages lacked the input display's eight texts (#404 had landed just before them); they were added. The example was not merged.
