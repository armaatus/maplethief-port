"""The coverage report (#40) on the real armv7 slice (local only: needs the IPA).

* ``python -m recomp coverage`` lifts all 2,655 functions, compiles every generated unit with
  the host compiler, and reports at least 99% of them lifted cleanly and compiled (the M3
  exit criterion), with no program problem; every function that is not clean has a reason.
* The counts by #10 tag add up, and the quality signals are there (dispatch, imports, SjLj,
  every census form in the snippet form lists).
* ``python -m recomp lift`` writes the same report without compiling (compiled: not
  measured), and ends with the coverage summary line.
"""

from __future__ import annotations

import json

import pytest

from recomp import coverage
from recomp.__main__ import main
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / "work" / "slices" / "MapleThief.armv7"


@pytest.fixture(scope="module")
def report(tmp_path_factory):
    if coverage.find_compiler() is None:
        pytest.skip("no C compiler")
    tmp = tmp_path_factory.mktemp("coverage")
    status = main(["coverage", "--slice", str(SLICE), "--out", str(tmp / "gen"),
                   "--coverage", str(tmp)])  # fmt: skip
    doc = json.loads((tmp / "coverage.json").read_text(encoding="utf-8"))
    return status, doc, (tmp / "coverage.md").read_text(encoding="utf-8")


def test_at_least_99_percent_lift_and_compile(report) -> None:
    status, doc, md = report
    c = doc["counts"]
    assert c["functions"] == 2655 and doc["compile"]["measured"] is True
    assert c["ok"] / c["functions"] >= 0.99, c
    assert c["discovered"] == c["lifted"] == c["compiled"] == c["ok"] == 2655  # all of them today
    assert doc["met"] is True and doc["program_problems"] == [] and status == 0
    assert doc["compile"]["units"] == 62 and doc["compile"]["failed_units"] == []
    assert "Target 99%: met." in md


def test_every_function_that_is_not_clean_has_a_reason(report) -> None:
    _, doc, md = report
    rows = doc["functions"]
    assert len(rows) == 2655 and len({r["ptr"] for r in rows}) == 2655
    for r in rows:
        if not (r["discovered"] and r["lifted"] and r["compiled"]):
            assert r["failures"], r
            assert all(f["reason"] and f["stage"] in coverage.STAGES for f in r["failures"])
    bad = [r for r in rows if r["failures"]]
    assert ("None." if not bad else f"{len(bad):,} functions") in md


def test_counts_by_tag_and_signals(report) -> None:
    _, doc, _ = report
    by_tag = doc["by_tag"]
    assert sum(t["functions"] for t in by_tag.values()) == 2655
    if coverage.TAGS_JSON.is_file():  # docs/re/libraries.md
        assert {k: v["functions"] for k, v in by_tag.items()} == {
            "app-objc": 620, "engine": 1353, "lib:apple-openal-support": 2,
            "lib:chartboost": 675, "lib:crt1": 1, "lib:uidevice-identifier": 4,
        }  # fmt: skip
    s = doc["signals"]
    assert s["dispatch"] == {"entries": 2655, "buckets": 55056, "bucket_bytes": 16,
                             "max_per_bucket": 4}  # fmt: skip
    assert s["imports"]["imports"] == 333 and s["imports"]["problems"] == 0
    assert s["sjlj"] == {"contexts": 205, "lsdas": 205, "call_sites": 1238, "problems": 0}
    assert s["forms"] == {"forms": 244, "listed": 244, "missing": []}


def test_lift_writes_the_report_without_compiling(tmp_path, capsys) -> None:
    status = main(["lift", "--slice", str(SLICE), "--out", str(tmp_path / "gen"),
                   "--coverage", str(tmp_path)])  # fmt: skip
    assert status == 0
    doc = json.loads((tmp_path / "coverage.json").read_text(encoding="utf-8"))
    assert doc["compile"]["measured"] is False and doc["met"] is None
    assert doc["counts"]["lifted"] == 2655 and doc["counts"]["compiled"] is None
    last = capsys.readouterr().out.strip().splitlines()[-1]
    assert last.startswith("recomp: coverage: 2655 functions, 2655 discovered, 2655 lifted")
    assert "compile not measured" in last and "target 99% met for lifting" in last
