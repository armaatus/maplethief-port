"""The Items section of the compendium (#219) on a synthetic knowledge base.

Every name and number is made up; nothing comes from the game.
"""

from __future__ import annotations

import copy
import re
from pathlib import Path

import pytest

from mtport import prose_lint
from mtport.compendium import site
from mtport.compendium.sections import items as sec
from mtport.compendium.siteapi import KB, Context, Images
from tests.fixtures.compendium import site_kb as fx


def _n(en: str, ko: str) -> dict:
    return {"en": en, "ko": ko}


def _stats(**ranges: tuple[int, int]) -> dict:
    keys = ("attack", "defense", "accuracy", "avoid", "hp", "mp")
    return {k: {"min": ranges.get(k, (0, 0))[0], "max": ranges.get(k, (0, 0))[1]} for k in keys}


def _etc(n: int, en: str, typ: int, **kw) -> dict:
    e = {
        "id": f"etc:{n}",
        "kind": "item",
        "name": _n(en, en + " ko"),
        "table": "etc",
        "table_id": n,
        "type": typ,
        "category": "etc",
        "price": 2,
        "max_stack": 100,
        "description": {"en": "A made-up thing.\nSecond line", "ko": "가짜"},
        "confidence": {},
    }
    e.update(kw)
    return e


def _equip(n: int, en: str, slot: int, **kw) -> dict:
    e = {
        "id": f"equip:{n}",
        "kind": "item",
        "name": _n(en, en + " ko"),
        "table": "equip",
        "table_id": n,
        "slot": slot,
        "category": sec.SLOTS[slot],
        "weapon_type": None,
        "job": 4,
        "req_level": 12,
        "speed": 0,
        "stats": _stats(defense=(3, 6)),
        "bonus": {"stat": "avoid", "value": 2},
        "price": 90,
        "description": {"en": None, "ko": None},
        "confidence": {},
    }
    e.update(kw)
    return e


def _ref(kind: str, eid: str, **attrs) -> dict:
    return {"kind": kind, "id": eid, **attrs}


KB_ROWS = {
    "area": [{"id": "1", "kind": "area", "name": _n("Fernvale", "펀베일"), "confidence": {}}],
    "map": [
        {
            "id": "1-0",
            "kind": "map",
            "name": _n("Fernvale Square", "펀베일 광장"),
            "area": [_ref("area", "1")],
            "confidence": {},
        },
        {
            "id": "1-1",
            "kind": "map",
            "name": _n("Fernvale Marsh", "펀베일 늪"),
            "area": [_ref("area", "1")],
            "confidence": {},
        },
    ],
    "monster": [
        {
            "id": "1",
            "kind": "monster",
            "name": _n("Bog Toad", "늪두꺼비"),
            "level": 4,
            "spawns_on": [_ref("map", "1-1", count=3)],
            "confidence": {},
        }
    ],
    "npc": [
        {
            "id": "5",
            "kind": "npc",
            "name": _n("Tilda", "틸다"),
            "stands_on": [_ref("map", "1-0", tile=[3, 4], script="Tilda")],
            "confidence": {},
        }
    ],
    "shop": [
        {
            "id": "2",
            "kind": "shop",
            "name": _n("Shop 2", "상점 2"),
            "opened_by": [_ref("npc", "5", map="1-0", script="common-Tilda", chapters=[2])],
            "unopened_reason": None,
            "confidence": {},
        },
        {
            "id": "9",
            "kind": "shop",
            "name": _n("Shop 9", "상점 9"),
            "opened_by": [],
            "unopened_reason": "no script opens it",
            "confidence": {},
        },
    ],
    "quest": [
        {
            "id": "1-main-3",
            "kind": "quest",
            "name": _n("Toad Trouble", "두꺼비 문제"),
            "chapter": 1,
            "given_by": [_ref("npc", "5", map="1-0", script="chapter1-Tilda")],
            "confidence": {},
        }
    ],
    "recipe": [
        {
            "id": "1-equip:1",
            "kind": "recipe",
            "name": _n("Reed Cap", "갈대모자"),
            "chapter": 1,
            "meso": 300,
            "crafted_by": [_ref("npc", "5", map="1-0")],
            "materials": [_ref("item", "etc:40", count=3)],
            "result": [_ref("item", "equip:1", count=1)],
            "confidence": {},
        }
    ],
    "trade": [
        {
            "id": "1-etc:40-etc:3",
            "kind": "trade",
            "name": _n("Marsh Tonic", "늪 강장제"),
            "chapter": 1,
            "offered_by": [_ref("npc", "5", map="1-0")],
            "cost": [_ref("item", "etc:40", count=10)],
            "result": [_ref("item", "etc:3", count=2)],
            "confidence": {},
        }
    ],
    "item": [
        _equip(
            1,
            "Reed Cap",
            0,
            dropped_by=[
                _ref("monster", "1", chance=30, per=100000, percent=0.0916, nominal_percent=0.03)
            ],
            sold_by=[_ref("shop", "2", price=600, listing="1")],
            crafted_by=[_ref("recipe", "1-equip:1", count=1)],
        ),
        _equip(
            2,
            "Rusty Claw",
            1,
            weapon_type="claw",
            speed=2,
            stats=_stats(attack=(4, 9)),
            sold_by=[_ref("shop", "9", price=900, listing="2")],
        ),
        _equip(3, "Lost Boots", 4),
        _etc(
            3,
            "Marsh Tonic",
            1,
            category="potion",
            hp=40,
            rewarded_by_quests=[_ref("quest", "1-main-3", count=5)],
            traded_for_in=[_ref("trade", "1-etc:40-etc:3", count=2)],
            sold_by=[_ref("shop", "2", price=20, listing="3")],
        ),
        _etc(
            40,
            "Toad Wart",
            5,
            dropped_by=[_ref("monster", "1", chance=500, per=1000, percent=50.36)],
            material_of=[_ref("recipe", "1-equip:1", count=3)],
            traded_away_in=[_ref("trade", "1-etc:40-etc:3", count=10)],
            needed_by_quests=[_ref("quest", "1-main-3", count=4)],
        ),
        _etc(
            41,
            "Sealed Note",
            6,
            given_by_npcs=[
                _ref("npc", "5", map="1-0", script="chapter1-Tilda", count=1, chapters=[1])
            ],
        ),
        _etc(42, "Odd Pebble", 5),
        _etc(129, "Skill Reset Card", 7),
        _etc(21, "Cap Polish", 3, defense=2, accuracy=1),
    ],
}

# Monster Bog Toad's drops, for the inverse side.
KB_ROWS["monster"][0]["drops"] = [
    _ref("item", "equip:1", chance=30, per=100000),
    _ref("item", "etc:40", chance=500, per=1000),
]

MANIFEST_IMAGES = {
    "item:equip:1": [
        {"path": "item/equip/1.png", "w": 1, "h": 1, "alt": "Reed Cap icon", "role": "icon"},
        {"path": "item/equip/1.worn.png", "w": 1, "h": 1, "alt": "worn", "role": "worn"},
    ]
}


@pytest.fixture
def built(tmp_path: Path, monkeypatch) -> site.Site:
    kb = fx.write_kb(tmp_path, KB_ROWS)
    pages = fx.write_prose(tmp_path, {})
    out = tmp_path / "serve" / "maplethief-port"
    monkeypatch.setattr(fx, "MANIFEST", {"schema": 1, "images": MANIFEST_IMAGES})
    manifest = fx.write_images(out)
    s = site.build_site(kb, pages, manifest, plugins=[sec.SECTION])
    s.img_root = out / "img"  # for check_links
    return s


def page(s: site.Site, path: str) -> str:
    return s.files[path].decode("utf-8")


def item_path(eid: str) -> str:
    return "item/" + eid.replace(":", "-") + ".html"


def href(s: site.Site, from_page: str, kind: str, eid: str) -> str:
    """The ``href`` attribute a link from ``from_page`` to ``kind:eid`` carries."""
    target, anchor = s.registry[(kind, eid)]
    return 'href="' + site.rel_url(target + (f"#{anchor}" if anchor else ""), from_page) + '"'


def part(html: str, anchor: str) -> str:
    """The ``<h2>`` section ``anchor`` of a page, up to the next ``<h2>``."""
    start = html.index(f'<h2 id="{anchor}"')
    end = html.find("<h2 ", start + 1)
    return html[start : end if end > 0 else len(html)]


def ctx_for(rows: dict) -> Context:
    return Context(KB(copy.deepcopy(rows)), Images(), None)


def test_every_item_has_a_page_and_the_lists_exist(built):
    for e in KB_ROWS["item"]:
        assert built.registry[("item", e["id"])] == (item_path(e["id"]), "")
    for p in ("items/index.html", "items/comparisons.html", "items/no-source.html"):
        assert p in built.pages
    for slug_ in (
        "weapons",
        "hats",
        "shoes",
        "potions",
        "scrolls",
        "materials",
        "quest-items",
        "cash-items",
    ):
        assert f"items/{slug_}.html" in built.pages
    assert site.completeness(built)[0] == []
    assert site.check_links(built.files, built.img_root) == []


def test_how_to_get_points_at_real_sources(built):
    ctx = ctx_for(KB_ROWS)
    for e in KB_ROWS["item"]:
        path = item_path(e["id"])
        how = part(page(built, path), "how-to-get")
        for s in sec.sources(ctx, e):
            if s.how == "cash":
                assert 'id="cash-shop"' in how
                continue
            assert ctx.kb.get(s.kind, s.id) is not None, s
            assert href(built, path, s.kind, s.id) in how, (e["id"], s)


def test_where_to_find_each_source(built):
    p = "item/equip-1.html"
    cap = part(page(built, p), "how-to-get")
    # shop -> NPC -> map (area), and the chapter when it differs
    shops = cap[cap.index('id="shops"') :]
    for kind, eid in (("npc", "5"), ("map", "1-0"), ("area", "1")):
        assert href(built, p, kind, eid) in shops, kind
    assert "chapter 2" in shops
    # drop -> monster -> its maps
    drops = cap[cap.index('id="drops"') :]
    assert href(built, p, "monster", "1") in drops and href(built, p, "map", "1-1") in drops
    # recipe -> crafter, its map, and the materials
    craft = cap[cap.index('id="crafting"') :]
    assert href(built, p, "npc", "5") in craft and href(built, p, "map", "1-0") in craft
    assert href(built, p, "item", "etc:40") in craft and "300 meso" in craft
    # trade -> trader and map
    t = "item/etc-3.html"
    trades = part(page(built, t), "how-to-get")
    trades = trades[trades.index('id="trades"') :]
    assert href(built, t, "trade", "1-etc:40-etc:3") in trades
    assert href(built, t, "npc", "5") in trades and href(built, t, "map", "1-0") in trades
    # quest -> giver and map
    quests = part(page(built, t), "how-to-get")
    quests = quests[quests.index('id="quest-rewards"') :]
    assert href(built, t, "quest", "1-main-3") in quests and href(built, t, "npc", "5") in quests
    # a shop no NPC opens is named as not reachable
    claw = part(page(built, "item/equip-2.html"), "how-to-get")
    assert href(built, "item/equip-2.html", "shop", "9") in claw and "not reachable" in claw
    # an NPC's gift
    note = part(page(built, "item/etc-41.html"), "how-to-get")
    assert 'id="gifts"' in note and href(built, "item/etc-41.html", "npc", "5") in note
    # the cash shop
    assert 'id="cash-shop"' in page(built, "item/etc-129.html")


def test_used_in(built):
    p = "item/etc-40.html"
    used = part(page(built, p), "used-in")
    assert href(built, p, "quest", "1-main-3") in used
    assert href(built, p, "recipe", "1-equip:1") in used
    assert href(built, p, "trade", "1-etc:40-etc:3") in used
    polish = part(page(built, "item/etc-21.html"), "used-in")
    assert "equipped hat" in polish and 'href="../items/hats.html"' in polish


def test_no_known_source(built):
    ctx = ctx_for(KB_ROWS)
    none = {e["id"] for e in sec.no_source_items(ctx)}
    # boots, pebble and polish have nothing; the claw only sits in a shop no NPC opens; the
    # skill reset card comes from the cash shop
    assert none == {"equip:2", "equip:3", "etc:21", "etc:42"}
    p = "items/no-source.html"
    html = page(built, p)
    for e in KB_ROWS["item"]:
        assert (href(built, p, "item", e["id"]) in html) == (e["id"] in none), e["id"]
    assert href(built, p, "shop", "9") in html
    assert "No known source" in page(built, "item/equip-3.html")


def test_equipment_page(built):
    p = "item/equip-1.html"
    cap = page(built, p)
    box = cap[cap.index('<aside class="infobox"') : cap.index("</aside>")]
    assert "item/equip/1.worn.png" in box and "item/equip/1.png" in box
    assert "<dt>DEF</dt><dd>3-6</dd>" in box
    assert "+2 AVOID (crafted copies only)" in box
    assert "5, or 7 when crafted" in box
    assert "Buy price</dt><dd>600 meso" in box and "Sell price</dt><dd>90 meso" in box
    notes = part(cap, "notes")
    assert "minimum of every stat range and 5 upgrade slots" in notes
    assert "7 upgrade slots" in notes
    assert href(built, p, "item", "etc:21") in part(cap, "upgrading")
    claw = page(built, "item/equip-2.html")
    assert "an attack every 4 ticks (0.2 s)" in claw
    assert "7 when crafted" not in claw and "7 upgrade slots" not in claw


def test_other_item_page(built):
    tonic = page(built, "item/etc-3.html")
    assert "Restores 40 HP." in tonic
    desc = '<p class="ib-desc" data-verbatim lang="en">A made-up thing.<br>Second line</p>'
    assert desc in tonic
    assert "<dt>Stack size</dt><dd>100</dd>" in tonic


def test_comparisons(built):
    p = "items/comparisons.html"
    html = page(built, p)
    assert href(built, p, "item", "equip:2") in part(html, "claws")
    potions = part(html, "potions")
    assert href(built, p, "item", "etc:3") in potions
    assert re.search(r'data-k="2(\.0)?">2</td>', potions)  # 40 HP for 20 meso


def test_prose_lint_is_clean(built):
    problems = []
    for path, data in built.files.items():
        if path.startswith(("item/", "items/")) and path.endswith(".html"):
            problems += prose_lint.lint_text(data.decode("utf-8"), kind="html")
    assert problems == []


def test_helpers():
    assert sec.and_list(["a", "b", "c"]) == "a, b, and c"
    assert sec.and_list(["a", "b"]) == "a and b"
    assert sec.chapters_text([1, 2, 3]) == "" and sec.chapters_text([2, 3]) == "chapters 2 and 3"
    assert sec.band_text(5) == "1-9" and sec.band_text(23) == "20-29"
