# 205. The pack tools in the game: dump, build, export and check from the binary

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** #371 (epic #294)
- **Builds on:** [ADR 0081](0081-texture-packs-and-nearest-texture-filter.md) (texture packs, the dump, the builder's scalers), [ADR 0106](0106-audio-packs.md) (audio packs), [ADR 0108](0108-text-packs.md) (text packs and the `.strings` syntax), [ADR 0142](0142-platform-capabilities-and-the-switch-frontend.md) (the pack folders), [ADR 0149](0149-pack-manifest-several-packs-and-what-the-project-hosts.md) (`pack.strings`, the pack folders on every platform, the hosting rules)

## Context

Epic #294 asks that a player with only the download can make a pack. Before this ADR:

- **The texture dump was a command-line flag** (`--dump-textures DIR`): on iOS and the Switch there is no command line, and on a desktop a player has to find the program inside its folder (inside the `.app` on a Mac).
- **The build, init and export tools were Python modules of the repository** (`mtport.texpack build`, `mtport.audiopack init`, `mtport.textpack export` and `check`): they need a checkout, Python and `scripts/bootstrap`, which extracts the IPA again into `work/`.
- **Only text packs had a check**, in Python, written to follow the runtime rule for rule. A texture or audio pack's broken file showed only as one log line in a run, after the game had started.
- **The steps for a texture pack** were in ADR 0081 and a settings page that sent the player to the repository.

The game binary already holds everything the tools need: the PNG codec and the texture rules (`runtime/src/texpack/`, `cg/codec.cpp`), the WAV, MP3 and FLAC readers and the audio rules (`runtime/src/audio/`), the `.strings` parser, the binary plist reader and CP949 (`runtime/src/textpack/`, `foundation/plist.cpp`, `cf/encoding.cpp`), and the bundle it found at start.

## Options considered

**Where the texture pack builder runs** (issue task 3: "the build step for texture packs inside the game, or a small tools download that needs no checkout").

1. **In the game binary.** Scale2x, Scale3x and nearest are about 150 lines of C++ on 32-bit pixels; the dump's manifest is read with the runtime's JSON reader (`stubs/json.h`) under the builder's rules. One program to download, sign and keep in step on every platform, including the Switch and iOS where a second program can't run. Chosen.
2. **A tools download** (a separate small executable or a frozen Python). A second artifact per OS and CPU for the release to build, sign and notarize; a second copy of the rules that could drift from the game's; nothing for iOS or the Switch.
3. **Keep Python, ship it as a zip of `tools/mtport`.** Still needs a Python install, and the rules still live twice.

**Where the checks' rules live.**

1. **In each kind's runtime module, sharing the loader's own code.** `mt_texpack_check` indexes with the loader's `index_pack` and reads each file through the same `read_checked` the upload hook uses; `mt_audio_pack_check` uses `wav_pcm`, `Mp3Stream::open_pack`, `check_format` and the `.loop` parser of the loader; `mt_textpack_check` uses `parse_strings`, `parse_field_key` and the manifest check of `mt_textpack_open`. The check writes its lines to a callback and never touches the pack the game has open. Chosen: the tool and the game agree because they are the same code.
2. **A checker in the frontend or in Python that mirrors the rules.** That is what text packs had; every rule then has two places to change.

**Where the menu's tools write.**

1. **In the pack folders the packs screen (#370) lists**, ADR 0149's `texture-packs/`, `audio-packs/` and `text-packs/` in `game.cpp`'s `packs_base()`: `<base>` (the program's folder, the folder around the `.app` on a Mac, `sdmc:/switch/maplethief` on the Switch), or where there is none (iOS) the folder that holds `game/` (the app's Files folder); the dump goes in `texture-dumps/dump` there. A pack the menu made shows up in the packs screen at once. Chosen.
2. A file dialog for each tool. No dialog on the Switch, and a dump chosen anywhere would not be found by Make texture pack.

**The dump from the menu.** The dump hook can be opened while the game runs: the menu runs between the guest's turns, with the game paused, so no upload meets a half-installed hook. Textures already on screen are written the next time the game uploads them (a map change), which the row's help says. `--dump-textures` stays for a dump from the very first frame.

**`pack.strings` in the exports.** ADR 0149 kept `textpack.strings` "until #371 moves the tools to `pack.strings`". Both exports (the game's and `mtport.textpack`) now write `pack.strings` with `kind = text`, an `id` and `content = derived`, which `mt_textpack_open` takes as the manifest when it is there (#370), and `textpack.strings` otherwise.

## Decision

- **Runtime** (`maplethief/packcheck.h`):
  - `mt_texpack_check(dir, bundle, fn, user)`, `mt_audio_pack_check(...)`, `mt_textpack_check(dir, fn, user)` and `mt_pack_check(dir, kind, bundle, fn, user)` (the kind from `pack.strings`, else the files: `mt_pack_kind`). Each line has a level: **rejected** (a file the game rejects, or, with `opens` 0, the whole pack refused), **ignored** (a name or entry it passes over), **note**. The report counts what the game uses (`good`), rejects and ignores.
  - The game's bundle, when given, sizes the `ui/` images (k times the game's own image) and tells effects (the game's `.wav`) from tracks (`.mp3`), with their limits (60 s, 30 minutes). Without it, a `ui/` image is checked as itself and every sound as a track, each with a note.
  - `pack.strings` is checked by #370's manifest reader (`mt_pack_manifest_read`, `maplethief/pack_manifest.h`), the one the packs screen and the start use, for every kind: a pack it refuses, the check refuses with the same reason. The check adds what the reader passes over in silence: keys given twice, keys format 1 lacks, a `content` that is neither `own` nor `derived` (`runtime/src/textpack/packcheck.cpp`).
  - `mt_packcheck_set_port_text`: the frontend hands its own text to the text check, which then passes over `Port.strings` keys the port lacks and translations whose `{0}` places differ, as `port_text.cpp` does at run time.
  - `mt_texpack_build(dump, pack, k, scaler, ui, fn, user)` (`maplethief/texpack.h`): `mtport.texpack build`'s rules (manifest lines with a bad name, a size that disagrees with the name or an unsafe `ui/` path skipped; Scale2x and Scale3x composed for 4x, 6x and 8x, nearest otherwise or on request), PNGs written atomically. `mt_texpack_dump_is_open` for the menu.
  - `mt_textpack_export(bundle, language, dir, fn, user)`: `pack.strings`, `Scripts.strings` and `ScriptExtra.strings` from the bundle's binary plists, `Tables.strings` from the text columns of the language's data tables (CP949 to UTF-8, each with its room), as `mtport.textpack export` lays them out.
- **Frontend** (`platform/src/pack_tools.h`): `Port.strings` from `port_text.def`, the registry and the menu's action rows; the folders; the report's words; the menu's four rows in the **Other** tab: **Dump textures** (Start / Stop), **Make texture pack** (the dump at 2x, Scale2x, into `texture-packs/dump-2x`), **Export text** (into `text-packs/text-<language>`), **Check packs** (a list with each pack's verdict, the rows' folders, ADR 0149's `|` lists included, then the pack folders; choosing a row shows its first problem; the whole reports go to the log). Their status lines are `Port.strings` texts.
- **Command line** (`main.cpp`): `--check-pack DIR [--pack-kind K]`, `--build-texture-pack DUMP PACK [--pack-scale K] [--pack-scaler scale|nearest] [--pack-no-ui]`, `--export-text DIR [--language en|ko]`. Each runs alone (with `--bundle`), prints the report and exits 0, or 3 when the game would reject a file or refuse the pack, or the tool failed.
- **Python** stays for the repository (`mtport.audiopack init` still lists the sounds with their lengths); its text check reads `pack.strings` too. The game's check is the one that decides.
- **Docs:** [docs/play/packs.md](../play/packs.md) walks through each kind from the download, with ADR 0149's legal note; the settings page points there.

### Delegated calls

Made by the agent under Joris's delegation of 2026-11-13; Joris can veto any of them on the PR.

1. The texture pack builder goes into the game binary; no separate tools download.
2. The menu's tools write in the packs screen's folders, at fixed paths (`texture-dumps/dump`, `texture-packs/dump-2x`, `text-packs/text-<language>`), with no dialog; Make texture pack is 2x Scale2x, other factors on the command line.
3. The tools sit in the **Other** tab until the packs screen (#370) gives packs a tab of their own.
4. The exports write `pack.strings` (kind text, an id, content derived), not `textpack.strings`.

## Evidence

- **CI (synthetic):** `tests/cpp/packcheck_test.cpp` (`objc.packcheck: *`): a texture pack with one good file of each k, the four rejections of ADR 0081 by their reasons (not k times, k = 9, no PNG, cut), the names passed over, `ui/` images against a synthetic bundle and without one, a symbolic link; each of `pack.strings`' rules with its refusal; an audio pack's effect and track limits from the game's names, a hidden duplicate, a name the game lacks, the three `.loop` cases; a text pack's refused file, cut value, unknown keys, `Port.strings` keys and places, `pack.strings` over `textpack.strings` and `mt_textpack_open` with `pack.strings` alone; the kind told from a folder; Scale2x and Scale3x worked by hand on a diagonal and on a flat image, nearest at 5x; the builder's skipped lines, a file that disagrees, `--pack-no-ui` and three refused requests; the export from a synthetic bundle (binary plists, five tables, a CP949 syllable) checking clean and opening; the `.strings` quoting. `tests/cpp/pack_tools_test.cpp` (`platform.pack_tools: *`): the folders, Check packs' list of packs, the report's words, `Port.strings` passing its own check whole, the menu's actions. `settings_menu_test.cpp`: the four rows' decisions on the Switch. `tests/test_textpack.py`: `pack.strings` in the Python check and export.
- **Local, real assets** (recorded on #371): the walk from the download, with the binary alone: `--import-ipa`, then `--dump-textures` (193 textures and 66 UIKit images), `--build-texture-pack` (259 of 259 pictures at 2x in 1.7 s; `--check-pack`: `opens, 259 usable`), a pack of two of them (one GL texture, one UIKit image) checked (`opens, 2 usable`; a wrong-sized file added is `rejected`, exit 3) and loaded (`1 replaced` of 205 uploads, `1 replaced` of 66 UIKit images). `--export-text` writes 3,886 entries whose `Scripts`, `ScriptExtra` and `Tables.strings` are byte-identical to `mtport.textpack export`'s, and checks clean.

## Consequences

- A player on any platform can dump, build, translate and check without the repository; on iOS and the Switch this is the first way to make a pack at all.
- The check and the game can't disagree on a rule: changing a limit in a loader changes the check with it. The text check's `Port.strings` rules depend on the frontend handing its table over (`mt_packcheck_set_port_text`); without it, `Port.strings` keys are counted as usable.
- Make texture pack runs on the frontend thread with the game paused: a whole dump at 2x is a few seconds of a frozen menu. If dumps grow, it can move to a worker thread with a progress line.
- #370's packs screen can show the same verdicts from `mt_pack_check` and act on the same folders.
- How we'd know this is wrong: a pack the check passes whose file the game rejects in a run (a rule outside the shared code), or players asking for factors other than 2x in the menu often enough that the row needs a choice.
