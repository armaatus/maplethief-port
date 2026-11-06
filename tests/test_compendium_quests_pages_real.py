"""The quests section (#223) on the real knowledge base (local only: needs work/ipa/).

One page per quest the extraction finds and per conversation; every dialogue key of every
reachable script on exactly one page in both languages (or a note that the game lacks one);
every quest's "How to complete" non-empty and lint-clean; no broken link.
"""

from __future__ import annotations

import re

import pytest

from mtport import prose_lint
from mtport.compendium import kb as kbm
from mtport.compendium import site
from mtport.compendium.sections import quests as qs
from mtport.compendium.siteapi import load_kb_dir

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def built(real_kb: kbm.KB, tmp_path_factory: pytest.TempPathFactory):
    out = tmp_path_factory.mktemp("kb") / "kb"
    kbm.write(real_kb, out)
    pages = tmp_path_factory.mktemp("pages")
    return site.build_site(out, pages, None, plugins=[qs.SECTION]), load_kb_dir(out)


def test_one_page_per_quest_and_conversation(built) -> None:
    s, kb = built
    quests = kb.entities("quest")
    dialogues = kb.entities("dialogue")
    assert len(quests) == 138 and len(dialogues) == 108
    for q in quests:
        assert s.registry[("quest", q["id"])] == (qs.quest_path(q["id"]), "")
    for e in dialogues:
        assert s.registry[("dialogue", e["id"])] == (qs.dialogue_path(e["id"]), "")
    missing, counts = site.completeness(s)
    assert counts["quest"] == (138, 138) and counts["dialogue"] == (108, 108)
    assert site.check_links(s.files) == []


def test_every_dialogue_key_once_in_both_languages(built) -> None:
    s, kb = built
    assert qs.key_problems(s.files, kb) == []
    keys = sum(len(e.get("text") or {}) for e in kb.entities("dialogue"))
    shown = set()
    for path, data in s.files.items():
        if path.endswith(".html"):
            shown.update(re.findall(r'data-key="([^"]*)"', data.decode("utf-8")))
    assert len(shown) >= len({k for e in kb.entities("dialogue") for k in e["text"]})
    assert keys > 1500
    # the one key without Korean says so
    owner = [p for p, d in s.files.items() if b"Chapter3_Quest_Zenumist_Chief.dialog.1" in d]
    assert len(owner) == 1
    assert b"The game has no Korean text for this line." in s.files[owner[0]]


def test_every_quest_has_steps_and_clean_prose(built) -> None:
    s, kb = built
    for q in kb.entities("quest"):
        page = s.files[qs.quest_path(q["id"])].decode("utf-8")
        m = re.search(r'<h2 id="how-to-complete">.*?</h2>(.*?)</section>', page, re.S)
        assert m and "<li>" in m.group(1) and 'href="' in m.group(1), q["id"]
    problems = []
    for path, data in s.files.items():
        if path.startswith("quests/") and path.endswith(".html"):
            problems += prose_lint.lint_text(data.decode("utf-8"), kind="html", path=path)
    assert problems == []


def test_chain_diagram_per_chapter(built) -> None:
    s, _kb = built
    for ch in (0, 1, 2, 3):
        page = s.files[qs.chapter_path(ch)].decode("utf-8")
        assert 'class="chain-svg"' in page, ch
