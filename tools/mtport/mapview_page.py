"""The pages of :mod:`mtport.mapview`: the CSS and JavaScript of the map page, one page per
map, the index, and the ``build`` / ``check`` / ``batch`` / ``export`` drivers.
``mtport.mapview`` re-exports every name defined here.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from mtport import attfile, png
from mtport import htmlpage as hp
from mtport.mapview_render import (
    ATT_STYLES,
    Assets,
    LoadedMap,
    MapId,
    att_color,
    discover,
    export_rgba,
    load_map,
    render_att,
    render_tiles,
)

# --- Pages ---------------------------------------------------------------------------------


def _encode(lm: LoadedMap, rgba: bytes | bytearray) -> bytes:
    width, height = lm.pixel_size
    return png.encode_rgba(width, height, bytes(rgba))


def _css_rgba(color: tuple[int, int, int, int]) -> str:
    r, g, b, a = color
    return f"rgba({r}, {g}, {b}, {a / 255:.3f})"


def _legend(lm: LoadedMap) -> str:
    if lm.att is None:
        return '<p class="muted">No collision grid.</p>'
    counts = lm.att.histogram()
    rows = []
    for value in sorted(set(counts) | set(ATT_STYLES)):
        color = att_color(value)
        known = value in ATT_STYLES
        meaning = ATT_STYLES[value][1] if known else "unknown value (not in docs/formats/att.md)"
        cls = "" if known else ' class="error"'
        rows.append(
            f'<li{cls}><span class="sw" style="background:{_css_rgba(color)}"></span>'
            f"<code>0x{value:02X}</code> {hp.esc(attfile.flag_names(value))}: {hp.esc(meaning)}"
            f' <span class="muted">({counts.get(value, 0)} cells)</span></li>'
        )
    return '<ul class="legend">' + "".join(rows) + "</ul>"


def _marker(o: dict[str, Any]) -> str:
    x, y = o["anchor"]
    return (
        f'<div class="mk {hp.esc(o["category"])}" style="left:{x}px;top:{y}px"'
        f' title="{hp.esc(o["label"])}"><i></i><b>{hp.esc(o["label"])}</b></div>'
    )


OBJECT_COLUMNS = (
    hp.Column("#", numeric=True),
    hp.Column("Category"),
    hp.Column("Kind", "the .obj kind byte", numeric=True),
    hp.Column("Label"),
    hp.Column("Script", "name: script/chapter<c>/ or script/common/<name>.out"),
    hp.Column("Tile", "x, y in tiles"),
    hp.Column("Anchor", "x, y in pixels: bottom centre of the tile"),
    hp.Column("Sprite"),
)


def _objects_table(lm: LoadedMap) -> str:
    rows = []
    for o in lm.objects:
        rows.append(
            [
                hp.num(o["index"]),
                hp.text(o["category"]),
                hp.num(o["kind"]),
                hp.text(o["label"]),
                hp.text(o["name"]),
                hp.text("{}, {}".format(*o["tile"])),
                hp.text("{}, {}".format(*o["anchor"])),
                hp.text(o.get("sprite", "")),
            ]
        )
    if not rows:
        return '<p class="muted">No objects.</p>'
    return hp.table(OBJECT_COLUMNS, rows)


MAP_CSS = """
.mv-tools { display: flex; flex-wrap: wrap; gap: 4px 14px; align-items: center; margin: 8px 0; }
.mv-tools button { font: inherit; min-width: 2.2em; }
.mv-view { position: relative; overflow: hidden; height: 72vh; border: 1px solid var(--line);
  background: #000; cursor: grab; touch-action: none; outline: none; }
.mv-view.drag { cursor: grabbing; }
.mv-back { position: absolute; inset: 0; background-repeat: repeat; image-rendering: pixelated; }
.mv-stage { position: absolute; left: 0; top: 0; transform-origin: 0 0; }
.mv-stage img { position: absolute; left: 0; top: 0; image-rendering: pixelated;
  pointer-events: none; }
.mv-objs { position: absolute; left: 0; top: 0; }
.mk { position: absolute; transform: scale(var(--inv, 1)); transform-origin: 0 0; }
.mk i { position: absolute; left: -4px; top: -8px; width: 7px; height: 7px;
  border: 1px solid #000; background: var(--c); }
.mk b { position: absolute; left: 6px; top: -10px; white-space: nowrap; font: 600 11px/1.2
  system-ui, sans-serif; color: #fff; background: rgba(0, 0, 0, 0.6); padding: 0 3px;
  border-left: 3px solid var(--c); }
.mk.portal { --c: rgb(40, 120, 255); } .mk.hidden_portal { --c: rgb(150, 150, 150); }
.mk.npc { --c: rgb(0, 190, 80); } .mk.mob { --c: rgb(230, 30, 30); }
.mk.inert { --c: #fff; }
.mv-nolabels .mk b { display: none; }
.legend { list-style: none; padding: 0; columns: 2 22em; }
.sw { display: inline-block; width: 14px; height: 14px; border: 1px solid var(--line);
  vertical-align: -2px; margin-right: 6px; }
kbd { border: 1px solid var(--line); border-radius: 3px; padding: 0 4px; font-size: 12px; }
"""

# Toggles, pan (drag, arrow keys), zoom (wheel, + / -, fit, 1:1) and map cycling (n / p).
# The view state travels in the URL hash, so cycling keeps zoom and toggles. No game pixels are
# touched: only CSS transforms and visibility.
MAP_SCRIPT = """
(function () {
  var view = document.getElementById("view"), stage = document.getElementById("stage");
  var back = document.getElementById("back"), zoomOut = document.getElementById("zoom");
  var W = +stage.dataset.w, H = +stage.dataset.h, BW = +back.dataset.w, BH = +back.dataset.h;
  var ZOOMS = [0.25, 0.5, 0.75, 1, 1.5, 2, 3, 4, 6, 8];
  var z = 1, tx = 0, ty = 0;
  var boxes = Array.prototype.slice.call(document.querySelectorAll("input[data-layer]"));
  function apply() {
    stage.style.transform = "translate(" + tx + "px," + ty + "px) scale(" + z + ")";
    stage.style.setProperty("--inv", 1 / z);
    back.style.backgroundSize = BW * z + "px " + BH * z + "px";
    zoomOut.textContent = Math.round(z * 100) + "%";
  }
  function toggle() {
    boxes.forEach(function (b) {
      var on = b.checked, sel = "[data-layer='" + b.dataset.layer + "']:not(input)";
      Array.prototype.forEach.call(document.querySelectorAll(sel), function (el) {
        if (b.dataset.layer === "labels") view.classList.toggle("mv-nolabels", !on);
        else el.hidden = !on;
      });
    });
  }
  function zoomAt(nz, cx, cy) {
    nz = Math.max(ZOOMS[0], Math.min(ZOOMS[ZOOMS.length - 1], nz));
    tx = cx - (cx - tx) * nz / z; ty = cy - (cy - ty) * nz / z; z = nz; apply();
  }
  function step(d) {
    var i = 0;
    while (i < ZOOMS.length - 1 && ZOOMS[i] < z - 1e-9) i++;
    if (d > 0) i = ZOOMS[i] > z + 1e-9 ? i : i + 1; else i = i - 1;
    i = Math.max(0, Math.min(ZOOMS.length - 1, i));
    zoomAt(ZOOMS[i], view.clientWidth / 2, view.clientHeight / 2);
  }
  function fit() {
    z = Math.min(view.clientWidth / W, view.clientHeight / H);
    tx = (view.clientWidth - W * z) / 2; ty = (view.clientHeight - H * z) / 2; apply();
  }
  function state() {
    var off = boxes.filter(function (b) { return !b.checked; })
      .map(function (b) { return b.dataset.layer; });
    var on = boxes.filter(function (b) { return b.checked && b.dataset.off; })
      .map(function (b) { return b.dataset.layer; });
    return "#z=" + z + "&off=" + off.join(",") + "&on=" + on.join(",");
  }
  function go(rel) {
    var a = document.querySelector("a[rel='" + rel + "']");
    if (a) location.href = a.getAttribute("href") + state();
  }
  var h = {};
  location.hash.slice(1).split("&").forEach(function (kv) {
    var p = kv.split("="); if (p[0]) h[p[0]] = decodeURIComponent(p[1] || "");
  });
  boxes.forEach(function (b) {
    if ((h.off || "").split(",").indexOf(b.dataset.layer) >= 0) b.checked = false;
    if ((h.on || "").split(",").indexOf(b.dataset.layer) >= 0) b.checked = true;
    b.addEventListener("change", toggle);
  });
  toggle();
  if (h.z && +h.z > 0) { z = +h.z; apply(); } else fit();
  Array.prototype.forEach.call(document.querySelectorAll("button[data-zoom]"), function (b) {
    b.addEventListener("click", function () {
      var k = b.dataset.zoom;
      if (k === "fit") fit(); else if (k === "1") zoomAt(1, 0, 0); else step(k === "+" ? 1 : -1);
    });
  });
  view.addEventListener("wheel", function (e) {
    e.preventDefault();
    var r = view.getBoundingClientRect();
    zoomAt(z * (e.deltaY < 0 ? 1.25 : 0.8), e.clientX - r.left, e.clientY - r.top);
  }, { passive: false });
  var drag = null;
  view.addEventListener("pointerdown", function (e) {
    drag = [e.clientX - tx, e.clientY - ty]; view.classList.add("drag");
    view.setPointerCapture(e.pointerId); view.focus();
  });
  view.addEventListener("pointermove", function (e) {
    if (drag) { tx = e.clientX - drag[0]; ty = e.clientY - drag[1]; apply(); }
  });
  function end() { drag = null; view.classList.remove("drag"); }
  view.addEventListener("pointerup", end);
  view.addEventListener("pointercancel", end);
  document.addEventListener("keydown", function (e) {
    if (e.target.tagName === "INPUT" && e.target.type !== "checkbox") return;
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    var k = e.key, P = 64;
    if (k === "n" || k === "PageDown") go("next");
    else if (k === "p" || k === "PageUp") go("prev");
    else if (k === "+" || k === "=") step(1);
    else if (k === "-") step(-1);
    else if (k === "0") fit();
    else if (k === "ArrowLeft") { tx += P; apply(); }
    else if (k === "ArrowRight") { tx -= P; apply(); }
    else if (k === "ArrowUp") { ty += P; apply(); }
    else if (k === "ArrowDown") { ty -= P; apply(); }
    else {
      var b = boxes.filter(function (x) { return x.dataset.key === k; })[0];
      if (!b) return;
      b.checked = !b.checked; toggle();
    }
    e.preventDefault();
  });
})();
"""

INDEX_SCRIPT = """
document.addEventListener("keydown", function (e) {
  if (e.target.tagName === "INPUT" || e.metaKey || e.ctrlKey || e.altKey) return;
  var a = document.querySelector(e.key === "n" ? "a[rel='first']" : e.key === "p" ?
    "a[rel='last']" : null);
  if (a) location.href = a.getAttribute("href");
});
"""


def _back_file(lm: LoadedMap) -> str:
    return f"back/{Path(lm.back_path).name}"


def map_page(lm: LoadedMap, prev: str | None, next_: str | None) -> str:
    m = lm.map
    assert m is not None
    width, height = lm.pixel_size
    mid = lm.id
    nav = [hp.link("index.html", "All maps")]
    if prev:
        nav.append(f'<a rel="prev" href="{hp.esc(prev)}.html">&larr; {hp.esc(prev)} (p)</a>')
    if next_:
        nav.append(f'<a rel="next" href="{hp.esc(next_)}.html">{hp.esc(next_)} (n) &rarr;</a>')
    script = lm.obj.map_script if lm.obj else None
    facts = (
        f"Area {mid.area}, map {mid.map_no}: {m.width} &times; {m.height} tiles "
        f"({width} &times; {height} px), {len(m.layers)} tile layers. Tileset "
        f"<code>{hp.esc(', '.join(lm.tileset_names) or '?')}</code>, backdrop "
        f"<code>{hp.esc(lm.back_path)}</code>, map script "
        + (f"<code>{hp.esc(script)}</code>" if script else "none")
        + f", {len(lm.objects)} objects."
    )
    toggles = []

    def toggle(layer: str, label: str, key: str, on: bool = True) -> None:
        checked = " checked" if on else ' data-off="1"'
        toggles.append(
            f'<label><input type="checkbox" data-layer="{layer}" data-key="{key}"{checked}> '
            f"{hp.esc(label)} <kbd>{hp.esc(key)}</kbd></label>"
        )

    toggle("back", "backdrop", "b")
    for n in range(len(m.layers)):
        toggle(f"l{n}", f"layer {n}", str(n + 1))
    toggle("att", "collision", "a", on=False)
    toggle("obj", "objects", "o")
    toggle("labels", "labels", "l")
    imgs = "".join(
        f'<img data-layer="l{n}" src="{lm.stem}/layer{n}.png" width="{width}" height="{height}"'
        f' alt="tile layer {n}">'
        for n in range(len(m.layers))
    )
    imgs += (
        f'<img data-layer="att" src="{lm.stem}/att.png" width="{width}" height="{height}"'
        ' alt="collision overlay" hidden>'
    )
    markers = "".join(_marker(o) for o in lm.objects)
    back_w, back_h = (lm.back.width, lm.back.height) if lm.back else (0, 0)
    back_style = f' style="background-image:url({_back_file(lm)})"' if lm.back else ""
    problems = (
        "<ul>" + "".join(f'<li class="error">{hp.esc(p)}</li>' for p in lm.problems) + "</ul>"
        if lm.problems
        else "<p>No problems.</p>"
    )
    body = (
        f"<style>{MAP_CSS}</style>\n"
        f"<p>{facts}</p>\n"
        '<div class="mv-tools">' + " ".join(toggles) + " <span>zoom "
        '<button data-zoom="-" title="zoom out (-)">&minus;</button> <span id="zoom">100%</span> '
        '<button data-zoom="+" title="zoom in (+)">+</button> '
        '<button data-zoom="fit" title="fit (0)">fit</button> '
        '<button data-zoom="1">1:1</button></span></div>\n'
        '<div class="mv-view" id="view" tabindex="0">'
        f'<div class="mv-back" id="back" data-layer="back" data-w="{back_w}" data-h="{back_h}"'
        f"{back_style}></div>"
        f'<div class="mv-stage" id="stage" data-w="{width}" data-h="{height}"'
        f' style="width:{width}px;height:{height}px">{imgs}'
        f'<div class="mv-objs" data-layer="obj">{markers}</div></div></div>\n'
        '<p class="muted">Drag or arrow keys to pan, wheel or <kbd>+</kbd>/<kbd>-</kbd> to zoom, '
        "<kbd>0</kbd> to fit, <kbd>n</kbd>/<kbd>p</kbd> for the next/previous map. The backdrop "
        "stays put like in the game (drawn at screen 0, 0, never scrolled), repeated past the "
        "480 &times; 320 iPhone screen. Tile layers are drawn in order with an alpha test, as "
        f'the game bakes them. <a href="{lm.stem}/objects.json">objects.json</a></p>\n'
        "<h2>Collision</h2>\n"
        + _legend(lm)
        + "\n<h2>Objects</h2>\n"
        + _objects_table(lm)
        + "\n<h2>Problems</h2>\n"
        + problems
        + f"\n<script>{MAP_SCRIPT}</script>\n"
    )
    return hp.page(f"Map {lm.stem}", body, " · ".join(nav))


INDEX_COLUMNS = (
    hp.Column("Map", "file stem: map/<stem>.map"),
    hp.Column("Area", numeric=True),
    hp.Column("No.", "map number in the area", numeric=True),
    hp.Column("Tileset"),
    hp.Column("Tiles", "width x height in tiles"),
    hp.Column("Layers", numeric=True),
    hp.Column("Portals", numeric=True),
    hp.Column("NPCs", numeric=True),
    hp.Column("Mobs", "mob spawn points", numeric=True),
    hp.Column("Map script"),
    hp.Column("Problems", numeric=True),
)


def index_page(maps: list[LoadedMap], problems: list[str], orphans: list[str]) -> str:
    rows = []
    for lm in maps:
        cats = [o["category"] for o in lm.objects]
        m = lm.map
        rows.append(
            [
                hp.Cell(hp.link(f"{lm.stem}.html", lm.stem) if m else hp.esc(lm.stem), lm.stem),
                hp.num(lm.id.area),
                hp.num(lm.id.map_no),
                hp.text(", ".join(lm.tileset_names)),
                hp.text(f"{m.width} x {m.height}" if m else "?"),
                hp.num(len(m.layers) if m else 0),
                hp.num(cats.count("portal") + cats.count("hidden_portal")),
                hp.num(cats.count("npc")),
                hp.num(cats.count("mob")),
                hp.text((lm.obj.map_script or "") if lm.obj else ""),
                hp.num(len(lm.problems)),
            ]
        )
    problems = problems + [p for lm in maps for p in lm.problems]
    pages = [lm for lm in maps if lm.map is not None]
    first = (
        f'<a rel="first" href="{pages[0].stem}.html">first map (n)</a>, '
        f'<a rel="last" href="{pages[-1].stem}.html">last map (p)</a>'
        if pages
        else ""
    )
    summary = f"{len(maps)} maps, {len(problems)} problems. " + (
        "No problems." if not problems else '<span class="error">Listed below.</span>'
    )
    body = (
        f"<p>{summary} Open a map to see its tile layers, backdrop, collision grid and objects; "
        f"on a map page <kbd>n</kbd>/<kbd>p</kbd> go to the next/previous map. {first}.</p>\n"
        + (
            "<ul>" + "".join(f'<li class="error">{hp.esc(p)}</li>' for p in problems) + "</ul>"
            if problems
            else ""
        )
        + (
            f'<p class="muted">Files with no .map (not maps): {hp.esc(", ".join(orphans))}.</p>\n'
            if orphans
            else ""
        )
        + hp.table(INDEX_COLUMNS, rows, [lm.stem for lm in maps])
        + f"<style>kbd {{ border: 1px solid var(--line); padding: 0 4px; }}</style>"
        f"<script>{INDEX_SCRIPT}</script>\n"
    )
    return hp.page("Maps", body)


# --- Commands ------------------------------------------------------------------------------


def _select(ids: list[MapId], maps: str | None) -> tuple[list[MapId], list[str]]:
    if not maps:
        return ids, []
    wanted = [s.strip() for s in maps.split(",") if s.strip()]
    by_stem = {m.stem: m for m in ids}
    missing = [f"no map {s!r} in map/" for s in wanted if s not in by_stem]
    return [by_stem[s] for s in sorted(set(wanted) & set(by_stem), key=lambda s: by_stem[s])], (
        missing
    )


def load_all(app: Path, maps: str | None = None) -> tuple[list[LoadedMap], list[str], list[str]]:
    """Load the selected maps (default: all). Returns the maps, global problems, orphans."""
    ids, problems, orphans = discover(Path(app))
    ids, missing = _select(ids, maps)
    assets = Assets(Path(app))
    return [load_map(assets, mid) for mid in ids], problems + missing, orphans


def render(app: Path, maps: str | None = None) -> tuple[dict[str, bytes], list[str]]:
    """Every file of the viewer directory, and every problem (one line each)."""
    loaded, general, orphans = load_all(app, maps)
    problems = list(general)
    files: dict[str, bytes] = {}
    for lm in loaded:
        problems += lm.problems
    pages = [lm for lm in loaded if lm.map is not None]  # a map that didn't parse has no page
    for n, lm in enumerate(pages):
        assert lm.map is not None
        for layer in range(len(lm.map.layers)):
            files[f"{lm.stem}/layer{layer}.png"] = _encode(lm, render_tiles(lm, [layer]))
        files[f"{lm.stem}/att.png"] = _encode(lm, render_att(lm))
        files[f"{lm.stem}/objects.json"] = (
            json.dumps(lm.objects, indent=1, sort_keys=True, ensure_ascii=False) + "\n"
        ).encode("utf-8")
        if lm.back is not None and _back_file(lm) not in files:
            files[_back_file(lm)] = png.encode_rgba(lm.back.width, lm.back.height, lm.back.rgba)
        prev = pages[n - 1].stem if n > 0 else None
        next_ = pages[n + 1].stem if n + 1 < len(pages) else None
        files[f"{lm.stem}.html"] = map_page(lm, prev, next_).encode("utf-8")
    files["index.html"] = index_page(loaded, general, orphans).encode("utf-8")
    return files, problems


def batch(app: Path, maps: str | None = None) -> tuple[int, list[str]]:
    """Load and render every selected map in memory. Returns (maps rendered, problems)."""
    loaded, problems, _ = load_all(app, maps)
    for lm in loaded:
        problems += lm.problems
        if lm.map is None:
            continue
        for layer in range(len(lm.map.layers)):
            render_tiles(lm, [layer])
        export_rgba(lm, back=True, att=True, objects=True)
    return len(loaded), problems


def export(app: Path, stem: str, out: Path, **kw: bool) -> list[str]:
    """Write the composited map ``stem`` to ``out``. Returns the map's problems."""
    loaded, problems, _ = load_all(app, stem)
    if not loaded:
        return problems
    lm = loaded[0]
    if lm.map is None:
        return problems + lm.problems
    width, height = lm.pixel_size
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_bytes(png.encode_rgba(width, height, bytes(export_rgba(lm, **kw))))
    return problems + lm.problems
