"""The compendium site's stylesheet and script (ADR 0122), written as ``site.css`` and
``site.js`` beside the pages.

Themes follow ``prefers-color-scheme``. Every text colour meets WCAG AA (4.5:1) on its
background in both themes (``tests/test_compendium_site.py`` computes the ratios from
:data:`THEMES`). The layout is one column below 720 px and never scrolls sideways at 320 px:
wide tables scroll inside their own box.

The script sorts and filters tables (``htmlpage.SCRIPT``), searches the index
(``search-index.js``, loaded on first use so it also works from ``file://``) from the
header's box with the keyboard (up and down to choose, Enter to open, Escape to close),
fills ``search.html``, and adds the language toggle to dialogue blocks.
"""

from __future__ import annotations

from mtport import htmlpage as hp

# name -> (light, dark). "bg" and "panel" are backgrounds; the rest are text colours.
THEMES: dict[str, tuple[str, str]] = {
    "fg": ("#1b1b1f", "#ececf1"),
    "bg": ("#ffffff", "#141417"),
    "panel": ("#f3f3f6", "#1f1f24"),
    "muted": ("#55555f", "#a9a9b4"),
    "link": ("#0b57d0", "#8ab4f8"),
    "line": ("#c9c9d3", "#3d3d46"),
    "hi": ("#fff3c4", "#3d3418"),
    "focus": ("#0b57d0", "#8ab4f8"),
}
TEXT_COLOURS = ("fg", "muted", "link")
BACKGROUNDS = ("bg", "panel", "hi")


def _vars(i: int) -> str:
    return " ".join(f"--{k}: {v[i]};" for k, v in THEMES.items())


CSS = (
    ":root { color-scheme: light dark; " + _vars(0) + " }\n"
    "@media (prefers-color-scheme: dark) { :root { "
    + _vars(1)
    + " } }\n"
    + """
*, *::before, *::after { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; }
body { margin: 0; font: 16px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, "Noto Sans",
  "Apple SD Gothic Neo", "Malgun Gothic", sans-serif; color: var(--fg); background: var(--bg);
  overflow-wrap: anywhere; }
a { color: var(--link); }
a:hover { text-decoration-thickness: 2px; }
:focus-visible { outline: 3px solid var(--focus); outline-offset: 2px; }
.skip { position: absolute; left: -999px; top: 0; background: var(--bg); padding: 8px; }
.skip:focus { left: 8px; z-index: 10; }
.sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0);
  white-space: nowrap; }
.muted, .label { color: var(--muted); }
.site-head { display: flex; flex-wrap: wrap; gap: 8px 16px; align-items: center;
  padding: 8px 16px; background: var(--panel); border-bottom: 1px solid var(--line); }
.brand { font-weight: 700; text-decoration: none; color: var(--fg); }
.search { position: relative; flex: 1 1 16em; max-width: 28em; margin: 0; }
.search input, .search-page input { font: inherit; width: 100%; padding: 6px 10px;
  color: var(--fg); background: var(--bg); border: 1px solid var(--line); border-radius: 6px; }
#q-results { position: absolute; left: 0; right: 0; top: 100%; z-index: 5; margin: 4px 0 0;
  padding: 4px 0; list-style: none; background: var(--bg); border: 1px solid var(--line);
  border-radius: 6px; max-height: 70vh; overflow: auto; }
#q-results li a, .results li a { display: block; padding: 4px 10px; text-decoration: none; }
#q-results li[aria-selected=true] a { background: var(--hi); }
.kindtag { color: var(--muted); font-size: 0.85em; margin-left: 0.5em; }
.site-nav { padding: 0 16px; border-bottom: 1px solid var(--line); }
.site-nav ul { display: flex; flex-wrap: wrap; gap: 0 16px; margin: 0; padding: 6px 0;
  list-style: none; }
.site-nav a { text-decoration: none; }
.site-nav a[aria-current] { font-weight: 700; text-decoration: underline; }
.crumbs ol { display: flex; flex-wrap: wrap; margin: 8px 0 0; padding: 0; list-style: none;
  font-size: 0.9em; color: var(--muted); }
.crumbs li + li::before { content: "\\203A"; padding: 0 6px; }
main { display: block; max-width: 72rem; margin: 0 auto; padding: 0 16px 32px; }
.page-top { padding: 0 16px; max-width: 72rem; margin: 0 auto; }
h1 { font-size: 1.75rem; line-height: 1.25; margin: 12px 0 8px; }
h2 { font-size: 1.35rem; margin: 24px 0 8px; clear: both; }
h3 { font-size: 1.1rem; margin: 16px 0 6px; }
p, li, dd { max-width: 44em; }
code { font-size: 0.9em; }
blockquote { margin: 8px 0; padding: 4px 12px; border-left: 4px solid var(--line); }
img { max-width: 100%; height: auto; }
img.pixel, .infobox img, .gallery img { image-rendering: pixelated; }
.infobox { background: var(--panel); border: 1px solid var(--line); border-radius: 6px;
  padding: 8px 12px; margin: 0 0 16px; }
@media (min-width: 720px) { .infobox { float: right; width: 20rem; margin: 0 0 16px 24px; } }
.ib-head { font-weight: 700; font-size: 1.1em; margin: 0 0 8px; text-align: center; }
.ib-image { margin: 0 0 8px; text-align: center; }
.infobox dl { display: grid; grid-template-columns: max-content 1fr; gap: 2px 12px; margin: 0; }
.infobox dt { color: var(--muted); }
.infobox dd { margin: 0; }
.wrap { overflow-x: auto; max-width: 100%; }
.viewer-table .wrap { max-height: 80vh; }
.tools { margin: 8px 0; }
.filter { font: inherit; padding: 4px 8px; width: 16em; max-width: 100%; color: var(--fg);
  background: var(--bg); border: 1px solid var(--line); border-radius: 4px; }
table { border-collapse: collapse; }
th, td { border: 1px solid var(--line); padding: 3px 8px; vertical-align: top; text-align: left; }
th { background: var(--panel); }
table.sortable th { position: sticky; top: 0; cursor: pointer; white-space: nowrap; }
th[aria-sort=ascending]::after { content: " \\25B2"; }
th[aria-sort=descending]::after { content: " \\25BC"; }
td.num, th.num { text-align: right; white-space: nowrap; }
tr:target { background: var(--hi); }
.gallery { display: flex; flex-wrap: wrap; gap: 12px; }
.gallery figure { margin: 0; text-align: center; }
figcaption { font-size: 0.9em; color: var(--muted); }
.chart { margin: 8px 0; max-width: 40rem; }
.chart svg { width: 100%; height: auto; color: var(--fg); }
.dialogue ol { list-style: none; padding: 0; margin: 8px 0; }
.dialogue li { padding: 4px 0; border-bottom: 1px solid var(--line); }
.dialogue .who { font-weight: 700; margin-right: 0.5em; }
.dialogue .ko { display: block; color: var(--muted); }
.dialogue[data-show=en] .ko, .dialogue[data-show=ko] .en { display: none; }
.dialogue[data-show=ko] .ko { color: var(--fg); }
.lang-toggle button { font: inherit; padding: 2px 8px; color: var(--fg);
  background: var(--panel); border: 1px solid var(--line); border-radius: 4px; }
.lang-toggle button[aria-pressed=true] { font-weight: 700; border-color: var(--focus); }
.site-foot { clear: both; margin-top: 32px; padding: 12px 16px; font-size: 0.9em;
  color: var(--muted); border-top: 1px solid var(--line); background: var(--panel); }
.site-foot p { margin: 0 auto; max-width: 72rem; }
.cards { display: grid; grid-template-columns: repeat(auto-fill, minmax(14rem, 1fr));
  gap: 12px; padding: 0; list-style: none; }
.cards li { border: 1px solid var(--line); border-radius: 6px; padding: 8px 12px; }
.cards a { font-weight: 700; }
"""
)

SEARCH_SCRIPT = r"""
(function () {
  var doc = document, root = doc.documentElement.getAttribute("data-root") || "";
  var data = null, waiting = [];
  function norm(s) { return String(s || "").normalize("NFKC").toLowerCase().trim(); }
  function load(cb) {
    if (data) { cb(data); return; }
    waiting.push(cb);
    if (waiting.length > 1) return;
    var s = doc.createElement("script");
    s.src = root + "search-index.js";
    s.onload = function () {
      data = prepare(window.MTSEARCH || []);
      waiting.forEach(function (f) { f(data); });
      waiting = [];
    };
    doc.head.appendChild(s);
  }
  function prepare(entries) {
    return entries.map(function (e) {
      e.h = [norm(e.t), norm(e.ko)].concat((e.a || []).map(norm));
      return e;
    });
  }
  function find(index, q, limit) {
    q = norm(q);
    if (!q) return [];
    var hits = [];
    index.forEach(function (e, n) {
      var best = 9;
      e.h.forEach(function (h) {
        if (!h) return;
        var s = h === q ? 0 : h.indexOf(q) === 0 ? 1 : h.indexOf(q) > 0 ? 2 : 9;
        if (s < best) best = s;
      });
      if (best < 9) hits.push([best, n, e]);
    });
    hits.sort(function (a, b) { return a[0] - b[0] || a[1] - b[1]; });
    return hits.slice(0, limit || hits.length).map(function (h) { return h[2]; });
  }
  window.MTSEARCHFN = { prepare: prepare, find: find };
  function item(e, n) {
    var li = doc.createElement("li"), a = doc.createElement("a");
    li.setAttribute("role", "option");
    li.id = "qr-" + n;
    a.href = root + e.u;
    a.tabIndex = -1;
    a.appendChild(doc.createTextNode(e.t));
    if (e.ko && e.ko !== e.t) {
      var ko = doc.createElement("span");
      ko.lang = "ko";
      ko.className = "kindtag";
      ko.textContent = e.ko;
      a.appendChild(ko);
    }
    var k = doc.createElement("span");
    k.className = "kindtag";
    k.textContent = e.k;
    a.appendChild(k);
    li.appendChild(a);
    return li;
  }
  var box = doc.getElementById("q"), list = doc.getElementById("q-results"), active = -1;
  if (box && list) {
    function close() {
      list.hidden = true;
      box.setAttribute("aria-expanded", "false");
      box.removeAttribute("aria-activedescendant");
      active = -1;
    }
    function mark(n) {
      var items = list.children;
      if (!items.length) return;
      active = (n + items.length) % items.length;
      for (var i = 0; i < items.length; i++) {
        items[i].setAttribute("aria-selected", i === active ? "true" : "false");
      }
      box.setAttribute("aria-activedescendant", items[active].id);
      items[active].scrollIntoView({ block: "nearest" });
    }
    function show() {
      load(function (index) {
        var hits = find(index, box.value, 12);
        list.textContent = "";
        hits.forEach(function (e, n) { list.appendChild(item(e, n)); });
        active = -1;
        if (hits.length) {
          list.hidden = false;
          box.setAttribute("aria-expanded", "true");
        } else {
          close();
        }
      });
    }
    box.addEventListener("focus", function () { load(function () {}); });
    box.addEventListener("input", show);
    box.addEventListener("keydown", function (ev) {
      if (ev.key === "ArrowDown") {
        ev.preventDefault();
        if (list.hidden) show(); else mark(active + 1);
      }
      else if (ev.key === "ArrowUp") { ev.preventDefault(); mark(active - 1); }
      else if (ev.key === "Escape") { close(); }
      else if (ev.key === "Enter" && !list.hidden && list.children.length) {
        ev.preventDefault();
        var li = list.children[active < 0 ? 0 : active];
        window.location.href = li.firstChild.href;
      }
    });
    box.addEventListener("blur", function () { setTimeout(close, 200); });
  }
  var page = doc.getElementById("search-page");
  if (page) {
    var q = "", m = /[?&]q=([^&]*)/.exec(window.location.search);
    if (m) q = decodeURIComponent(m[1].replace(/\+/g, " "));
    var out = doc.getElementById("search-results"), count = doc.getElementById("search-count");
    var input = doc.getElementById("sq");
    function run() {
      load(function (index) {
        var hits = find(index, input.value);
        out.textContent = "";
        hits.forEach(function (e, n) {
          var li = item(e, "p" + n);
          li.removeAttribute("role");
          li.firstChild.removeAttribute("tabindex");
          out.appendChild(li);
        });
        count.textContent = input.value ? hits.length + (hits.length === 1 ? " result" : " results") : "";
      });
    }
    input.value = q;
    input.addEventListener("input", run);
    run();
  }
})();
"""

DIALOGUE_SCRIPT = r"""
(function () {
  Array.prototype.forEach.call(document.querySelectorAll(".dialogue"), function (d) {
    var bar = d.querySelector(".lang-toggle");
    if (!bar) return;
    bar.hidden = false;
    Array.prototype.forEach.call(bar.querySelectorAll("button"), function (b) {
      b.addEventListener("click", function () {
        d.setAttribute("data-show", b.getAttribute("data-lang"));
        Array.prototype.forEach.call(bar.querySelectorAll("button"), function (o) {
          o.setAttribute("aria-pressed", o === b ? "true" : "false");
        });
      });
    });
  });
})();
"""

JS = hp.SCRIPT.lstrip("\n") + SEARCH_SCRIPT + DIALOGUE_SCRIPT
