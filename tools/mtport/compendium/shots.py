"""Screenshots and the layout checks of the built compendium site (#230, #231).

Serves the site on ``127.0.0.1`` and opens a fixed list of pages in headless Chrome (or
Chromium) at the widths of :data:`WIDTHS` (320 px phones to 1920 px desktops):

- **overflow:** a probe page loads every page in an ``<iframe>`` of each width and reports
  any page that scrolls sideways (the document is wider than the viewport), with the
  elements that stick out;
- **narrow tables:** at :data:`WIDE` px and up, the probe also reports any table that is
  narrower than :data:`NARROW` of its room (the width of its block, less an infobox
  floated beside it) and not marked compact (``templates.table(..., compact=True)``);
- **screenshots:** every page at every width in the light and the dark theme, as PNG files
  in ``build/compendium/shots/`` for review by eye. They are build output: never committed.

Command line (``PYTHONPATH=tools python3 -m mtport.compendium.shots ...``)::

    [--site DIR] [--out DIR] [--widths 320,390,768] [--page PATH]... [--no-screenshots]

It prints one line per page and width and exits 1 when a page scrolls sideways or has a
narrow table, 2 when no Chrome is found (``$CHROME`` names one explicitly).
"""

from __future__ import annotations

import argparse
import functools
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from collections.abc import Sequence
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from mtport.compendium.siteapi import OUT_DIR

SHOTS_DIR = Path("build/compendium/shots")
WIDTHS = (320, 390, 768, 1024, 1280, 1600, 1920)
# From this viewport width up, a table narrower than NARROW of its room (the width of its
# block, less a float beside it) is flagged, unless it is marked compact.
WIDE = 1024
NARROW = 0.6
THEMES = ("light", "dark")
# One page of each kind that exists, preferring these (they have drops, maps and images).
PREFERRED = (
    "monster/4.html",
    "monster/1.html",
    "item/equip-3.html",
    "item/etc-3.html",
    "npc/1.html",
    "npc/7.html",
    "shop/0.html",
    "shop/1.html",
    "map/0-3.html",
    "map/1-1.html",
    "area/0.html",
    "area/1.html",
    "skill/3.html",
    "world/map.html",
)
PROBE = "__mtport-probe.html"
FRAME = "__mtport-frame.html"
SHOT_HEIGHT = 1400
GAP = 24
CHROME_PATHS = (
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
)
CHROME_NAMES = ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "chrome")


def find_chrome() -> str | None:
    """``$CHROME``, else a Chrome or Chromium in the usual places, else ``None``."""
    env = os.environ.get("CHROME")
    if env:
        return env if Path(env).is_file() else None
    for p in CHROME_PATHS:
        if Path(p).is_file():
            return p
    for n in CHROME_NAMES:
        hit = shutil.which(n)
        if hit:
            return hit
    return None


def default_pages(site: Path) -> list[str]:
    """The home page, the search page, every section index and one page of each entity kind
    (a :data:`PREFERRED` one when present, else the first by name)."""
    site = Path(site)
    pages = [p for p in ("index.html", "search.html") if (site / p).is_file()]
    kinds: dict[str, list[str]] = {}
    for d in sorted(p for p in site.iterdir() if p.is_dir() and p.name != "img"):
        files = sorted(f.name for f in d.glob("*.html"))
        if "index.html" in files:
            pages.append(f"{d.name}/index.html")
        rest = [f"{d.name}/{f}" for f in files if f != "index.html"]
        if rest:
            kinds[d.name] = rest
    for _kind, files in sorted(kinds.items()):
        pick = next((p for p in PREFERRED if p in files), files[0])
        pages.append(pick)
    return pages


# The probe: loads each page in an iframe of the given width, then posts what sticks out.
_PROBE_HTML = """<!doctype html><meta charset="utf-8"><title>probe</title>
<body style="margin:0"><script>
var pages = %(pages)s, widths = %(widths)s, wide = %(wide)d, narrow = %(narrow)s;
var out = [], jobs = [];
pages.forEach(function (p) { widths.forEach(function (w) { jobs.push([p, w]); }); });
function next() {
  if (!jobs.length) {
    var x = new XMLHttpRequest();
    x.open("POST", "/__result");
    x.send(JSON.stringify(out));
    return;
  }
  var job = jobs.shift(), f = document.createElement("iframe");
  f.style.cssText = "border:0;display:block;height:900px;width:" + job[1] + "px";
  f.onload = function () {
    setTimeout(function () {
      var d = f.contentDocument, el = d.documentElement, cw = el.clientWidth, bad = [];
      Array.prototype.forEach.call(d.body.querySelectorAll("*"), function (e) {
        var r = e.getBoundingClientRect(), p = e.parentElement.getBoundingClientRect();
        if (r.width && r.right > cw + 1 && p.right <= cw + 1) {
          var cls = typeof e.className === "string" && e.className ?
            "." + e.className.split(" ").join(".") : "";
          bad.push(e.tagName.toLowerCase() + (e.id ? "#" + e.id : "") + cls +
            " (" + Math.round(r.right) + " px)");
        }
      });
      out.push({page: job[0], width: job[1], scroll: el.scrollWidth, client: cw,
                overflow: el.scrollWidth > cw, offenders: bad.slice(0, 5),
                narrow: job[1] >= wide ? narrowTables(d) : []});
      f.remove();
      next();
    }, 50);
  };
  f.src = job[0];
  document.body.appendChild(f);
}
// Tables that leave more than 40%% of their room empty: the room is the width of the
// table's block (.viewer-table) less any float that sits beside the table.
function narrowTables(d) {
  var found = [], floats = [];
  Array.prototype.forEach.call(d.body.querySelectorAll("*"), function (e) {
    if (d.defaultView.getComputedStyle(e).cssFloat !== "none") floats.push(e);
  });
  Array.prototype.forEach.call(d.querySelectorAll("table"), function (t, n) {
    var r = t.getBoundingClientRect();
    if (!r.width || t.closest(".compact")) return;
    var box = (t.closest(".viewer-table") || t.parentElement).getBoundingClientRect();
    var left = box.left, right = box.right;
    floats.forEach(function (f) {
      var fr = f.getBoundingClientRect();
      if (fr.bottom <= r.top || fr.top >= r.bottom) return;
      if (fr.left >= r.right - 1) right = Math.min(right, fr.left);
      else if (fr.right <= r.left + 1) left = Math.max(left, fr.right);
    });
    var room = right - left;
    if (room > 0 && r.width < narrow * room) {
      var where = t.closest("section[id], section[aria-labelledby]");
      var name = where ? (where.id || where.getAttribute("aria-labelledby")) : "table " + n;
      found.push(name + " (" + Math.round(r.width) + " of " + Math.round(room) + " px)");
    }
  });
  return found;
}
next();
</script>
"""


# A frame page for screenshots: the page in an iframe of each width, side by side. Chrome
# does not shrink its window below about 500 px, so a phone width needs a frame of its own.
_FRAME_HTML = """<!doctype html><meta charset="utf-8"><title>frame</title>
<body style="margin:0;background:#808080;display:flex;gap:%(gap)dpx;align-items:flex-start">
<script>
var q = new URLSearchParams(location.search);
q.get("w").split(",").forEach(function (w) {
  var f = document.createElement("iframe");
  f.style.cssText = "border:0;flex:none;height:%(h)dpx;width:" + w + "px";
  f.src = q.get("page");
  document.body.appendChild(f);
});
</script>
"""


class _Server:
    """``site`` over HTTP on a free local port, plus the probe page and its result."""

    def __init__(self, site: Path, pages: Sequence[str], widths: Sequence[int]):
        self.result: list[dict] | None = None
        self.done = threading.Event()
        probe = _PROBE_HTML % {
            "pages": json.dumps(list(pages)),
            "widths": json.dumps(list(widths)),
            "wide": WIDE,
            "narrow": json.dumps(NARROW),
        }
        frame = _FRAME_HTML % {"gap": GAP, "h": SHOT_HEIGHT}
        outer = self

        class Handler(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *args):  # quiet
                pass

            def do_GET(self):
                route = self.path.split("?")[0]
                if route in ("/" + PROBE, "/" + FRAME):
                    body = (probe if route == "/" + PROBE else frame).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                try:
                    super().do_GET()
                except (BrokenPipeError, ConnectionResetError):
                    pass  # the probe moved on to the next page while an image was loading

            def do_POST(self):
                n = int(self.headers.get("Content-Length") or 0)
                data = self.rfile.read(n)
                self.send_response(204)
                self.end_headers()
                if self.path == "/__result":
                    outer.result = json.loads(data.decode("utf-8"))
                    outer.done.set()

        handler = functools.partial(Handler, directory=str(site))
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}/"
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def __enter__(self) -> _Server:
        self.thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()


def _chrome_args(chrome: str, profile: str, theme: str, w: int, h: int) -> list[str]:
    scheme = 0 if theme == "dark" else 1  # Blink's PreferredColorScheme: 0 dark, 1 light
    root = getattr(os, "geteuid", lambda: 1)() == 0  # Chrome refuses root without this
    return [
        chrome,
        *(["--no-sandbox"] if root else []),
        "--headless=new",
        "--disable-gpu",
        "--hide-scrollbars",
        "--no-first-run",
        "--no-default-browser-check",
        f"--user-data-dir={profile}",
        f"--blink-settings=preferredColorScheme={scheme}",
        f"--window-size={w},{h}",
    ]


def overflow(
    chrome: str,
    site: Path,
    pages: Sequence[str],
    widths: Sequence[int] = WIDTHS,
    timeout: float = 180,
) -> list[dict]:
    """One result per page and width: ``{page, width, scroll, client, overflow,
    offenders}``. Raises ``RuntimeError`` when Chrome does not report back in time."""
    with _Server(Path(site), pages, widths) as srv, tempfile.TemporaryDirectory() as prof:
        args = _chrome_args(chrome, prof, "light", max(widths) + 40, 1000)
        proc = subprocess.Popen(
            [*args, "--remote-debugging-port=0", srv.base + PROBE],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            if not srv.done.wait(timeout):
                raise RuntimeError("Chrome did not report the probe's result in time")
        finally:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        return list(srv.result or [])


def shot_groups(widths: Sequence[int]) -> list[tuple[int, ...]]:
    """Phone widths (below 600 px) share one screenshot, side by side; wider ones get one
    each."""
    phones = tuple(w for w in widths if w < 600)
    return ([phones] if phones else []) + [(w,) for w in widths if w >= 600]


def shot_name(page: str, widths: Sequence[int], theme: str) -> str:
    stem = page[: -len(".html")].replace("/", "_")
    return f"{stem}_{'-'.join(map(str, widths))}_{theme}.png"


def screenshots(
    chrome: str,
    site: Path,
    pages: Sequence[str],
    out: Path,
    widths: Sequence[int] = WIDTHS,
    themes: Sequence[str] = THEMES,
) -> list[Path]:
    """Every page in every theme, as PNG files in ``out``: the phone widths side by side in
    one file, each wider width in a file of its own."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    jobs = [(p, g, t) for p in pages for g in shot_groups(widths) for t in themes]
    with _Server(Path(site), pages, widths) as srv:

        def one(job) -> Path:
            page, group, theme = job
            target = out / shot_name(page, group, theme)
            w = sum(group) + GAP * (len(group) - 1)
            url = f"{srv.base}{FRAME}?page={page}&w={','.join(map(str, group))}"
            with tempfile.TemporaryDirectory() as prof:
                args = _chrome_args(chrome, prof, theme, max(w, 600), SHOT_HEIGHT)
                subprocess.run(
                    [*args, "--virtual-time-budget=3000", f"--screenshot={target}", url],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=60,
                    check=False,
                )
            return target

        with ThreadPoolExecutor(max_workers=4) as pool:
            return list(pool.map(one, jobs))


def report_line(r: dict) -> str:
    """One line of the report for a probe result: ``ok``, ``OVERFLOW`` (the page scrolls
    sideways, with what sticks out) or ``NARROW`` (a table uses too little of its room)."""
    state = "OVERFLOW" if r["overflow"] else "NARROW" if r.get("narrow") else "ok"
    extra = ""
    if r["overflow"]:
        extra += f" {r['scroll']} > {r['client']} px: {', '.join(r['offenders'])}"
    if r.get("narrow"):
        extra += f" narrow table: {', '.join(r['narrow'])}"
    return f"{state:8} {r['width']:5} px  {r['page']}{extra}"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="python3 -m mtport.compendium.shots", description=__doc__.split("\n\n")[0]
    )
    ap.add_argument("--site", type=Path, default=OUT_DIR, help=f"the site (default {OUT_DIR})")
    ap.add_argument("--out", type=Path, default=SHOTS_DIR, help=f"PNGs (default {SHOTS_DIR})")
    ap.add_argument("--widths", default=",".join(map(str, WIDTHS)), help="viewport widths")
    ap.add_argument("--page", action="append", help="a site path (default: one of each kind)")
    ap.add_argument("--no-screenshots", action="store_true", help="only the overflow check")
    args = ap.parse_args(argv)
    chrome = find_chrome()
    if chrome is None:
        print("error: no Chrome or Chromium found (set $CHROME)", file=sys.stderr)
        return 2
    if not (args.site / "index.html").is_file():
        print(
            f"error: {args.site}: no built site (run mtport.compendium.site build)", file=sys.stderr
        )
        return 1
    widths = [int(w) for w in args.widths.split(",") if w]
    pages = args.page or default_pages(args.site)
    results = overflow(chrome, args.site, pages, widths)
    bad = narrow = 0
    for r in sorted(results, key=lambda r: (r["page"], r["width"])):
        print(report_line(r))
        bad += bool(r["overflow"])
        narrow += bool(r.get("narrow"))
    if len(results) != len(pages) * len(widths):
        print(f"error: {len(results)} results for {len(pages) * len(widths)} page widths")
        return 1
    if not args.no_screenshots:
        files = screenshots(chrome, args.site, pages, args.out, widths)
        print(f"shots: {len(files)} screenshots in {args.out}")
    print(
        f"shots: {len(pages)} pages x {len(widths)} widths, {bad} with sideways scroll,"
        f" {narrow} with a narrow table"
    )
    return 1 if bad or narrow else 0


if __name__ == "__main__":
    sys.exit(main())
