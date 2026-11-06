"""The compendium site's stylesheet and script (ADR 0122), written as ``site.css`` and
``site.js`` beside the pages.

Themes follow ``prefers-color-scheme``. Every text colour meets WCAG AA (4.5:1) on its
background in both themes, and every mark that is not text 3:1 on the page
(``tests/test_compendium_site.py`` computes the ratios from :data:`THEMES`, :data:`PAIRS` and
:data:`MARKS`). The layout is one column below 760 px and never scrolls sideways at 320 px:
wide tables scroll inside their own box and the section tabs scroll inside the header.

The script sorts and filters tables (``htmlpage.SCRIPT``), searches the index
(``search-index.js``, loaded on first use so it also works from ``file://``) from the
header's box (and the home page's) with the keyboard (up and down to choose, Enter to open,
Escape to close, ``/`` to jump to the box), fills ``search.html``, adds the language toggle
to dialogue blocks and scrolls the current section's tab into view on a phone.
"""

from __future__ import annotations

from mtport import htmlpage as hp

# The theme (#230): warm paper and plum ink by day, a night-plum page by night, a dusk-plum
# header band in both and one maple-orange accent. name -> (light, dark).
THEMES: dict[str, tuple[str, str]] = {
    # page text and backgrounds
    "fg": ("#231d2a", "#ede7f2"),
    "muted": ("#5c5466", "#aca2b6"),
    "link": ("#a3360b", "#ffa56e"),
    "bg": ("#faf7f2", "#15121b"),
    "panel": ("#f2ece2", "#1f1a27"),
    "raised": ("#fffdf9", "#251f2e"),
    "hi": ("#fbe2c2", "#3e2b1b"),
    # the header band
    "head": ("#2a2133", "#0e0c13"),
    "head-fg": ("#f7f0e7", "#ede7f2"),
    "head-muted": ("#c8bcd0", "#aea4b8"),
    "head-accent": ("#ffa060", "#ffa060"),
    "field": ("#3b3046", "#221c2b"),
    # lines and marks (not text)
    "line": ("#ddd3c5", "#3b3346"),
    "line-strong": ("#958777", "#7d7090"),
    "accent": ("#e2620e", "#ff8a3d"),
    "focus": ("#a3360b", "#ffa56e"),
    "shadow": ("#2a213326", "#00000066"),
}
TEXT_COLOURS = ("fg", "muted", "link")
BACKGROUNDS = ("bg", "panel", "raised", "hi")
# Other text on background pairs the page uses (the contrast test checks them too).
PAIRS = (
    ("head-fg", "head"),
    ("head-muted", "head"),
    ("head-accent", "head"),
    ("head-fg", "field"),
    ("head-muted", "field"),
)
# Marks that are not text need 3:1 against the page (WCAG 1.4.11).
MARKS = (
    ("accent", "bg"),
    ("focus", "bg"),
    ("focus", "raised"),
    ("line-strong", "bg"),
    ("line-strong", "raised"),
    ("accent", "head"),
    ("head-accent", "head"),
)

SANS = (
    'system-ui, -apple-system, "Segoe UI", Roboto, "Noto Sans", "Apple SD Gothic Neo",'
    ' "Malgun Gothic", "Noto Sans KR", sans-serif'
)
SERIF = (
    '"Iowan Old Style", "Palatino Linotype", Palatino, "Book Antiqua", Georgia, "Noto Serif",'
    ' "Apple SD Gothic Neo", "Malgun Gothic", serif'
)


def _vars(i: int) -> str:
    return " ".join(f"--{k}: {v[i]};" for k, v in THEMES.items())


# The search box's magnifier, drawn in the header's muted colour (the header is dark in both
# themes).
_GLASS = (
    "url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'"
    " fill='none' stroke='%23aea4b8' stroke-width='2.4' stroke-linecap='round'%3E%3Ccircle"
    " cx='10.5' cy='10.5' r='6.5'/%3E%3Cpath d='M15.5 15.5 21 21'/%3E%3C/svg%3E\")"
)

CSS = (
    ":root { color-scheme: light dark; " + _vars(0) + " }\n"
    "@media (prefers-color-scheme: dark) { :root { "
    + _vars(1)
    + " } }\n"
    + ":root { --sans: "
    + SANS
    + "; --serif: "
    + SERIF
    + "; --glass: "
    + _GLASS
    + "; }\n"
    + """
:root { --s1: 4px; --s2: 8px; --s3: 12px; --s4: 16px; --s5: 24px; --s6: 32px; --s7: 48px;
  --radius: 10px; --page: 76rem; --gutter: 16px; }
@media (min-width: 720px) { :root { --gutter: 24px; } }
*, *::before, *::after { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; text-size-adjust: 100%; }
body { margin: 0; font: 16px/1.6 var(--sans); color: var(--fg); background: var(--bg);
  overflow-wrap: break-word; min-height: 100vh; display: flex; flex-direction: column; }
main { flex: 1 0 auto; }
a { color: var(--link); text-underline-offset: 0.15em; text-decoration-thickness: 1px; }
a:hover { text-decoration-thickness: 2px; }
:focus-visible { outline: 3px solid var(--focus); outline-offset: 2px; border-radius: 3px; }
.skip { position: absolute; left: -999px; top: 0; z-index: 20; padding: var(--s2) var(--s3);
  color: var(--fg); background: var(--raised); }
.skip:focus { left: var(--s2); top: var(--s2); }
[hidden] { display: none !important; }
.sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0);
  white-space: nowrap; }
.muted, .label, .unit { color: var(--muted); }
[lang=ko] { word-break: keep-all; }

/* header band: logo, search, section tabs */
.site-head { color: var(--head-fg); background: var(--head); }
.head-in { display: flex; flex-wrap: wrap; align-items: center; gap: var(--s3) var(--s5);
  max-width: var(--page); margin: 0 auto; padding: var(--s3) var(--gutter); }
.brand { display: inline-flex; align-items: center; gap: 10px; color: var(--head-fg);
  text-decoration: none; line-height: 1.1; flex: none; }
.brand .mark { width: 28px; height: 28px; color: var(--accent); flex: none; }
.brand-name { font: 700 1.3rem/1 var(--serif); letter-spacing: 0.01em; }
.brand-sub { display: block; font-size: 0.68rem; font-weight: 600; letter-spacing: 0.14em;
  text-transform: uppercase; color: var(--head-accent); margin-top: 3px; }
.brand:hover .brand-name { text-decoration: underline; text-decoration-thickness: 2px;
  text-underline-offset: 0.2em; }
.search { position: relative; flex: 1 1 18rem; max-width: 30rem; margin: 0 0 0 auto; }
.search input { font: inherit; font-size: 1rem; width: 100%; min-height: 42px;
  padding: 8px 12px 8px 38px; color: var(--head-fg); background: var(--field) var(--glass)
  no-repeat 11px 50% / 18px; border: 1px solid transparent; border-radius: 999px; }
.search input::placeholder { color: var(--head-muted); opacity: 1; }
.search input:hover { border-color: var(--line-strong); }
.search input:focus-visible { outline-color: var(--head-accent); }
.site-nav { max-width: var(--page); margin: 0 auto; padding: 0 var(--gutter); }
.site-nav ul { display: flex; gap: var(--s1); margin: 0 calc(-1 * var(--gutter)); padding: 0
  var(--gutter); list-style: none; overflow-x: auto; scrollbar-width: none;
  -webkit-overflow-scrolling: touch; }
.site-nav ul::-webkit-scrollbar { display: none; }
.site-nav li { flex: none; }
.site-nav a { display: block; padding: 10px 12px 9px; color: var(--head-muted);
  text-decoration: none; font-weight: 600; font-size: 0.95rem; white-space: nowrap;
  border-bottom: 3px solid transparent; }
.site-nav a:hover { color: var(--head-fg); border-bottom-color: var(--line-strong); }
.site-nav a[aria-current], .site-nav a.in { color: var(--head-fg);
  border-bottom-color: var(--accent); }
.site-nav a:focus-visible { outline-color: var(--head-accent); outline-offset: -3px; }
@media (max-width: 719px) {
  .search { flex-basis: 100%; max-width: none; margin: 0; }
  .site-nav ul { -webkit-mask-image: linear-gradient(90deg, #000 85%, transparent);
    mask-image: linear-gradient(90deg, #000 85%, transparent); }
  .site-nav ul.end { -webkit-mask-image: none; mask-image: none; }
}

/* search results: the header's list and the search page */
#q-results, .search ul[role=listbox] { position: absolute; left: 0; right: 0; top: 100%;
  z-index: 15; margin: 6px 0 0; padding: var(--s1) 0; list-style: none; color: var(--fg);
  background: var(--raised); border: 1px solid var(--line); border-radius: var(--radius);
  box-shadow: 0 12px 32px var(--shadow); max-height: 70vh; overflow: auto; }
.search li a, .results li a { display: flex; flex-wrap: wrap; align-items: baseline;
  gap: 0 var(--s2); padding: 7px 14px; color: var(--fg); text-decoration: none; }
.search li[aria-selected=true] a, .search li a:hover, .results li a:hover {
  background: var(--hi); }
.kindtag { color: var(--muted); font-size: 0.85em; }
.kindtag:last-child { margin-left: auto; }
.search-page input { font: inherit; width: 100%; max-width: 36rem; min-height: 46px;
  padding: 8px 14px; color: var(--fg); background: var(--raised);
  border: 1px solid var(--line-strong); border-radius: var(--radius); }
.results { list-style: none; padding: 0; max-width: 40rem; }
.results li { border-bottom: 1px solid var(--line); }

/* page frame */
.page-top { width: 100%; max-width: var(--page); margin: 0 auto; padding: 0 var(--gutter); }
.crumbs ol { display: flex; flex-wrap: wrap; margin: var(--s4) 0 0; padding: 0;
  list-style: none; font-size: 0.875rem; color: var(--muted); }
.crumbs li + li::before { content: "\\203A"; padding: 0 8px; }
.crumbs a { color: var(--muted); }
main { display: block; width: 100%; max-width: var(--page); margin: 0 auto;
  padding: 0 var(--gutter) var(--s7); }
main::after { content: ""; display: table; clear: both; }

/* type */
h1, h2 { font-family: var(--serif); font-weight: 700; letter-spacing: -0.005em; }
h1 { font-size: clamp(1.9rem, 1.5rem + 1.6vw, 2.6rem); line-height: 1.15;
  margin: var(--s3) 0 var(--s5); }
h2 { display: flow-root; font-size: 1.55rem; line-height: 1.25; margin: var(--s6) 0 var(--s3);
  padding-bottom: 6px; border-bottom: 1px solid var(--line); }
h3 { font-size: 1.1rem; line-height: 1.3; margin: var(--s5) 0 var(--s2); }
p, li, dd { max-width: 68ch; }
p { margin: 0 0 var(--s3); }
code { font: 0.88em/1 ui-monospace, "SF Mono", Menlo, Consolas, monospace; }
blockquote { margin: var(--s3) 0; padding: var(--s2) var(--s4); background: var(--panel);
  border-radius: var(--radius); }
img { max-width: 100%; height: auto; }
img.pixel, .infobox img, .gallery img, .thumb img, .card-pic img {
  image-rendering: pixelated; }
section { display: block; }

/* infobox */
.infobox { margin: 0 0 var(--s5); background: var(--raised); border: 1px solid var(--line);
  border-radius: 14px; overflow: hidden; box-shadow: 0 1px 2px var(--shadow); }
@media (min-width: 760px) {
  .infobox { float: right; width: 21rem; margin: 0 0 var(--s5) var(--s6); }
}
.ib-title { padding: var(--s3) var(--s4); text-align: center; background: var(--panel);
  border-bottom: 1px solid var(--line); }
.ib-head { margin: 0; font: 700 1.3rem/1.25 var(--serif); }
.ib-sub { margin: 2px 0 0; color: var(--muted); font-size: 0.95rem; }
.ib-image { display: grid; place-items: center; gap: var(--s2); min-height: 9rem; margin: 0;
  padding: var(--s5) var(--s4);
  background: radial-gradient(ellipse at 50% 85%, var(--hi), transparent 70%), var(--raised); }
.ib-image img { display: block; }
.ib-image img[width] { max-width: 100%; }
.infobox dl { display: grid; grid-template-columns: minmax(7.5rem, max-content) 1fr;
  margin: 0; padding: var(--s1) var(--s4) var(--s2); }
.infobox dt, .infobox dd { margin: 0; padding: 7px 0; border-top: 1px solid var(--line); }
.infobox dt:first-of-type, .infobox dd:first-of-type { border-top: 0; }
.infobox dt { color: var(--muted); font-size: 0.9rem; font-weight: 600; padding-right: var(--s3);
  line-height: 1.5rem; }
.infobox dd { font-variant-numeric: tabular-nums; }
.ib-foot { padding: var(--s3) var(--s4); border-top: 1px solid var(--line);
  background: var(--panel); font-size: 0.95rem; }
.ib-desc { margin: 0; font-style: italic; }
.ib-desc[lang=ko] { font-style: normal; color: var(--muted); margin-top: var(--s1); }

/* tables */
.viewer-table { margin: var(--s3) 0 var(--s5); }
.tools { display: flex; flex-wrap: wrap; align-items: center; gap: var(--s2) var(--s3);
  margin: 0 0 var(--s2); color: var(--muted); font-size: 0.875rem; }
.filter { font: inherit; font-size: 0.95rem; padding: 6px 10px; width: 16rem; max-width: 100%;
  color: var(--fg); background: var(--raised); border: 1px solid var(--line-strong);
  border-radius: 8px; }
.wrap { display: flow-root; width: fit-content; max-width: 100%; overflow-x: auto;
  border: 1px solid var(--line); border-radius: var(--radius); background: var(--raised); }
.viewer-table .wrap { max-height: 80vh; }
table { border-collapse: separate; border-spacing: 0; font-variant-numeric: tabular-nums; }
th, td { padding: 7px 14px; vertical-align: middle; text-align: left;
  border-bottom: 1px solid var(--line); }
tbody tr:last-child td { border-bottom: 0; }
th { background: var(--panel); font-size: 0.85rem; font-weight: 700; color: var(--fg);
  white-space: nowrap; }
table.sortable th { position: sticky; top: 0; z-index: 1; cursor: pointer;
  user-select: none; }
table.sortable th:hover { background: var(--hi); }
th[aria-sort=ascending]::after { content: " \\25B2"; font-size: 0.75em; }
th[aria-sort=descending]::after { content: " \\25BC"; font-size: 0.75em; }
td.num, th.num { text-align: right; white-space: nowrap; }
tbody tr:hover td { background: var(--panel); }
tr:target td { background: var(--hi); }
.entry { display: inline-flex; align-items: center; gap: 10px; min-height: 32px; }
.thumb { display: inline-grid; place-items: center; width: 36px; height: 36px; flex: none; }
.thumb img { max-width: 36px; max-height: 36px; width: auto; height: auto;
  object-fit: contain; }
.per { color: var(--muted); font-size: 0.85em; }
.drops td:nth-child(3) { white-space: nowrap; }
@media (max-width: 559px) { td .per { display: block; } }
.pct { font-weight: 600; }

/* figures */
.gallery { display: flex; flex-wrap: wrap; gap: var(--s3); margin: var(--s3) 0; }
.gallery figure { display: grid; justify-items: center; align-content: end; gap: 6px;
  min-width: 7rem; margin: 0; padding: var(--s3); text-align: center;
  background: var(--raised); border: 1px solid var(--line); border-radius: var(--radius); }
figcaption { font-size: 0.85rem; color: var(--muted); }
.chart { margin: var(--s3) 0; max-width: 40rem; }
.chart svg { width: 100%; height: auto; color: var(--fg); }

/* dialogue */
.dialogue ol { list-style: none; padding: 0; margin: var(--s2) 0; }
.dialogue li { padding: var(--s2) 0; border-bottom: 1px solid var(--line); }
.dialogue .who { font-weight: 700; margin-right: 0.5em; }
.dialogue .ko { display: block; color: var(--muted); }
.dialogue[data-show=en] .ko, .dialogue[data-show=ko] .en { display: none; }
.dialogue[data-show=ko] .ko { color: var(--fg); }
.lang-toggle button { font: inherit; font-size: 0.9rem; padding: 4px 12px; color: var(--fg);
  background: var(--raised); border: 1px solid var(--line-strong); border-radius: 999px; }
.lang-toggle button[aria-pressed=true] { font-weight: 700; color: var(--fg);
  background: var(--hi); border-color: var(--accent); }

details.spoiler { margin: var(--s3) 0; padding: var(--s2) var(--s4);
  border: 1px dashed var(--line-strong); border-radius: var(--radius);
  background: var(--panel); }
details.spoiler > summary { cursor: pointer; font-weight: 700; color: var(--fg); }
details.spoiler[open] > summary { margin-bottom: var(--s2); }

/* home */
main:has(> .hero) > h1 { margin: var(--s6) 0 var(--s4); }
.hero { display: grid; gap: var(--s4); margin: 0 0 var(--s6); padding: 0 0 var(--s6);
  border-bottom: 1px solid var(--line); }
.hero .lede { margin: 0; font-size: 1.15rem; line-height: 1.55; max-width: 40rem; }
.search.search-hero { margin: 0; max-width: 36rem; flex: none; width: 100%; }
.search-hero input { min-height: 52px; font-size: 1.05rem; padding-left: 44px;
  color: var(--fg); background-color: var(--raised); background-position: 14px 50%;
  background-size: 20px; border: 1px solid var(--line-strong);
  box-shadow: 0 2px 10px var(--shadow); }
.search-hero input::placeholder { color: var(--muted); }
.search-hero input:focus-visible { outline-color: var(--focus); }
.hero .counts { margin: 0; color: var(--muted); font-size: 0.95rem; }
.cards { display: grid; grid-template-columns: repeat(auto-fill, minmax(17rem, 1fr));
  gap: var(--s4); margin: 0; padding: 0; list-style: none; }
.cards li { max-width: none; }
.cards a { display: grid; grid-template-columns: 88px 1fr; align-items: center;
  gap: var(--s4); height: 100%; padding: var(--s4); color: var(--fg); text-decoration: none;
  background: var(--raised); border: 1px solid var(--line); border-radius: 14px;
  transition: border-color 0.2s cubic-bezier(0.25, 1, 0.5, 1),
    box-shadow 0.2s cubic-bezier(0.25, 1, 0.5, 1); }
.cards a:hover { border-color: var(--accent); box-shadow: 0 6px 20px var(--shadow); }
.card-pic { display: grid; place-items: center; width: 88px; height: 88px; overflow: hidden;
  border-radius: var(--radius);
  background: radial-gradient(ellipse at 50% 80%, var(--hi), transparent 72%), var(--panel); }
.card-pic img { max-width: 80px; max-height: 80px; width: auto; height: auto; }
.card-pic img.cover { max-width: none; max-height: none; width: 100%; height: 100%;
  object-fit: cover; image-rendering: auto; }
.card-pic .initial { font: 700 2.2rem/1 var(--serif); color: var(--accent); }
@media (max-width: 559px) {
  .cards a { grid-template-columns: 64px 1fr; gap: var(--s3); padding: var(--s3); }
  .card-pic { width: 64px; height: 64px; }
  .card-pic img { max-width: 60px; max-height: 60px; }
}
.map-figure { position: relative; display: block; width: fit-content; max-width: 100%;
  margin: var(--s3) 0; }
.map-figure img { display: block; image-rendering: pixelated; }
.map-figure .overlay { position: absolute; inset: 0; width: 100%; height: auto; }
.map-figure figcaption { margin-top: 6px; }
.card-title { display: block; font: 700 1.2rem/1.25 var(--serif); color: var(--link); }
.card-blurb { display: block; margin-top: 2px; font-size: 0.95rem; }
.card-count { display: block; margin-top: 4px; font-size: 0.85rem; color: var(--muted); }

/* footer */
.site-foot { clear: both; flex: none; padding: var(--s5) var(--gutter); font-size: 0.875rem;
  color: var(--muted); border-top: 1px solid var(--line); background: var(--panel); }
.site-foot p { margin: 0 auto; max-width: var(--page); }
.site-foot { width: 100%; }
.site-foot a { color: var(--muted); }

@media (prefers-reduced-motion: reduce) {
  *, *::before, *::after { transition: none !important; scroll-behavior: auto !important; }
}
@media print {
  .site-head, .site-foot, .tools { display: none; }
  .infobox { float: none; }
}
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
  // Every search box with a result list (the header's, the home page's): the ARIA
  // combobox pattern, up and down to choose, Enter to open, Escape to close.
  function attach(box) {
    var list = doc.getElementById(box.getAttribute("aria-controls")), active = -1;
    if (!list) return;
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
        hits.forEach(function (e, n) { list.appendChild(item(e, box.id + "-" + n)); });
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
  Array.prototype.forEach.call(doc.querySelectorAll("input[role=combobox]"), attach);
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
        var word = hits.length === 1 ? " result" : " results";
        count.textContent = input.value ? hits.length + word : "";
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

NAV_SCRIPT = r"""
(function () {
  // "/" jumps to the header's search box, as on most wikis.
  document.addEventListener("keydown", function (ev) {
    var t = ev.target, box = document.getElementById("q");
    if (ev.key !== "/" || !box || ev.ctrlKey || ev.metaKey || ev.altKey) return;
    if (t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName))) return;
    ev.preventDefault();
    box.focus();
  });
})();
(function () {
  // On a phone the section tabs scroll sideways: bring the current one into view, and drop
  // the fade at the right edge once the end is reached.
  var ul = document.querySelector(".site-nav ul");
  if (!ul) return;
  var cur = ul.querySelector("[aria-current], .in");
  if (cur && ul.scrollWidth > ul.clientWidth) {
    var li = cur.parentNode;
    ul.scrollLeft = li.offsetLeft - (ul.clientWidth - li.offsetWidth) / 2;
  }
  function edge() {
    ul.classList.toggle("end", ul.scrollLeft + ul.clientWidth >= ul.scrollWidth - 2);
  }
  ul.addEventListener("scroll", edge, { passive: true });
  edge();
})();
"""

JS = hp.SCRIPT.lstrip("\n") + SEARCH_SCRIPT + DIALOGUE_SCRIPT + NAV_SCRIPT
