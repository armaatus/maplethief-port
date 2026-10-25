"""Sprite/animation viewer for the ``.sprprj`` sprite projects (issue #27, ADR 0010).

Plays every action of every sprite project with the offsets, flips and timing of the game
(docs/formats/sprprj.md). Python does all the pixel work: it composites every frame of an
action onto **one canvas per action**, with the sprite position (the *anchor*) at the same
canvas pixel in every frame and both facings, so playback cannot jitter by construction. The
page's JavaScript only swaps those frame PNGs at the spec's durations and toggles overlays.

Drawing follows the ``CMFrame`` draw at 0x1F119 (called by the ``CMAction`` draw 0x1F2B9):

- the frame is drawn 1 px below the sprite position (``y + 1``);
- layers in file order (first at the back), hidden layers skipped;
- a layer's own alpha is applied (the action alpha is 255, its constructor default);
- unflipped, a layer's top-left is at ``(x + layer.x, y + layer.y)``; with the action flipped
  (``facing == 2``, 0x1E619) at ``(x - layer.x - (image width - 1), y + layer.y)``;
- a layer is mirrored when exactly one of the action flip and the layer's ``hflip`` is set.

Images are ``CMSpriteImage`` (0x218F1): a palette from the project replaces the PNG's
``PLTE`` data before decoding (0xB9F1), ``tRNS`` is kept. Layers are blended "over" in
straight alpha (see :func:`blend`).

Timing: a frame is current for ``max(delay, 1)`` game ticks and an action ends after the sum
over all its frames (``CMAction`` update 0x1E585, :class:`mtport.sprprj.ActionPlayer`). A tick
is 65, 50 or 40 ms (game speed table 0x108C58; 50 ms by default, docs/re/timing-threading.md).
Actions never wrap; the page loops them the way an owner does that resets an action when it
reports *ended*, or holds the last frame when looping is off.

Output (``assets-out/viewers/sprites/``, owned by this command alone)::

    index.html                       every project and its actions
    <folder>-<n>.html                one player per action (data inlined: works from file://)
    <folder>-<n>.json                the same data: canvas, anchor, frame ticks, bounds, files
    <folder>-<n>/a<k>-f<i>.png       frame i of action k, normal facing
    <folder>-<n>/a<k>-f<i>-flip.png  the same frame with the action flipped

Command line (``PYTHONPATH=tools python3 -m mtport.spriteview ...``)::

    build [--app DIR] [--out DIR] [--projects P...]   write the pages (P: mob/3 or hero)
    strip PROJECT ACTION --out FILE.png [--flip] [--guides] [--background RRGGBB] [--scale N]
    batch [--app DIR] [--projects P...]               load and render everything, check it

``batch`` loads every project, decodes every image, renders every frame of every action in
both facings and runs the anchor check (:func:`check_action`) on each, printing one line per
error and exiting 1 on any. The output is deterministic. Input files are untrusted data:
they are parsed, never executed.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from mtport import png, sprprj
from mtport.names import parse_hex
from mtport.spriteview_page import (
    CSS,
    SCRIPT,
    BatchReport,
    batch,
    build,
    render,
    render_index,
    render_page,
    run,
)
from mtport.spriteview_render import (
    _EMPTY,
    DEFAULT_TICK_MS,
    DRAW_Y_OFFSET,
    FACINGS,
    SPRITE_DIR,
    TICK_MS,
    VIEWER,
    ActionRender,
    Box,
    ImageCache,
    Loaded,
    Placement,
    ProjectId,
    Rendered,
    ViewerError,
    blend,
    check_action,
    clean,
    composite,
    decode_image,
    dumps,
    find_projects,
    frame_file,
    image_box,
    inline_json,
    layer_alpha,
    load_project,
    mirror_rows,
    parse_id,
    placements,
    project_data,
    render_action,
    render_project,
    select,
    sizes_of,
    spec_box,
    strip,
    union,
    upscale,
    visible_box,
)

# mtport.spriteview is the public face of the renderer (spriteview_render) and the pages
# (spriteview_page): it re-exports their names and keeps the command line.
__all__ = [
    "ActionRender",
    "BatchReport",
    "Box",
    "CSS",
    "DEFAULT_TICK_MS",
    "DRAW_Y_OFFSET",
    "FACINGS",
    "ImageCache",
    "Loaded",
    "Placement",
    "ProjectId",
    "Rendered",
    "SCRIPT",
    "SPRITE_DIR",
    "TICK_MS",
    "VIEWER",
    "ViewerError",
    "_EMPTY",
    "batch",
    "blend",
    "build",
    "check_action",
    "clean",
    "composite",
    "decode_image",
    "dumps",
    "find_projects",
    "frame_file",
    "image_box",
    "inline_json",
    "layer_alpha",
    "load_project",
    "mirror_rows",
    "parse_id",
    "placements",
    "project_data",
    "render",
    "render_action",
    "render_index",
    "render_page",
    "render_project",
    "run",
    "select",
    "sizes_of",
    "spec_box",
    "strip",
    "union",
    "upscale",
    "visible_box",
]


_REPO = Path(__file__).resolve().parents[2]
DEFAULT_APP = _REPO / "work" / "ipa" / "Payload" / "MapleThief.app"
DEFAULT_OUT = _REPO / "assets-out" / "viewers" / VIEWER


def _color(text: str) -> tuple[int, int, int]:
    if not re.fullmatch(r"[0-9A-Fa-f]{6}", text):
        raise argparse.ArgumentTypeError(f"{text!r}: expected RRGGBB")
    v = parse_hex(text)
    return (v >> 16, (v >> 8) & 0xFF, v & 0xFF)


def _strip_cmd(args: argparse.Namespace) -> int:
    try:
        pid = parse_id(args.project)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    loaded = load_project(args.app, pid)
    project = loaded.project
    if project is None or not 0 <= args.action < len(project.actions):
        problem = "; ".join(loaded.errors) or f"{pid.key}: no action {args.action}"
        print(f"error: {problem}", file=sys.stderr)
        return 1
    action = project.actions[args.action]
    used = {la.image for fr in action.frames for la in fr.layers}
    if not action.frames or any(loaded.images[i] is None for i in used):
        print(f"error: {'; '.join(loaded.errors) or 'no frames'}", file=sys.stderr)
        return 1
    images = [img if img is not None else _EMPTY for img in loaded.images]
    render = render_action(action, images)
    problems = check_action(f"{pid.key} action {args.action}", action, images, render)
    img = strip(render, args.flip, args.gap, args.guides, args.background, args.scale)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    png.save(args.out, img)
    for p in problems:
        print(f"error: {p}", file=sys.stderr)
    print(
        f"{args.out}: {len(action.frames)} frames of {render.width}x{render.height}, anchor "
        f"({render.anchor_x}, {render.anchor_y}), ticks "
        f"{[sprprj.frame_ticks(f) for f in action.frames]}"
    )
    return 1 if problems else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.spriteview", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="write the viewer pages")
    c = sub.add_parser("batch", help="load, render and check every project")
    s = sub.add_parser("strip", help="export the frames of one action as a PNG strip")
    for p in (b, c, s):
        p.add_argument("--app", type=Path, default=DEFAULT_APP, help="MapleThief.app directory")
    b.add_argument("--out", type=Path, default=DEFAULT_OUT)
    for p in (b, c):
        p.add_argument("--projects", nargs="+", metavar="P", help="mob/3 or a folder (hero)")
    s.add_argument("project", help="<folder>/<n>, e.g. hero/0")
    s.add_argument("action", type=int)
    s.add_argument("--out", type=Path, required=True)
    s.add_argument("--flip", action="store_true", help="flip the whole action")
    s.add_argument("--gap", type=int, default=1, help="transparent columns between frames")
    s.add_argument("--guides", action="store_true", help="anchor cross behind each frame")
    s.add_argument("--background", type=_color, help="fill colour RRGGBB behind each frame")
    s.add_argument("--scale", type=int, default=1, help="nearest-neighbour enlargement")
    args = parser.parse_args(argv)
    if not args.app.is_dir():
        print(f"error: {args.app}: not a directory (run scripts/bootstrap)", file=sys.stderr)
        return 1
    if args.cmd == "strip":
        if args.gap < 0 or not 1 <= args.scale <= 16:
            print("error: --gap must be >= 0 and --scale 1..16", file=sys.stderr)
            return 1
        return _strip_cmd(args)
    if args.cmd == "batch":
        report = batch(args.app, args.projects)
        for e in report.errors:
            print(f"error: {e}")
        print(report.summary())
        return 1 if report.errors else 0
    try:
        files, errors = build(args.app, args.out, args.projects)
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    print(f"{len(files)} files -> {args.out}, {len(errors)} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
