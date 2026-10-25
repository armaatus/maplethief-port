"""Unit tests for the data-table browser (mtport.tablebrowser) on a synthetic .app tree."""

from __future__ import annotations

import dataclasses
import re
from pathlib import Path

import pytest

from mtport import datatables as dt
from mtport import htmlpage as hp
from mtport import pngdat
from mtport import tablebrowser as tb

HOSTILE = '<script>alert("x")</script>&'


def rec(table: str, /, **values):
    """A record of ``table``: zeros and empty strings, overridden by ``values``."""
    record = dt.TABLES[table].record
    base = {c.name: "" if c.kind == "str" else 0 for c in dt.layout(record)}
    base.update(values)
    return record(**base)


def tiny_png(seed: int) -> bytes:
    """A distinct 2x1 palette PNG."""
    img = pngdat.Indexed(2, 1, (bytes([0, 1]),), bytes([seed % 256, 0, 0, 0, 0, 255]), None)
    return pngdat.encode_indexed(img)


def english_tables() -> dict[str, list]:
    return {
        "hero": [rec("hero", level=1, exp=10), rec("hero", level=2, exp=25)],
        "mob": [
            rec(
                "mob",
                id=1,
                name="Blue Snail",
                hp=20,
                etc_drop_1=1,
                etc_drop_1_chance=930,
                etc_drop_2=99,  # dangling id, but chance 0: never rolls, so not an error
            ),
            rec("mob", id=2, name="<b>&\"Snail'", hp=40, equip_drop=0, equip_drop_chance=600),
        ],
        "npc": [rec("npc", id=0, name="Mr. $Lee"), rec("npc", id=1, name=HOSTILE)],
        "item_equip": [rec("item_equip", id=0, name="Red Woolen Hat", icon=2)],
        "item_etc": [
            rec("item_etc", id=0, name="Meso", icon=0),
            rec("item_etc", id=1, name="Blue Snail Shell", icon=5, description="a\nb"),
        ],
        "skill": [rec("skill", id=0, name="Lucky Seven", icon=1)],
        "market": [rec("market", shop=0, table=0, item=0, price=170)],
    }


KOREAN = {
    ("mob", 0): "파란달팽이",
    ("item_etc", 1): "파란달팽이의 껍질",
    ("item_equip", 0): "빨간털모자",
}


def korean_tables() -> dict[str, list]:
    out = {}
    for name, records in english_tables().items():
        out[name] = [
            dataclasses.replace(r, name=KOREAN[(name, i)]) if (name, i) in KOREAN else r
            for i, r in enumerate(records)
        ]
    return out


def write_lang(app: Path, lang: str, tables: dict[str, list]) -> None:
    d = app / lang / "data"
    d.mkdir(parents=True, exist_ok=True)
    for name, records in tables.items():
        (d / f"{name}.dat").write_bytes(dt.encode(name, records))
    (d / "buildup.dat").write_bytes(b"1")


def make_app(root: Path) -> Path:
    app = root / "MapleThief.app"
    write_lang(app, "English", english_tables())
    write_lang(app, "Japanese", english_tables())
    write_lang(app, "Korean", korean_tables())
    for ip in tb.ICONS.values():
        pack = pngdat.BY_PATH[ip.pack]
        path = app / ip.pack
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"".join(tiny_png(pack.id * 1000 + i) for i in range(pack.count)))
    return app


@pytest.fixture
def app(tmp_path):
    return make_app(tmp_path / "src")


def test_build_writes_every_table_with_parser_counts(app, tmp_path):
    out = tmp_path / "out"
    files, errors = tb.build(app, out)
    assert errors == []
    assert {f for f in files if f.endswith(".html")} == {
        "index.html",
        *(f"{n}.html" for n in tb.PAGES),
    }
    assert len(tb.PAGES) == 8
    want = {n: len(r) for n, r in english_tables().items()} | {"buildup": 2}
    assert tb.expected_rows(tb.load(app)) == want
    for name, n in want.items():
        assert tb.page_rows((out / f"{name}.html").read_bytes()) == (n, n), name
    index = (out / "index.html").read_text(encoding="utf-8")
    assert "byte-identical to English" in index
    assert "No errors." in index
    assert tb.check(app, out) == []
    assert (out / hp.MARKER).read_text(encoding="utf-8") == "tables\n"


def test_english_and_korean_side_by_side(app):
    files, _ = tb.render(tb.load(app))
    mob = files["mob.html"].decode()
    assert "<th" in mob and ">name (en)</th>" in mob and ">name (ko)</th>" in mob
    assert '<td>Blue Snail</td><td lang="ko">파란달팽이</td>' in mob
    # Numeric columns appear once; hero has no text, so one set of columns only.
    assert mob.count(">hp</th>") == 1
    assert "(en)" not in files["hero.html"].decode()
    # Drops link to the item row, with the Korean name as the tooltip.
    assert '<a href="item_etc.html#r1" title="파란달팽이의 껍질">Blue Snail Shell</a>' in mob
    assert '930 <span class="label">93 %</span>' in mob
    assert '99 <span class="label">none (chance 0)</span>' in mob
    market = files["market.html"].decode()
    assert '<a href="item_equip.html#r0" title="빨간털모자">Red Woolen Hat</a>' in market
    assert 'id="r1"' in files["item_etc.html"].decode()


def test_table_text_is_escaped(app):
    files, _ = tb.render(tb.load(app))
    for path, data in files.items():
        if path.endswith(".html"):
            page = data.decode()
            assert page.count("<script>") == 1, path  # only the page's own inline script
            assert "alert(" not in page or "&lt;script&gt;alert(&quot;x&quot;)" in page
    assert "<td>&lt;script&gt;alert(&quot;x&quot;)&lt;/script&gt;&amp;</td>" in (
        files["npc.html"].decode()
    )
    assert "<td>&lt;b&gt;&amp;&quot;Snail&#x27;</td>" in files["mob.html"].decode()


def test_icons_are_copied_from_the_packs(app):
    files, errors = tb.render(tb.load(app))
    assert errors == []
    icons = sorted(f for f in files if f.startswith("icons/"))
    assert icons == [
        "icons/equip/02.png",
        "icons/etc/000.png",
        "icons/etc/005.png",
        "icons/skillico/01.png",
    ]
    etc = pngdat.split((app / "png/item/etc.dat").read_bytes())
    assert files["icons/etc/005.png"] == etc[5].data
    img = '<img class="icon" src="icons/etc/005.png" alt="icon 5">'
    assert img in files["item_etc.html"].decode()
    assert "<img" not in files["mob.html"].decode()


def test_output_is_deterministic(app, tmp_path):
    a, _ = tb.build(app, tmp_path / "a")
    b, _ = tb.build(app, tmp_path / "b")
    assert a == b
    for rel in a:
        assert (tmp_path / "a" / rel).read_bytes() == (tmp_path / "b" / rel).read_bytes()
    # A rebuild into the same directory replaces it.
    (tmp_path / "a" / "stray.txt").write_text("x", encoding="utf-8")
    tb.build(app, tmp_path / "a")
    assert not (tmp_path / "a" / "stray.txt").exists()


def test_errors_are_reported(app):
    ko = korean_tables()
    ko["npc"] = []  # row count differs
    ko["skill"] = [dataclasses.replace(ko["skill"][0], max_level=20)]  # numeric differs
    write_lang(app, "Korean", ko)
    en = english_tables()
    en["item_etc"][1] = dataclasses.replace(en["item_etc"][1], icon=500)  # outside the pack
    en["mob"][0] = dataclasses.replace(en["mob"][0], etc_drop_2_chance=5)  # dangling, now rolls
    write_lang(app, "English", en)  # Japanese is now different from English too
    write_lang(app, "Korean", {**ko, "item_etc": en["item_etc"], "mob": en["mob"]})
    (app / "English" / "data" / "hero.dat").write_bytes(b"\0\0")
    files, errors = tb.render(tb.load(app))
    text = "\n".join(errors)
    assert "English/data/hero.dat: hero: 2 bytes is too short" in text
    assert "npc: English has 2 rows, Korean 0" in text
    assert "skill row 0: numeric columns differ between EN and KO" in text
    assert "item_etc row 1: icon 500 outside png/item/etc.dat" in text
    assert "mob row 0: item_etc id 99 does not exist" in text
    assert "Japanese/data differs from English/data: hero, mob, item_etc" in text
    assert '<li class="error">' in files["index.html"].decode()


def test_check_finds_stale_missing_and_miscounted_pages(app, tmp_path):
    out = tmp_path / "out"
    tb.build(app, out)
    (out / "extra.html").write_text("x", encoding="utf-8")
    (out / "npc.html").unlink()
    page = (out / "mob.html").read_text(encoding="utf-8")
    (out / "mob.html").write_text(
        re.sub(r'<tr data-i="1".*?</tr>', "", page, flags=re.S), encoding="utf-8"
    )
    errors = tb.check(app, out)
    assert "npc.html: missing" in errors
    assert "extra.html: not part of a fresh build" in errors
    assert "mob.html: stale (differs from a fresh build)" in errors
    assert "mob.html: 1 rows, states 2; the parser has 2" in errors


def test_cli(app, tmp_path, capsys):
    out = tmp_path / "out"
    assert tb.main(["build", "--app", str(app), "--out", str(out)]) == 0
    assert tb.main(["check", "--app", str(app), "--out", str(out)]) == 0
    assert ", 0 errors" in capsys.readouterr().out
    (out / "hero.html").write_text("changed", encoding="utf-8")
    assert tb.main(["check", "--app", str(app), "--out", str(out)]) == 1
    assert "hero.html: stale" in capsys.readouterr().err
    assert tb.main(["build", "--app", str(tmp_path / "nope"), "--out", str(out)]) == 1


def test_build_refuses_a_foreign_directory(app, tmp_path):
    out = tmp_path / "out"
    out.mkdir()
    (out / "notes.txt").write_text("mine", encoding="utf-8")
    with pytest.raises(FileExistsError):
        tb.build(app, out)
    assert (out / "notes.txt").read_text(encoding="utf-8") == "mine"
    assert tb.main(["build", "--app", str(app), "--out", str(out)]) == 1
