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

- **Fullscreen**: F11 or Alt+Enter. On a Mac you can choose between a fullscreen Space (macOS's own, with its animation) and a borderless window over the desktop (`fullscreen_mode`).
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
