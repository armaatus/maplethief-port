# 77. Desktop conveniences: fullscreen, the window remembered, the cursor, the wheel, screenshots, pause when unfocused, vsync and the language as settings

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #145 (epic #105)
- **Builds on:** [ADR 0042](0042-frontend-game-loop-and-presentation.md) (the frontend loop, vsync), [ADR 0043](0043-scaling-modes-mouse-mapping-and-settings-file.md) (the placement and the mouse mapping), [ADR 0063](0063-app-lifecycle-background-and-minimise.md) (the lifecycle, the minimise), [ADR 0070](0070-settings-registry-and-presets.md) (the registry and presets), [ADR 0049](0049-menus-online-buttons-language-and-menu-flows.md) (`MAPLETHIEF_LANGUAGE`), [ADR 0051](0051-controller-focus-navigation.md) (the focus on UIKit screens)

## Context

The port's window had none of the standard desktop behaviour: fullscreen existed only on iOS (`main.cpp`'s `SDL_WINDOW_FULLSCREEN`), vsync was always on with no setting, the language was an environment variable (`MAPLETHIEF_LANGUAGE`), the window opened at 960 × 640 in the middle every time, screenshots needed `--shot`, and the cursor stayed over the image while a pad played. #145 asks for each of these with a setting where it is one, applied at once where possible (#89's menu calls the same path), and with today's behaviour when there is no settings file.

What had to be decided:

- Which conveniences are settings rows, and their preset columns.
- What "fullscreen" is, and how the hotkeys reach the setting.
- Where the window's size and position live.
- What the screenshot shows and where it goes.
- How a focus loss pauses the game.
- What vsync off means for the loop's pacing, while #80 changes the pacing.

## Options considered

**Fullscreen.**

1. **Exclusive fullscreen** (a display mode switch). Con: a mode switch on every toggle, black flashes, and the 480 × 320 image has no mode of its own; the scale modes already fill any size.
2. **Desktop fullscreen** (`SDL_SetWindowFullscreen` with no mode: borderless over the display at its own mode; on macOS a full-screen Space). The placement follows the window's new size as on any resize.

**The window's size and position.**

1. **Settings rows** (`window_x`, `window_width`, …). Con: they change every run, so `settings.ini` would be rewritten each quit; #89's menu would list them; the presets would need `kNotInPresets` rows nobody edits.
2. **A state file of its own** (`<data dir>/window.ini`), written when the game ends and read before the window opens.

**The screenshot.**

1. **The window read back** (the scale, the bars, a filter, the focus frame, the frame-time overlay).
2. **The composed screen as `--shot` writes it**: the game's image at the render scale, without the port's window-only overlays (the focus frame, the frame-time overlay, `render_scale`'s notice from #152, the cash shop's price line from #156). It is what the device showed, the same at any window size.

**Pausing when unfocused.**

1. **A new pause of the frontend's own** (stop calling the guest). Con: the game would not hear it, its music would play on.
2. **Through the lifecycle (ADR 0063), as a minimise:** the delegate resigns (`Clet_Pause` stops the timer and the music), the guest waits in the turn hook, and focus back makes it active again.

## Decision

### The settings (registry rows, ADR 0070)

| Group | Key | Original | Original + controller | Enhanced | Default | Applies |
|---|---|---|---|---|---|---|
| Video | `fullscreen = off \| desktop` | off | off | desktop | off | at once |
| Video | `vsync = on \| off` | on | on | on | on | at once |
| Gameplay | `pause_unfocused = on \| off` | off | on | on | off | at once |
| Other | `language = en \| ko` | – | – | – | en | next start |

Each has its flag (`--fullscreen`, `--vsync`, `--pause-unfocused`, `--language`, game options). The defaults are today's behaviour, so a run without a settings file is unchanged (the device goldens pass byte for byte). With `pause_unfocused` off by default and on in Original + controller, the defaults no longer equal that column: `matching_preset(Settings{})` is now none, and the test `presets: no settings file is today's defaults` pins the differences (Original: `hide_keypad`; Original + controller: `pause_unfocused`).

### The desktop (`platform/src/desktop.{h,cpp}`)

- **Fullscreen** is desktop fullscreen (option 2). F11, Alt+Enter and Cmd+Ctrl+F toggle it (F11 alone is macOS's Show Desktop; Cmd+Ctrl+F is macOS's own shortcut). A toggle is a change of the setting: `apply_setting` (a preset that set the other value becomes `custom`, ADR 0070), applied at once, written to `settings.ini`. macOS's green button and the system's shortcut change the window too; `SDL_EVENT_WINDOW_ENTER_FULLSCREEN` / `_LEAVE_FULLSCREEN` carry that into the setting the same way. With `fullscreen = desktop` the window is created fullscreen. iOS is always fullscreen and ignores it. The mouse keeps mapping through the placement of each frame (ADR 0043), which follows the window's size. The toggle's resize reaches `render_scale = auto` the same way: the next window frame gives the governor the new placement (`set_cap`, ADR 0080), which re-checks the scale at once.
- **The window** (option 2): `window.ini` has `x`, `y`, `width`, `height` (points) and `maximized`. It is the windowed rectangle only: `Desktop` records it on moves and resizes while the window is neither fullscreen, maximised nor minimised (nor exactly the display's size: macOS's green button can resize before the window reports fullscreen), and keeps the file's while the window starts fullscreen. It is read and written only in an interactive run of the game (`is_interactive_run`: the first-launch picker's rule, no scripted option, not the dummy or offscreen driver) without `--window`, never on iOS. The position is used only if the middle of the window's top edge lies in a display's usable bounds; otherwise the window opens centred at the saved size.
- **The cursor** hides on a key press, a pad button or a stick or trigger past half, and shows on a real mouse motion, click or wheel. A touch's or a pen's mouse events don't count.
- **The wheel**: in a menu (every key pad panel but the field's, `Context::kMenu`), one notch up or down is the d-pad's Up or Down through `Actions`, so the popup menu's lists, the shops and the UIKit screens' focus (ADR 0051) move one row; at most three a wheel event; a trackpad's fractions add up to whole notches. On the field the wheel does nothing (Up there climbs).
- **Screenshots** (option 2): F12, a pad's Share / Capture button (SDL's `MISC1`), or Back while the right stick is held in (R3 is unbound; pressed first it makes Back the port's, not the map). The screen is composed in the next turn hook and written to `<data dir>/screenshots/MapleThief_YYYY-MM-DD_HH-MM-SS-mmm.png` (local time, `_2`, `_3` … if taken). Never into `--capture`'s directory, so no scripted output changes.
- The hotkeys and screenshot buttons are taken before the remap overlay and the input handlers, with their repeats and releases, so the game never sees them.
- **Applied at once:** `Desktop::apply(settings)` sets fullscreen and vsync. `game.cpp`'s `apply_settings_at_once(Game&, const Settings&)` calls it, sets the lifecycle's `pause_unfocused` and keeps the frame telemetry's vsync. Since #89 (ADR 0076) both it and the settings menu do this through `apply_live_setting` (`live_settings.cpp`), the one path for every `Applies::kAtOnce` row, and a hotkey's change is written like the menu's: replayed over `settings.ini` (`save_menu_changes`).

### pause_unfocused (option 2)

`Lifecycle` takes `SDL_EVENT_WINDOW_FOCUS_LOST` and `_FOCUS_GAINED` (desktop mapping only; on iOS the app events already say it). It always tracks the focus; with the setting on, an unfocused window counts like a minimised one: one `applicationWillResignActive:` (cause `SDL_EVENT_WINDOW_FOCUS_LOST`), hidden (nothing presented, the guest waits), one `applicationDidBecomeActive:` when the focus comes back. A minimise while unfocused sends nothing more, and a restore without the focus does not resume. The window is still on screen: `paused_visible()` makes `wait_hidden` redraw the last frame (the Presenter's texture, no guest call) on a resize or an expose. Switching the setting while unfocused pauses or resumes at once. The fixed-step mode ignores the window's focus (logged and counted) as it ignores a real minimise. The test aid `--unfocus T,S` scripts a focus loss at T and its return S seconds later, through the same path.

### vsync and the pacing (#80)

`vsync = on` is `SDL_SetRenderVSync(renderer, 1)`, as before; `off` is `0`, and `SDL_RenderPresent` returns at once. The run loop already waits for the guest's own timer (`on_turn`'s wait until `next`), so with vsync off it is paced by the wall clock. #80's display-locked pacing needs vsync, and `pacing = display` with `vsync = off` falls back to the wall clock (ADR 0075): `run_game` starts with the Desktop's answer as `Game::vsync`, and after each `Desktop::apply` (the start's and every live change through `apply_settings_at_once`) `follow_vsync` (`desktop.h`) copies `Desktop::vsync()` into `Game::vsync` and the frame telemetry. The turn hook re-plans the paced clock from it every turn, so the next turn after vsync goes off is on the wall clock with the reason `vsync off`, and vsync on again locks it. A renderer that refuses vsync reports it off, as before (SDL's dummy driver takes it). Test: `desktop: vsync at the start and toggled live re-plans the paced clock (#80): vsync off is the wall clock`.

### The language

`language` sets `mt_cf_config.language` before the boot. `MAPLETHIEF_LANGUAGE` still overrides it when set (scripts and tests keep working). The log names the source: `game: language ko (the language setting)` or `(MAPLETHIEF_LANGUAGE)`. It applies from the next start: the bundle's localisation is fixed once the game boots.

## Evidence

- **CI (synthetic, `platform_test`):**
  - `desktop: the settings rows persist, take their flags and apply at once (#145)`: each key changed, written (new file and over an old one), read back; its flag; bad values in the file's wording.
  - `presets: the registry matches #89's table; each preset sets exactly its column` with the four rows in `kExpected`; `presets: no settings file is today's defaults` (the differences above); `desktop: no settings file keeps today's window`.
  - `desktop: the fullscreen hotkeys and the screenshot key`, `… the cursor hides on keys and pads and shows on the mouse`, `… the wheel's notches`, `… window.ini: parse, write, the display check, load and save`, `… the screenshot's name and file` (written, loaded back pixel for pixel, never overwritten), `… --unfocus T,S`, `… window.ini and the picker only in a player's run`.
  - `desktop: on a window: …` (dummy driver): vsync off and on at the renderer, fullscreen on and off at once with the windowed size kept, the enter event reported, the hotkeys taken with repeats and releases (Enter alone is the game's), Share and its release, the cursor hidden and shown, and a click at the window point that shows (120, 80) touches (120, 80) windowed, fullscreen at 1920 × 1080 and windowed again.
  - `lifecycle: pause_unfocused: …`: on (one resign and one become-active, a minimise and restore inside it send nothing), off (focus changes nothing), switched on and off while unfocused, the iOS mapping, the fixed-step mode, a quit while paused.
  - `platform.desktop_usage`: 10 invalid command lines exit 2.
- **Local (real assets, `platform.game_desktop`, Debug, dummy drivers):**
  - `--fixed-step 16.667 --pause-unfocused on --unfocus 6,4 --quit 12`: `applicationWillResignActive:` at 6.000 s (FOCUS_LOST), the guest's `phone suspend`, hidden at window frame 173 and shown again at 10.000 s at the same frames, `phone resume`, `applicationDidBecomeActive:`, the quit: 3 messages.
  - The same without the setting: only the quit's message, never hidden.
  - `--language ko` and `MAPLETHIEF_LANGUAGE=ko` write the same shot at 5 s (`87c0eceb…`), en another (`6ad9f971…`); `MAPLETHIEF_LANGUAGE=en` overrides `--language ko`.
  - `--fullscreen desktop --vsync off`: the start log has `fullscreen, vsync off` and the telemetry `vsync off`; the default run has neither and no `desktop:` line. No scripted run wrote `window.ini` or a screenshot.
  - `platform.device_golden` against the recorded renders: 7 of 7 byte-identical, so a run without a settings file is unchanged.
- **Local (real assets, a Cocoa window on the Mac, Metal):** a run with `preset = custom` quit by SIGTERM wrote `window.ini` (`x = 480, y = 235, 960 × 640`); with `x = 100, y = 200, 800 × 500` the next run opened there (`window 800x500`) and wrote the same back; with `x = 50000` it logged `the saved position is on no display` and opened centred at 800 × 500.
- **By hand (Joris, `needs-human`):** the fullscreen toggles, the cursor and the wheel on the Mac, Windows and Linux.

## Consequences

- #89's menu lists the four rows; three apply at once through `apply_live_setting` (ADR 0076), the language says "from the next start".
- A fullscreen toggle under a preset that set the other value makes the preset `custom`, as any change through the port does (ADR 0070). If players find that surprising, the hotkey could become a session-only toggle that is written only when no preset controls the key.
- `matching_preset(Settings{})` is none since #145; the first-launch picker is unaffected (it shows for "no preset", not for a match).
- **How we'd know this was wrong:**
  - Some window manager sends a focus loss while toggling fullscreen, and `pause_unfocused` stutters: ignore focus changes within a short time of a fullscreen change.
  - Players want the window's screenshot (with the filter): add `screenshot = screen | window`.
  - `window.ini` restores onto a display arrangement where the title bar is unreachable: test the whole title bar, not its middle.

## Addendum: window size keys and the fullscreen display (#196, 2026-11-05)

[#196](https://github.com/armaatus/maplethief-port/issues/196) asks for two desktop comforts from emulators and PC ports. One key sizes the window to an exact integer scale: with `scale_mode = integer` (ADR 0043) a window a few points short drops a whole step. Fullscreen goes on the display the player picks. It builds on #179 (ADR 0099): one fullscreen request per change, never `SDL_SetWindowFullscreenMode`.

**The size keys.**

- **The keys.** Ctrl+1 … Ctrl+5 (Cmd+1 … Cmd+5 on macOS) set the window to exactly N × the picture. The picture is 480 × 320 points, or 568 × 320 with `widescreen = auto` (ADR 0090; the start's value, since widescreen applies from the next start). `Desktop::handle` takes them before the bindings, like F11, with their repeats and releases (`Request::window_scale`). `game.cpp`'s `size_window` turns `fullscreen` off through the setting first: one request, written to `settings.ini` as F11's is. Then `Desktop::size_window` restores a maximised window, reads the window's display's usable bounds, its frame (`SDL_GetWindowBordersSize`, kept from the last windowed moment, since a fullscreen window has none) and the minimum window, and calls `SDL_SetWindowSize` and `SDL_SetWindowPosition`. On macOS SDL defers both while a fullscreen transition runs (`pending_size`, `pending_position`), so leaving fullscreen and sizing in one press is safe.
- **The rectangle** (`window_rect_for_scale`, pure). The largest scale from N down to 1 whose window, frame included, fits the usable bounds. Never smaller than the minimum window (ADR 0103: 480 × 320). Centred in the usable bounds, with the frame's top left corner kept on the display, so the title bar stays reachable even on a display smaller than 1x. A clamped scale is logged ("window 5x does not fit the display (1366x728 usable): 2x").
- **Bindable.** `bindings.ini` gains a `[window]` section, `size_1x … size_5x = key Ctrl+3, …` or `none` ([remapping.md](../controls/remapping.md#the-windows-size-keys)). Keys are chords (`KeyChord`: an SDL keycode plus Ctrl, Alt, Shift, Gui), matched with exactly those modifiers, either side, Num and Caps Lock ignored. The binding table has no modifiers (ADR 0054), and a size key is not a game action in a context, so the keys live beside the table in `BindingSet::window` instead of becoming `MT_ACTION_*` port actions. The remap overlay doesn't list them, but it opens on the Desktop's keys (`Remap::set_window_keys`), its Save writes them, and its Reset restores the defaults. A chord without a modifier that the table also binds is warned about; the window takes it.
- **No collision with the defaults.** No default binding is a chord. The field's 1–4 (quick slots) have no modifier, and keys held with Cmd or the Windows key are left to the system anyway (keyboard.md). On Windows and Linux, Ctrl alone is the field's attack, so Ctrl+3 in the field attacks once on the Ctrl press. A player who minds rebinds `[window]` (or attack).

**`fullscreen_display = current | NAME`** (Video, `--fullscreen-display`, not in the presets, applies at once).

- **The display.** `current` (empty in `Settings`) is the window's display, as before. A name is `SDL_GetDisplayName`'s, matched exactly, then ASCII case-insensitively (`pick_display`, as `audio_device`'s `pick_audio_device`). Two displays of one model share a name; the first is taken.
- **Going fullscreen.** When `Desktop::apply` turns fullscreen on and the named display is connected and not already the window's, it first calls `SDL_SetWindowPosition(SDL_WINDOWPOS_CENTERED_DISPLAY(id))`. SDL keeps that as the window's pending display, and `SDL_SetWindowFullscreen` takes it: SDL's documented "set position, then fullscreen" pattern. It is still one fullscreen request per change, and nothing syncs the window between the two calls (ADR 0099's crash came from a sync pumping a queued transition). At the start, a window created fullscreen is created on that display (`SDL_WINDOWPOS_CENTERED_DISPLAY` at creation in `main.cpp`), so no request is made at all. Leaving fullscreen, the window comes back on that display, centred, at its windowed size. `window.ini` keeps the windowed rectangle from before.
- **Not connected.** Fullscreen uses the window's display, with one log line, and the setting is kept, so the file still names the display for when it's back (`audio_device`'s rule).
- **A change while fullscreen** counts from the next time fullscreen turns on. Moving a fullscreen window means leaving fullscreen and entering it again: two transitions back to back, the pattern ADR 0099 removed. `apply_live_setting` takes the row, so the menu shows no "next start" mark, and its help says "from the next time it turns on".
- **The menu's list.** The menu offers `current` and then SDL's display names (`set_display_choices`, through the registry row's `offer`, ADR 0045's addendum). `Desktop` refreshes them at `attach` and on `SDL_EVENT_DISPLAY_ADDED` / `_REMOVED`.

**Evidence.**

- `desktop_test.cpp` (CI, synthetic):
  - `desktop: the size keys' window (#196): N times the picture, with and without widescreen, clamped on a small display`: N = 1 … 5 at 480 and 568 on a 4K display, centred with the title bar; a 1366 × 768 laptop with a Windows frame (3x, 4x and 5x give 2x, 960 × 640 or 1136 × 640); a display left of the main one; a display smaller than 1x (the minimum window, top left on the display); a minimum above the picture.
  - `desktop: fullscreen_display picks a display by its name, else the window's (#196)`: a fake list (exact, case-insensitive, exact first, missing → -1); the row's parse, text, file round trip, flag and menu choices.
  - `desktop: on a window: a size key sizes it, and fullscreen goes to the named display or the window's (#196)` (dummy driver): the default chord taken with its repeat and release; 3 alone and with Shift not taken; rebound keys; the window sized to 1x and to 5x clamped to the dummy's 1024 × 768 (2x), recorded for `window.ini`; fullscreen with a missing and with the named display, one request each; a size from fullscreen.
- `bindings_test.cpp`: `bindings: the window's size keys: chords, the [window] section, the round trip (#196)`.
- `remap_test.cpp`: `remap: Save keeps bindings.ini's [window] size keys; Reset restores them (#196)`.
- `presets_test.cpp`: the new row is outside the presets.
- **By hand (Joris, `needs-human`):** on a two-display Mac or PC, fullscreen on the second display and Ctrl+3 (or Cmd+3).
