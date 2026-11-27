# Making packs

A pack is a folder that changes what the game shows or plays, without touching the game's files: a **texture pack** (pictures), an **audio pack** (music and sounds) or a **text pack** (a translation). You make and check all three with the game you downloaded. You don't need the source code or Python.

**Before you start, the legal part.** A texture dump, a text export and every pack made from them hold the game's own art or text. Keep them to yourself, and don't share them or links to them. Only packs with nothing from the game in them can be shared in the project's places: your own music and sounds, and translations of the port's own menus. [Sharing packs](sharing-packs.md) has the rules and [What's included, and what isn't](../legal.md#packs) has the reasons.

## Where the tools work

The tools are rows in the settings menu's **Other** tab (F1, or the gear), and options on the command line. They read and write in the pack folders the [packs screen](settings.md#the-packs-screen) lists:

| System | The folder that holds the pack folders |
|---|---|
| Windows, Linux | The program's folder |
| Mac | The folder that holds `MapleThief.app` |
| iPhone | The app's folder in the Files app |
| Nintendo Switch | `sdmc:/switch/maplethief/` |

In it the tools use `texture-dumps/` for the dump, and `texture-packs/`, `audio-packs/` and `text-packs/` for the packs. A pack in its pack folder shows up in the packs screen; a pack anywhere else works too, by its path.

To run a tool from the command line, open a terminal in the game's folder and start the program with the option: `MapleThief.exe --check-pack MyPack` on Windows, `./MapleThief.app/Contents/MacOS/MapleThief --check-pack MyPack` on a Mac, `./MapleThief.sh --check-pack MyPack` on Linux. A tool prints its report, doesn't open a window, and exits.

## Checking a pack

**Other → Check packs** checks every pack the settings name and every pack in the pack folders, with the game's own rules. Each pack gets a line: *opens, 812 usable*, *opens, 810 usable, 2 rejected*, or *refused*. Choose a line to see its first problem. The whole report, file by file, is in the log (**Saves → Open logs folder** on a computer).

From the command line: `MapleThief --check-pack MyPack`. Add `--pack-kind texture`, `audio` or `text` when the folder doesn't say what it is. The exit status is 0 when the game uses every file, and 3 when it rejects one or refuses the pack.

The report has four kinds of lines:

- **refused**: the game won't open the pack at all (a broken `pack.strings`, a text pack without a manifest).
- **rejected**: the game skips this file and keeps its own picture, sound or text (a picture of the wrong size, a damaged file, a sound that's too long).
- **ignored**: a name the game doesn't look at (a file it has no use for, a text key that doesn't exist).
- **note**: good to know, nothing is lost (a value that will be cut to fit, a pack without `pack.strings`).

The check and the game share their code, so the check's verdict is what the game will do.

## Texture packs

A texture pack is a folder of pictures the game draws instead of its own: HD versions of the characters, monsters, maps and menus, or pictures you edited by hand.

1. **Dump the textures.** Choose **Other → Dump textures** (the row says *Start*). Close the menu and play through the maps you want: every picture the game draws from then on is written once into `texture-dumps/dump/`, named by its contents and size (`<hash>_<W>x<H>.png`), with the menus' images in its `ui/` folder. Pictures already on screen are written the next time the game loads them, so change maps once. Choose **Dump textures** again (*Stop*) when you're done. To catch everything from the title screen on, start the game with `--dump-textures DIR` instead.
2. **Make them larger**, in either of two ways:
   - **Other → Make texture pack** writes every picture of the dump twice as large into `texture-packs/dump-2x/`, with the Scale2x method, which keeps edges sharp. From the command line you choose the size and method: `MapleThief --build-texture-pack texture-dumps/dump texture-packs/MyPack --pack-scale 4` (1 to 8), with `--pack-scaler nearest` for plain blocky pixels, or `--pack-no-ui` to leave the menus out.
   - **By hand**: make a folder in `texture-packs/`, copy the PNGs you want to change from the dump into it, and edit them in any image editor. Keep each file's name, and make the picture the same size or a whole multiple of it (2, 3, … 8 times as wide and as high, at most 4096 pixels a side). Keep transparent areas transparent.
3. **Name it** (optional): a file `pack.strings` in the pack's folder gives the name, version and author the game shows. See [The pack.strings file](#the-packstrings-file).
4. **Check it**: **Other → Check packs**, or `MapleThief --check-pack texture-packs/MyPack`.
5. **Play with it**: choose **Video → Texture pack** to open the packs screen and turn the pack on (or set `texture_pack = /path/to/MyPack` in `settings.ini`, or start with `--texture-pack /path/to/MyPack`), then start the game again. When it quits, the log says how many pictures the pack replaced.

At a render scale above 1, **Video → Texture filter** `nearest` keeps a pack's pixels crisp. A picture the dump didn't see (a map you didn't visit) stays the game's own: dump again into the same folder, which adds only what's new, and make the pack again.

The steps behind this are in [ADR 0081](../adr/0081-texture-packs-and-nearest-texture-filter.md) and [ADR 0205](../adr/0205-pack-tools-in-the-game.md).

## Audio packs

An audio pack is a folder of your own music and sound files that play instead of the game's.

1. **Find the sounds to replace.** The game's own sounds are the files in `game/MapleThief.app/sound/` (on a Mac, right-click `MapleThief.app` inside `game/` and choose **Show Package Contents**). The music is MP3 (the main menu's is `31.mp3`; the title screen has none) and the effects are WAV. The music of each map is listed in [the audio notes](../re/audio.md#map-music).
2. **Make a folder** in `audio-packs/` and put your files in it, named like the game's, with any of these extensions: `31.flac`, `31.mp3` or `31.wav` replaces `31.mp3`, and `12.wav` or `12.mp3` replaces the effect `12.wav`. WAV (PCM), MP3 or FLAC, mono or stereo, 8,000 to 192,000 Hz, up to 128 MB a file; an effect up to 60 seconds, a track up to 30 minutes. Long tracks are best as FLAC or MP3: a WAV track stays in memory whole.
3. **Loops** (optional): music loops by itself, as the game's does. To loop only part of a track (an intro, then a loop), add `31.loop`, a text file with two numbers: the sample frame where the loop starts and the one where it ends (the end isn't played), in your file's own frames, for example `441000 2646000`. A WAV with a loop saved by a sound editor (a `smpl` loop) works too.
4. **Name it** (optional) with a `pack.strings`, with `content = "own";` when everything in it is yours.
5. **Check it**: **Other → Check packs**, or `MapleThief --check-pack audio-packs/MyPack`. The check knows from the game's files which of your files are effects and which are music, and says which game sound each one replaces.
6. **Play with it**: choose **Audio → Audio pack** and turn the pack on in the packs screen (or `audio_pack = /path/to/MyPack`, or `--audio-pack /path/to/MyPack`) and start the game again.

A file the game can't use (damaged, too long, too many channels) is skipped: the game's own sound plays instead, and the log says why, once. An audio pack of only your own music and sounds can be shared in the project's Discussions ([Sharing packs](sharing-packs.md)).

## Text packs

A text pack is a folder of translated text: the game's dialogue, the names and descriptions of its items, monsters, NPCs and skills, and the port's own menus.

1. **Export the text.** Choose **Other → Export text**. It writes `text-packs/text-en/` (or `text-ko/` when the game plays in Korean) with:
   - `pack.strings`: the pack's name and language;
   - `Scripts.strings` and `ScriptExtra.strings`: the dialogue;
   - `Tables.strings`: names and descriptions, each with the room it has;
   - `Port.strings`: the port's menus.

   From the command line: `MapleThief --export-text text-packs/MyTranslation` (add `--language ko` for the Korean text).
2. **Rename and translate.** Rename the folder, then in `pack.strings` give the pack your own `id` and `name`, set its `language` (`de`, `fr`, `pt-BR`, …) and your name as `author`. Translate the values, the text on the right of each `=`, in any text editor that saves UTF-8. Keep the keys on the left as they are, and keep each `{0}`, `{1}`, … in `Port.strings`. Delete the lines you don't translate: anything a pack doesn't have stays in the game's own words.
3. **Check it**: **Other → Check packs**, or `MapleThief --check-pack text-packs/MyTranslation`. It says which values will be cut to fit and which keys the game doesn't have.
4. **Play with it**: choose **Other → Text pack** and turn the pack on in the packs screen (or `text_pack = /path/to/MyTranslation`, or `--text-pack /path/to/MyTranslation`) and start the game again.

The game's text is drawn with its own font, plus a second font for accented Latin, Greek and Cyrillic letters. Names and descriptions have a fixed room in the game's tables: a longer one is cut at a whole letter, and the log says so, once. The format is described in [the text pack spec](../formats/text-pack.md).

A translation of the game's text is made from the game: keep it to yourself. A pack with only `Port.strings`, the port's menus, has nothing of the game's in it: you can share that one ([Sharing packs](sharing-packs.md)).

## The pack.strings file

Every pack may have a `pack.strings` at its top, a text file in UTF-8 with one `"key" = "value";` a line ([ADR 0149](../adr/0149-pack-manifest-several-packs-and-what-the-project-hosts.md#the-manifest)):

```
"format" = "1";
"kind" = "texture";
"id" = "my-hd-sprites";
"name" = "My HD sprites";
"version" = "1.0";
"author" = "Your name";
"game_version" = "1.5";
"content" = "derived";
```

- `format` (`1`), `kind` (`texture`, `audio` or `text`), `id` (1 to 64 lower-case letters, digits, `.`, `_` and `-`) and `name` (up to 64 characters) are needed. A text pack also needs `language`.
- `version`, `author`, `description`, `game_version` (`1.5`), `port_min` (the oldest port version the pack works with, such as `1.4.0`), `content` (`own` or `derived`) and `license` are optional.

A pack without `pack.strings` still works: the game shows its folder's name. A `pack.strings` that breaks a rule makes the game refuse the whole pack, and the check says which rule. Text packs made before `pack.strings` have `textpack.strings` instead; the game still reads it.

## If you have the source code

The repository's Python tools do the same as the game's: `python3 -m mtport.texpack build`, `python3 -m mtport.audiopack init` (a list of the game's sounds with their lengths) and `python3 -m mtport.textpack export` and `check`, after `scripts/bootstrap`. Use either; the game's check is the one that decides.
