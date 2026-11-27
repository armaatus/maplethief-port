"""docs/re/coverage-gaps.md is current (#305): regenerated from its own list of unexecuted
functions and the current work/ (the Ghidra export, the ObjC and C++ metadata, the bundle's
scripts, the difftest report), it must not change. Local only: skipped without work/ipa/,
the Ghidra export (scripts/re-analyze) or work/recomp/difftest.json (scripts/difftest)."""

from __future__ import annotations

import re

import pytest

from mtport import coveragegaps as cg
from repo_paths import REPO, assert_committed_doc

pytestmark = pytest.mark.real_assets

DOC = "docs/re/coverage-gaps.md"


@pytest.fixture(scope="module")
def program() -> cg.Program:
    for path, how in (
        (cg.XREFS, "scripts/re-analyze"),
        (cg.OBJC, "scripts/re-analyze"),
        (cg.DIFFTEST, "scripts/difftest"),
    ):
        if not path.is_file():
            pytest.skip(f"{path.relative_to(REPO)} absent ({how})")
    return cg.load_program()


def test_the_doc_is_current(program) -> None:
    text = (REPO / DOC).read_text(encoding="utf-8")
    generated, _ = cg.document(program, cg.executed_from_doc(program, text))
    assert_committed_doc(DOC, generated, cg.CHECK)


def test_every_function_has_a_reason_a_test_or_a_follow_up(program) -> None:
    """#305's bar, as narrowed on the issue: every unexecuted game function is dead (with
    evidence) or difftested; the unreachable and missed ones name their follow-up (#432,
    #433, then #435). Every rule still matches a function, so none is stale."""
    text = (REPO / DOC).read_text(encoding="utf-8")
    _, gaps = cg.document(program, cg.executed_from_doc(program, text))
    follow_ups = (f"; follow-up #{cg.HOST_TESTS}", f"; follow-up #{cg.RUN_STEPS}")
    open_gaps = [
        f"0x{g.address:X} {g.name}"
        for g in gaps
        if g.cls not in ("dead", "difftest") and not g.evidence.endswith(follow_ups)
    ]
    assert not open_gaps, f"{len(open_gaps)} functions without a reason, a test or a follow-up"
    names = [g.name for g in gaps]
    stale = [r.pattern for r in cg.RULES if not any(re.fullmatch(r.pattern, n) for n in names)]
    assert not stale, f"rules that match no unexecuted function: {stale}"
