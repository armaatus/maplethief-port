"""The job advancements of the Skills and jobs section checked in the running port (#222, local
only).

``play.jobs_*_synthetic`` (tests/CMakeLists.txt, #302) starts the port from a built save at one
advancement point, talks to the Dark Lord by play and takes the advancement. Each step writes
one line to ``jobs.txt``: what the guest held before the talk (chapter, level, branch, job tier,
the quests not at 0) and the job after it. Each line is matched here with its row of the
knowledge base's ``job_advancement`` records, the ones ``skills/index.html`` shows: the row of
that chapter whose advancement is the tier the port reached and whose job is the branch it
reached (or the kept one). The guest's state before the talk meets every condition of the row
that the line can show (level, advancement count, quest states), and the index page's row
names that job and level. Every one of the 11 advancements has a run.

Skipped without work/ipa/ or without the play runs' output (build/test-output/mac-debug, or the
directory ``MT_JOBS_OUTPUT`` names): ``ctest --preset mac-debug -R jobs_``.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

import pytest

from mtport.compendium import kb as kbm
from mtport.compendium import site
from tests.fixtures.compendium import site_kb as fx

pytestmark = pytest.mark.real_assets

REPO = Path(__file__).resolve().parents[1]
PAGES = REPO / "docs" / "compendium" / "pages"
OUTPUT = Path(os.environ.get("MT_JOBS_OUTPUT", REPO / "build" / "test-output" / "mac-debug"))
# The play_real_test modes (tests/CMakeLists.txt): one line per advancement step.
RUNS = {
    "jobs_1_1": 1,
    "jobs_1_2_assassin": 1,
    "jobs_1_2_bandit": 1,
    "jobs_2_2_assassin": 1,
    "jobs_2_2_bandit": 1,
    "jobs_2_3_assassin": 1,
    "jobs_2_3_bandit": 1,
    "jobs_3_1": 1,
    "jobs_3_2_assassin": 1,
    "jobs_3_2_bandit": 1,
    "jobs_3_4_assassin": 2,
    "jobs_3_4_bandit": 2,
}
JOBS = {0: "Rogue", 1: "Assassin", 2: "Bandit"}  # hero +0x88, the branch
LINE = re.compile(
    r"chapter (\d+) level (\d+) branch (\d+) tier (\d+) quests((?: \d\.\d+=\d)*)"
    r" -> branch (\d+) tier (\d+)$"
)
# Conditions a jobs.txt line can't show: a quest's requirement met (checkRequire), its reward
# not yet taken (getReward) and free bag slots (isInvenAvailable). The starts give each.
UNSEEN = {"checkRequire", "getReward", "isInvenAvailable"}


@pytest.fixture(scope="module")
def built(real_kb: kbm.KB, tmp_path_factory: pytest.TempPathFactory) -> site.Site:
    root = tmp_path_factory.mktemp("skills-port-site")
    kb_dir = fx.write_kb(root, {k: list(v.values()) for k, v in real_kb.entities.items()})
    return site.build_site(kb_dir, PAGES, None)


def steps(mode: str) -> list[dict]:
    f = OUTPUT / f"play_{mode}" / "jobs.txt"
    if not f.is_file():
        pytest.skip(f"{f}: no run (ctest --preset mac-debug -R jobs_)")
    out = []
    for line in f.read_text(encoding="utf-8").splitlines():
        m = LINE.match(line.strip())
        assert m, (f, line)
        g = m.groups()
        quests = {}
        for q in g[4].split():
            key, state = q.split("=")
            sort, n = key.split(".")
            quests[(int(sort), int(n))] = int(state)
        out.append(
            {
                "chapter": int(g[0]),
                "level": int(g[1]),
                "branch": int(g[2]),
                "tier": int(g[3]),
                "quests": quests,
                "to_branch": int(g[5]),
                "to_tier": int(g[6]),
            }
        )
    assert len(out) == RUNS[mode], f
    return out


def row_of(kb: kbm.KB, step: dict) -> dict:
    """The one advancement row the step took."""
    rows = [
        a
        for a in kb.entities["job_advancement"].values()
        if a["chapter"] == step["chapter"]
        and a["advancement"] == step["to_tier"]
        and (
            a["job"] == step["to_branch"]
            if not a["keeps_job"]
            else step["to_branch"] == step["branch"]
        )
    ]
    assert len(rows) == 1, (step, [a["id"] for a in rows])
    return rows[0]


def holds(cond: dict, step: dict) -> bool | None:
    """Whether the guest before the talk meets one condition; None when the line can't show it."""
    native, op, value = cond.get("native"), cond["op"], cond["value"]
    if native == "getLevel":
        have = step["level"]
    elif native == "getClassUpCount":
        have = step["tier"]  # the advancement count is the job tier (hero +0x8C)
    elif native == "getQuestState":
        sort, n = cond["args"]
        have = step["quests"].get((sort, n), 0)
    else:
        assert native in UNSEEN, cond
        return None
    return {"==": have == value, ">=": have >= value, "<=": have <= value}[op]


@pytest.mark.parametrize("mode", sorted(RUNS))
def test_advancement_matches_the_running_port(built: site.Site, real_kb: kbm.KB, mode: str) -> None:
    index = built.files["skills/index.html"].decode("utf-8")
    for step in steps(mode):
        a = row_of(real_kb, step)
        assert step["to_tier"] == step["tier"] + 1, step
        assert len(a["conditions"]) == 1, a["id"]
        for c in a["conditions"][0]:
            assert holds(c, step) is not False, (a["id"], c["text"], step)
        start = index.find(f'id="job_advancement-{a["id"]}"')
        assert start >= 0, a["id"]
        row = index[start : index.find("</tr>", start)]
        job = "same job" if a["keeps_job"] else JOBS[step["to_branch"]]
        assert job in row, (a["id"], job)
        for c in a["conditions"][0]:
            if c.get("native") == "getLevel":
                assert f"level {c['value']} or higher" in row, a["id"]


def test_every_advancement_has_a_run(real_kb: kbm.KB) -> None:
    taken = {row_of(real_kb, s)["id"] for mode in RUNS for s in steps(mode)}
    assert taken == set(real_kb.entities["job_advancement"])
