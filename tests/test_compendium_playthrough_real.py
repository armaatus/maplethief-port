"""The playthrough checklist (#296) on the real knowledge base (local only: needs work/ipa/).

It ticks every quest of the knowledge base (138 in 1.5), its 5 bosses, 11 job advancements
and 3 climbs, each once in the story's order, and every entity :data:`RUNS` names exists.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from mtport.compendium import kb as kbm
from mtport.compendium import playthrough as pt
from mtport.compendium.sections import monsters as mons
from mtport.compendium.siteapi import Context, Images, load_kb_dir

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def ctx(real_kb: kbm.KB, tmp_path_factory: pytest.TempPathFactory) -> Context:
    out = tmp_path_factory.mktemp("kb") / "kb"
    kbm.write(real_kb, out)
    return Context(load_kb_dir(out), Images(), None)


def test_counts_match_the_knowledge_base(ctx: Context, tmp_path: Path) -> None:
    cl = pt.write(ctx, tmp_path)
    quests = {str(q["id"]) for q in ctx.kb.entities("quest")}
    bosses = {str(m["id"]) for m in ctx.kb.entities("monster") if mons.is_boss(m)}
    advancements = {str(a["id"]) for a in ctx.kb.entities("job_advancement")}
    assert cl.ids("quest") == quests and len(quests) == 138
    assert cl.ids("monster") == bosses and len(bosses) == 5
    assert cl.ids("job_advancement") == advancements and len(advancements) == 11
    assert cl.ids("climb") == set(pt.climbs(ctx)) and len(cl.ids("climb")) == 3
    text = (tmp_path / "playthrough.md").read_text(encoding="utf-8")
    assert text.count("- [ ] Quest `") == 138  # each quest once


def test_every_climb_and_boss_is_in_a_chapter(ctx: Context) -> None:
    cl = pt.build(ctx)
    chapters = [s for s in cl.sections if s.title.startswith("Chapter ")]
    placed = {r.key for s in chapters for p in s.parts for r in p.rows}
    for area in pt.climbs(ctx):
        assert f"climb:{area}" in placed
    for m in ctx.kb.entities("monster"):
        if mons.is_boss(m):
            assert f"monster:{m['id']}" in placed


def test_runs_name_real_entities(ctx: Context) -> None:
    climbs = set(pt.climbs(ctx))
    for key in pt.RUNS:
        kind, eid = key.split(":", 1)
        assert (eid in climbs) if kind == "climb" else ctx.kb.get(kind, eid) is not None, key
