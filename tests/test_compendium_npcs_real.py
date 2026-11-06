"""The NPCs and shops section (#220) on the real bundle (local only: needs work/ipa/).

The pages are checked against the game's own files, not only the knowledge base: every used
``market.dat`` row is in exactly one shop's stock with its price; every ``makeEquipItem`` the
script walk finds has a recipe row with the script's materials and fee; five NPCs stand where
their maps' ``.obj`` files put them, under their ``npc.dat`` names.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

import pytest

from mtport import datatables, objfile
from mtport.compendium import kb as kbm
from mtport.compendium.sections import npcs
from mtport.compendium.siteapi import KB, Context, Images, Page, name_of

pytestmark = pytest.mark.real_assets

APP = Path("Payload") / "MapleThief.app"
USED_MARKET_ROWS = 202  # rows 202 to 219 are all zero (docs/formats/data-tables.md)
CHECKED_NPCS = ("1", "10", "12", "18", "20")


@pytest.fixture(scope="module")
def section(real_kb: kbm.KB) -> tuple[Context, dict[str, Page]]:
    kb = KB({kind: list(rows.values()) for kind, rows in real_kb.entities.items()})
    ctx = Context(kb, Images(), Path("docs/compendium/pages"))
    return ctx, {p.path: p for p in npcs.pages(ctx)}


def rows_of(body: str) -> dict[str, str]:
    """Each anchored table row of a page body by its id."""
    return {m.group(1): m.group(0) for m in re.finditer(r'<tr[^>]* id="([^"]+)".*?</tr>', body)}


def test_counts(section) -> None:
    ctx, pages = section
    assert sum(p.startswith("npc/") for p in pages) == 39
    assert sum(p.startswith("shop/") for p in pages) == len(ctx.kb.entities("shop")) == 14
    crafting = rows_of(pages[npcs.CRAFTING].body)
    assert sum(k.startswith("recipe-") for k in crafting) == 48
    assert sum(k.startswith("trade-") for k in rows_of(pages[npcs.TRADES].body)) == 20


def test_every_market_row_is_in_exactly_one_shop(section, ipa_dir: Path) -> None:
    _, pages = section
    market = datatables.load_dir(ipa_dir / APP / "English" / "data")["market"].records
    used = [(i, r) for i, r in enumerate(market) if any((r.shop, r.table, r.item, r.price))]
    assert len(used) == USED_MARKET_ROWS
    where: dict[str, list[str]] = {}
    for path, page in pages.items():
        for anchor in rows_of(page.body):
            if anchor.startswith("listing-"):
                where.setdefault(anchor, []).append(path)
    assert set(where) == {f"listing-{i}" for i, _ in used}
    for i, r in used:
        assert where[f"listing-{i}"] == [f"shop/{r.shop}.html"], i
        row = rows_of(pages[f"shop/{r.shop}.html"].body)[f"listing-{i}"]
        item = f"{'equip' if r.table == 0 else 'etc'}:{r.item}"
        assert f"\x00Eitem\x01{item}\x00" in row, i  # links the item market.dat names
        assert f"{r.price:,}" in row, i
    # Two shops in full, in market.dat order: Mr. Lee's in the first towns and shop 8.
    for shop in (2, 8):
        anchors = [a for a in rows_of(pages[f"shop/{shop}.html"].body) if a.startswith("listing")]
        assert anchors == [f"listing-{i}" for i, r in used if r.shop == shop]
    # The data slip is noted on its shop's page and on the page of the shop it belongs to.
    assert "Row 146</a> is a probable data slip" in pages["shop/0.html"].body
    assert "Row 146" in pages["shop/8.html"].body


def _walk(dialogue: dict[str, Any]):
    """Every path of a conversation from its root with the calls made before it, yielding
    ``(history, path)``; loops are not followed."""
    nodes = dialogue["nodes"]

    def go(nid: str, history: tuple, seen: frozenset):
        for p in nodes[nid]["paths"]:
            calls = history + tuple(x for x in p["do"] if "call" in x)
            yield calls, p
            nxt = p.get("next")
            if nxt and nxt not in seen:
                yield from go(nxt, calls, seen | {nxt})

    if dialogue.get("tree"):
        yield from go(dialogue["tree"], (), frozenset({dialogue["tree"]}))


def test_every_make_equip_item_has_a_recipe(section, real_kb: kbm.KB) -> None:
    ctx, pages = section
    crafting = rows_of(pages[npcs.CRAFTING].body)
    found = set()
    calls = 0
    for d in real_kb.entities["dialogue"].values():
        for history, path in _walk(d):
            for x in path["do"]:
                if x.get("call") != "makeEquipItem":
                    continue
                calls += 1
                quest = x["args"][:2]
                start = max(
                    i
                    for i, c in enumerate(history)
                    if c["call"] == "setQuest" and c["args"][:2] == quest
                )
                prep = history[start:]
                reward = [c["args"] for c in prep if c["call"] == "regRewardItem"]
                needs = sorted(
                    (f"{'equip' if a[0] == 0 else 'etc'}:{a[1]}", a[2])
                    for a in (c["args"] for c in prep if c["call"] == "regRequireItem")
                )
                meso = sum(c["args"][0] for c in prep if c["call"] == "regRequireMeso")
                for kind, item, _count in reward:
                    target = f"{'equip' if kind == 0 else 'etc'}:{item}"
                    for chapter in d["chapters"]:
                        rid = next(
                            r["id"]
                            for r in real_kb.entities["recipe"].values()
                            if r["chapter"] == chapter
                            and r["result_item"] == target
                            and sorted((m["item"], m["count"]) for m in r["material_items"])
                            == needs
                        )
                        r = real_kb.entities["recipe"][rid]
                        assert r["meso"] == meso, rid
                        row = crafting[f"recipe-{rid.replace(':', '-')}"]
                        assert f"{meso:,}" in row
                        found.add(rid)
    assert calls > 0
    assert found == set(real_kb.entities["recipe"])


def test_five_npcs_stand_where_the_obj_files_put_them(section, ipa_dir: Path) -> None:
    ctx, pages = section
    names = {
        r.id: " ".join(r.name.replace("$", " ").split())
        for r in datatables.load_dir(ipa_dir / APP / "English" / "data")["npc"].records
    }
    for nid in CHECKED_NPCS:
        npc = ctx.kb.get("npc", nid)
        page = pages[f"npc/{nid}.html"]
        assert name_of(npc) == names[int(nid)]
        assert page.title == names[int(nid)]
        placed = 0
        for m in ctx.kb.entities("map"):
            obj = objfile.load(ipa_dir / APP / "map" / f"{m['file']}.obj")
            for rec in obj.objects:
                if rec.kind != int(nid) + objfile.NPC_KINDS.start:
                    continue
                x, y = rec.tile(int(m["width"]))
                placed += 1
                assert f"x {x}, y {y}" in page.body, (nid, m["id"])
                if rec.name:
                    assert f"<code>{rec.name}</code>" in page.body, (nid, m["id"])
                assert f"\x00Emap\x01{m['id']}\x00" in page.body
        assert placed > 0, nid


def test_two_recipes_and_two_shops_say_where(section) -> None:
    ctx, pages = section
    crafting = rows_of(pages[npcs.CRAFTING].body)
    for rid in ("1-equip:1", "3-equip:11"):
        r = ctx.kb.get("recipe", rid)
        row = crafting[f"recipe-{rid.replace(':', '-')}"]
        crafter = r["crafted_by"][0]
        assert f"\x00Enpc\x01{crafter['id']}\x00" in row
        assert f"\x00Emap\x01{crafter['map']}\x00" in row
        assert "\x00Earea\x01" in row
    for sid in ("2", "8"):
        page = pages[f"shop/{sid}.html"]
        assert not page.title.startswith("Shop")
        for r in ctx.kb.get("shop", sid)["opened_by"]:
            assert f"\x00Enpc\x01{r['id']}\x00" in page.body
            assert f"\x00Emap\x01{r['map']}\x00" in page.body
    for sid in ("0", "1", "16"):
        assert "No script opens it" in pages[f"shop/{sid}.html"].body
