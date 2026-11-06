"""The "About the game" section (#225) on the real bundle (local only: needs work/ipa/).

The unused scripts' text in the knowledge base (``script.text``), and the section built with
the committed prose: every unused script has its entry, every script its row.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from mtport.compendium import kb as kbm
from mtport.compendium import site
from mtport.compendium.sections import about

pytestmark = pytest.mark.real_assets


def test_unused_scripts_keep_their_text(real_kb: kbm.KB) -> None:
    scripts = real_kb.entities["script"]
    unused = {i: s for i, s in scripts.items() if not s["used"]}
    assert len(unused) == 39
    assert all(s["text"] == [] for s in scripts.values() if s["used"])
    lines = {i: len(s["text"]) for i, s in unused.items() if s["text"]}
    # The literal Korean sentences (formats/script-out.md: 127 pushes, these distinct).
    literal = {
        i: sum(about.korean_only(r) for r in s["text"]) for i, s in unused.items() if s["text"]
    }
    assert {i: n for i, n in literal.items() if n} == {
        "chapter3-Darklord_new": 29,
        "common-Barun_old": 1,
        "common-Manzie_error": 1,
        "common-Spiegelman1": 25,
        "common-Spiegelman2": 25,
        "common-Spiegelman3": 25,
        "common-pig_old": 10,
    }
    assert lines["chapter3-Darklord_ori"] == 34
    # Every keyed line has both languages; the order follows the script.
    for s in unused.values():
        for r in s["text"]:
            assert r["en"] and r["ko"], r["key"]
    keys = [r["key"] for r in unused["common-BrokerHan"]["text"]]
    assert keys[0].startswith("Common_Dialog_BrokerHan.")


def test_section_builds_on_the_real_kb(real_kb: kbm.KB, tmp_path: Path) -> None:
    kbm.write(real_kb, tmp_path / "kb")
    built = site.build_site(tmp_path / "kb", site.PAGES_DIR, None)  # every section
    missing, counts = site.completeness(built)
    assert missing == []
    assert counts["script"] == (307, 307)
    unused = built.files["about/unused.html"].decode("utf-8")
    scripts = built.files["about/scripts.html"].decode("utf-8")
    for sid, s in real_kb.entities["script"].items():
        anchor = about.script_anchor(sid)
        assert f'id="{anchor}"' in scripts
        if not s["used"]:
            assert f'<h3 id="{anchor}">' in unused
    assert unused.count("This text is Korean only") == 7
    assert "39 of the 307 script files never run" in unused
    assert "No map object names 11 of them; 4 are named only" in unused
