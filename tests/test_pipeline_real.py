"""Real-asset checks for the asset pipeline: convert the whole shipped bundle, twice.

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

from __future__ import annotations

import json
import shutil
from collections import Counter

import pytest

from mtport import pipeline as pl

pytestmark = pytest.mark.real_assets

BUNDLE_FILES = 2172


def tree(root):
    return {
        p.relative_to(root).as_posix(): (p.read_bytes(), p.stat().st_mtime_ns)
        for p in sorted(root.rglob("*"))
        if p.is_file()
    }


@pytest.fixture(scope="module")
def app(ipa_dir):
    return ipa_dir / "Payload" / "MapleThief.app"


@pytest.fixture(scope="module")
def runs(app, tmp_path_factory):
    out = tmp_path_factory.mktemp("assets-out")
    first = pl.run(app, out)
    after_first = tree(out)
    second = pl.run(app, out)
    yield out, first, after_first, second
    shutil.rmtree(out)  # ~35 MB of converted game assets: do not leave them behind


def test_full_run_has_no_conversion_errors(runs):
    _, first, _, _ = runs
    assert first.plan.errors == []
    c = first.plan.counts
    assert (c[pl.CONVERTED], c[pl.COPIED], c[pl.IGNORED], c[pl.ERROR]) == (912, 1253, 7, 0)
    print(f"full run: {first.summary()} in {first.seconds:.1f}s")


def test_manifest_lists_every_bundle_file_once(app, runs):
    out = runs[0]
    m = json.loads((out / pl.MANIFEST).read_text(encoding="utf-8"))
    shipped = {p.relative_to(app).as_posix() for p in app.rglob("*") if p.is_file()}
    assert len(shipped) == BUNDLE_FILES
    assert set(m["files"]) == shipped
    assert m["outcomes"] == {"converted": 912, "copied": 1253, "error": 0, "ignored": 7}
    formats = Counter(e["format"] for e in m["files"].values())
    assert formats["script-out"] == 307 and formats["png-dat"] == 40 and formats["map"] == 90
    for rel, entry in m["files"].items():
        assert entry["outcome"] in (pl.CONVERTED, pl.COPIED, pl.IGNORED), rel
        assert entry["outputs"] or entry["outcome"] == pl.IGNORED, rel
        assert entry["reason"] or entry["outcome"] == pl.CONVERTED, rel
    # Every png-dat pack was checked against the length table in the bundle's binary.
    for rel, entry in m["files"].items():
        if entry["format"] == "png-dat":
            index = rel[: -len(".dat")] + "/index.json"
            assert json.loads((out / index).read_text())["length_table_checked"] is True


def test_outputs_match_the_manifest(runs):
    out, _, after_first, _ = runs
    m = json.loads(after_first[pl.MANIFEST][0])
    outputs = {o: h for e in m["files"].values() for o, h in e["outputs"].items()}
    assert set(after_first) == set(outputs) | {pl.MANIFEST}
    for o, digest in outputs.items():
        assert pl._sha(after_first[o][0]) == digest, o


def test_second_run_is_a_no_op_with_identical_bytes(runs):
    out, _, after_first, second = runs
    assert second.plan.errors == []
    assert second.plan.counts["skipped"] == BUNDLE_FILES
    assert second.written == [] and second.stale == []
    assert tree(out) == after_first  # same bytes and mtimes: nothing was rewritten
    print(f"no-op run: {second.summary()} in {second.seconds:.1f}s")


def test_a_fresh_run_elsewhere_is_byte_identical(app, runs, tmp_path):
    _, _, after_first, _ = runs
    other = tmp_path / "assets-out"
    assert pl.run(app, other).ok
    assert {k: v[0] for k, v in tree(other).items()} == {k: v[0] for k, v in after_first.items()}
    shutil.rmtree(other)
