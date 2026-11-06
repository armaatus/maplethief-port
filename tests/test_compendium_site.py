"""The compendium site generator (#215, ADR 0122) on a synthetic knowledge base."""

from __future__ import annotations

import functools
import http.server
import json
import re
import shutil
import subprocess
import threading
import urllib.parse
import urllib.request
from pathlib import Path

import pytest

from mtport.compendium import assets, defaults, prose, site
from mtport.compendium import templates as t
from mtport.compendium.siteapi import BuildError, Context, Images, Page, Section, load_kb_dir
from tests.fixtures.compendium import site_kb as fx


@pytest.fixture
def inputs(tmp_path: Path):
    kb = fx.write_kb(tmp_path)
    pages = fx.write_prose(tmp_path)
    out = tmp_path / "serve" / "maplethief-port"
    manifest = fx.write_images(out)
    return kb, pages, out, manifest


def built(inputs, **kw) -> site.Site:
    kb, pages, _out, manifest = inputs
    return site.build_site(kb, pages, manifest, plugins=kw.pop("plugins", []), **kw)


def html(s: site.Site, path: str) -> str:
    return s.files[path].decode("utf-8")


def test_every_page_type_renders(inputs):
    s = built(inputs)
    expected = {
        "index.html",  # home
        "search.html",
        "monsters/index.html",  # section index with a list table
        "monster/1.html",  # entity page
        "item/etc-3.html",
        "npc/7.html",
        "shop/1.html",
        "skill/3.html",
        "area/1.html",
        "map/1-1.html",
        "mechanics/drops.html",  # a prose page of its own
        "walkthrough/index.html",  # an empty section
        "about/index.html",
    }
    assert expected <= set(s.pages)
    for path in s.pages:
        page = html(s, path)
        assert page.startswith("<!doctype html>")
        for landmark in (
            '<header class="site-head">',
            'aria-label="Sections"',
            '<main id="main">',
            '<footer class="site-foot">',
            site.DISCLAIMER,
            site.LEGAL_URL,
        ):
            assert landmark in page, (path, landmark)
    mon = html(s, "monster/1.html")
    assert '<aside class="infobox"' in mon
    assert 'aria-label="Breadcrumb"' in mon
    assert '<span data-verbatim lang="ko">웅덩이민달팽이</span>' in mon
    assert 'class="gallery"' in mon
    # the infobox shows the move still, not the first by path (die)
    box = mon[mon.index('<aside class="infobox"') : mon.index("</aside>")]
    assert "monster/1/stand.png" in box and "die.png" not in box
    assert "<h1><span data-verbatim" in mon
    # a game name in a heading is game text (the prose lint's heading rule skips it)
    for path in s.pages:
        for h in re.findall(r"<h[1-6][^>]*>(.*?)</h[1-6]>", html(s, path)):
            if any(e["t"] in h for e in s.index if e["k"] != "Page"):
                assert "data-verbatim" in h, (path, h)
    # anchored kinds are rows of their section's table
    assert s.registry[("listing", "1:0")] == ("npcs/index.html", "listing-1-0")
    assert s.registry[("level", "2")] == ("skills/index.html", "level-2")
    assert 'id="listing-1-0"' in html(s, "npcs/index.html")
    # the section's prose and its articles
    assert "The monsters of the synthetic world" in html(s, "monsters/index.html")
    assert 'href="drops.html">Drop rates</a>' in html(s, "mechanics/index.html")
    # an entity's own prose
    assert '<h2 id="strategy">Strategy</h2>' in html(s, "monster/2.html")
    assert ">a few shells</a>" in html(s, "monster/2.html")


def test_templates_render():
    drops = t.drop_table([t.Drop("<b>x</b>", 25, "rare"), t.Drop("y", 1000)])
    assert '<td class="num" data-k="2.5"><span class="pct"' in drops
    assert '>2.5%</span> <span class="per">(25/1000)</span></td>' in drops
    assert '>100%</span> <span class="per">(1000/1000)</span>' in drops
    assert "<th>Note</th>" in drops or ">Note</th>" in drops
    # real odds beside the table's face value; equipment chances are in 100000
    real = t.drop_table([t.Drop("z", 40, percent=4.0283), t.Drop("w", 5, per=100000)])
    assert '>4.03%</span> <span class="per">(40/1000)</span>' in real
    assert '>0.005%</span> <span class="per">(5/100000)</span>' in real
    assert "Note" not in real
    assert '<p class="tools" hidden>' in real  # two rows: no filter box
    assert t.percent_text(0.00305) == "0.0031%" and t.percent_text(93.1641) == "93.16%"
    assert t.number(1250) == "1,250" and t.num_cell(12000, "meso").markup.startswith("12,000")
    dlg = t.dialogue([t.Line("Fen", "Hello & welcome", "안녕")])
    assert '<span data-verbatim lang="en" class="en">Hello &amp; welcome</span>' in dlg
    assert '<span data-verbatim lang="ko" class="ko">안녕</span>' in dlg
    assert 'data-show="both"' in dlg and "lang-toggle" in dlg
    assert t.gallery([]) == "" and 'class="gallery"' in t.gallery([("<img alt=a>", "cap")])
    assert t.see_also([]) == "" and "<li><a>z</a></li>" in t.see_also(["<a>z</a>"])
    box = t.infobox("H", [("Level", "3")], "<img alt=a>")
    assert "<dt>Level</dt><dd>3</dd>" in box and 'class="ib-image"' in box
    svg = t.svg_line_chart([(1, 15), (2, 34), (3, 57)], "EXP per level", "level", "EXP")
    assert svg.startswith('<figure class="chart"><svg') and 'role="img"' in svg
    assert 'aria-label="EXP per level"' in svg and "57" in svg
    assert t.percent(600) == "60%" and t.percent(1) == "0.1%"


def test_tables_fill_their_column_unless_compact():
    cols = [t.Column("Name"), t.Column("Price", numeric=True)]
    rows = [[t.text_cell("a"), t.num_cell(1)]]
    assert '<div class="viewer-table">' in t.table(cols, rows)
    assert '<div class="viewer-table compact">' in t.table(cols, rows, compact=True)
    css = assets.CSS
    # no shrink-wrapped box: the table fills .wrap, which fills the column (or the room
    # beside a floated infobox); only a compact table keeps its natural width
    wrap = css[css.index(".wrap {") : css.index("}", css.index(".wrap {"))]
    assert "fit-content" not in wrap and "overflow-x: auto" in wrap
    assert ".wrap > table { width: 100%; }" in css
    assert ".compact > .wrap { width: fit-content; }" in css
    assert "th.num { width: 1%; }" in css  # numbers stay tight, text takes the slack
    assert "position: sticky" in css  # long tables keep their header
    assert "data-scrolls" in css and "data-scrolls" in assets.JS  # the narrow-screen cue


def test_every_row_of_a_picture_table_has_the_same_box(tmp_path):
    assert [defaults.thumb_scale(w, h) for w, h in ((19, 14), (20, 20), (36, 30), (60, 37))] == [
        2,
        2,
        1,
        1,
    ]
    assert "--thumb: 40px" in assets.CSS and defaults.THUMB_BOX == 40
    s = built(_rich_inputs(tmp_path))
    for path in ("monster/1.html", "items/index.html", "monsters/index.html"):
        page = html(s, path)
        for table in re.findall(r"<table.*?</table>", page, re.S):
            entries = table.count('<span class="entry">')
            boxed = table.count('<span class="entry"><span class="thumb"')
            assert boxed in (0, entries), (path, entries, boxed)
    # the meso row of the drop table has no picture: an empty box keeps it in line
    assert defaults.EMPTY_THUMB in html(s, "monster/1.html")


def test_entity_references_resolve(inputs):
    s = built(inputs)
    page = html(s, "mechanics/drops.html")
    assert (
        '<a href="../monster/1.html"><span data-verbatim lang="en">Puddle Slug</span></a>' in page
    )
    assert '<a href="../item/etc-3.html">' in page
    assert '<a href="../items/index.html">the item list</a>' in page
    assert '<a href="#rates">the rates</a>' in page
    assert "<table" in page and "<blockquote>" in page and "<ol><li>First</li>" in page
    assert "\x00" not in "".join(html(s, p) for p in s.pages)


def test_unused_prose_without_a_title_fails(inputs):
    _without_entity_prose(inputs)
    (inputs[1] / "monster" / "9.md").write_text("Text.\n", encoding="utf-8")
    with pytest.raises(BuildError, match="no page uses this file"):
        built(inputs)


def test_unknown_reference_fails_the_build(inputs):
    kb, pages, out, manifest = inputs
    (pages / "mechanics" / "bad.md").write_text("# Bad\n\nSee {{monster:99}}.\n", encoding="utf-8")
    with pytest.raises(BuildError, match=r"mechanics/bad.md:3: unknown entity reference"):
        site.build_site(kb, pages, manifest, plugins=[])
    assert site.main(["build", "--kb", str(kb), "--pages", str(pages), "--out", str(out)]) == 1


@pytest.mark.parametrize(
    "text, message",
    [
        ("# T\n\n{{nokind}}\n", "bad entity reference"),
        ("# T\n\n<b>x</b>\n", "raw HTML"),
        ("# T\n\n# Again\n", "only the first line"),
        ("# T\n\n[x](/abs.html)\n", "bad site path"),
        ("# T\n\n| a | b |\n|---|---|\n| 1 |\n", "table row has 1 cells"),
    ],
)
def test_prose_errors(inputs, text, message):
    ctx = Context(load_kb_dir(inputs[0]), Images(), None)
    with pytest.raises(BuildError, match=message):
        prose.render(text, ctx, "x.md")


def test_two_builds_are_byte_identical(inputs, tmp_path):
    kb, pages, out, manifest = inputs
    args = ["--kb", str(kb), "--pages", str(pages), "--out", str(out)]
    assert site.main(["build", *args]) == 0
    first = {p.relative_to(out): p.read_bytes() for p in sorted(out.rglob("*")) if p.is_file()}
    assert site.main(["build", *args]) == 0
    second = {p.relative_to(out): p.read_bytes() for p in sorted(out.rglob("*")) if p.is_file()}
    assert first == second
    assert built(inputs).files == built(inputs).files
    assert site.main(["check", *args]) == 0
    # the images step's files survive a build
    assert (out / "img" / "monster" / "1" / "stand.png").is_file()


def test_check_catches_a_stale_file(inputs, capsys):
    kb, pages, out, _ = inputs
    args = ["--kb", str(kb), "--pages", str(pages), "--out", str(out)]
    assert site.main(["build", *args]) == 0
    (out / "monster" / "1.html").write_text("changed", encoding="utf-8")
    (out / "extra.html").write_text("x", encoding="utf-8")
    capsys.readouterr()
    assert site.main(["check", *args]) == 1
    text = capsys.readouterr().out
    assert "monster/1.html: stale" in text and "extra.html: not part of a fresh build" in text


def test_link_checker_catches_planted_broken_links(inputs):
    s = built(inputs)
    img_root = inputs[2] / "img"
    assert site.check_links(s.files, img_root) == []
    files = dict(s.files)
    files["monster/1.html"] += b'<a href="../monster/404.html">x</a><a href="2.html#nope">y</a>'
    files["monster/1.html"] += b'<a href="/maplethief-port/index.html">z</a>'
    files["monster/1.html"] += b'<img src="../img/monster/1/gone.png" alt="">'
    problems = site.check_links(files, img_root)
    assert any("broken link '../monster/404.html'" in p for p in problems)
    assert any("'2.html#nope'" in p and "no #nope" in p for p in problems)
    assert any("is absolute" in p for p in problems)
    assert any("gone.png" in p for p in problems)
    assert len(problems) == 4


def _without_entity_prose(inputs):
    (inputs[1] / "monster" / "2.md").unlink()


def test_completeness_reports_missing_entities(inputs):
    _without_entity_prose(inputs)
    (inputs[1] / "mechanics" / "drops.md").unlink()  # it links to monster 2

    def pages(ctx):
        body = ctx.prose("monsters/index.md") or ""
        yield Page("monsters/index.html", "Monsters", body).documents("monster", "1")

    s = built(inputs, plugins=[Section("monsters", "Monsters", 10, ("monster",), pages)])
    missing, counts = site.completeness(s)
    assert missing == ["completeness: monster:2 has no page or anchor"]
    assert counts["monster"] == (1, 2)


def test_link_to_an_undocumented_entity_fails(inputs):
    # monster 2 has no page now, but the prose links to it
    _without_entity_prose(inputs)

    def pages(ctx):
        body = ctx.prose("monsters/index.md") or ""
        yield Page("monsters/index.html", "Monsters", body).documents("monster", "1")

    with pytest.raises(BuildError, match="link to monster:2, which no page documents"):
        built(inputs, plugins=[Section("monsters", "Monsters", 10, ("monster",), pages)])


def test_a_plugin_replaces_or_extends_a_section(inputs):
    def pages(ctx):
        for e in ctx.kb.entities("monster"):
            page = ctx.default_entity_page("monster", e)
            page.body += '<p class="plugin">extended</p>'
            yield page
        body = ctx.prose("monsters/index.md") or ""
        yield Page("monsters/index.html", "Monsters", body + ctx.ref("monster", "2"))

    plugin = Section("monsters", "Bestiary", 5, ("monster",), pages)
    s = built(inputs, plugins=[plugin, Section("extra", "Extra", 95, ())])
    assert 'class="plugin">extended' in html(s, "monster/1.html")
    nav = re.search(r'aria-label="Sections"><ul>(.*?)</ul>', html(s, "index.html")).group(1)
    assert nav.index("Bestiary") < nav.index("Items") < nav.index("Extra")
    assert '<a href="../monster/2.html">' in html(s, "monsters/index.html")


def test_plugin_discovery_finds_the_package_modules():
    from mtport.compendium import sections

    found = sections.discover()
    assert all(isinstance(x, Section) for x in found)
    ids = [x.id for x in site.all_sections()]
    assert ids[:9] == [d.id for d in defaults.DEFAULT_SECTIONS]


def test_reads_kb_with_and_without_index(inputs):
    kb = load_kb_dir(inputs[0])  # with index.json: #213's load_kb
    assert (inputs[0] / "index.json").is_file()
    assert kb.get("item", "etc:3")["price"] == 2
    (inputs[0] / "index.json").unlink()
    assert load_kb_dir(inputs[0]).keys() == kb.keys()


def test_unclaimed_kinds_go_to_other(inputs, tmp_path):
    kb = dict(fx.KB)
    kb["oddity"] = [{"id": "1", "kind": "oddity", "name": {"en": "Thing", "ko": "것"}}]
    d = fx.write_kb(tmp_path / "odd", kb)
    s = site.build_site(d, inputs[1], inputs[3], plugins=[])
    assert ("oddity", "1") in s.registry and "other/index.html" in s.pages


def test_search_finds_entities_by_english_and_korean_names(inputs):
    s = built(inputs)
    raw = html(s, site.SEARCH_INDEX)
    assert raw.startswith(site.SEARCH_PREFIX) and raw.endswith(";\n")
    index = json.loads(raw[len(site.SEARCH_PREFIX) : -2])
    assert index == s.index
    assert site.search(index, "puddle slug")[0]["u"] == "monster/1.html"
    assert site.search(index, "웅덩이민달팽이")[0]["u"] == "monster/1.html"
    assert site.search(index, "민달팽이")[0]["u"] == "item/etc-3.html"  # prefix before substring
    assert site.search(index, "SLUG")[0]["u"] == "monster/1.html"  # the alias, exactly
    assert site.search(index, "Drop rates")[0]["u"] == "mechanics/drops.html"
    assert site.check_index(s) == []
    covered = {e["u"].split("#")[0] for e in index}
    assert {"npcs/index.html", "monster/1.html", "mechanics/drops.html"} <= covered


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_site_script_search_matches_python(inputs, tmp_path):
    s = built(inputs)
    script = tmp_path / "run.js"
    script.write_text(
        "global.window = global;\n"
        "global.document = { documentElement: { getAttribute: function () { return ''; } },"
        " getElementById: function () { return null; }, querySelectorAll: function () {"
        " return []; } };\n"
        + html(s, site.SEARCH_INDEX)
        + assets.SEARCH_SCRIPT
        + "var idx = window.MTSEARCHFN.prepare(window.MTSEARCH);\n"
        "var qs = JSON.parse(process.argv[2]);\n"
        "console.log(JSON.stringify(qs.map(function (q) {"
        " return window.MTSEARCHFN.find(idx, q).map(function (e) { return e.u; }); })));\n",
        encoding="utf-8",
    )
    queries = ["puddle", "웅덩이민달팽이", "민달팽이", "SLUG", "tin", "레벨", "zzz"]
    got = subprocess.run(
        ["node", str(script), json.dumps(queries)], capture_output=True, text=True, check=True
    )
    want = [[e["u"] for e in site.search(s.index, q)] for q in queries]
    assert json.loads(got.stdout) == want
    assert want[1] == ["monster/1.html"]


def _all_links(path: str, page: str) -> list[str]:
    links = re.findall(r'(?:href|src|action)="([^"]+)"', page)
    return [x for x in links if not x.startswith(("http://", "https://", "#"))]


def test_works_from_file_url(inputs):
    kb, pages, out, _ = inputs
    assert site.main(["build", "--kb", str(kb), "--pages", str(pages), "--out", str(out)]) == 0
    seen = 0
    for page in sorted(out.rglob("*.html")):
        text = page.read_text(encoding="utf-8")
        for link in _all_links(str(page), text):
            assert not link.startswith("/")
            url = page.as_uri().rsplit("/", 1)[0] + "/" + link.split("#")[0]
            with urllib.request.urlopen(url) as r:  # noqa: S310 (a file:// URL of the test)
                assert r.read() is not None
            seen += 1
    assert seen > 100


def test_works_from_a_sub_path_over_http(inputs):
    kb, pages, out, _ = inputs
    assert site.main(["build", "--kb", str(kb), "--pages", str(pages), "--out", str(out)]) == 0
    handler = functools.partial(_QuietHandler, directory=str(out.parent))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        base = f"http://127.0.0.1:{server.server_address[1]}/maplethief-port/"
        checked = set()
        for page in sorted(out.rglob("*.html")):
            rel = page.relative_to(out).as_posix()
            with urllib.request.urlopen(base + rel) as r:  # noqa: S310
                assert r.status == 200
                text = r.read().decode("utf-8")
            for link in _all_links(rel, text):
                target = urllib.parse.urljoin(base + rel, link.split("#")[0])
                assert target.startswith(base), (rel, link)
                if target in checked:
                    continue
                with urllib.request.urlopen(target) as r:  # noqa: S310
                    assert r.status == 200
                checked.add(target)
        assert base + "search-index.js" in checked or base + "site.js" in checked
    finally:
        server.shutdown()
        server.server_close()


class _QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):  # keep the test output clean
        pass


def test_write_refuses_a_foreign_directory(tmp_path):
    d = tmp_path / "other"
    d.mkdir()
    (d / "precious.txt").write_text("x", encoding="utf-8")
    with pytest.raises(FileExistsError):
        site.write_site(d, {"index.html": b"x"})
    img_only = tmp_path / "fresh"
    (img_only / "img").mkdir(parents=True)
    site.write_site(img_only, {"index.html": b"x"})
    assert (img_only / site.MARKER).is_file()


def _luminance(hex_colour: str) -> float:
    def ch(v: int) -> float:
        c = v / 255
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

    r, g, b = (int(hex_colour[i : i + 2], 16) for i in (1, 3, 5))
    return 0.2126 * ch(r) + 0.7152 * ch(g) + 0.0722 * ch(b)


@pytest.mark.parametrize("theme", [0, 1], ids=["light", "dark"])
def test_theme_colours_meet_wcag_aa(theme):
    for fg in assets.TEXT_COLOURS:
        for bg in assets.BACKGROUNDS:
            a = _luminance(assets.THEMES[fg][theme])
            b = _luminance(assets.THEMES[bg][theme])
            ratio = (max(a, b) + 0.05) / (min(a, b) + 0.05)
            assert ratio >= 4.5, (fg, bg, theme, ratio)
    for pairs, least in ((assets.PAIRS, 4.5), (assets.MARKS, 3.0)):
        for fg, bg in pairs:
            a = _luminance(assets.THEMES[fg][theme])
            b = _luminance(assets.THEMES[bg][theme])
            ratio = (max(a, b) + 0.05) / (min(a, b) + 0.05)
            assert ratio >= least, (fg, bg, theme, ratio)
    assert "@media (prefers-color-scheme: dark)" in assets.CSS
    assert ":focus-visible" in assets.CSS


def test_every_image_has_alt_text(inputs):
    s = built(inputs)
    for path in s.pages:
        for tag in re.findall(r"<img [^>]*>", html(s, path)):
            assert re.search(r'alt="[^"]+"', tag), (path, tag)


def test_synthetic_site_passes_the_prose_lint(inputs):
    s = built(inputs)
    assert site.prose_lint(s.files) == []


def _rich_inputs(tmp_path: Path):
    """The synthetic world with knowledge-base fields shaped like #213's and 2x pictures."""
    import copy

    kb = copy.deepcopy(fx.KB)
    mon = kb["monster"][0]
    mon.update(
        {
            "meso": {"amount": 5, "min": 4, "max": 6, "percent": 80},
            "class": "normal",
            "sprite": 63,
            "name_fills_field": False,
            "movement_kind": 1,
            "walk_speed": 1,
            "drops": [
                {
                    "kind": "item",
                    "id": "etc:3",
                    "chance": 40,
                    "per": 1000,
                    "percent": 4.0283,
                    "nominal_percent": 4.0,
                    "slot": 3,
                },
                {
                    "kind": "item",
                    "id": "equip:1",
                    "chance": 5,
                    "per": 100000,
                    "percent": 0.0153,
                    "nominal_percent": 0.005,
                    "slot": 0,
                },
            ],
            "spawns_on": [{"kind": "map", "id": "1-1", "count": 4}],
        }
    )
    del mon["maps"]
    kb["item"][1].update(
        {
            "category": "potion",
            "max_stack": 100,
            "unknown_84": 7,
            "confidence": {"unknown_84": "unknown"},
        }
    )
    manifest = copy.deepcopy(fx.MANIFEST)
    manifest["images"]["monster:1"][1].update({"w": 20, "h": 16})
    manifest["images"]["monster:1"].append(
        {
            "path": "monster/1/move.anim.png",
            "w": 20,
            "h": 16,
            "alt": "Puddle Slug moving",
            "role": "anim",
            "action": "move",
            "delays_ms": [100, 100],
        }
    )
    manifest["images"]["item:etc:3"] = [
        {"path": "item/etc-3.png", "w": 18, "h": 15, "alt": "Slug Shell", "role": "icon"},
        {"path": "item/etc-3@2x.png", "w": 36, "h": 30, "alt": "Slug Shell", "role": "icon2x"},
    ]
    kb_dir = fx.write_kb(tmp_path, kb)
    pages = fx.write_prose(tmp_path)
    out = tmp_path / "site"
    img = out / "img"
    img.mkdir(parents=True)
    (img / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    for rows in manifest["images"].values():
        for r in rows:
            (img / r["path"]).parent.mkdir(parents=True, exist_ok=True)
            (img / r["path"]).write_bytes(fx.PNG_1X1)
    return kb_dir, pages, out, img / "manifest.json"


def test_infobox_speaks_the_style_guide(tmp_path):
    s = built(_rich_inputs(tmp_path))
    mon = html(s, "monster/1.html")
    box = mon[mon.index('<aside class="infobox"') : mon.index("</aside>")]
    # humane labels and units; internal fields hidden; the Korean name is the subtitle
    for want in (
        "<dt>Level</dt><dd>Lv. 2</dd>",
        "<dt>Meso</dt><dd>4-6 meso (80% of kills)",
        "<dt>Movement</dt><dd>walks</dd>",
        "<dt>Boss</dt><dd>no</dd>",
        '<p class="ib-sub"><span data-verbatim lang="ko">웅덩이민달팽이</span></p>',
        "<dt>Found on</dt>",
    ):
        assert want in box, want
    for hidden in ("Sprite", "Name fills field", "Walk speed", "Korean name", "Confidence"):
        assert f"<dt>{hidden}</dt>" not in box, hidden
    # the sprite is scaled up crisply (160 // 20 = 8, capped at 4) and plays its animation
    assert 'width="80" height="64" class="pixel"' in box
    assert '<source media="(prefers-reduced-motion: no-preference)"' in box
    assert "monster/1/move.anim.png" in box
    # the drop table shows the real odds with the table's own value, no raw columns
    assert '>4.03%</span> <span class="per">(40/1000)</span>' in mon
    assert '>0.015%</span> <span class="per">(5/100000)</span>' in mon
    for raw in (">Per<", ">Nominal percent<", ">Slot<"):
        assert raw not in mon
    item = html(s, "item/etc-3.html")
    ibox = item[item.index('<aside class="infobox"') : item.index("</aside>")]
    assert "img/item/etc-3@2x.png" in ibox  # the 2x icon for an icon shown 4x
    assert 'width="72" height="60"' in ibox
    assert "<dt>Stack size</dt><dd>100</dd>" in ibox
    assert "Unknown 84" not in ibox  # fields of unknown meaning stay out
    assert "<dt>Sell price</dt><dd>2 meso</dd>" in ibox


def test_shell_home_and_navigation(inputs):
    s = built(inputs)
    home = html(s, "index.html")
    assert '<div class="hero">' in home and 'id="hq"' in home
    assert 'aria-controls="hq-results"' in home
    cards = re.findall(r'<li><a href="[^"]+"><span class="card-pic">', home)
    assert len(cards) == len(s.sections)
    assert 'class="brand"' in home and 'class="mark"' in home
    mon = html(s, "monster/1.html")
    nav = re.search(r'aria-label="Sections"><ul>(.*?)</ul>', mon).group(1)
    assert 'href="../monsters/index.html" class="in">Monsters' in nav
    idx = html(s, "monsters/index.html")
    assert 'href="../monsters/index.html" aria-current="page">Monsters' in idx
    assert '<h2 id="monster">' not in idx  # one table under the page title: no second heading
