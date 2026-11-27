# Settings

## The settings menu

Press **F1** (or the gamepad's Guide button, or click both sticks, or the gear button beside the picture) at any time. The game pauses while the menu is open.

| Tab | What's in it |
|---|---|
| **Preset** (top) | Original, Original + controller, Enhanced, or Custom |
| **Video** | How the picture fills the window, the border around it, fullscreen, sharpness, vsync |
| **Audio** | Master, music and effects volume, gapless music, mute in the background, mono, the output device, and on an iPhone the audio session |
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

On an iPhone, **Audio session** (`ios_audio`) sets how the game's sound shares the phone. It applies the next time you start the game.

- `original` (default), as the original game did: other apps' music keeps playing with it, and the Ring/Silent switch silences the game.
- `own`: the game plays even with the switch on silent, and other apps' music is turned down while it runs.

Neither shows on a computer.

## Gameplay and assists

- **Save on quit** (`autosave = on_quit`, part of Enhanced): saves wherever the game's own Save would when you quit, minimise or pick the menu's exit to the main menu. Going to the main menu from the death popup never saves: the hero is dead there, and the game never lets a fallen hero save, so you go back to your last save (and skip the fall's penalty, as in the original).
- **Crash recovery** (`crash_recovery = on`, part of Enhanced): keeps a recovery point of your game, so that after a crash the next start can put you back a few minutes before it. See [Saves](saves.md#after-a-crash-recovery-points).
- **Cash shop for meso** (`cash_shop = meso`, an Assist, off in every preset): the original's cash shop needed real money and an online server. With this on, its items and revives cost in-game meso instead, at a price that grows with your level.
- **Hide online buttons** (`hide_online_ui`) and **local Game Center** (`local_game_center`): hide the menu buttons whose web pages are gone, and turn Game Center's button into a leaderboard of your own save slots, with a few achievements.

## Packs

Packs change what the game shows and plays without touching its files: a **texture pack** (HD or edited pictures), an **audio pack** (your own music and sounds) or a **text pack** (a translation). Set one with **Video → Texture pack**, **Audio → Audio pack** or **Other → Text pack** (or `texture_pack`, `audio_pack`, `text_pack` in `settings.ini`, or `--texture-pack`, `--audio-pack`, `--text-pack`), and start the game again. At a render scale above 1, **Texture filter** `nearest` (`texture_filter = nearest`, the sharp sprites of [Display](#display)) keeps a texture pack's pixels crisp.

You make and check packs with the game itself: the **Other** tab has **Dump textures**, **Make texture pack**, **Export text** and **Check packs**. [Making packs](packs.md) walks through each kind step by step. A pack the game can't use, or a file in it, is skipped with the reason in the log, and **Check packs** shows the same reasons before you play.

Texture dumps, text exports and packs made from them hold the game's own art or text: keep them to yourself, and don't share them ([Sharing packs](sharing-packs.md)). On the Nintendo Switch the pack rows list the folders in `sdmc:/switch/maplethief/texture-packs/`, `audio-packs/` and `text-packs/` ([below](#on-the-nintendo-switch)).

The game's text is drawn with its own font, plus a second font for accented Latin, Greek and Cyrillic letters. The port's menus have plain letters only: their accents are dropped ("Pötion" shows as "Potion").

## Language

**Language** (`language`, in the **Other** tab) plays the game in English (`en`, the default) or in Korean (`ko`). Your IPA holds both: the Korean build has its own text, its two-page help and a terms screen on the first start. It applies the next time you start the game.

From the command line, `--language ko` sets it for one run. The environment variable `MAPLETHIEF_LANGUAGE=ko` overrides the setting and the flag.

The port's own menus, such as this settings menu, stay in English. To play in another language, use a [text pack](packs.md#text-packs) with a translation.

## Checking for new versions

The game itself never goes online. If you want to hear about new versions of the port, the first launch asks once, and **Check for new versions** in the **Other** tab changes your answer (`update_check = on`, off by default). It is off in every preset and never runs in the Original preset.

When it is on, the port asks github.com once a day which version is the latest. It downloads nothing and installs nothing. If a newer version is out, one line shows in the corner of the title screen and in **Port update** in the **Other** tab; choosing either opens the release page in your browser. Nothing appears during play.

GitHub sees your IP address and the port's version, as it would for any visit to the releases page. The port sends nothing else about you, your device or your saves. It keeps the time of its last check in `update_check.txt`, next to `settings.ini`. On Linux the check needs the system's libcurl; without it there is simply no check.

## Speedrun timer

**Speedrun timer** and **Speedrun category** in the **Other** tab (`run_timer`, `run_category`) time a run from a new character with automatic splits. On a computer, **LiveSplit server** and **LiveSplit game time** (`livesplit = off | HOST:PORT`, `livesplit_game_time`) send the run to LiveSplit. [Speedrun timer](speedrun.md) explains them.

## Recording runs

**Record runs** in the **Other** tab (`record_runs = on`, off by default and in every preset) keeps a replay of every session, from the start until you quit, in the `runs` folder of your data folder. A file holds the times the game read and what you pressed and touched, your settings and your own save files from the start, and nothing of the game itself; an hour is well under a megabyte.

`MapleThief --play-run FILE` plays one back exactly, as fast as your computer goes, in a scratch folder of its own (your saves are never touched). It needs the same game and the same version of the port that recorded it. It says `all the same` when every checkpoint matched.

A save state's load, a save import or backup restore, a data folder switch and `autosave` cannot be played back: the recording ends just before one, and the file says which.

## On the Nintendo Switch

The console's menu has only the rows that work there, so no row does nothing. Open it with **+**, by clicking both sticks, or with a tap on the gear in handheld mode (the HOME button belongs to the console). Then use the d-pad, A to choose, B to go back, and L / R to change tabs.

Not in the Switch's menu:

| Row | Why |
|---|---|
| **Fullscreen**, **Fullscreen mode**, **Fullscreen display** | The game always fills the screen, docked or handheld |
| **Output device** | The console moves the sound between its speakers and the dock by itself |
| **Right mouse button** | No mouse |
| **Save folder** (`save_dir`) | The saves stay on the SD card, in `sdmc:/switch/maplethief/data/saves` |
| **Touch key pad layout** | **Edit the touch key pad** (Controls) changes it |
| **Check for new versions**, **Port update** | The console build doesn't go online |
| **Data location**, **Open data folder**, **Open logs folder** | The data folder is fixed, and the console has no file manager |

Some defaults differ on the console, in every preset. **Scale mode** is `sharp`, and the touch key pad hides while a controller is used. **Renderer** is `gpu`, because the console's processor can't draw the game in software at full speed. **Render scale** is `1`, which keeps the game at its pace. You can still choose `auto` or a higher scale, but it may slow the game down.

The keyboard shortcuts for the window (F11, Alt+Enter, Ctrl+1 to 5) and F12 for a screenshot do nothing there. **Quit** goes back to the Homebrew Menu.

Packs and border pictures go in folders on the SD card, beside `game`. The menu lists what is in them: choose with Left / Right, and start the game again.

| Row | Folder |
|---|---|
| **Texture pack** | `sdmc:/switch/maplethief/texture-packs/<pack>/` |
| **Audio pack** | `sdmc:/switch/maplethief/audio-packs/<pack>/` |
| **Text pack** | `sdmc:/switch/maplethief/text-packs/<pack>/` |
| **Border image** | `sdmc:/switch/maplethief/borders/<picture>.png` |

Exporting and importing saves works without a file dialog: see [Saves](saves.md#on-the-nintendo-switch).

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
