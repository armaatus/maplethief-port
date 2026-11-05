# The game's full-screen flashes, and the port's `flashes`

Where the game fills the whole screen with white or black for a frame, and how the port's `flashes = 0..100` tones it down ([#186](https://github.com/armaatus/maplethief-port/issues/186), [ADR 0093 addendum](../adr/0093-accessibility-colour-filters-screen-shake-and-text-height.md#addendum-flashes-186-2026-11-05)). Addresses are armv7 VM addresses; functions carry the Thumb bit, return addresses too. The evidence is the Ghidra decompilation and disassembly of the 1.5 binary (2026-11-05), the colour table read from the binary (`tests/test_flashes_names.py`), and the real-asset runs under [Verification](#verification). The names are `python3 -m mtport.flashes names`.

## Verdict

**The strobe is the hero's draw, two frames long.** A level-up (and three other hero effects) draws one frame filled with white and the next filled with black, over the whole 480 × 320 screen, at full opacity. The HUD and the level text are drawn on top. The scenes' fades in from white are the only other bright full-screen fills. All of them go through `FillRect` `0x8079` from a handful of call sites, so the port can scale the alpha of exactly those fills and nothing else.

## The fill path

- **`FillRect` `0x8079`** (surface, x, y, w, h; the graphics context at sp + 4) draws a solid quad in the context's colour (`+0x14`, 0xRRGGBB) at its alpha (`+0x20`; −1 is opaque). Opaque, it sets `glColor4x(r << 8, g << 8, b << 8, 0x10000)`. With an alpha it enables blending (`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`), sets `glColor4x(…, a << 8)` and disables blending again unless the context's `+0x30` is −1. It leaves the fill's colour current in GL. The context's attributes are written by `0x87AD(ctx, index, value)` at `+0x10 + 4 × index` (index 4 is the alpha); `0x6225` resets them (alpha 255, `+0x30` 255).
- **`Canvas_FillRect` `0x23D1D`** (x, y, w, h) calls `FillRect` with the canvas's context (`SkyCanvas` at `0x11A950`, `+0x10`). When the alpha flag (byte `0x11A514`) is set it first applies the draw alpha that `Canvas_SetAlpha` `0x22479` stored (`0x237ED`), and saves and restores the attributes around the fill (`0x238F9`).
- **`Canvas_FillRectColor` `0x23D95`** (x, y, w, h, colour at sp) sets the context's colour (`0x238A1`), then calls `Canvas_FillRect`.
- **`Canvas_FillScreen` `0x23DC1`** (colour) fills 480 × 320.

The game never calls `glClear`: every frame is drawn over the last one.

## The flashes

All the hero's flashes are in **`CHero::Paint` `0x61A19`** (the hero's draw, called from `CStateGame::Paint` `0x87501`). Each reads its colour from the table at `0x117150` (8 rows of 50 colours, 200 bytes a row) by the effect's frame. Row 2 is white (`0xFFFFFF`), row 3 black; the counts at `0x117790` are 5, 17, 1, 1, 31, 31, 17, 5, and on the other rows the same code draws that many 1-pixel vertical beam lines instead of a fill.

| Flash | Started by | White fill (call, return) | Black fill (call, return) |
|---|---|---|---|
| Revive | the hero's `+0x128` counter, set to 1 by the server packet handler (`0x40228`, `0x40BC2`) and counted up to 0x19 by `CHero::Flush` `0x62ADD` | frame 2: `0x61CDA`, `0x61CDF` | frame 3: `0x61D2E`, `0x61D33` |
| Effect 12 (**a level-up**) or 13 | the effect id (global `0x11A9A4`), set by `CHero::StartEffect` `0x6049D` (hero, id); `CHero::LevelUp` `0x65089` starts 12. The frame is the hero's `+0x13C`, minus 6 for effect 13 | frame 2: `0x6200A`, `0x6200F` (it also plays sound 11) | frame 3: `0x62054`, `0x62059` |
| Effect 15 | `CHero::Flush` when effect 14 ends (15 or 16, with `CHero::SetFace` and popup 0x66, "CHero.Flush.4/5") | frame 1: `0x621DA`, `0x621DF` | frame 2: `0x6220E`, `0x62213` |

The effect 12/13 fills are drawn only in the field: game mode 0, no popup (−1), and `CDataMgr +0xB4` clear. So a level-up won during a quest dialog flashes after the dialog closes. At these fills the context's alpha is 255 and `+0x30` is 255 (measured in the port): an opaque quad. The game's white reads back as 254 (`glColor4x` `0xFF00`).

**Scene fades from a colour.** A scene's command 29 sets a countdown (`CStateGame +0x494`) and a colour (`+0x4B8`). `CStateGame::Paint` fills the screen with that colour through `Canvas_FillRect` at `0x8A6EA` (return `0x8A6EF`), opaque, then at alpha 170, 85 and 0 over the countdown's last three frames. With white (`0xFFFFFF`, the intro's and the boss endings' fades) it is a white frame for as long as the countdown runs, then a fade; with black it is an ordinary fade.

**Not flashes** (left as they are):

- the fade-out at `0x8A78E` (always black) and the fades in from black;
- the scenes' letterbox bars and scan lines;
- the title and menu screens' backgrounds (`CStateTitle::Paint`, `0x98005`, `CStateMainMenu::Paint`, through `Canvas_FillScreen`);
- the touch widgets' fills `0xA0CBD` and `0xA0D41` (alpha 100);
- the popup menu's dim `0x56B99` (black at alpha 100; widescreen stretches it, [widescreen.md](widescreen.md));
- the hero's hit blink (`+0x11C` skips the hero's draws; no fill).

## The port's hooks

`flashes` (Accessibility, 0 to 100 in steps of 25, default 100, not in the presets; `--flashes`; live) installs, through the guest function hooks (ADR 0069, `runtime/src/hooks/flashes.cpp`, two new hookable functions in `recomp/hooks.py`), all off at 100:

- **`Canvas_FillRectColor` `0x23D95`**: when its return address is one of the six hero sites above, it marks the call and runs the original.
- **`Canvas_FillRect` `0x23D1D`**: when called from the fade-in site `0x8A6EF` with a colour other than black, the same.
- **A wrapper on `FillRect` `0x8079`** (`mt_hook_wrap`, beside widescreen's handler for the popup dim): for a marked fill only, it writes `round(alpha × P / 100)` into the context's alpha (255 for opaque), runs the fill, writes the game's alpha back, and sets GL's colour to the one the game's fill would have left (`glColor4x` through its import stub `0xF0F10`). At 0 the fill is still drawn, at alpha 0, so GL's blend state after it is the game's.

The hooks read and write only the graphics context, for one call. The game's state, its timers and its random numbers are untouched. At 100 nothing is registered on: the original.

## Verification

- `tests/cpp/hooks_test.cpp` (CI, synthetic guest functions with the same arguments and context): `hooks: mt_flash_scale_alpha scales a fill's alpha, opaque as 255 (#186)` and `hooks: flashes scales the flash fills' alpha, skips them at 0, nothing else (#186)`: off at 100; at 50 the six hero sites and a white fade-in fill at 128 (a fade frame's 170 at 85), the game's alpha back afterwards and GL's colour the game's; the HP bar's and the menu dim's fills, a fade-out and a fade from black untouched; at 0 alpha 0; widescreen's `FillRect` handler still sees every fill.
- `tests/test_flashes_names.py`: the names, the sites against the hook, and the colour table's flash rows in the shipped binary (skipped without it).
- `play.flashes_original` and `play.flashes_{100,50,0}` (real assets, local, fixed step): a new game through the tutorial to its first level-up (MacArthur's first quest), every GL frame from the level-up to 60 ticks after the dialog closes, hashed and counted (a frame is a full-screen white frame when at least half its pixels have every channel ≥ 240, black when ≤ 15).

  | Run | level-up tick, last tick | `rand()` state after | full-screen white, black frames | flash fills taken | frames byte-identical to the original's |
  |---|---|---|---|---|---|
  | original (no hooks) | 768, 923 | `0xEFDB42BC` | 1 (tick 864, 975 ‰ white), 1 (tick 865, 985 ‰ black) | none | — |
  | 100 | 768, 923 | `0xEFDB42BC` | 1, 1 | none | 155 of 155 |
  | 50 | 768, 923 | `0xEFDB42BC` | 0, 0 (32 ‰ white, 26 ‰ black at ticks 864 and 865) | 2 + the intro's white fade-in | 152 of 155 |
  | 0 | 768, 923 | `0xEFDB42BC` | 0, 0 | 2 + the intro's white fade-in | 96 of 155 |

  Every frame before the level-up's first fill is the original's byte for byte at every percent.

**The frames after a flash.** Because the game never clears the screen and some of the field's pixels are blended over what the last frame left, a few pixels of the frames after a flash still differ by one level when the flash was toned down: at 50 % only the frame after the black fill (tick 866), at 0 % 93 pixels at tick 880 and a channel sum within 76 of the original's (of 460,800 channels) in the last frame. That is the toned-down flash's own trace, not a change to anything else; the runs check that the last frame's channel sum is within a thousandth of a level per channel of the original's.
