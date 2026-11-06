"""The Items section (#219) on the real knowledge base (local only: needs work/ipa/).

A page for each of the 212 items, every "How to get" entry a knowledge-base source, the
items with no known source, and the four items the scripts hand over with ``getItem``.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from mtport.compendium import kb as kbm
from mtport.compendium import site
from mtport.compendium.sections import items as sec
from mtport.compendium.siteapi import Context, Images, load_kb_dir

pytestmark = pytest.mark.real_assets

# Items nothing in 1.5 hands out (ids only): two equipment rows, the Monster Carnival
# tickets, quest items and materials no script gives, Reverse, the two cash items no product
# grants, the 100% scrolls, and equip 22, listed only in shop 0, which no NPC opens.
NO_SOURCE = {
    "equip:22",
    "equip:70",
    "equip:72",
    "etc:47",
    "etc:48",
    "etc:49",
    "etc:86",
    "etc:89",
    "etc:91",
    "etc:92",
    "etc:107",
    "etc:109",
    "etc:113",
    "etc:128",
    "etc:130",
    "etc:131",
    "etc:133",
    "etc:134",
    "etc:135",
    "etc:136",
    "etc:137",
}

# getItem(kind, id, n) outside quest rewards: (item, npc, map, chapter).
GIFTS = {
    ("etc:108", "13", "1-0", 1),
    ("etc:87", "16", "9-0", 2),
    ("etc:114", "16", "9-0", 2),
    ("etc:119", "33", "7-0", 3),
}


@pytest.fixture(scope="module")
def real_site(real_kb: kbm.KB, tmp_path_factory) -> site.Site:
    out: Path = tmp_path_factory.mktemp("kb")
    kbm.write(real_kb, out)
    return site.build_site(out, None, None)


def test_every_item_has_a_page(real_site: site.Site) -> None:
    item_pages = [p for p in real_site.pages if p.startswith("item/")]
    assert len(item_pages) == 212
    missing, counts = site.completeness(real_site)
    assert missing == [] and counts["item"] == (212, 212)
    assert site.check_links(real_site.files) == []
    for path in ("items/index.html", "items/no-source.html", "items/comparisons.html"):
        assert path in real_site.pages


def test_sources_are_real_and_no_source_is_complete(real_kb: kbm.KB, tmp_path: Path) -> None:
    kbm.write(real_kb, tmp_path)
    ctx = Context(load_kb_dir(tmp_path), Images(), None)
    for e in ctx.kb.entities("item"):
        for s in sec.sources(ctx, e):
            assert s.how == "cash" or ctx.kb.get(s.kind, s.id) is not None, (e["id"], s)
    assert {e["id"] for e in sec.no_source_items(ctx)} == NO_SOURCE


def test_script_gifts(real_kb: kbm.KB) -> None:
    got = set()
    for iid, item in real_kb.entities["item"].items():
        for r in item["given_by_npcs"]:
            got.add((iid, r["id"], r["map"], tuple(r["chapters"])))
    assert got == {(i, n, m, (c,)) for i, n, m, c in GIFTS}
