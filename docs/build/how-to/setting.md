# Adding a setting

Every setting is one row of the **settings registry**, the table `kRegistry` in `platform/src/settings.cpp` ([ADR 0070](../../adr/0070-settings-registry-and-presets.md)). The row gives the key its line and comment in `settings.ini`, its command-line flag, its value in each preset, and its row in the settings menu ([ADR 0076](../../adr/0076-settings-menu.md)). Parsing, writing, the command line, the presets and the menu all read the rows, so the setting itself is two edits; the rest of this page is where it is used, tested and documented.

## 1. Decide what it is

- **The key**: lower case, digits and `_`, named for what it does (`gapless_music`, not `music_fix`). Its flag is the key with dashes: `--gapless-music`.
- **The value**: on/off, one of a list of names (an enum), a whole number in a range, a colour, or a custom parser.
- **The default**: what the game does today, normally the Original preset's value. A setting changes nothing until someone turns it on.
- **The group** (the menu's section): Video, Audio, Controls, Gameplay, Accessibility, Assists, Saves or Other. Anything that changes gameplay (speed, cheats, auto-actions) is an **Assist**, and no preset ever turns an Assist on.
- **The presets**: a value that is part of an experience (presentation, comfort, pacing) has a value in each of Original, Original + controller and Enhanced. A personal value (a volume, a strength, a path, Accessibility, the language) is `kNotInPresets` and stays as the player set it.
- **When it applies**: at the next start (`Applies::kNextStart`, the default), or at once while the game runs (`Applies::kAtOnce`), which needs a branch in `apply_live_setting` (`platform/src/live_settings.cpp`).

A new behaviour behind the setting that is a design decision of its own gets an ADR.

## 2. Add the field and the row

The field, in `struct Settings` (`platform/src/settings.h`), with a comment naming the key and flag like its neighbours:

```cpp
    bool scratch_demo = false; // scratch_demo, --scratch-demo (on | off)
```

The row, in `kRegistry` (`platform/src/settings.cpp`), where a new file should list it:

```cpp
    {.key = "scratch_demo",
     .group = SettingGroup::kAudio,
     .comment = "# a scratch example of a setting (#333): on | off",
     .value = on_off<&Settings::scratch_demo>(),
     .flag = "--scratch-demo",
     .label = "Scratch demo",
     .help = "Does nothing; it shows how a setting is added"},
```

- `.value` binds the type to the field: `on_off<&Settings::f>()`, `choice<&Settings::f>(kFooNames)`, `uint_range<&Settings::f>(min, max, step)`, `color<&Settings::f>()` or `custom_value(parse, print, expect)`. A new enum needs its names list `kFooNames` in `settings.h`, in the enum's order.
- `.presets = {"off", "off", "on"}` gives Original, Original + controller and Enhanced, as the file writes them. Leave it out for `kNotInPresets`.
- `.label` and `.help` are the menu's; a text pack translates them as `setting.<key>.label` and `.help` ([ADR 0108](../../adr/0108-text-packs.md)), with nothing to add to `port_text.def`. If the help names keys, add `.help_pad` for a controller.
- `.applies = Applies::kAtOnce`, `.needs` (platform capabilities, `platform_caps.h`), `.ios_only` and `.game_only` as needed. A different default on one platform is a row of `kPlatformDefaults`.

`check_registry` checks the table at compile time (a `static_assert`) and in the tests: a bad key, a preset value the row can't parse, an Assist that a preset turns on.

## 3. Use it

Read the field where the behaviour lives: from `options.settings` in the frontend (`platform/src/game.cpp`, `main.cpp`), or pass it down to the runtime with a setter, as `gapless_music` does with `mt_audio_set_gapless_music` in `platform/src/audio_out.cpp`. The command line, the environment (`MAPLETHIEF_SETTINGS`) and the file all arrive there through the registry: the flag works without any code of its own.

The flag's **usage line** is not generated: add `[--scratch-demo on|off]` next to its neighbours in `print_usage()` in `platform/src/main.cpp`, and in the option list in the comment at the top of that file (one line saying what it does). Nothing tests these, so they are easy to forget.

## 4. Test it

These tests have a hand-written line per setting, so a new row fails each of them until you add its line. That is on purpose: each says what was decided for the row.

| Test | File | What to add |
|---|---|---|
| `platform.presets: the registry matches #89's table; each preset sets exactly its column` | `tests/cpp/presets_test.cpp` | the key and its three preset values in `kExpected`, at the row's position |
| `platform.settings menu: every row is supported or hidden on the Nintendo Switch (#241)` | `tests/cpp/settings_menu_test.cpp` | a line in `kSwitchRows`: shown on the Switch or not, and why |

Then a test of the behaviour itself: the setting's default, a value read from the file, and what changes when it is on. If the default isn't the Original column, also add the key to `presets: no settings file is today's defaults` and say why.

## 5. Document it

- A row in the table under [All settings](../running.md#all-settings), with the default, the presets and what it does.
- For a player-facing setting, a line in [Settings](../../play/settings.md) in its group's section.

Then [the quick check](../testing.md#before-you-push): `scripts/lint` and `scripts/test`.

## Worked example

Followed for [#333](https://github.com/armaatus/maplethief-port/issues/333) on a scratch branch: `scratch_demo` as above, an on/off Audio setting outside the presets. The field, the row and its line in `presets_test.cpp`'s `kExpected` built cleanly; `cmake --workflow --preset mac-debug` then failed exactly one test, `platform.settings menu: every row is supported or hidden on the Nintendo Switch (#241)`, until `kSwitchRows` had its line. `MapleThief --scratch-demo maybe` was refused as a usage error (`scratch_demo: 'maybe' is not on or off`, exit 2) with no code but the row, which showed that the usage line had not picked it up. The example was not merged.
