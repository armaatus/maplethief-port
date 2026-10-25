# 10. Asset pipeline and viewers: one Python renderer, static HTML pages

- **Status:** Accepted
- **Date:** 2026-10-25
- **Issue:** #24, #25, #26, #27, #28 (epic #97)

## Context

Milestone M2 turns the M1 parsers into an inspectable `assets-out/` tree and validates them visually: a map viewer for the 90 maps (#26), a sprite/animation viewer for the 138 sprite projects (#27) and a data-table browser (#28), next to the pipeline command itself (#25) and the CgBI un-crusher (#24). The viewers exist to check the format specs before the engine runs, and Joris compares their output with the real game.

Constraints already settled:

- The parsers are Python (`tools/mtport/`, ADR 0002) and tested with synthetic fixtures in CI. ADR 0003 keeps Python tools on the standard library unless an ADR lists a package; Pillow and NumPy are not listed, and the CgBI un-crusher uses `zlib`/`struct`.
- The C++ side has SDL3 and stb_image/stb_image_write (ADR 0003), but no JSON library and none of the format parsers.
- `assets-out/` is git-ignored and regenerated from the IPA; nothing in it is committed.
- Map exports must be scriptable (PNG files for local golden checks and for Joris's comparison), and batch modes must load every map and sprite project and report errors.

## Options considered

1. **C++ viewers on SDL3** (`stb_image` for PNGs). Closest to the eventual renderer and fast. But every format parser would be written a second time in C++ (or a JSON library added to read the pipeline's output), the viewers would need a GUI build to run, and headless PNG export needs an offscreen renderer.
2. **Python with a GUI toolkit** (tkinter). Reuses the parsers. Tk isn't present in every Python (Homebrew and Linux distributions ship it separately), `PhotoImage` has weak alpha and scaling support, and a GUI toolkit is awkward to test.
3. **Python renders everything, viewers are generated static HTML pages.** The parsers and one stdlib renderer produce every pixel as PNG files (whole maps, per-layer images, sprite frames, frame strips) plus JSON metadata. A small hand-written HTML/JS template, written by the generator next to the images, only stacks layers, toggles them, pans/zooms and plays frames at the spec's durations. Any browser opens it from disk; there is nothing to build or install.

## Decision

We use **option 3**.

- **One renderer, in Python.** `tools/mtport/png.py` is the standard-library PNG codec (decode all colour types and bit depths the game uses, unfilter, encode RGBA with deterministic settings), and `tools/mtport/cgbi.py` (#24) builds the CgBI un-crusher on it. Map and sprite rendering (#26, #27) composite with that codec into RGBA buffers and write PNGs. The browser never composites game pixels itself, so a viewer page and the exported PNG cannot disagree.
- **Viewers are generated static pages** under `assets-out/viewers/<name>/` (`maps/`, `sprites/`, `tables/`), each with an `index.html` that works from `file://` without a server. Their JavaScript is small, hand-written and inline in a template under `tools/mtport/`: no framework, no build step, no network access. Images and JSON sit next to the page with relative paths.
- **Every viewer has a batch mode** that loads every input, prints one line per error or missing asset and exits non-zero on any error. The batch mode is what the exit criteria and the real-asset tests check; the HTML pages are what people look at.
- **`assets-out/` layout.** The pipeline (#25) owns `assets-out/` and `assets-out/manifest.json` and never deletes `assets-out/viewers/`; each viewer command owns only its own `assets-out/viewers/<name>/`. Outputs are deterministic (sorted keys, fixed PNG compression settings, no timestamps), so two runs give identical bytes.
- Generated images of game art stay in `assets-out/` and `work/`; tests check them by hash or dimensions, never against committed copies. Synthetic fixtures cover the renderers in CI.

## Consequences

- The parsers stay the single source of truth for the formats, and the viewers are as testable as the rest of `tools/`: render a synthetic map or sprite project, check the pixels.
- Pure-Python decoding and compositing is slow next to C++. It must stay fast enough for a full batch over all maps and sprite projects to finish in minutes; if it doesn't, a later ADR can add NumPy or move rendering to C++ without changing the page format.
- The viewers say nothing about the engine's renderer (#55, #57). Disagreements found later between viewer and engine point at one of the two renderers, and the viewer is the simpler one to check.
- Hand-written JavaScript is a fourth language in the repo, limited to these templates. If the viewers ever need more than layer toggles, pan/zoom and frame playback, that is the point to reconsider option 1.
