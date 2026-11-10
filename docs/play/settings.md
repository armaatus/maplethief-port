# Settings

## The settings menu

Press **F1** (or the gamepad's Guide button, or click both sticks, or the gear button beside the picture) at any time. The game pauses while the menu is open.

| Tab | What's in it |
|---|---|
| **Preset** (top) | Original, Original + controller, Enhanced, or Custom |
| **Video** | How the picture fills the window, the border around it, fullscreen, sharpness, vsync |
| **Audio** | Master, music and effects volume, gapless music, mute in the background, mono, the output device |
| **Controls** | Rumble, the on-screen buttons, and **Bindings** to remap keys and buttons |
| **Gameplay** | Save on quit, and the game's own options (sound, speed, vibration) |
| **Assists** | Optional help that changes the game, such as the cash shop for meso. Always off unless you turn it on |
| **Saves** | Export and import your saves, and the backups |
| **Other** | The language, text packs, and the optional [check for new versions](#checking-for-new-versions) |

Move with the arrow keys, the d-pad or the mouse. **Tab**, **Q / E** or **LB / RB** switch tabs. **Esc** or **B** closes the menu. Most settings apply at once; those marked `*` apply the next time you start the game. **Quit** is at the bottom of every tab.

## The presets

| Preset | For |
|---|---|
| **Original** | The iPhone game exactly as it was. The on-screen buttons always show |
| **Original + controller** | The original game, made comfortable for a keyboard or gamepad. The on-screen buttons hide while you use them, and buttons for the long-gone online services are hidden |
| **Enhanced** | Every enhancement that doesn't change the gameplay: sharper text, sharp sprites at high resolution, gapless music, save on quit, a local Game Center |
| **Custom** | Your own choices. Changing a setting a preset controls switches you to Custom |

Volumes, your controls and the border colour are always yours, whatever the preset. Nothing that changes the gameplay (such as the cash shop assist) is ever turned on by a preset.

## Display

- **Fullscreen**: F11 or Alt+Enter. On a Mac you can choose between a fullscreen Space (macOS's own, with its animation) and a borderless window over the desktop (`fullscreen_mode`). **Fullscreen display** (`fullscreen_display`) picks the screen fullscreen goes to, from the displays connected now; `current` is the one the window is on. If the chosen display isn't connected, fullscreen uses the window's display and the setting is kept for when it's back. A change while already fullscreen counts from the next time fullscreen turns on.
- **Window size**: Ctrl+1 to Ctrl+5 (Cmd+1 to Cmd+5 on a Mac) make the window exactly 1 to 5 times the game's picture, centred on its display, so `scale_mode = integer` never drops a step because the window is a few pixels short. With widescreen on, the picture is 568 points wide. A size that doesn't fit the display gives the largest one that does.
- **How the picture fills the window** (`scale_mode`). The iPhone's screen was 480 × 320, so the picture has to be scaled up:
  - `integer` (default): the largest whole multiple. Every pixel stays crisp and even; there may be bars around it.
  - `sharp`: fills the window, with sharp but smooth edges. A good choice for most screens.
  - `fit`: fills the window, blocky pixels.
  - `smooth`: fills the window, soft and blurry.
- **Border** around the picture: a colour (`border_color`), the game itself blurred behind it (`border = blur`), or a picture of your own (`border = image`).
- **Render scale** (`render_scale`): draws the game at a higher resolution, so text and shapes are sharper. `auto` picks the highest your computer can keep up with.
- **GPU** (`gfx_backend = gpu`): draws with your graphics card instead of the processor. Faster at high render scales. The default, `software`, works everywhere.
- **Sharp sprites** (`texture_filter = nearest`): keeps characters and monsters pixel-sharp at high render scales.

## Sound

Master, music and effects volume (0 to 100), and mute while the window isn't focused (`mute_unfocused`, on by default).

**Mono** (`audio_mono = on`) mixes left and right into both channels, for one ear or a single speaker. **Output device** (`audio_device`) picks where the sound plays from the devices your computer has; `default` follows the system. If the chosen device is unplugged, the game plays on the default and goes back to it when it returns.

The game's music files end in a few seconds of silence, so on the iPhone the music paused at every loop. **Gapless music** (`gapless_music = on`, part of Enhanced) skips that silence.

## Gameplay and assists

- **Save on quit** (`autosave = on_quit`, part of Enhanced): saves wherever the game's own Save would when you quit or minimise.
- **Cash shop for meso** (`cash_shop = meso`, an Assist, off in every preset): the original's cash shop needed real money and an online server. With this on, its items and revives cost in-game meso instead, at a price that grows with your level.
- **Hide online buttons** (`hide_online_ui`) and **local Game Center** (`local_game_center`): hide the menu buttons whose web pages are gone, and turn Game Center's button into a leaderboard of your own save slots, with a few achievements.

## Texture packs

You can make HD versions of the game's graphics from your own copy, or edit them by hand. It's a few steps, explained in [ADR 0081](../adr/0081-texture-packs-and-nearest-texture-filter.md). Texture dumps and packs are made from the game's art: keep them to yourself.

## Audio packs

An audio pack is a folder of your own music and sound files that play instead of the game's: a better-quality version of a track, a remaster, or different sound effects. The game's files stay as they are.

1. List the sounds you can replace: `python3 -m mtport.audiopack init MyPack` (from the project folder, after `scripts/bootstrap`) makes the folder `MyPack` with `audiopack.txt` in it. Each line is one of the game's 44 sounds: its file name, whether it's music (BGM) or an effect (SFX), and its length. The main menu's music is `31.mp3`; the title screen has none. The music of each map is in [the audio notes](../re/audio.md#map-music).
2. Put your files in the folder, named like the game's, with any of these extensions: `31.flac`, `31.mp3` or `31.wav` replaces `31.mp3`, and `12.wav` or `12.mp3` replaces the effect `12.wav`. WAV (PCM), MP3 or FLAC, mono or stereo, 8,000 to 192,000 Hz, up to 128 MB a file; an effect up to 60 seconds, a track up to 30 minutes. Long tracks are best as FLAC or MP3: a WAV track stays in memory whole.
3. Music loops by itself, as the game's does. To loop only part of a track (an intro, then a loop), add `31.loop`, a text file with two numbers, the sample frame where the loop starts and the one where it ends (the end isn't played), in your file's own frames, e.g. `441000 2646000`. A WAV with a loop saved by a sound editor (a `smpl` loop) works too.
4. Set **Audio pack** (`audio_pack = /path/to/MyPack` in `settings.ini`, or start with `--audio-pack /path/to/MyPack`) and start the game again.

A file the game can't use (damaged, too long, too many channels) is skipped: the game's own sound plays instead, and the log says why, once. Audio packs are your own files: the project ships none.

## Text packs

A text pack is a folder of translated text: the game's dialogue, the names and descriptions of its items, monsters, NPCs and skills, and the port's own menus. A community translation can be shared as a text pack without touching the game's files.

1. Write out the text to translate: `python3 -m mtport.textpack export MyPack` (from the project folder, after `scripts/bootstrap`). It makes the folder `MyPack` with `textpack.strings` (the pack's name and language), `Scripts.strings` and `ScriptExtra.strings` (dialogue), `Tables.strings` (names and descriptions, each with the room it has) and `Port.strings` (the port's menus).
2. Translate the values, the text on the right of each `=`, in any text editor that saves UTF-8. Keep the keys on the left as they are. Delete the lines you don't translate: anything a pack doesn't have stays in the game's own words. Check the pack with `python3 -m mtport.textpack check MyPack`.
3. Set **Text pack** (`text_pack = /path/to/MyPack` in `settings.ini`, or start with `--text-pack /path/to/MyPack`) and start the game again.

The game's text is drawn with its own font, plus a second font for accented Latin, Greek and Cyrillic letters. Names and descriptions have a fixed room in the game's tables: a longer one is cut at a whole letter, and the log says so, once. The port's menus have plain letters only: their accents are dropped ("Pötion" shows as "Potion"). The format is described in [the text pack spec](../formats/text-pack.md). A text pack holds the game's text: keep packs made from it to yourself, and share only your own translations.

## Checking for new versions

The game itself never goes online. If you want to hear about new versions of the port, the first launch asks once, and **Check for new versions** in the **Other** tab changes your answer (`update_check = on`, off by default). It is off in every preset and never runs in the Original preset.

When it is on, the port asks github.com once a day which version is the latest. It downloads nothing and installs nothing. If a newer version is out, one line shows in the corner of the title screen and in **Port update** in the **Other** tab; choosing either opens the release page in your browser. Nothing appears during play.

GitHub sees your IP address and the port's version, as it would for any visit to the releases page. The port sends nothing else about you, your device or your saves. It keeps the time of its last check in `update_check.txt`, next to `settings.ini`. On Linux the check needs the system's libcurl; without it there is simply no check.

## The settings file

Everything in the menu is saved in a text file, `settings.ini`, next to your [saves](saves.md#where-your-saves-are). You can edit it with any text editor while the game is closed. One setting per line:

```ini
# lines starting with # are comments
scale_mode = sharp
border = blur
render_scale = auto
gfx_backend = gpu
music_volume = 80
gapless_music = on
rumble_strength = 60
```

A setting with a typo is skipped and mentioned in the log. Every setting also works on the command line for one run, e.g. `MapleThief --scale-mode sharp`. The complete list of settings and what each one does is in the [full reference](../build/running.md#all-settings).
