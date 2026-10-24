"""Scaffolding shared by the generated static HTML viewers (ADR 0010).

The viewers under ``assets-out/viewers/<name>/`` are plain pages that open from ``file://``:
inline CSS, a little hand-written inline JavaScript, no framework, no CDN, no network access.
This module holds what every viewer needs and nothing more:

- :func:`esc` HTML-escapes untrusted text (every string that comes from the game's files);
- :func:`page` wraps a body in the page shell (UTF-8, base CSS, the table script);
- :func:`table` renders a sortable, filterable table from :class:`Column` and :class:`Cell`;
- :func:`write_dir` / :func:`diff_dir` write a viewer's own output directory deterministically
  and compare it with what a fresh render would write (the batch ``check`` modes).

Output is deterministic: no timestamps, no randomness, files written in sorted order.
"""

from __future__ import annotations

import html
import shutil
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

# A viewer's output directory holds this file, naming the viewer. write_dir only replaces a
# directory that is empty or carries the marker of the same viewer, so a mistyped --out can
# never wipe unrelated files.
MARKER = ".mtport-viewer"


def esc(value: object) -> str:
    """HTML-escape ``value`` (``str()`` of it) for text and for quoted attribute values."""
    return html.escape(str(value), quote=True)


@dataclass(frozen=True)
class Column:
    """One table column. ``numeric`` columns sort by number; ``tip`` becomes the header's
    tooltip."""

    title: str
    tip: str = ""
    numeric: bool = False


@dataclass(frozen=True)
class Cell:
    """One table cell: ``markup`` is already-safe HTML, ``key`` the sort key (``None``: the
    cell's text). ``lang`` sets the cell's language (e.g. ``"ko"``)."""

    markup: str
    key: str | int | float | None = None
    lang: str = ""


def text(value: str, lang: str = "") -> Cell:
    """A cell showing untrusted text, escaped."""
    return Cell(esc(value), None, lang)


def num(value: int | float, label: str = "") -> Cell:
    """A numeric cell, optionally followed by a (escaped) label; sorts by ``value``."""
    shown = esc(value) + (f' <span class="label">{esc(label)}</span>' if label else "")
    return Cell(shown, value)


def link(href: str, label: str, title: str = "") -> str:
    """An ``<a>`` element (escaped ``href``, label and title)."""
    t = f' title="{esc(title)}"' if title else ""
    return f'<a href="{esc(href)}"{t}>{esc(label)}</a>'


def table(columns: Sequence[Column], rows: Sequence[Sequence[Cell]], row_ids=None) -> str:
    """A sortable table (click a header; again to reverse) with a filter box above it.

    Each body row is ``<tr data-i="n">`` (n = original position, the sort's tie-break), with
    ``id`` from ``row_ids[n]`` when given, so pages can link to ``page.html#id``.
    """
    head = []
    for c in columns:
        cls = ' class="num"' if c.numeric else ""
        tip = f' title="{esc(c.tip)}"' if c.tip else ""
        head.append(f'<th{cls}{tip} tabindex="0">{esc(c.title)}</th>')
    body = []
    for n, row in enumerate(rows):
        if len(row) != len(columns):
            raise ValueError(f"row {n} has {len(row)} cells, the table {len(columns)} columns")
        rid = f' id="{esc(row_ids[n])}"' if row_ids is not None else ""
        cells = []
        for col, cell in zip(columns, row, strict=True):
            attrs = ' class="num"' if col.numeric else ""
            if cell.key is not None:
                attrs += f' data-k="{esc(cell.key)}"'
            if cell.lang:
                attrs += f' lang="{esc(cell.lang)}"'
            cells.append(f"<td{attrs}>{cell.markup}</td>")
        body.append(f'<tr data-i="{n}"{rid}>' + "".join(cells) + "</tr>")
    return (
        '<div class="viewer-table">\n'
        '<p class="tools"><input type="search" class="filter" placeholder="Filter rows"'
        ' aria-label="Filter rows"> <span class="shown"></span></p>\n'
        '<div class="wrap"><table class="sortable">\n<thead><tr>'
        + "".join(head)
        + "</tr></thead>\n<tbody>\n"
        + "\n".join(body)
        + "\n</tbody>\n</table></div>\n</div>\n"
    )


CSS = """
:root { color-scheme: light dark; --fg: #1d1d1f; --bg: #fff; --muted: #6e6e73;
  --line: #d2d2d7; --head: #f5f5f7; --hi: #fff6d6; --link: #0a58ca; }
@media (prefers-color-scheme: dark) {
  :root { --fg: #f5f5f7; --bg: #161617; --muted: #a1a1a6; --line: #3a3a3c;
    --head: #232325; --hi: #3b3420; --link: #6ea8fe; }
}
body { margin: 0 16px 32px; font: 14px/1.4 system-ui, -apple-system, "Segoe UI", sans-serif;
  color: var(--fg); background: var(--bg); }
h1 { font-size: 20px; margin: 16px 0 4px; }
p, li { max-width: 72em; }
a { color: var(--link); }
.muted, .label { color: var(--muted); }
.error { color: #d70015; font-weight: 600; }
.tools { margin: 8px 0; }
.filter { font: inherit; padding: 4px 8px; width: 16em; }
.wrap { overflow: auto; max-height: 85vh; }
table { border-collapse: collapse; }
th, td { border: 1px solid var(--line); padding: 2px 6px; vertical-align: top; }
th { background: var(--head); position: sticky; top: 0; cursor: pointer; white-space: nowrap;
  text-align: left; }
th[aria-sort=ascending]::after { content: " \\25B2"; }
th[aria-sort=descending]::after { content: " \\25BC"; }
td { white-space: pre-wrap; }
td.num, th.num { text-align: right; white-space: nowrap; }
tr:target { background: var(--hi); }
img.icon { image-rendering: pixelated; width: 40px; height: auto; vertical-align: middle; }
"""

# Sort on header click (numbers by value, text by locale), stable by original row order;
# filter rows by substring. Works from file:// and touches nothing outside the page.
SCRIPT = """
(function () {
  function key(td) {
    var k = td.getAttribute("data-k");
    return k === null ? td.textContent : k;
  }
  Array.prototype.forEach.call(document.querySelectorAll(".viewer-table"), function (v) {
    var t = v.querySelector("table.sortable");
    var heads = t.tHead.rows[0].cells, body = t.tBodies[0];
    Array.prototype.forEach.call(heads, function (th, i) {
      function sort() {
        var dir = th.getAttribute("aria-sort") === "ascending" ? -1 : 1;
        var numeric = th.classList.contains("num");
        Array.prototype.forEach.call(heads, function (h) { h.removeAttribute("aria-sort"); });
        th.setAttribute("aria-sort", dir > 0 ? "ascending" : "descending");
        var rows = Array.prototype.slice.call(body.rows);
        rows.sort(function (a, b) {
          var x = key(a.cells[i]), y = key(b.cells[i]);
          var c = numeric ? Number(x) - Number(y) : x.localeCompare(y);
          return c * dir || a.getAttribute("data-i") - b.getAttribute("data-i");
        });
        rows.forEach(function (r) { body.appendChild(r); });
      }
      th.addEventListener("click", sort);
      th.addEventListener("keydown", function (e) { if (e.key === "Enter") sort(); });
    });
    var box = v.querySelector(".filter"), shown = v.querySelector(".shown");
    function filter() {
      var q = box.value.toLowerCase(), n = 0;
      Array.prototype.forEach.call(body.rows, function (r) {
        var hit = r.textContent.toLowerCase().indexOf(q) >= 0;
        r.hidden = !hit;
        n += hit;
      });
      shown.textContent = n + " of " + body.rows.length + " rows";
    }
    box.addEventListener("input", filter);
    filter();
  });
})();
"""


def page(title: str, body: str, nav: str = "") -> str:
    """A complete HTML document: ``title`` (escaped), ``nav`` and ``body`` (safe markup)."""
    return (
        "<!doctype html>\n"
        '<html lang="en">\n<head>\n<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
        f"<title>{esc(title)}</title>\n<style>{CSS}</style>\n</head>\n<body>\n"
        + (f"<nav>{nav}</nav>\n" if nav else "")
        + f"<h1>{esc(title)}</h1>\n"
        + body
        + f"<script>{SCRIPT}</script>\n</body>\n</html>\n"
    )


def write_dir(out: Path, files: Mapping[str, bytes], viewer: str) -> None:
    """Replace the viewer's output directory ``out`` with exactly ``files`` (relative path ->
    bytes) plus the :data:`MARKER` file. Refuses a non-empty directory without the marker of
    the same ``viewer``."""
    out = Path(out)
    if out.exists():
        marker = out / MARKER
        owned = marker.is_file() and marker.read_text(encoding="utf-8").strip() == viewer
        if not owned and any(out.iterdir()):
            raise FileExistsError(
                f"{out}: not empty and not a {viewer} viewer directory (no {MARKER}); "
                "refusing to replace it"
            )
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for rel in sorted(files):
        path = out / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(files[rel])
    (out / MARKER).write_text(viewer + "\n", encoding="utf-8")


def diff_dir(out: Path, files: Mapping[str, bytes]) -> list[str]:
    """How ``out`` differs from ``files``: one line per missing, stale or extra file."""
    out = Path(out)
    if not out.is_dir():
        return [f"{out}: missing (run the build command)"]
    problems = []
    for rel in sorted(files):
        path = out / rel
        if not path.is_file():
            problems.append(f"{rel}: missing")
        elif path.read_bytes() != files[rel]:
            problems.append(f"{rel}: stale (differs from a fresh build)")
    on_disk = {p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file()}
    for rel in sorted(on_disk - set(files) - {MARKER}):
        problems.append(f"{rel}: not part of a fresh build")
    return problems
