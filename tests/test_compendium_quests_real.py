"""The quests stage (#217) on the real bundle (local only: needs work/ipa/).

Every script walks; every quest a reachable conversation prepares exists with a giver; every
dialogue key has its English and Korean text; every warp, shop and crafting target exists;
the quest counts per chapter (docs/ANALYSIS.md).
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Iterator
from typing import Any

import pytest

from mtport.compendium import kb as kbm
from mtport.compendium import quests

pytestmark = pytest.mark.real_assets

# The one key with English text only (Scripts.strings in ko.lproj lacks it).
NO_KOREAN = {"Chapter3_Quest_Zenumist_Chief.dialog.1"}


def calls(dialogue: dict[str, Any], native: str) -> Iterator[list]:
    """The arguments of every call of ``native`` on the paths of a conversation."""
    for node in dialogue["nodes"].values():
        for path in node["paths"]:
            for row in path["do"]:
                if row.get("call") == native:
                    yield row["args"]


def test_every_script_walks(real_kb: kbm.KB) -> None:
    e = real_kb.entities
    assert len(e["script"]) == 307
    assert real_kb.problems == []  # walk errors and unresolved values are problems
    # Exact everywhere but one NPC-state pass (its or-chains are past the exact budget).
    approx = {
        i: s["approximate_entries"] for i, s in e["script"].items() if s["approximate_entries"]
    }
    assert approx == {"chapter3-ZenumistChief": ["state"]}


def test_every_prepared_quest_has_a_giver(real_kb: kbm.KB) -> None:
    e = real_kb.entities
    seen = 0
    for d in e["dialogue"].values():
        for args in calls(d, "setQuest"):
            kind, number = args[:2]
            for c in d["chapters"]:
                q = e["quest"].get(quests.quest_id(c, kind, number))
                if q is not None:
                    assert q["given_by"], q["id"]
                else:  # a recipe or a trade: prepared and completed in one conversation
                    deals = [*e["recipe"].values(), *e["trade"].values()]
                    assert any(x["chapter"] == c and x["quest"] == [kind, number] for x in deals)
                seen += 1
    assert seen > 0
    assert all(q["given_by"] and q["started"] and q["reachable"] for q in e["quest"].values())


def test_every_dialogue_key_has_both_languages(real_kb: kbm.KB) -> None:
    keys = 0
    for d in real_kb.entities["dialogue"].values():
        for key, text in d["text"].items():
            keys += 1
            assert text["en"], key
            assert text["ko"] or key in NO_KOREAN, key
        assert set(d["missing_text"]) <= NO_KOREAN
    assert keys > 1000


def test_every_target_exists(real_kb: kbm.KB) -> None:
    e = real_kb.entities
    for w in e["warp"].values():
        assert w["target_map"] in e["map"], w["id"]
        assert [t["id"] for t in w["target"]] == [w["target_map"]]
    for d in e["dialogue"].values():
        for (shop,) in calls(d, "showMarket"):
            assert str(shop) in e["shop"], (d["id"], shop)
        for kind, number in calls(d, "makeEquipItem"):
            for c in d["chapters"]:
                made = [
                    r
                    for r in e["recipe"].values()
                    if r["chapter"] == c and r["quest"] == [kind, number]
                ]
                assert made and all(r["result_item"] in e["item"] for r in made), (d["id"], c)
    for npc in e["npc"].values():
        assert all(s["id"] in e["shop"] for s in npc["opens_shop"])


def test_quest_counts_per_chapter(real_kb: kbm.KB) -> None:
    e = real_kb.entities
    by = Counter((q["chapter"], q["quest_kind"]) for q in e["quest"].values())
    assert dict(by) == {
        (0, "main"): 7,
        (1, "main"): 15,
        (1, "sub"): 26,
        (2, "main"): 12,
        (2, "sub"): 36,
        (3, "main"): 9,
        (3, "sub"): 33,
    }
    assert len(e["quest"]) == 138
    assert Counter(r["chapter"] for r in e["recipe"].values()) == {1: 12, 2: 18, 3: 18}
    assert len(e["trade"]) == 20
    assert len(e["job_advancement"]) == 11
    assert len(e["dialogue"]) == 108


def test_unused_scripts(real_kb: kbm.KB) -> None:
    s = real_kb.entities["script"]
    unused = [x for x in s.values() if not x["used"]]
    reasons = Counter(x["unused_reason"].split(",")[0].split(" ")[0] for x in unused)
    # 11 files no map object names (10 names: YetiMap is in chapter2/ and common/), 4
    # portals on maps no chapter reaches, 24 common/ files a chapter file overrides
    assert reasons == {"no": 15, "in": 24}
    assert sum(x["unused_reason"].startswith("no map that") for x in unused) == 4
    unnamed = {x["base_name"] for x in unused if x["unused_reason"] == "no map object names it"}
    assert unnamed == {
        "Barun_old",
        "Darklord_0827",
        "Darklord_new",
        "Darklord_ori",
        "Manzie_error",
        "Spiegelman1",
        "Spiegelman2",
        "Spiegelman3",
        "YetiMap",
        "pig_old",
    }


def test_every_shop_recipe_and_trade_can_be_found(real_kb: kbm.KB) -> None:
    """Shop, recipe or trade -> the NPC that offers it -> a map it stands on -> the area."""
    e = real_kb.entities

    def placed(npc_ref: dict) -> None:
        npc = e["npc"][npc_ref["id"]]
        assert npc_ref["map"] in {m["id"] for m in npc["stands_on"]}, npc_ref
        assert e["map"][npc_ref["map"]]["area"], npc_ref

    unopened = set()
    for ident, shop in e["shop"].items():
        assert shop["listings"]
        if shop["opened_by"]:
            assert shop["unopened_reason"] is None
            for ref in shop["opened_by"]:
                assert ref["chapters"]
                placed(ref)
        else:
            assert shop["unopened_reason"] == quests.NO_OPENER
            unopened.add(ident)
    # No script calls showMarket with these (the only way to the shop screen 0x4628D).
    assert unopened == {"0", "1", "16"}
    for kind, field in (("recipe", "crafted_by"), ("trade", "offered_by")):
        for x in e[kind].values():
            assert x[field], x["id"]
            for ref in x[field]:
                placed(ref)
