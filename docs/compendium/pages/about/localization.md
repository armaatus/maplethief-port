# Localization

The game ships in English and Korean, and it carries an unfinished Japanese slot. Which language a player sees depends on the device's first preferred language: Korean for `ko`, the Japanese folder for `ja`, and English for everything else ([formats/data-tables.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/data-tables.md)).

## Where the text lives

- **Data tables.** `English/data`, `Korean/data`, and `Japanese/data` each hold the same eight files: the tables of monsters, NPCs, equipment, other items, skills, shop stock, and levels, and the one-byte `buildup.dat`. The English and Korean tables have the same rows and the same numbers, and differ only in their text. The Korean text is EUC-KR.
- **Scripts.** The scripts carry no text: they pass keys that the game looks up in `en.lproj` or `ko.lproj` (`Scripts.strings` for dialogue, `ScriptExtra.strings` for the rest). Both languages resolve every key but one: `ko.lproj` lacks `Chapter3_Quest_Zenumist_Chief.dialog.1`, so a Korean player would see the key itself there ([formats/script-out.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/script-out.md#dialogue-keys)).
- **The shell.** The main menu, character select, and alerts are drawn from images and strings in the binary; the help pages and some buttons have Korean images of their own.

## What differs

- **Names.** Every monster, item, NPC, map, and skill has a Korean name beside its English one, and every page of this compendium shows both. Some Korean NPC names are roles rather than names: the weapon and potion merchants of Kerning City are called by their trade ([Glossary](about/glossary.html)).
- **Images.** Korean players get their own pictures for the help pages (`MainMenuHelp_kor.png` plus a second page, `MainMenuHelpGradeInfo.png`), the NPC dialogue's Accept and Refuse buttons, the death popup's buttons, and the effect sprites (`sprite/effect_kor/`, beside `sprite/effect/`).
- **Text length.** English text is longer than Korean text, because one Hangul character holds a whole syllable. The figures below are counted from the knowledge base at build time.
- **Korean-only text.** A few unused scripts write Korean sentences straight into the bytecode instead of using keys, so they would show Korean in every language ([Unused content](about/unused.html#unused-scripts)).

## Japanese leftovers

`Japanese/data` is a byte-for-byte copy of `English/data`, and there is no `ja.lproj`. A device set to Japanese reads the English tables; which strings file it would use for the scripts was not traced. Japanese was probably planned, but this version has no Japanese text ([ANALYSIS.md](https://github.com/armaatus/maplethief-port/blob/main/docs/ANALYSIS.md)).
