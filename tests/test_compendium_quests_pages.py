"""The quests section of the compendium (#223) on a synthetic knowledge base.

Every quest has a page with a non-empty, lint-clean "How to complete"; every text key of every
conversation is on exactly one page in both languages; conversations split into parts with
the right links; the chapter pages draw the quest chain.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from mtport import prose_lint
from mtport.compendium import site
from mtport.compendium.sections import _talk
from mtport.compendium.sections import quests as qs
from tests.fixtures.compendium import quests_kb as fx


@pytest.fixture
def built(tmp_path: Path) -> site.Site:
    kb = fx.write_kb(tmp_path)
    pages = tmp_path / "pages"
    pages.mkdir()
    return site.build_site(kb, pages, None, plugins=[qs.SECTION])


def html(s: site.Site, path: str) -> str:
    return s.files[path].decode("utf-8")


def section_body(page: str, anchor: str) -> str:
    m = re.search(rf'<h2 id="{anchor}">.*?</h2>(.*?)</section>', page, re.S)
    assert m, anchor
    return m.group(1)


def test_one_page_per_quest_and_conversation(built: site.Site) -> None:
    for q in fx.KB["quest"]:
        assert f"quests/{q['id']}.html" in built.pages
        assert built.registry[("quest", q["id"])] == (f"quests/{q['id']}.html", "")
    for e in fx.KB["dialogue"]:
        assert built.registry[("dialogue", e["id"])] == (f"quests/dialogue/{e['id']}.html", "")
    for p in (
        "quests/index.html",
        "quests/chapter-1.html",
        "quests/by-giver.html",
        "quests/by-area.html",
        "quests/by-level.html",
        "quests/dialogue/index.html",
    ):
        assert p in built.pages
    missing, _ = site.completeness(built)
    assert [m for m in missing if ":quest:" in m or "dialogue:" in m] == []
    assert site.check_links(built.files) == []


def test_every_quest_has_steps_in_our_words(built: site.Site) -> None:
    for q in fx.KB["quest"]:
        page = html(built, f"quests/{q['id']}.html")
        steps = section_body(page, "how-to-complete")
        assert steps.count("<li>") >= 2, q["id"]
        assert 'href="' in steps
        findings = prose_lint.lint_text(page, kind="html", path=q["id"])
        assert findings == [], findings
    side = section_body(html(built, "quests/1-sub-5.html"), "how-to-complete")
    # giver and map, the item with its source and odds, the turn-in
    assert "Talk to" in side and "Old Fen" in side and "Hollow Edge" in side
    assert "Collect 4" in side and "Slug Shell" in side and "60.2%" in side
    assert "Return to" in side
    main = html(built, "quests/1-main-1.html")
    assert "First, finish" in section_body(main, "how-to-complete")
    assert "Defeat 3" in section_body(main, "how-to-complete")
    # no turn-in: said in the steps and the notes
    assert "No conversation finishes this quest" in main


def test_prose_file_replaces_the_generated_steps(tmp_path: Path) -> None:
    kb = fx.write_kb(tmp_path)
    pages = tmp_path / "pages"
    (pages / "quests").mkdir(parents=True)
    (pages / "quests" / "1-sub-5.md").write_text(
        "1. Visit {{npc:7}} at dawn.\n2. Bring {{item:etc:3}}.\n", encoding="utf-8"
    )
    s = site.build_site(kb, pages, None, plugins=[qs.SECTION])
    steps = section_body(html(s, "quests/1-sub-5.html"), "how-to-complete")
    assert "at dawn" in steps and "Collect" not in steps


def test_every_dialogue_key_once_in_both_languages(built: site.Site) -> None:
    from mtport.compendium.siteapi import KB

    kb = KB(fx.KB)
    assert qs.key_problems(built.files, kb) == []
    homes = {}
    for path, data in built.files.items():
        if path.endswith(".html"):
            for k in re.findall(r'data-key="([^"]*)"', data.decode("utf-8")):
                homes[k] = path
    # quest titles and descriptions on the quest page; quest lines there too
    assert homes["Fen.q5.title"] == homes["Fen.q5.desc"] == "quests/1-sub-5.html"
    assert homes["Fen.offer.1"] == homes["Fen.reward.1"] == "quests/1-sub-5.html"
    # general lines, the menu and lines no path shows on the conversation's page
    assert homes["Fen.hello.1"] == "quests/dialogue/chapter1-Fen.html"
    assert homes["Fen.menu.b"] == "quests/dialogue/chapter1-Fen.html"
    assert homes["Fen.unused.1"] == "quests/dialogue/chapter1-Fen.html"
    # the gate repeats Fen's line: it links to it instead of showing it again
    gate = html(built, "quests/1-main-1.html")
    assert "Says the line of" in gate
    assert "Hello again." not in gate
    # the line without Korean says so
    assert "The game has no Korean text for this line." in html(built, homes["Fen.wait.1"])


def test_key_problems_reports_a_missing_or_doubled_key(built: site.Site) -> None:
    from mtport.compendium.siteapi import KB

    kb = KB(fx.KB)
    files = dict(built.files)
    p = "quests/dialogue/chapter1-Fen.html"
    files[p] = files[p].replace(b'data-key="Fen.unused.1"', b"")
    files["extra.html"] = b'<li data-key="Fen.hello.1">Hello again.</li>'
    problems = qs.key_problems(files, kb)
    assert any("Fen.unused.1 is on 0" in x for x in problems)
    assert any("Fen.hello.1 is on 2" in x for x in problems)


def test_parts_of_a_conversation() -> None:
    fen = fx.kb()["dialogue"][0]
    talk = _talk.build(fen)
    by_first = {p.lines[0].key: p for p in talk.parts}
    offer = by_first["Fen.offer.1"]
    assert [ln.key for ln in offer.lines] == ["Fen.offer.1", "Fen.offer.2"]
    labels = {lab: talk.part(n).lines[0].key for lab, n in offer.nexts}
    assert labels == {"Accept": "Fen.accept.1", "Refuse": "Fen.refuse.1"}
    assert offer.owner == (1, 5) and offer.when == {
        "getQuestState(1, 5)": frozenset({0}),
        "getLevel()": frozenset(range(5, 100)),
    }
    assert ("setQuest", (1, 5, "Fen.q5.title", "Fen.q5.desc")) in offer.calls
    accept = by_first["Fen.accept.1"]
    assert ("startQuest", (1, 5)) in accept.calls and accept.ends
    # each menu row starts a part, which runs on into its own reply
    row_a, row_b = by_first["Fen.menu.a"], by_first["Fen.menu.b"]
    assert row_a.menu and row_b.menu
    assert [ln.key for ln in row_a.lines] == ["Fen.menu.a", "Fen.menu.a.reply"]
    assert [ln.who for ln in row_b.lines] == ["choice", "hero"]
    assert row_a.ends and not row_a.nexts
    hello = by_first["Fen.hello.1"]
    assert [ln.key for ln in hello.lines] == ["Fen.hello.1", "Fen.menu.ask"]
    assert {talk.part(n).lines[0].key for _, n in hello.nexts} == {"Fen.menu.a", "Fen.menu.b"}
    # every shown key is in exactly one part
    keys = [ln.key for p in talk.parts for ln in p.lines]
    assert len(keys) == len(set(keys))


def test_necessary_conditions() -> None:
    alts = [
        [
            {"term": "getQuestState(1, 2)", "op": "!=", "value": 0},
            {"term": "x()", "op": "==", "value": 1},
        ],
        [{"term": "getQuestState(1, 2)", "op": "==", "value": 2}],
        [  # impossible: dropped
            {"term": "getQuestState(1, 2)", "op": ">=", "value": 2},
            {"term": "getQuestState(1, 2)", "op": "!=", "value": 2},
        ],
    ]
    assert _talk.necessary(alts) == {"getQuestState(1, 2)": frozenset({1, 2})}
    assert (
        _talk.compatible(
            [[{"term": "getQuestState(1, 2)", "op": "==", "value": 1}]],
            [[{"term": "getQuestState(1, 2)", "op": "==", "value": 2}]],
        )
        is False
    )


def test_quest_page_sections_and_notes(built: site.Site) -> None:
    side = html(built, "quests/1-sub-5.html")
    order = [
        side.index(f'id="{a}"')
        for a in (
            "prerequisites",
            "requirements",
            "rewards",
            "how-to-complete",
            "dialogue",
            "follow-up",
            "notes",
        )
    ]
    assert order == sorted(order)
    assert '<span data-verbatim lang="ko">펜의 껍질</span>' in side
    assert "1,500" in side and "Lv. 5" in side
    # a side quest of a chapter that ends with clearAll is warned about
    assert "resets every quest state" in side
    # the approximate NPC-state pass of Fen's script is noted
    assert "approximate" in side
    # accept and refuse links, and the dialogue toggle
    assert "Accept leads to" in side and "Refuse leads to" in side
    assert 'class="lang-toggle"' in side
    # the gate's part runs while the main quest is in progress: it is on that quest's page
    assert "The chapter ends, and every quest state is reset" in html(built, "quests/1-main-1.html")


def test_chapter_page_draws_the_chain(built: site.Site) -> None:
    ch = html(built, "quests/chapter-1.html")
    svg = ch[ch.index('<svg class="chain-svg"') :]
    svg = svg[: svg.index("</svg>")]
    assert svg.count("<rect") == 2
    assert 'href="1-sub-5.html"' in svg and 'href="1-main-1.html"' in svg
    assert 'marker-end="url(#arrow)"' in svg and "stroke-dasharray" not in svg
    assert 'data-verbatim lang="en"' in svg


def test_lists(built: site.Site) -> None:
    index = html(built, "quests/index.html")
    assert index.index("1-main-1.html") < index.index("1-sub-5.html")  # main quests first
    assert "original bug 11" in index
    level = html(built, "quests/by-level.html")
    assert level.index("1-main-1.html") < level.index("1-sub-5.html")  # no level, then 5
    assert "Old Fen" in html(built, "quests/by-giver.html")
    assert "Mossy Hollow" in html(built, "quests/by-area.html")
    for path, data in built.files.items():
        if path.startswith("quests/") and path.endswith(".html"):
            assert prose_lint.lint_text(data.decode("utf-8"), kind="html", path=path) == []
