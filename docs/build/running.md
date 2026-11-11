# Running and options

Every command-line option and setting of `MapleThief`, for people who build the port or want the details. The everyday guide is [Settings](../play/settings.md) and [Controls](../play/controls.md). To build the program, see [Build from source](README.md).

## Starting the program

Run it from where you built it ([One-command build](README.md#one-command-build-just-build)), or from the unpacked download. `MapleThief --version` prints its version and exits. An unknown option or a bad value is a usage error (exit 2); the program prints the usage line.

With the generated code it runs the game. Without it (CI, a clean clone, the kits) it shows a solid placeholder colour: `--smoke-frames N` runs that placeholder and quits after N frames, and with `SDL_VIDEO_DRIVER=dummy` it runs without a display, which is how the `platform.smoke` test and CI run it. `--gfx-prototype` shows the rendering backend's prototype scene instead.

Options and settings stack up: the program reads `settings.ini` in its [data folder](#the-data-folder) first, then the preset (`--preset`), then the other flags. Most options are a setting's flag ([All settings](#all-settings)), and the rest are below.

## Finding the game

The program looks for the game's bundle in this order ([ADR 0127](../adr/0127-data-folder-next-to-the-game.md)):

1. `--bundle DIR`
2. `MAPLETHIEF_BUNDLE_DIR` (both of these must exist)
3. the build's own default: a build compiled with the generated code uses `work/ipa/Payload/MapleThief.app`, and the macOS game app its embedded copy. A player build has neither.
4. `game/MapleThief.app` in the game folder, a bundle put there by hand
5. the data location's `game` folder, where an imported bundle lives
6. otherwise the import screen: the player picks or drops `MapleThief-1.5.ipa`, and the game checks it and imports it ([ADR 0125](../adr/0125-player-builds-icon-guard-modes-and-release-flow.md)). A scripted run with no bundle exits 4.

`--import-ipa FILE` imports again without a window. It exits 0, or 3 with the reason.

## The data folder

Everything the port writes for a player goes in one data folder, and one resolver picks it ([ADR 0127](../adr/0127-data-folder-next-to-the-game.md)). `<base>` is the folder of `MapleThief.exe`, the folder around `MapleThief.app` (never inside it), or the Linux game folder (the one that holds `MapleThief.sh` and `bin/`). The first rule that applies wins:

1. `MAPLETHIEF_DATA_DIR`
2. `--data-location game|user`, for this run
3. `<base>/user-data.txt` exists: the per-user folder
4. macOS, with `<base>` in an Applications folder: the per-user folder
5. `<base>/data`, if the game can write there
6. otherwise the per-user folder, with a fallback reason

The per-user folder is `~/Library/Application Support/MapleThief`, `%APPDATA%\MapleThief` or `~/.local/share/MapleThief`. A fallback (a protected or read-only folder, or macOS's App Translocation) is logged once, shown in the Saves tab and noted once on the title screen. A `portable.txt` from older versions is accepted and does nothing.

```text
<base>/
  game/                       the imported bundle (can be imported again; not backed up)
  user-data.txt               only when the player chose the user folder
  data/                       everything of the player's:
    settings.ini  bindings.ini  window.ini
    saves/                    the game's save files and gamecenter.json
    backups/  states/  screenshots/  exports/  import/  logs/  crashes/
```

Builds in `build/` follow the same rule, so their data lands in `build/` beside the executable and a clean build deletes it. Under `MAPLETHIEF_DATA_DIR` the first-start copy, the marker and the write probe don't run, which keeps tests isolated.

| Option | What it does |
|---|---|
| `--print-data-dir` | prints `data`, `location`, `reason`, a fallback's `why`, `saves` and `game`, without opening a window, and exits 0 |
| `--data-location game\|user` | use that folder for this run; writes no marker |
| `--switch-data-location game\|user` | what the Saves tab's *Data location* row does: copies the data to the other folder (the source is never changed) and updates the marker. A destination that already has data is used as it is. Exits 0, or 1 with the reason |
| `--migrate-from DIR [--force]` | copies saves and settings from any folder into this one: an old per-user folder, a backup, an old kit's `data/`. Without `--force` it refuses a destination that holds data; with it, what it replaces goes to `backups/import/<time>/` |

## Saves, backups and bindings

| Option | What it does |
|---|---|
| `--export-saves ZIP\|DIR` | copies your saves out |
| `--import-saves ZIP\|DIR` | brings saves in, from a zip, a folder or a copied `Documents/` folder. Every file is checked first; the whole set is refused, changing nothing, if any file would break the game. Files it replaces go to `backups/import/<time>/` |
| `--check-saves ZIP\|DIR` | only reports |
| `--list-backups` | lists the backups. To restore one, import its folder |
| `--bindings FILE` | reads another bindings file |
| `--save-bindings` | writes out every binding in effect |
| `--save-settings` | writes the settings in effect back to `settings.ini`, keeping your comments |
| `--preset NAME` | `original`, `original_controller`, `enhanced` or `custom`, for this run, skipping the first-start picker |
| `--edit-keypad` | opens the touch key pad editor ([keypad](../controls/keypad.md)) |

Only one of the export, import, check and list options can be given at a time. After every save the port copies the saved character to `backups/<slot>/<time>/` (`save_backups` in [All settings](#all-settings)). From an iPhone or iPod backup, run `python3 -m mtport.iosbackup import <backup> --program <MapleThief>` from `tools/`, or follow [Saves from your iPhone](../play/iphone-saves.md).

## Window and display

| Option | What it does |
|---|---|
| `--window WxH` | the window's size in points (960x640 by default) |
| `--safe-area T,L,B,R` | shows the layout of a phone's safe area (points) in a desktop window ([ADR 0055](../adr/0055-ios-safe-area-landscape-orientations-and-input.md)) |

## Test and debugging aids

These script a run, for tests and for reproducing a problem. Scripted and headless runs never show the first-start picker or the settings menu.

| Option | What it does |
|---|---|
| `--frames N` | quits after N frames and checks that the window showed the last one |
| `--seconds S` | quits after S seconds |
| `--fixed-step MS` | a fixed virtual step per frame, so the run is deterministic |
| `--capture DIR` | writes read-backs of the window as PNGs |
| `--capture-at N[,N...]` | captures only those frames |
| `--shot T,NAME` | a screenshot at T seconds (needs `--capture`) |
| `--shot-gl` | shots of the GL frame alone |
| `--tap T,X,Y` | clicks screen point (X, Y) at T seconds, through the same path as the mouse |
| `--hold T,X,Y,S` | presses screen point (X, Y) at T seconds for S seconds |
| `--swipe T,X1,Y1,X2,Y2` | drags from (X1, Y1) to (X2, Y2) in 0.3 s |
| `--minimise T,S`, `--background T,S`, `--unfocus T,S` | minimises, backgrounds or unfocuses the window at T for S seconds |
| `--quit T` | quits at T seconds (sends `applicationWillTerminate:` first, as closing the window does) |
| `--hold-fast-forward T,S` | holds the fast-forward key at T for S seconds |
| `--restore-backup T,N` | restores the Nth backup row at T seconds |
| `--slice FILE` | another armv7 slice |
| `--no-audio`, `--audio-capture FILE`, `--audio-test-tone` | no sound; write the mix to a file; the placeholder plays a test tone and checks it |
| `--perf-csv FILE` | a row per window frame ([frame telemetry](#frame-telemetry)) |
| `--dump-textures DIR` | writes every texture the game uploads ([texture packs](#texture-packs)) |

## Exit codes

| Code | Meaning |
|---|---|
| 0 | quit cleanly, and every requested check passed |
| 1 | SDL or the window could not start, or a tool refused (for example `--switch-data-location`) |
| 2 | a usage error |
| 3 | a check failed or an action failed: a read-back that didn't match, a failed `--import-ipa` or `--migrate-from` |
| 4 | no generated code, no bundle or no slice, or the load failed |
| 5 | the game itself trapped, stopped or exited non-zero |

## All settings

Each setting is a line `key = value` in `settings.ini`. Each also works for one run as a flag named after it, with dashes for underscores: `gapless_music` is `--gapless-music`. The file is plain text: blank lines and lines starting with `#` are comments, values go after one `=`, keys and mode names ignore case, and on/off also accepts `true`, `false`, `yes`, `no`, `1` and `0`. Unknown keys and bad values are logged and ignored. `MAPLETHIEF_SETTINGS="key = value; key = value"` applies settings from the environment, after the file and `--preset` and before the other flags.

**The Default column** is what a file without that line gives. **Presets** lists the value in Original / Original + controller / Enhanced for the keys a preset controls ([ADR 0070](../adr/0070-settings-registry-and-presets.md)). `-` means the key is yours in every preset. Choosing a preset other than `custom` sets those keys. `preset = original | original_controller | enhanced | custom` in the file, `--preset`, or the menu's preset line picks one; `custom` keeps your own values. Changing a controlled key moves a preset to `custom`.

| Setting | Default | Presets | What it does |
|---|---|---|---|
| `scale_mode` | integer (Switch: sharp) | integer / integer / integer (Switch: sharp in each) | Integer, fit, smooth, sharp |
| `border_color` | #000000 | - | The colour of the bars around the image, as `#RRGGBB` |
| `render_scale` | 1 | 1 / 1 / auto | The renderbuffers' internal scale, 1 to 8, or auto (the largest that fits the frame budget and the window) |
| `gfx_backend` | software | software / software / software | What draws the game's GL frames: software (the reference), gpu (SDL3 GPU) |
| `filter` | none | none / none / none | A shader filter over the scaled image: none, xbr, crt, lcd |
| `hd_text` | off | off / off / on | Draw the game's text textures at the render scale: on, off |
| `master_volume` | 100 | - | Volume of everything, 0 to 100 (percent) |
| `music_volume` | 100 | - | Volume of the music, 0 to 100 |
| `effects_volume` | 100 | - | Volume of the sound effects, 0 to 100 |
| `mute_unfocused` | on | on / on / on | Silence while the window is not focused: on, off |
| `gapless_music` | off | off / off / on | Loop the music without the silence at the start and end of its files: on, off (off: as the device) |
| `audio_mono` | off | - | Both channels the same mix, for one ear or one speaker: on, off |
| `audio_device` | default | - | The sound's output device, by its name, or default (the system's; from the menu's list). Not in the Switch's menu |
| `ios_audio` | original | - | iOS: the audio session: original (mixes with other apps' music, quiet with the Silent switch), own (plays with the switch on silent, other apps' music turned down) |
| `audio_pack` | (empty) | - | A directory of your own music and sound files that replace the game's, by name (python3 -m mtport.audiopack init lists them); empty: none. Switch: the menu offers the folders in `sdmc:/switch/maplethief/audio-packs/` |
| `right_touch` | off | - | The right mouse button: a touch at screen point X,Y, or off. Not in the Switch's menu |
| `hide_keypad` | auto | never / auto / auto | Hide the on-screen key pad: auto (while a gamepad or the keyboard is used), always, never |
| `rumble` | on | on / on / on | The gamepad rumbles on a vibration, a hit and a level-up: on, off |
| `rumble_strength` | 100 | - | How strongly, 0 to 100 (percent) |
| `keypad_haptics` | off (light on iOS) | - | IOS: a light tap of the phone at each press of a touch key pad key: off, light, medium |
| `perf_overlay` | off | off / off / off | The frame-time overlay (F3 shows or hides it while the game runs): on, off |
| `cash_shop` | off | off / off / off | Assist: the cash shop and the paid revives: off (as the original offline), meso (sold for the hero's meso, dearer with the level, the falls and the purchases) |
| `pacing` | display | display / display / display | Frame pacing: display (each game tick lasts whole display refreshes when the refresh rate allows), wall (the wall clock) |
| `save_dir` | (empty) | - | The folder for the game's save files, for example a cloud-synced one (an absolute path; empty: `saves/` in the data folder). Don't play the same slot on two machines at once. Not in the Switch's menu |
| `save_backups` | 10 | - | Backups kept of each character, taken after each save, 0 to 100 (0: none) |
| `fullscreen` | off | off / off / on (Switch: off in each) | The window: off, on (the whole display; F11 or Alt+Enter toggles it). `desktop` and `borderless` also mean on, in that mode. Not in the iPhone's or the Switch's menu |
| `fullscreen_mode` | desktop | desktop / desktop / borderless | How macOS shows fullscreen: desktop (its own Space, as the green button), borderless (over the desktop, no Space or animation); elsewhere the same. Not in the iPhone's or the Switch's menu |
| `fullscreen_display` | current | - | The display fullscreen goes to, by its name, or current (the window's; from the menu's list). Not in the iPhone's or the Switch's menu |
| `vsync` | on | on / on / on | Wait for the display's refresh to present a frame (no tearing): on, off |
| `pause_unfocused` | off | off / on / on | Pause the game while the window is not focused, as when it is minimised: on, off |
| `language` | en | - | The game's language: en, ko (MAPLETHIEF_LANGUAGE overrides it; from the next start) |
| `text_pack` | (empty) | - | A directory of translated text in UTF-8 (python3 -m mtport.textpack export makes one to edit); empty: none. Switch: the menu offers the folders in `sdmc:/switch/maplethief/text-packs/` |
| `fix_original_bugs` | off | off / off / on | Fixes the original game's crashes and dead ends ([original-bugs](../re/original-bugs.md)), never its balance: on, off |
| `texture_pack` | (empty) | - | A directory of HD textures made from your own `--dump-textures` (python3 -m mtport.texpack); empty: none. Switch: the menu offers the folders in `sdmc:/switch/maplethief/texture-packs/` |
| `texture_filter` | game | game / game / nearest | How textures are sampled at render scale 2 and up: game (smooth, the device's look), nearest (sharp) |
| `hide_online_ui` | off | off / on / on | Hides the title menu's buttons whose web pages are gone (Game Center too while `local_game_center` is off): on, off |
| `local_game_center` | off | off / off / on | Game Center on this computer: the leaderboard screen and local achievements: on, off |
| `menu_repeat` | off | off / on / on | A held direction repeats in the popup menu, NPC chat, the shop (its amount too) and the map (after 0.4 s, then every 0.15 s): on, off |
| `confirm_guard` | off | off / on / on | A popup that just opened ignores confirm and back for 0.3 s, and while a button pressed in the field is still held: on, off |
| `focus_highlight` | on | on / on / on | With a gamepad or the keyboard, a frame around the focused button or the chosen item where the game shows none: on, off |
| `button_prompts` | off | off / on / on | With a gamepad or the keyboard, its button next to each of the game's buttons (Xbox, PlayStation, Nintendo or key glyphs): on, off |
| `button_labels` | on | on / on / on | With a gamepad or the keyboard, its button beside each key of the field's key pad: on, off |
| `pad_pointer` | off | off / both / both | A gamepad pointer for what needs a finger: the right stick moves it (its click held, or the touchpad's click, taps), a finger on a touchpad places it: off, stick, touchpad, both |
| `keypad_layout` | original | - | The touch key pad: original, or for each group moved (dpad, actions, slots) GROUP=SIZE,DX,DY,OPACITY (percent, screen points, percent), ';' between groups. Not in the Switch's menu (its key pad editor row writes it) |
| `border` | color | color / color / blur | What fills the window around the picture: color (`border_color`), blur (the game, blurred), image (`border_image`) |
| `border_image` | (empty) | - | The PNG of border = image, scaled to cover the window; empty: none. Switch: the menu offers the PNGs in `sdmc:/switch/maplethief/borders/` |
| `game_speed` | game | game / game / game | Assist: the game's tick length: game (the game's own Speed option), 65, 50, 40, 33, 25 (ms; 33 and 25 are faster than any the game offers) |
| `fast_forward` | off | off / off / off | Assist: holding the fast-forward key (Tab, the right stick's click) runs the game: off, x2, x3 (times as fast) |
| `autosave` | off | off / off / on_quit | Save the game when it quits, goes to the background or exits to the main menu, where the game's own Save is allowed (not on boss maps, in a scene or with a popup open): off, on_quit |
| `widescreen` | off | off / off / auto | The field up to 16:9 (568 x 320) on maps wide enough; menus and narrow maps stay 3:2: off, auto |
| `widescreen_hud` | edges | edges / edges / edges | With widescreen, the touch key pad and its icons at the wide view's edges, or the HUD and the key pad kept at their 3:2 places in the middle: edges, narrow |
| `colorblind_filter` | off | - | The picture's colours corrected for colour blindness: off, protanopia, deuteranopia, tritanopia |
| `screen_shake` | 100 | - | How far the scenes shake the camera, 0 to 100 (percent of the game's) |
| `flashes` | 100 | - | How strong the game's full-screen flashes are (a level-up's white and black frames, the scenes' white-outs), 0 to 100 (percent of the game's) |
| `brightness` | 100 | - | The picture's brightness, 50 to 150 (percent; 100: as the game drew it) |
| `contrast` | 100 | - | The picture's contrast about mid-grey, 50 to 150 (percent; 100: the game's) |
| `saturation` | 100 | - | How colourful the picture is, 50 to 150 (percent; 100: the game's colours) |
| `gamma` | 1.00 | - | The mid-tones' gamma, 0.80 to 1.40 (above 1.00 lighter, below darker; black and white stay) |
| `frame_rate` | original | original / original / original | How often the window shows the game: original (each game tick's frame, 20 a second), smooth (every refresh, interpolated between the last two ticks: smoother, but about a tick later) |
| `ui_animations` | normal | normal / normal / normal | How fast the menus' UIKit animations run: normal, fast, instant |
| `turbo_attack` | off | off / off / off | Assist: holding attack in the field attacks again and again: on, off |
| `auto_potion` | off | off / off / off | Assist: drink the HP potion of a quick slot under this share of max HP: off, 25, 50, 75 (percent) |
| `skip_intros` | off | off / off / off | Scenes (the intro, chapter cards, boss entrances) run fast, each speech box tapped: on, off |
| `minimap` | off | off / off / off | The port's minimap of the map in the field (platforms, ladders, the hero, portals and NPCs): off, border (beside the picture where there is room, else over it), overlay |
| `exp_percent` | off | off / off / on | The hero's EXP to the next level as a percentage beside the HUD's bar: on, off |
| `auto_run` | off | off / off / off | Assist: the auto-run key (R, the left stick's click) keeps the hero walking: on, off |
| `hold_toggle` | off | off / off / off | Assist: a press starts these held actions and the next one stops them: off, walk, fast_forward, all |
| `auto_loot` | off | off / off / off | Assist: the hero picks up drops near it without walking over them: on, off |
| `save_states` | off | off / off / off | Assist: quick-save (F5) and quick-load (F9) a state of the game in the field, one per character, beside the game's own saves: on, off |
| `pause_on_disconnect` | off | off / on / on | Pause when the gamepad in use disconnects, until a pad connects or a key: on, off |

## Features in more detail

The settings above in context, with the decision behind each.

### Keyboard, mouse and gamepads

Every key, button and touch is listed in [Controls](../play/controls.md), and the keyboard's layout and reasons in [keyboard](../controls/keyboard.md). The left mouse button is your finger; on a touch screen every finger is a touch ([ADR 0047](../adr/0047-touch-input-mouse-and-fingers.md)). `right_touch` makes the right mouse button a second finger held at a fixed point: `right_touch = 440,290` puts it on the attack key. Gamepads use SDL3; the first pad you press is the one that plays ([ADR 0048](../adr/0048-gamepad-actions-and-delivery.md)). The on-screen key pad ([ADR 0052](../adr/0052-hiding-the-on-screen-key-pad.md)) fades while you play with a pad or the keyboard and returns when you touch the screen.

**Remapping** ([remapping](../controls/remapping.md), [ADR 0054](../adr/0054-bindings-file-remap-overlay-and-button-glyphs.md)): the settings menu's Controls → Bindings row opens the remap overlay. The bindings live in `bindings.ini` in the data folder, a text file you can edit too; a bad line is logged with its line number and skipped. `nintendo_buttons = label` in that file puts a Nintendo pad's A on jump instead of the bottom button.

### Window, scaling and fullscreen

F11 or Alt+Enter switches fullscreen; `fullscreen_mode` picks how macOS does it, from the next start, and elsewhere both modes are the same borderless window ([ADR 0099](../adr/0099-fullscreen-modes-and-the-first-start-crash.md)). The window's size and position are kept in `window.ini`. F12 (or a gamepad's Share button, or R3 + Back) saves a screenshot to `screenshots/` in the data folder ([ADR 0077](../adr/0077-desktop-conveniences.md)). On a HiDPI display the window is drawn at its full pixel resolution.

The picture is 480 × 320 points, scaled by `scale_mode` ([ADR 0043](../adr/0043-scaling-modes-mouse-mapping-and-settings-file.md)). `border`, `border_color` and `border_image` fill the bars around it ([ADR 0085](../adr/0085-borders-around-the-picture.md)). `render_scale = auto` picks the largest scale that keeps a frame within its budget; a fixed one that's too slow says so once ([ADR 0080](../adr/0080-render-scale-auto-and-frame-budget-guard.md)).

### GPU rendering

`gfx_backend = gpu` draws the game's GL frames through SDL3 GPU (Metal on macOS and iOS, D3D12 or Vulkan on Windows, Vulkan on Linux, deko3d on the Nintendo Switch, [ADR 0143](../adr/0143-gpu-backend-on-deko3d.md)) instead of the software rasterizer, which stays the default and the reference ([ADR 0060](../adr/0060-gpu-rendering-backend.md), [ADR 0068](../adr/0068-gpu-presentation-without-read-back.md)). It is what makes high render scales cheap. On the Switch it, a shader filter, the colour-blind filter and the picture adjustments take effect at the next start when they need SDL's GPU renderer and the window was made without it. The log says what runs, and without a usable GPU the game says so and draws in software. `MAPLETHIEF_GPU_DRIVER` picks a driver, `MAPLETHIEF_GFX_GPU_TIMING=1` (or `--gfx-gpu-timing`, for the Switch, whose netloader passes no environment) logs the GPU time per frame at exit, and `MAPLETHIEF_GPU_READBACK=1` brings back the read-back path for comparisons. The shaders are described in [Build from source](README.md#shaders).

### Texture packs

Make HD textures from your own copy in three steps ([ADR 0081](../adr/0081-texture-packs-and-nearest-texture-filter.md)):

1. `MapleThief --dump-textures DIR` writes every texture the game uploads while you play as `DIR/<hash>_<W>x<H>.png` (straight alpha) and the menus' images under `DIR/ui/`.
2. `python3 -m mtport.texpack build DIR PACK --scale 2`, run from `tools/`, writes them upscaled (`--scaler scale` is Scale2x/Scale3x, `nearest` plain blocks; any factor 1 to 8). You can also edit a dumped PNG by hand and keep its name: a file must be the texture's size times a whole k from 1 to 8, or it's logged once and the game's texture is kept.
3. `texture_pack = PACK` in `settings.ini` plays with them. The exit log counts what the pack replaced.

`texture_filter = nearest` keeps sprites sharp at render scales above 1. Dumps and packs are made from the game's art: keep them to your own machines. Audio packs and text packs have their own sections in [Settings](../play/settings.md#audio-packs).

### Sound

The music files end in seconds of silence, so the music pauses at each loop as it did on the iPhone. `gapless_music = on` skips that silence ([ADR 0062](../adr/0062-optional-gapless-music-loops.md)). The volume and mute settings have flags, and `--save-settings` writes them to the file. See also [ADR 0043's addendum](../adr/0043-scaling-modes-mouse-mapping-and-settings-file.md#addendum-the-audio-keys-and-writing-the-file-64-2026-10-29).

### The settings menu and first start

F1, the gamepad's Guide button, both stick clicks, or the gear button open the settings menu on any screen and pause the game, with a tab per group and the preset on top ([ADR 0076](../adr/0076-settings-menu.md)). Rows marked `*` apply at the next start, and changes are written to `settings.ini` when the menu closes. Scripted runs never open it.

On the first start, before the game, a screen asks how you want to play: Original, Original + controller or Enhanced ([ADR 0070](../adr/0070-settings-registry-and-presets.md)). The choice is saved as `preset`. Anything that changes gameplay (game speed, the cash shop, auto-potion) is an Assist and is never turned on by a preset.

### Language

`language = ko` (or `--language ko`, or `MAPLETHIEF_LANGUAGE=ko`, which overrides both) plays the Korean build, with its terms screen, from the next start ([ADR 0049](../adr/0049-menus-online-buttons-language-and-menu-flows.md)).

### Lifecycle

Minimising the window pauses the game as leaving it did on the iPhone: nothing is drawn and the game waits until the window is restored. Closing it sends `applicationWillTerminate:` first ([ADR 0063](../adr/0063-app-lifecycle-background-and-minimise.md)). `pause_unfocused = on` also pauses it while its window isn't focused.

### The cash shop and offline polish

Offline, the cash shop and the paid revives sell nothing, as on a phone with purchases switched off. The Assist `cash_shop = meso` sells the items and revives for the hero's meso instead, at a price that follows the hero's level and rises with each fall and purchase until the next level-up ([ADR 0072](../adr/0072-offline-cash-shop-and-revives.md)).

`hide_online_ui` hides the title menu's buttons whose web pages are gone. `local_game_center` turns Game Center's button into a leaderboard of your save slots on this computer and unlocks a few achievements of the port's own, kept in `gamecenter.json` in `saves/` ([ADR 0084](../adr/0084-offline-polish-hidden-online-ui-local-game-center.md)).

### Frame telemetry

Every game run logs at exit its tick rate and tick intervals (p50, p99, max, the count over 55 ms), where a window frame's time went, and how many vblanks frames stayed on screen ([ADR 0071](../adr/0071-frame-telemetry-and-frame-time-overlay.md)). `--perf-csv FILE` writes a row per window frame. F3 (or `perf_overlay = on`) shows the last second's numbers over the game; it is never drawn in `--fixed-step`, `--shot` or `--capture` runs. For timings use a Release build, because a Debug build is much slower.

## Environment variables

| Variable | What it does |
|---|---|
| `MAPLETHIEF_DATA_DIR` | the data folder, beating every other rule ([The data folder](#the-data-folder)) |
| `MAPLETHIEF_BUNDLE_DIR` | the game's bundle ([Finding the game](#finding-the-game)) |
| `MAPLETHIEF_SETTINGS` | settings applied after the file and `--preset` ([All settings](#all-settings)) |
| `MAPLETHIEF_LANGUAGE` | `en` or `ko`, overriding the setting |
| `MAPLETHIEF_GPU_DRIVER` | the SDL3 GPU driver, for example `vulkan` |
| `MAPLETHIEF_GFX_GPU_TIMING=1` | logs the GPU time per frame at exit |
| `MAPLETHIEF_GPU_READBACK=1` | the read-back path of the GPU presentation |
| `MAPLETHIEF_MAIN_THREAD_PRIORITY=normal` | keeps the main thread's default priority ([ADR 0120](../adr/0120-main-thread-priority-in-real-time-runs.md)) |
| `MAPLETHIEF_TRACE=CHANNELS` | trace channels, comma-separated, e.g. `boot,objc` |
| `MAPLETHIEF_TRACE_HLE=FILTERS` | traces HLE calls by import name ([ADR 0030](../adr/0030-runtime-diagnostics.md)) |
| `MAPLETHIEF_GL_TRACE_FILE=PATH` | writes the GL trace to a file |
| `MAPLETHIEF_COVERAGE=PATH` | function-entry coverage (builds with `MT_TRACE_ENTRY`) |

The build's own variables (`MAPLETHIEF_VERSION`, `PYTHON`, and the rest) are on [Build from source](README.md).
