"""The NPCs and shops section (#220) on a synthetic knowledge base: a page per NPC and shop,
an anchor per listing, recipe and trade, shops named and located, the data slip noted, the
conditions simplified, and the prose lint clean on every page the section writes."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from mtport import prose_lint
from mtport.compendium import site
from mtport.compendium.sections import npcs
from mtport.compendium.siteapi import KB, Context, Images
from tests.fixtures.compendium import npcs_kb as fx
from tests.fixtures.compendium import site_kb


@pytest.fixture
def built(tmp_path: Path) -> site.Site:
    kb = site_kb.write_kb(tmp_path, fx.kb())
    pages = site_kb.write_prose(tmp_path, {})
    return site.build_site(kb, pages, None, plugins=[npcs.SECTION])


def text(s: site.Site, path: str) -> str:
    return s.files[path].decode("utf-8")


def plain(html: str) -> str:
    main = html[html.index("<main") : html.index("</main>")]
    return " ".join(re.sub(r"<[^>]+>", " ", main).split())


def ctx() -> Context:
    return Context(KB(fx.kb()), Images(), None)


# --- pages and anchors ------------------------------------------------------------------------


def test_every_npc_and_shop_has_a_page_and_every_row_an_anchor(built):
    assert {f"npc/{n['id']}.html" for n in fx.KB["npc"]} <= set(built.pages)
    assert {f"shop/{s['id']}.html" for s in fx.KB["shop"]} <= set(built.pages)
    reg = built.registry
    for r in fx.KB["recipe"]:
        assert reg[("recipe", r["id"])] == (npcs.CRAFTING, f"recipe-{r['id'].replace(':', '-')}")
    for x in fx.KB["trade"]:
        assert reg[("trade", x["id"])][0] == npcs.TRADES
    missing, _ = site.completeness(built)
    assert missing == []
    assert site.check_links(built.files) == []


def test_every_listing_is_in_exactly_one_shop(built):
    seen: dict[str, list[str]] = {}
    for path in built.pages:
        for m in re.finditer(r'<tr[^>]* id="listing-(\d+)"', text(built, path)):
            seen.setdefault(m.group(1), []).append(path)
    assert seen == {lst["id"]: [f"shop/{lst['shop'][0]['id']}.html"] for lst in fx.KB["listing"]}


def test_shops_are_named_by_keeper_and_town_not_by_number(built):
    page = text(built, "shop/2.html")
    title = re.search(r"<title>([^<]*)</title>", page).group(1)
    assert title.startswith("Tinker Bo&#x27;s equipment shop in Fernvale")
    assert "Shop 2" not in plain(page)
    # where to find it: the keeper, the map and the area, linked, and the chapters
    assert 'href="../npc/1.html"' in page
    assert 'href="../map/1-0.html"' in page and 'href="../area/1.html"' in page
    assert "in chapters 1 and 2" in plain(page)
    # the shop link elsewhere carries the name too
    assert "equipment shop in" in plain(text(built, "npc/1.html"))
    assert "equipment shop in" in plain(text(built, "npcs/index.html"))


def test_unopened_shop_and_data_slip(built):
    page = plain(text(built, "shop/0.html"))
    assert page.startswith("Unopened equipment shop")
    assert "No script opens it" in page and "showMarket" in page
    assert "Row 1 is a probable data slip" in page
    assert "where it probably belongs" in page  # points at shop 2, whose rows surround it
    assert "Bought equipment" not in page
    home = plain(text(built, "shop/2.html"))
    assert "Row 1" in home and "probably belongs here" in home


def test_npc_page_location_route_and_roles(built):
    smith = plain(text(built, "npc/2.html"))
    assert "crafter" in smith and "quest giver" in smith
    assert "From the town map Fernvale Square (1.0), Mossy Path is 1 step away." in smith
    assert "Warm the Forge" in smith  # quests, linked
    page = text(built, "npc/2.html")
    assert 'href="../npcs/crafting.html"' in page
    # dialogue: links only; the conversations belong to the quests section
    assert 'href="../dialogue/chapter1-forge.html"' in page
    assert "K.busy" not in page


def test_hidden_and_icon_notes(built):
    smith = plain(text(built, "npc/2.html"))
    assert "the game hides it when main quest 5 ( Warm the Forge ) is started or done." in smith
    assert "setNpcNotify" in smith and "2 in chapter 1" in smith


def test_decoration_and_unplaced_npcs(built):
    cat = plain(text(built, "npc/4.html"))
    assert "appears only as decoration" in cat
    assert "No portal or warp leads to this map." in cat
    assert "(2.3)" in cat  # shares its name with 1-0: the number tells them apart
    owl = plain(text(built, "npc/5.html"))
    assert "no map object places it" in owl


def test_crafting_page_lists_recipes_by_slot_with_conditions(built):
    page = plain(text(built, npcs.CRAFTING))
    assert page.index("Weapons") < page.index("Hats")
    assert "2 × Pebble" in page and "50 meso" in page
    assert "Smith Ada on Mossy Path" in page
    # two root alternatives that differ only in sub quest 2 merge into one condition
    assert "when main quest 5 ( Warm the Forge ) is started or done." in page
    trades = plain(text(built, npcs.TRADES))
    assert "50 × Pebble" in trades and "3 × Shard" in trades and "at any time" in trades


def test_prose_lint_clean_on_the_section(built):
    for path, page in built.pages.items():
        if page.section != "npcs":
            continue
        assert prose_lint.lint_text(text(built, path), kind="html") == [], path


# --- helpers ----------------------------------------------------------------------------------


def test_chapters_words():
    assert npcs.chapters_text([1, 2, 3]) == "chapters 1 to 3"
    assert npcs.chapters_text([2, 3]) == "chapters 2 and 3"
    assert npcs.chapters_text([3]) == "chapter 3"
    assert npcs.chapters_text([0, 1]) == "the tutorial and chapter 1"
    assert npcs.chapters_text([1, 3]) == "chapters 1 and 3"
    assert npcs.chapters_cell([1, 2, 3]) == "1-3"
    assert npcs.chapters_cell([0, 2]) == "tutorial, 2"
    assert npcs.join_words(["a", "b", "c"]) == "a, b, and c"


def q(term: str, op: str, value: int) -> dict:
    return {"term": term, "op": op, "value": value}


def test_simplify_merges_and_drops():
    gq = "getQuestState(0, 40)"
    sq = "getQuestState(1, 7)"
    s33 = "getQuestState(1, 33)"
    alts = npcs.simplify(
        [
            [q(gq, ">=", 1), q(sq, "<", 1)],
            [q(gq, ">=", 1), q(s33, ">=", 2), q(sq, ">=", 1)],
            [q(gq, ">=", 1), q(s33, "<", 2), q(sq, ">=", 1), q("answer#1", "==", 2)],
        ]
    )
    assert len(alts) == 1
    ((var, vals),) = alts[0].items()
    assert var.args == (0, 40) and vals == frozenset({1, 2})
    # a condition that covers another drops it; contradictions vanish
    assert npcs.simplify([[q(gq, "==", 1)], [q(gq, ">=", 1)]])[0][
        npcs.Var("getQuestState", (0, 40))
    ] == frozenset({1, 2})
    assert npcs.simplify([[q(gq, "==", 1), q(gq, "==", 2)]]) == []
    assert npcs.simplify([[]]) == [{}]


def test_condition_words():
    c = ctx()
    v, vals = npcs._parse(q("checkRequire(1, 77)", "!=", 1))
    assert "lacks" in npcs.condition_markup(c, 1, v, vals)
    v, vals = npcs._parse(q("getLevel()", ">=", 15))
    assert npcs.condition_markup(c, 1, v, vals) == "the hero is level 15 or higher"
    assert npcs._parse(q("answer#2", "==", 1)) is None


def test_offer_conditions_follow_the_conversation():
    c = ctx()
    recipe = c.kb.get("recipe", "1-equip:2")
    alts = npcs.offer_conditions(c, recipe, "equip:2")
    assert alts == [{npcs.Var("getQuestState", (0, 5)): frozenset({1, 2})}]
    # an item the conversation never prepares is never offered
    assert npcs.offer_conditions(c, recipe, "etc:4") == []


def test_shop_title_and_slip_home():
    c = ctx()
    assert npcs.shop_title(c, "2") == "Tinker Bo's equipment shop in Fernvale"
    assert npcs.shop_title(c, "0") == "Unopened equipment shop"
    assert npcs.slip_home(c, c.kb.get("listing", "1")) == "2"
    assert npcs.slip_home(c, c.kb.get("listing", "0")) == ""
