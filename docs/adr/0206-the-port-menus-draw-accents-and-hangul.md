# 206. The port's menus draw accents and Hangul

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#372](https://github.com/armaatus/maplethief-port/issues/372) (epic [#294](https://github.com/armaatus/maplethief-port/issues/294))
- **Builds on:** [ADR 0036](0036-coregraphics-and-uikit-drawing.md) (text drawing, NanumGothic), [ADR 0054](0054-bindings-file-remap-overlay-and-button-glyphs.md) (the Canvas and its 5x7 font), [ADR 0076](0076-settings-menu.md) (the settings menu), [ADR 0103](0103-ui-scaling-window-text-veil-and-minimum-window.md) (UI scaling), [ADR 0108](0108-text-packs.md) (text packs, `Port.strings`)

## Context

ADR 0108 let a text pack translate the port's own menus, but the overlays draw with the Canvas's 5x7 pixel font, which has printable ASCII only. A translation was folded to ASCII when it was looked up ("Einstellungen für" drew "Einstellungen fur") and every character outside Latin drew as "?". The overlays' layout also counted bytes: a wrap, a "..." cut or a column of `/ 6` treated each byte of UTF-8 as a character.

The game's own text has no such limit: the runtime draws it with stb_truetype from two bundled fonts, NanumGothic (Latin and Hangul) and, for what NanumGothic lacks, Fira Sans (Latin-1, Latin Extended, Greek, Cyrillic). A pack may not bring its own font (#189: stb_truetype is not hardened against malformed fonts), so the menus must use these.

The overlays are the same code on every platform (`platform/src`: desktop, iOS, Switch, Android), and their English drawings are checked by hash in many tests (settings menu, preset picker, controls card, remap, disconnect pause, key pad editor, start question, the Saves tab).

## Options considered

1. **All menu text in the bundled fonts, proportional.** One look for every language. But every overlay's layout counts fixed 6-point cells (columns, wraps, cuts, centring), every English drawing would change, and NanumGothic at the menus' 8-point line is softer than the pixel font at 1x.
2. **ASCII in the 5x7 font, every other character from the bundled fonts, in cells.** English stays the same bytes. A character outside ASCII is rasterized by stb_truetype from NanumGothic, else Fira Sans, at the image's pixels (sharp at 2x and up), into one 6-point cell, or two for an East Asian wide character (a Hangul syllable is about 9 points wide). The layout keeps its cell arithmetic, now counting cells instead of bytes.
3. **Accents drawn as pixel marks over the 5x7 letters.** Crisp, but covers only Latin, and every mark would be hand-drawn.
4. **A bitmap font with Hangul** (GNU Unifont and the like). A new dependency of several megabytes, a licence to vet, and a second look beside the game's own Hangul.

## Decision

Option 2.

- **The glyph path.** `mt_cg_bundled_glyph(cp, em_pixels, max_width, ...)` (`maplethief/cg.h`, `runtime/src/cg/text.cpp`) gives a code point's coverage from NanumGothic, else Fira Sans, as the game's text is drawn. It loads its own copies of the fonts behind a lock, so the frontend's thread never touches the game's. A glyph wider than `max_width` is narrowed to it.
- **The Canvas** (`platform/src/canvas.{h,cpp}`) decodes UTF-8 strictly (`next_code_point`). Printable ASCII draws in the 5x7 font as before. Any other character draws from the bundled fonts at an em of 9 font pixels times the scale, centred in its cells, on the 5x7 font's baseline, its coverage blended with the text's colour. A character neither font has, a control character or a byte that is not UTF-8 draws as "?". Glyphs are kept per code point and size.
- **Cells.** `Canvas::char_columns`: two for an East Asian wide character (Hangul Jamo and syllables, CJK, kana, fullwidth forms), one for everything else. `columns`, `fit_columns` (a prefix of whole characters) and `fit_width` (a prefix with "..." that fits a width in points) are what the layout uses. `text_width` is 6 points a cell less the last gap, so ASCII measures as before.
- **`tr()` returns the pack's text as it is** (UTF-8). `ascii_fold` remains for the one line drawn with SDL's debug font, the cash shop's price line.
- **Layout.** Every overlay that wraps or cuts counts cells and cuts at a whole character: the settings menu's `wrap` and `fit`, the start question's and data notices' `wrap_notice`, the import screen's `wrap_text` and `shorten_path`, the controls card's `fit`, the remap overlay's status, the disconnect pause's pad name. Texts that had no limit get one, so a longer translation is cut with "..." where it would have run past its box: the preset picker's and start question's lines and buttons, the remap overlay's columns and footer, the controls card's title, device line and touch notes (kept clear of the key names), the disconnect pause's lines, the key pad editor's status and hint, the import screen's lines and buttons, and every row of hints (a hint with no room left is not drawn). Two bars share their room: the settings menu's tabs (`SettingsMenu::tab_name`) and the key pad editor's buttons are cut to the most cells per name that fit, only when the full names would not. English fits everywhere, so nothing English is cut.
- **Coverage** (also ADR 0108's addendum): Latin with accents and Hangul are covered and tested; Greek and Cyrillic draw from Fira Sans. Not covered: scripts written right to left (Arabic, Hebrew: no bidirectional layout), scripts that need shaping (Devanagari and the other Indic scripts, Thai, Khmer), combining marks (a decomposed accent draws as "?": a translation must be NFC), CJK ideographs and kana where neither font has them, and emoji. These draw as "?".

## Evidence

- `tests/cpp/menu_glyphs_test.cpp` (`platform_test`):
  - `menu glyphs: cells, whole-character cuts, UTF-8 decoding (#372)`.
  - `menu glyphs: accents and Hangul from the bundled fonts, '?' for what neither has`: an accented letter is neither its base letter nor "?"; a line of German, Latin ligatures, Hangul and typographic quotes at 1x, 2x and 3x against `tests/golden/data/menu_glyphs/line_*.png`.
  - `menu glyphs: a pseudo-translation draws every overlay without '?' or clipping`: a pack made at run time gives every `Port.strings` key (the table, the registry's labels and help, the menu actions' fields) accents on every letter that has one and Hangul syllables a third longer than the English. Every overlay (each settings tab, a list view, the preset picker, the controls card for touch, keyboard and gamepad, the remap overlay, the disconnect pause, the key pad editor, both start questions, the import screen waiting and refused) is drawn at UI scales 1 to 4. A Canvas text observer (`set_text_observer`) checks that no text drew a "?", every text lies inside its panel and the screen, and none overlaps another. 1x images (and the first tab at 2x) are goldens with a tolerance for stb_truetype's float rasterization.
- English unchanged: every overlay drawing test that checks by hash passes with its old hash (settings menu, preset picker, controls card, remap, disconnect, start question, the Saves tab, the key pad editor).
- `tests/cpp/port_text_test.cpp`: a pack's text comes back as UTF-8; `ascii_fold` for the price line.

## Consequences

- A translation's menus show its real letters, in Latin with accents, Hangul, Greek and Cyrillic, on every platform the overlays run on (desktop, iOS, Switch, Android).
- A long translation is cut with "..." rather than running past its box: the settings tabs and the key pad editor's buttons can get short when every name is long. `mtport.textpack export` says so in `Port.strings`' header.
- Mixed text pairs a pixel font with an outline font. At 1x a NanumGothic Hangul syllable 8 pixels tall is legible but soft; from 2x up it is sharp.
- The price line still folds to ASCII (SDL's debug font).
- How we'd know this is wrong: a menu row with "?" for a character a pack uses (a script listed as not covered, or a gap in the fonts), a cut that splits a character, or an English overlay hash that changes.
