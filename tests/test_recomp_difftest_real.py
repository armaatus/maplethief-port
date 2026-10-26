"""The differential test on the real armv7 slice (#41, ADR 0017; local only: it needs the IPA
and the ``difftest`` group, ``scripts/difftest --pytest``).

At least 200 real leaf functions must pass 1,000 random inputs each, in Unicorn and as
recompiled C, with zero mismatches. The run writes the report to work/recomp/difftest.json
and work/recomp/difftest.md, as ``python -m recomp difftest`` does.
"""

from __future__ import annotations

import os

import pytest

from recomp import difftest
from recomp_harness import host_cc
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / "work" / "slices" / "MapleThief.armv7"
REQUIRED = 200


def test_leaf_functions_agree_with_unicorn() -> None:
    pytest.importorskip("unicorn", reason="unicorn not installed (scripts/difftest --pytest)")
    host_cc()
    jobs = max(1, min(8, (os.cpu_count() or 2) - 2))
    run = difftest.run(SLICE, jobs=jobs)
    difftest.write_report(run)
    s = run.summary()
    bad = [(f"0x{r.ptr:X}", r.mismatches, r.examples[:1], r.driver_error) for r in run.mismatched]
    assert not bad, bad
    assert s["passed"] >= REQUIRED, s
    assert all(r.returned >= difftest.INPUTS for r in run.passed)
    print(f"difftest: {s}; {run.seconds}")
