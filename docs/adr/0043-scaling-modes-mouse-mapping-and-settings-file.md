# 43. Scaling: integer by default, three fit modes, the mouse inverse, and the first settings file

- **Status:** Accepted
- **Date:** 2026-10-29
- **Issue:** #59
- **Builds on:** [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (the 480 × 320-point landscape screen, `mt_uikit_touch` in screen points), [ADR 0039](0039-rendering-backend-software-rasterizer.md) (render scale, the SDL streaming texture), [ADR 0042](0042-frontend-game-loop-and-presentation.md) (the `present_rect()` seam, the data directory)

## Context

After #58 the window shows the game's screen image, 480 × 320 × render scale texels, as a plain aspect fit with black bars and nearest filtering. The bundle has no Retina art (only icon `@2x` files), so 480 × 320 is the native resolution. A window is any size, on displays of any pixel density.

What has to be decided:

- **Which scale modes**, and which one is the default.
- **How integer scaling relates to the render scale.** The image is N times bigger at render scale N: is "integer" a whole multiple of the image's texels or of the 480 × 320 screen points?
- **HiDPI.** SDL reports window sizes and mouse positions in points; the drawable has pixel density times as many pixels.
- **Mouse and touch input (M7, #65)** needs the inverse of the placement: a window point to a game screen point, and whether it fell on the image or the bars.
- **Where the choice lives.** The frontend had no config file; the M10 settings menu (#89) will need one to write.

## Options considered

**The modes.**

1. **Integer nearest only.** Pixel-exact, but a 1080p display shows a 1440 × 960 image (3×) with wide bars, and a window smaller than 1× shows nothing useful.
2. **Integer nearest (default), fit nearest, fit smooth (bilinear), fit sharp (sharp bilinear).**
   - Integer gives crisp, even pixels. Fit nearest fills more of the window but its texels are uneven (3 and 4 pixels wide at 3.375×). Smooth hides that unevenness by blurring. Sharp keeps texels crisp and blends only across texel edges, so they look even without the blur.
   - The pinned SDL 3.4.18 has `SDL_SCALEMODE_PIXELART`, a sharp-bilinear filter in the GPU renderers' shaders (Metal, D3D11/12, Vulkan, GL). Sharp costs nothing to add.

**Integer scaling and the render scale.**

1. **A whole multiple of the image's texels** (k × 480N). Each texel is k pixels at every render scale, but the image shrinks when N grows: at N = 2 a 1080p window gets 960 × 640, not 1440 × 960.
2. **A whole multiple k of the 480 × 320 screen points, whatever N.** The image is 480k × 320k pixels, so the window's layout doesn't change with the render scale. Each texel is k / N pixels, whole when N divides k and always at N = 1, the default.

**HiDPI.**

1. **SDL's logical presentation** (`SDL_SetRenderLogicalPresentation`), which has letterbox and integer modes of its own. It knows nothing of a fit-with-sharp mode or of our border colour, and #58 already left it.
2. **Our own placement in drawable pixels** (`SDL_GetCurrentRenderOutputSize`), with the window's pixel density for input.

**The settings file.**

1. **A real INI or TOML library.** That's a new dependency ([ADR 0003](0003-dependencies.md)) for three keys.
2. **JSON**, which the runtime's stubs already read (`runtime/src/stubs/json.h`). It is awkward to edit by hand, and the stubs' reader is runtime-internal.
3. **A minimal, hand-written `key = value` format** in `settings.ini`, with its rules written down exactly.

## Decision

- **Four modes** (`scale_mode`, `--scale-mode`), in `platform/src/scaling.{h,cpp}` (pure arithmetic, no SDL):

  | Mode | Rectangle | Filter |
  |---|---|---|
  | `integer` (default) | 480k × 320k with the largest whole k that fits, centred | nearest |
  | `fit` | the largest 3:2 rectangle, size rounded to whole pixels, centred | nearest |
  | `smooth` | the fit rectangle | bilinear, `SDL_SCALEMODE_LINEAR` |
  | `sharp` | the fit rectangle | `SDL_SCALEMODE_PIXELART` |

  - Integer mode at a drawable smaller than 480 × 320 falls back to the fit rectangle (nearest; `Placement::integer_scale` 0).
  - With odd bars, the extra pixel goes to the right and bottom bar.
  - SDL's software renderer draws `SDL_SCALEMODE_PIXELART` as nearest. The GPU renderers blend across texel edges.
- **Integer k scales the screen points (option 2):** the image is 480k × 320k pixels whatever the render scale N, and each texel is k / N pixels. A user who raises N for smoother polygons keeps the window's layout. Exact texels are N = 1 (the default) or an N that divides k.
- **Bars in a border colour** (`border_color = #RRGGBB`, `--border-color`, default black). `Presenter::draw()` clears the drawable with it. The game's own black letterbox inside its 480 × 320 screen (on maps) is part of the image, not of the bars.
- **HiDPI (option 2):** the placement is always computed in drawable pixels (`SDL_GetCurrentRenderOutputSize`). The window keeps `SDL_WINDOW_HIGH_PIXEL_DENSITY`. A 1440 × 900-point Retina window is a 2880 × 1800 drawable and gets integer 5×; the same window at 1× gets 2×.
- **The mouse inverse, for #65:**

  ```cpp
  // scaling.h, pure: window points -> drawable pixels (x density) -> screen points.
  ScreenPoint window_to_screen(const Placement &p, float wx, float wy, float density_x, float density_y);
  void screen_to_window(const Placement &p, float sx, float sy, float density_x, float density_y, float *wx, float *wy);
  ScreenPoint drawable_to_screen(const Placement &p, float px, float py);
  // present.h, with the SDL window's density (pixel size / size per axis):
  ScreenPoint window_point_to_screen(SDL_Window *window, const Placement &placement, float x, float y);
  ```

  - `ScreenPoint{x, y, inside}` is in the game's 480 × 320 points, as `mt_uikit_touch` takes them. `inside` is false over the bars; x and y are then extrapolated (below 0 or past 480 × 320), so #65 can clamp a drag that leaves the image or drop a click in the bars.
  - The placement to pass is `Presenter::placement()`, the one the last frame was drawn with.
  - `--tap` now goes screen point → window point → `window_point_to_screen` → `mt_uikit_touch`, so every scripted run exercises the mapping in the mode it runs in.
- **`settings.ini` (option 3)**, in the data directory (`MAPLETHIEF_DATA_DIR`, else `SDL_GetPrefPath("", "MapleThief")`), parsed by `platform/src/settings.{h,cpp}`. The format in full:
  - UTF-8; a leading byte order mark is skipped. Lines end in LF or CR LF.
  - Blank lines and lines whose first non-blank character is `#` are comments. There are no comments after a value (`#` starts a colour), no sections, no quoting and no escapes.
  - Every other line is `key = value`, blanks around both dropped. Keys are ASCII letters, digits and `_`, matched case-insensitively, as are the mode names.
  - Keys: `scale_mode` (`integer`, `fit`, `smooth`, `sharp`), `border_color` (`#RRGGBB`), `render_scale` (1 to 8).
  - Unknown keys, malformed lines, bad values and `[section]` lines are logged with their line number and ignored; the setting keeps its earlier value. A key given twice: the last counts, with a note. A file over 64 KiB is ignored whole. Nothing in the file is ever fatal.
  - Precedence: the defaults, then `settings.ini`, then the command line (`--scale-mode`, `--border-color`, `--render-scale`). A bad command-line value is a usage error (exit status 2), like every other flag.
  - Only the game reads the file. The placeholder and `--gfx-prototype` are tests and must not depend on a user's settings (the flags still apply).
  - The frontend doesn't write the file yet. #89's settings menu will, with the same keys; the line-oriented format lets it rewrite one value and keep the user's comments.
- **`--window WxH`** sets the window's size in points (default 960 × 640), for trying the modes at other sizes.
- **The read-back check** (`Presenter::check_readback`, run by `--frames`) now covers every mode:
  - It checks every output pixel: bars in the border colour; inside the image, nearest modes must show exactly one of the texels the pixel covers, and blending modes a colour within the range of those texels and their neighbours (±2).
  - It accepts any covered texel because the renderer may round a pixel centre near a texel edge either way at a fractional scale: SDL's software renderer steps in 16.16 fixed point, and 0.9 % of a 5.625× fit's pixels land on the neighbour.

## Evidence

- **CI** (`tests/cpp/scaling_test.cpp`, `platform.scaling: *`, `platform.settings: *`, `platform.present: *`; synthetic, SDL's dummy video driver):
  - **Placements for every mode** at 1920 × 1080 (integer 3×: 1440 × 960 at (240, 60); fit 1620 × 1080 at (150, 0)), 2560 × 1440 (4×; fit 2160 × 1440), and the 16:10 Retina drawable 2880 × 1800 (5×: 2400 × 1600 at (240, 100); fit 2700 × 1800 at (90, 0)). Also odd sizes (1001 × 667, 959 × 639), tall and wide windows, sizes under 1× (the fit fallback), empty sizes, and render scales 2 and 3.
  - **The mouse inverse in every mode**, for windows of 1920 × 1080 and 2560 × 1440 at density 1 and 1440 × 900 and 1001 × 667 at density 2, at render scales 1 and 2. Checked: the image's corners and centre, its last pixel (inside) and far edge (outside), both pairs of bars (outside, extrapolated), and round trips screen → window → screen → window on a grid, within 1e-3. Worked examples: window point (120, 50) of the Retina window is screen (0, 0), and (720, 450) is (240, 160).
  - **`settings.ini`**: a full file, CR LF, a BOM, case, blanks, comments, repeated keys, and 25 kinds of bad lines (unknown keys, bad values, out-of-range and overflowing numbers, signs, quotes, trailing comments, sections, binary, NUL bytes, a UTF-16 BOM). Also a 10,000-byte value (reported cut short), a file over 64 KiB, and 4,000 bytes of garbage.
  - **Read-backs** at 1920 × 1080, 2560 × 1440 and 2880 × 1800 in every mode, at render scale 1 (and 2 at 1080p and the Retina size), with a synthetic image whose every texel has its own colour:
    - Every pixel outside the placement has the border colour, every pixel inside does not.
    - Integer: every output pixel is the texel under its centre, each texel exactly k × k pixels. Fit: every output pixel is a texel it covers.
    - Smooth and sharp, on a ramp image: the interior pixels are within 3 levels of the ramp.
    - A wrong border colour fails the check, and a 300 × 300 window falls back to the fit.
  - The dummy driver has no HiDPI, so its 2880 × 1800 window stands in for the Retina drawable; the density-2 half is the mapping's arithmetic.
- **Real assets (local, a real macOS window, Metal, vsync)**, `MapleThief --window 1440x900 --frames 240 --tap 3,106,165 --capture-at 20,100 --capture <dir>` in each mode. Each read-back was looked at and measured:
  - **Integer** (border `#203040`): 960 × 640 at (240, 130). All 153,600 2 × 2 blocks are uniform and the bars are exactly `#203040`. Crisp, even pixels.
  - **Integer at a 1920 × 1080 window.** macOS gives it 1920 × 1018 under the menu bar: 3×, 1440 × 960 at (240, 29), all 3 × 3 blocks uniform.
  - **Fit**: 1350 × 900 at (45, 0), nearest. 1,019 distinct colours in the image, as in integer mode: no blending.
  - **Smooth** (selected by `settings.ini` with `scale_mode = smooth`, `border_color = #402010` and an unknown key that was logged and ignored): 66,483 distinct colours, visibly blurred, brown bars.
  - **Sharp**: 19,128 colours, crisp texels with soft edges.
  - Every run's last frame read back with 0 mismatches, and the Play tap (through the window mapping) started the game in every mode.
  - **`render_scale = 2` from `settings.ini`**: a 960 × 640 image at integer 2×, read back with 0 mismatches.
  - This Mac's display is 1920 × 1080 at 1×, so no real Retina drawable was available: the HiDPI path ran only at density 1 here.

## Consequences

- **M7 (#65)** converts SDL mouse events with `window_point_to_screen(window, presenter.placement(), x, y)` and sends `inside` points to `mt_uikit_touch`. It decides what a press in the bars or a drag out of the image means (the extrapolated coordinates allow clamping).
- **M10's settings menu (#89)** writes `settings.ini` with these keys and applies a new `scale_mode` or `border_color` at once (`Presenter::set_scaling`). A new `render_scale` needs new renderbuffers, so it applies at the next start unless #89 adds a resize path.
- **A GPU backend (#120)** keeps `place()` and the inverse; only the filter mapping moves.
- **How we'd know this was wrong:**
  - A Retina display shows integer mode at the wrong size or blurred (the drawable is not window × density), or clicks land off target with density 2. Then the density must come from SDL's own `SDL_RenderCoordinatesFromWindow`.
  - Users find fit-nearest's uneven texels or smooth's blur the common complaint. Then sharp becomes the non-integer default.
  - Render scale > 1 users want texel-exact integer steps. Then integer mode grows an option to step by N.

## Addendum: the audio keys and writing the file (#64, 2026-10-29)

M6 adds the first settings that are not about the picture, and the frontend now writes the file.

- **Four keys** (`platform/src/settings.{h,cpp}`), validated like the others:

  | Key | Values | Default | Flag |
  |---|---|---|---|
  | `master_volume` | 0 to 100 (percent, digits only) | 100 | `--master-volume N` |
  | `music_volume` | 0 to 100 | 100 | `--music-volume N` |
  | `effects_volume` | 0 to 100 | 100 | `--effects-volume N` |
  | `mute_unfocused` | `on`, `off` (also `true`/`false`, `yes`/`no`, `1`/`0`, any case; written `on`/`off`) | `on` | `--mute-unfocused on\|off` |

  The volumes drive [ADR 0045](0045-audio-mixer-and-output.md)'s buses (master, music, effects; gain = volume / 100, linear) through `apply_audio_settings` at start-up, in every mode. That includes `--audio-test-tone`, where only the flags apply: the placeholder reads no file. The game's own sound switches (config byte 0) and `alListenerf` gain still multiply. `mute_unfocused` mutes the whole output (`mt_audio_set_muted`) on `SDL_EVENT_WINDOW_FOCUS_LOST` and unmutes it on `SDL_EVENT_WINDOW_FOCUS_GAINED` (`FocusMute` in `audio_out.{h,cpp}`). It is off under `--audio-capture`, whose bytes must not depend on the window's focus. The output starts unmuted, and only a reported loss mutes it.
- **The audio flags are settings lines.** `--master-volume 40` is checked by `apply_setting("master_volume", "40")` as it is parsed (a bad value is a usage error, exit status 2) and applied over the file in command-line order. Unlike `--render-scale` they are not game-only options.
- **Writing (`serialise_settings`, `save_settings`):** with no file, a new one lists every key after a comment line. Over an existing file, only the values of the lines that set a known key change. Everything up to the old value (the key's case, the blanks around `=`) is kept, and the new value replaces the rest. Comments, unknown keys, malformed lines, a byte order mark and the line endings (LF or CR LF, taken from the first line) stay byte for byte. Missing keys are appended, each after its comment. `parse_settings` of the result always gives the settings written. An old file over 64 KiB, or a result that would grow past it, is replaced by a new one. `save_settings` writes `settings.ini.tmp` and renames it over the file, so a failed write leaves the old one. This is the writer #89's settings menu calls.
- **`--save-settings`** (a game option) writes the settings in effect (the file with the command line over it) back before the game starts: `MapleThief --music-volume 50 --save-settings` makes the change stick. The frontend otherwise still never writes the file on its own.
- **Evidence (CI, synthetic):** `tests/cpp/scaling_test.cpp`: the keys, each spelling of on/off, 12 bad audio lines (out of range, signs, units, fractions, overflow, trailing comments); serialise → parse round trips (a new file, the defaults and each mode, a hand-edited CR LF file with a BOM, odd spacing, an unknown key, a malformed and a repeated line, kept byte for byte), idempotence, hostile and oversized old text; `save_settings` → `load_settings` in a temporary data directory, a user's comment and unknown key kept across a second save. `tests/cpp/audio_out_test.cpp`: the buses' gains after `apply_audio_settings` and a constant effect and music source rendered at exactly volume × level (with the listener gain and the player's 0.7), focus events toggling the mute only with the option on. **Local (real assets):** `MapleThief --frames 30 --master-volume 40 --music-volume 70 --mute-unfocused off --save-settings` wrote a new `settings.ini` with those values; a second run without flags read it (`volume 40% (music 70%, effects 100%), mute unfocused off`).
- **Later key:** `gapless_music` (`on`/`off`, default `off`, `--gapless-music on|off`, #125) loops the music without its files' leading and trailing digital silence ([ADR 0062](0062-optional-gapless-music-loops.md)); `apply_audio_settings` sets it before the game opens its players.
- **How we'd know this was wrong:** users find the top of the volume range does nothing audible and the bottom drops off suddenly (linear gain is not how loudness is heard). Then the gain becomes a curve, e.g. (volume / 100)², with the same keys.

## Addendum: the settings registry and the preset key (#144, 2026-10-30)

M10 adds about 15 settings and a menu, so the file's keys are now rows of one table, and the file gets a `preset` key. The details are in [ADR 0070](0070-settings-registry-and-presets.md).

- **One table per setting** (`settings_registry()`, `kRegistry` in `settings.cpp`). Each row has its key, group, comment line(s), value type bound to its `Settings` field, flag, menu label and help, and preset columns. `apply_setting`, `parse_settings`, `serialise_settings` and `main.cpp`'s flags are loops over it. The per-key branches and the key list are gone.
- **The format is unchanged.** The same keys, values, spellings, messages and comment lines. The same order in a new file, now after `preset` when one is set. The same byte-for-byte rewrite (an older file keeps every line, and only the keys it lacks are appended). The existing `settings: *` tests pass unchanged.
- **`preset = original | original_controller | enhanced | custom`.**
  - A preset other than `custom` wins over the lines of the keys it controls. A line that disagrees is reported and ignored, and `custom` keeps every line.
  - Changing such a key through the port (the command line, #89's menu) moves the preset to `custom`.
  - Without a `preset` line, everything is as before this addendum, and the defaults stay the defaults. "No settings file" is unchanged: no preset, and the game shows the first-launch picker in an interactive run.
- **Precedence:** the defaults, then `settings.ini` (with its preset), then `--preset`, then every other flag in command-line order.
- **The flags come from the rows.** Each one is `--` plus the key with `-` for `_`, checked by `apply_setting`. `--scale-mode`, `--border-color` and `--render-scale` now take the same path. They still mean the same values, but bad values are reported in the file's wording, and `--render-scale` takes at most three digits.
