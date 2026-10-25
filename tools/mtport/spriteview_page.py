"""The pages of :mod:`mtport.spriteview`: the CSS and JavaScript of the player, one page
per project, the index, and the ``build`` / ``batch`` drivers.
``mtport.spriteview`` re-exports every name defined here.
"""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from mtport import htmlpage as hp
from mtport import png, sprprj
from mtport.spriteview_render import (
    DEFAULT_TICK_MS,
    FACINGS,
    TICK_MS,
    VIEWER,
    ActionRender,
    ImageCache,
    ProjectId,
    Rendered,
    dumps,
    find_projects,
    frame_file,
    inline_json,
    load_project,
    project_data,
    render_project,
    select,
)

# --- Pages ----------------------------------------------------------------------------------

CSS = """
.controls { position: sticky; top: 0; z-index: 2; background: var(--bg); padding: 6px 0;
  border-bottom: 1px solid var(--line); display: flex; flex-wrap: wrap; gap: 6px 16px; }
.controls label { white-space: nowrap; }
section.action { margin: 16px 0 24px; }
section.action h2 { font-size: 16px; margin: 8px 0 4px; }
.player { display: flex; flex-wrap: wrap; align-items: flex-start; gap: 12px; }
.stage { position: relative; outline: 1px solid var(--line); }
.stage img, .cell img { display: block; image-rendering: pixelated; }
.bg-checker .stage, .bg-checker .cell .box { background-color: #fff;
  background-image: linear-gradient(45deg, #ccc 25%, transparent 25%),
  linear-gradient(-45deg, #ccc 25%, transparent 25%),
  linear-gradient(45deg, transparent 75%, #ccc 75%),
  linear-gradient(-45deg, transparent 75%, #ccc 75%);
  background-size: 16px 16px; background-position: 0 0, 0 8px, 8px -8px, -8px 0; }
.bg-dark .stage, .bg-dark .cell .box { background: #202020; }
.bg-light .stage, .bg-light .cell .box { background: #f0f0f0; }
.bg-green .stage, .bg-green .cell .box { background: #6c8; }
.ov { position: absolute; pointer-events: none; }
.cross { background: #ff00c8; }
.bounds { outline: 1px solid rgba(0, 160, 255, .9); }
.hide-cross .cross, .hide-bounds .bounds { display: none; }
.buttons button { font: inherit; min-width: 3.5em; }
.cells { display: flex; flex-wrap: wrap; gap: 6px; }
.cell { font-size: 12px; text-align: center; cursor: pointer; }
.cell .box { position: relative; outline: 1px solid var(--line); }
.cell.cur .box { outline: 2px solid var(--link); }
.status { font-variant-numeric: tabular-nums; }
"""

# Plays the pre-rendered frames. Per action: frame i shows for ticks[i] ticks; after the last
# frame the action is ended and, when looping, reset to frame 0 (as the game's owners do).
# One timer drives every player at the chosen tick length. No network, works from file://.
SCRIPT = """
(function () {
  var data = JSON.parse(document.getElementById("sprite-data").textContent);
  var body = document.body, players = [], timer = null;
  var opts = { zoom: 3, tick: data.default_tick_ms, flip: false, loop: true };
  function px(v) { return (v * opts.zoom) + "px"; }
  function place(el, x, y, w, h) {
    el.style.left = px(x); el.style.top = px(y); el.style.width = px(w); el.style.height = px(h);
  }
  function Player(sec, act) {
    var self = this;
    this.act = act; this.cur = 0; this.hold = 0;
    this.playing = act.frames.length > 1;
    this.stage = sec.querySelector(".stage"); this.img = this.stage.querySelector("img");
    this.box = this.stage.querySelector(".bounds");
    this.h = this.stage.querySelector(".cross.h"); this.v = this.stage.querySelector(".cross.v");
    this.status = sec.querySelector(".status");
    this.play = sec.querySelector(".play");
    this.cells = sec.querySelectorAll(".cell");
    sec.querySelector(".prev").onclick = function () { self.step(-1); };
    sec.querySelector(".next").onclick = function () { self.step(1); };
    this.play.onclick = function () {
      if (!self.playing && self.ended()) { self.cur = 0; self.hold = 0; }
      self.playing = !self.playing; self.draw();
    };
    Array.prototype.forEach.call(this.cells, function (c, i) {
      c.onclick = function () { self.playing = false; self.cur = i; self.hold = 0; self.draw(); };
    });
  }
  Player.prototype.ended = function () {
    return this.cur === this.act.frames.length - 1 && this.hold >= this.act.frames[this.cur].ticks;
  };
  Player.prototype.step = function (d) {
    var n = this.act.frames.length;
    this.playing = false; this.cur = (this.cur + d + n) % n; this.hold = 0; this.draw();
  };
  Player.prototype.tick = function () {
    var f = this.act.frames;
    if (!this.playing || f.length < 2) return;
    this.hold++;
    if (this.hold >= f[this.cur].ticks) {
      if (this.cur + 1 < f.length) { this.cur++; this.hold = 0; }
      else if (opts.loop) { this.cur = 0; this.hold = 0; }
      else { this.playing = false; }
    }
    this.draw();
  };
  Player.prototype.draw = function () {
    var a = this.act, f = a.frames[this.cur], face = opts.flip ? "flipped" : "normal";
    var b = f.bounds[face];
    this.stage.style.width = px(a.width); this.stage.style.height = px(a.height);
    this.img.src = f.files[face];
    this.img.style.width = px(a.width); this.img.style.height = px(a.height);
    place(this.h, 0, a.anchor[1] + 0.5, a.width, 0);
    this.h.style.height = "1px";
    place(this.v, a.anchor[0] + 0.5, 0, 0, a.height);
    this.v.style.width = "1px";
    if (b) { this.box.hidden = false; place(this.box, b[0], b[1], b[2] - b[0], b[3] - b[1]); }
    else { this.box.hidden = true; }
    var t = 0;
    for (var i = 0; i < this.cur; i++) t += a.frames[i].ticks;
    this.status.textContent = "frame " + this.cur + " / " + (a.frames.length - 1) +
      " \\u00b7 delay " + f.delay + " (" + f.ticks + " tick" + (f.ticks === 1 ? "" : "s") +
      ", " + f.ticks * opts.tick + " ms) \\u00b7 tick " + (t + Math.min(this.hold, f.ticks - 1)) +
      " of " + a.loop_ticks;
    this.play.textContent = this.playing ? "Pause" : "Play";
    Array.prototype.forEach.call(this.cells, function (c, i) {
      c.classList.toggle("cur", i === this.cur);
      var cimg = c.querySelector("img");
      cimg.src = a.frames[i].files[face];
    }, this);
  };
  data.actions.forEach(function (act) {
    var sec = document.getElementById("a" + act.index);
    if (sec) players.push(new Player(sec, act));
  });
  function run() {
    players.forEach(function (p) { p.tick(); });
    timer = setTimeout(run, opts.tick);
  }
  function redraw() { players.forEach(function (p) { p.draw(); }); }
  function bind(id, fn) {
    var el = document.getElementById(id);
    if (!el) return;
    el.addEventListener("change", function () { fn(el); redraw(); });
    fn(el);
  }
  bind("opt-zoom", function (el) { opts.zoom = Number(el.value); });
  bind("opt-tick", function (el) {
    opts.tick = Number(el.value);
    if (timer !== null) { clearTimeout(timer); timer = setTimeout(run, opts.tick); }
  });
  bind("opt-flip", function (el) { opts.flip = el.checked; });
  bind("opt-loop", function (el) { opts.loop = el.checked; });
  bind("opt-cross", function (el) { body.classList.toggle("hide-cross", !el.checked); });
  bind("opt-bounds", function (el) { body.classList.toggle("hide-bounds", !el.checked); });
  bind("opt-bg", function (el) {
    ["checker", "dark", "light", "green"].forEach(function (k) {
      body.classList.toggle("bg-" + k, el.value === k);
    });
  });
  document.getElementById("all-play").onclick = function () {
    players.forEach(function (p) { p.playing = p.act.frames.length > 1; p.cur = 0; p.hold = 0; });
    redraw();
  };
  document.getElementById("all-pause").onclick = function () {
    players.forEach(function (p) { p.playing = false; });
    redraw();
  };
  redraw();
  timer = setTimeout(run, opts.tick);
})();
"""


def _controls() -> str:
    ticks = "".join(
        f'<option value="{ms}"{" selected" if ms == DEFAULT_TICK_MS else ""}>{ms} ms</option>'
        for ms in TICK_MS
    )
    zooms = "".join(
        f'<option value="{z}"{" selected" if z == 3 else ""}>{z}x</option>' for z in (1, 2, 3, 4, 6)
    )
    return (
        '<div class="controls">'
        '<span class="buttons"><button id="all-play" type="button">Play all</button> '
        '<button id="all-pause" type="button">Pause all</button></span>'
        f'<label>Tick <select id="opt-tick" title="Game speed option: 65, 50 (default) or 40 ms '
        f'per tick (0x108C58)">{ticks}</select></label>'
        f'<label>Zoom <select id="opt-zoom">{zooms}</select></label>'
        '<label title="Flip the whole action (CMAction +0x14, facing == 2)">'
        '<input type="checkbox" id="opt-flip"> Flipped</label>'
        '<label title="Reset the action when it ends, as its owner does; off: hold the last '
        'frame"><input type="checkbox" id="opt-loop" checked> Loop</label>'
        '<label title="The sprite position (the anchor)"><input type="checkbox" id="opt-cross" '
        "checked> Anchor</label>"
        '<label title="The frame bounds (0x1E77D), where the draw puts them">'
        '<input type="checkbox" id="opt-bounds"> Bounds</label>'
        '<label>Background <select id="opt-bg"><option value="checker">checker</option>'
        '<option value="dark">dark</option><option value="light">light</option>'
        '<option value="green">green</option></select></label>'
        "</div>\n"
    )


def _action_section(pid: ProjectId, a: int, action: sprprj.Action, render: ActionRender) -> str:
    n = len(action.frames)
    loop = sum(sprprj.frame_ticks(f) for f in action.frames)
    timing = (
        "1 frame (static)"
        if n == 1
        else f"{n} frames, {loop} ticks ({loop * DEFAULT_TICK_MS} ms at {DEFAULT_TICK_MS} ms/tick)"
    )
    cells = []
    for i, frame in enumerate(action.frames):
        ticks = sprprj.frame_ticks(frame)
        cells.append(
            f'<div class="cell" title="delay {frame.delay}, {len(frame.layers)} layers">'
            f'<div class="box"><img src="{hp.esc(frame_file(pid, a, i, False))}" '
            f'width="{render.width}" height="{render.height}" alt="frame {i}"></div>'
            f"f{i} &middot; {ticks}t</div>"
        )
    first = hp.esc(frame_file(pid, a, 0, False))
    return (
        f'<section class="action" id="a{a}">\n'
        f'<h2>Action {a} <span class="muted">{hp.esc(timing)}; canvas {render.width}x'
        f"{render.height}, anchor ({render.anchor_x}, {render.anchor_y})</span></h2>\n"
        '<div class="player">'
        f'<div class="stage"><img src="{first}" alt="action {a}">'
        '<div class="ov bounds"></div><div class="ov cross h"></div>'
        '<div class="ov cross v"></div></div>'
        '<div><p class="buttons"><button type="button" class="prev">&lt; Step</button> '
        '<button type="button" class="play">Play</button> '
        '<button type="button" class="next">Step &gt;</button></p>'
        '<p class="status"></p></div></div>\n'
        f'<div class="cells">{"".join(cells)}</div>\n'
        "</section>\n"
    )


def render_page(r: Rendered, data: dict) -> str:
    pid, project = r.loaded.pid, r.loaded.project
    assert project is not None
    rows = []
    for i, (im, img) in enumerate(zip(project.images, r.loaded.images, strict=True)):
        size = "failed" if img is None else f"{img.width}x{img.height}"
        palette = f"{len(im.palette) // 3} colours" if im.palette else ""
        rows.append([hp.num(i), hp.text(im.name), hp.text(size), hp.text(palette)])
    images = hp.table(
        [hp.Column("#", numeric=True), hp.Column("PNG"), hp.Column("Size"), hp.Column("Palette")],
        rows,
    )
    errors = "".join(f'<li class="error">{hp.esc(e)}</li>' for e in r.errors)
    sections = "".join(
        _action_section(pid, a, project.actions[a], render)
        for a, render in sorted(r.actions.items())
    )
    body = (
        f'<p class="muted">sprite/{hp.esc(pid.folder)}/{pid.index}.sprprj: '
        f"{len(project.images)} images, {len(project.actions)} actions. Frames are composited "
        "in Python onto one canvas per action, so the anchor (the sprite position) is the same "
        "canvas pixel in every frame. A frame shows for max(delay, 1) game ticks; the frame "
        "draw puts the layers 1 px below the anchor (0x1F119).</p>\n"
        + (f"<ul>{errors}</ul>\n" if errors else "")
        + _controls()
        + sections
        + "<h2>Images</h2>\n"
        + images
        + '<script type="application/json" id="sprite-data">'
        + inline_json(data)
        + "</script>\n"
        + f"<style>{CSS}</style><script>{SCRIPT}</script>\n"
    )
    nav = hp.link("index.html", "All sprite projects")
    return hp.page(f"Sprite {pid.key}", body, nav)


def render_index(results: Sequence[Rendered], errors: Sequence[str]) -> str:
    rows, ids = [], []
    for r in results:
        pid, project = r.loaded.pid, r.loaded.project
        if project is None:
            rows.append(
                [
                    hp.Cell(hp.esc(pid.key), pid.key),
                    hp.text(pid.folder),
                    hp.num(0),
                    hp.num(0),
                    hp.num(0),
                    hp.Cell('<span class="error">failed to load</span>'),
                ]
            )
        else:
            frames = sum(len(a.frames) for a in project.actions)
            links = " ".join(
                hp.link(
                    f"{pid.slug}.html#a{a}",
                    str(a),
                    f"{len(project.actions[a].frames)} frames",
                )
                for a in sorted(r.actions)
            )
            name = hp.link(f"{pid.slug}.html", pid.key) if r.actions else hp.esc(pid.key)
            note = links or '<span class="muted">empty</span>'
            rows.append(
                [
                    hp.Cell(name, pid.key),
                    hp.text(pid.folder),
                    hp.num(len(project.images)),
                    hp.num(len(project.actions)),
                    hp.num(frames),
                    hp.Cell(note),
                ]
            )
        ids.append(pid.slug)
    table = hp.table(
        [
            hp.Column("Project"),
            hp.Column("Folder"),
            hp.Column("Images", numeric=True),
            hp.Column("Actions", numeric=True),
            hp.Column("Frames", numeric=True),
            hp.Column("Actions to play", "click an action number to open its player"),
        ],
        rows,
        ids,
    )
    errs = (
        "<ul>" + "".join(f'<li class="error">{hp.esc(e)}</li>' for e in errors) + "</ul>\n"
        if errors
        else ""
    )
    body = (
        f'<p class="muted" data-projects="{len(results)}">{len(results)} sprite projects from '
        "sprite/&lt;folder&gt;/&lt;n&gt;.sprprj (docs/formats/sprprj.md), "
        f"{len(errors)} errors.</p>\n" + errs + table
    )
    return hp.page("MapleStory Thief sprites", body)


def render(app: Path, wanted: Sequence[str] | None = None) -> tuple[dict[str, bytes], list[str]]:
    """Every output file (relative path -> bytes) and every error."""
    files: dict[str, bytes] = {}
    errors: list[str] = []
    results = []
    for r in run(app, wanted, errors):
        results.append(r)
        if r.loaded.project is None or not r.actions:
            continue
        pid = r.loaded.pid
        for a, ar in r.actions.items():
            for flip in FACINGS:
                for i, rgba in enumerate(ar.frames[flip]):
                    files[frame_file(pid, a, i, flip)] = png.encode_rgba(ar.width, ar.height, rgba)
        data = project_data(r)
        files[f"{pid.slug}.json"] = (dumps(data) + "\n").encode()
        files[f"{pid.slug}.html"] = render_page(r, data).encode()
    files["index.html"] = render_index(results, errors).encode()
    return files, errors


def run(
    app: Path, wanted: Sequence[str] | None, errors: list[str], check: bool = True
) -> Iterable[Rendered]:
    """Load and render every selected project, adding every error to ``errors``."""
    ids = select(find_projects(app, errors), wanted)
    if wanted:
        for w in wanted:
            if not any(p.key == w or p.folder == w for p in ids):
                errors.append(f"{w}: no such sprite project or folder")
    cache: ImageCache = {}
    for pid in ids:
        r = render_project(load_project(app, pid, cache), check)
        errors += r.errors
        yield r


@dataclass
class BatchReport:
    projects: int = 0
    empty: int = 0
    actions: int = 0
    frames: int = 0
    errors: list[str] = field(default_factory=list)

    def summary(self) -> str:
        return (
            f"projects={self.projects} empty={self.empty} actions={self.actions} "
            f"frames={self.frames} errors={len(self.errors)}"
        )


def batch(app: Path, wanted: Sequence[str] | None = None) -> BatchReport:
    """Load every project, render every frame of every action in both facings and check it."""
    report = BatchReport()
    for r in run(app, wanted, report.errors):
        report.projects += 1
        project = r.loaded.project
        if project is None:
            continue
        report.empty += not project.images and not project.actions
        report.actions += len(r.actions)
        report.frames += sum(len(project.actions[a].frames) for a in r.actions)
    return report


def build(app: Path, out: Path, wanted: Sequence[str] | None = None):
    files, errors = render(app, wanted)
    hp.write_dir(out, files, VIEWER)
    return files, errors
