"""docs/re/host-surface.md is what the generator writes from this checkout's analysis.

Needs work/re/imports.json and work/re/objc.json (python3 -m mtport.objc generate, then
python3 -m mtport.imports generate, which needs the Ghidra exports): skipped without them, and
always in CI.
"""

from __future__ import annotations

import json

import pytest

from mtport import hostsurface as hs
from repo_paths import REPO, assert_committed_doc

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def docs() -> tuple[dict, dict]:
    paths = [REPO / hs.IMPORTS_JSON, REPO / hs.OBJC_JSON]
    for p in paths:
        if not p.is_file():
            pytest.skip(f"{p.relative_to(REPO)} absent (run python3 -m mtport.imports generate)")
    return tuple(json.loads(p.read_text(encoding="utf-8")) for p in paths)


def test_committed_markdown_is_current(docs):
    rows = hs.surface(*docs)
    assert_committed_doc(
        hs.MARKDOWN_OUT, hs.to_markdown(rows), "python3 -m mtport.hostsurface generate"
    )


def test_dead_functions_are_app_methods(docs):
    imports_doc, _ = docs
    names = {f["name"] for f in imports_doc["functions"]}
    assert sorted(hs.DEAD_FUNCTIONS - names) == []


def test_foundation_surface(docs):
    rows = {(r.cls, r.selector) for r in hs.surface(*docs) if r.area == "Foundation"}
    # The save files (docs/ANALYSIS.md → Save data) and the localized dialogue.
    for row in [
        ("NSFileHandle", "+fileHandleForUpdatingAtPath:"),
        ("NSFileHandle", "-writeData:"),
        ("NSFileManager", "-fileExistsAtPath:"),
        ("NSBundle", "-localizedStringForKey:value:table:"),
        ("NSString", "+stringWithFormat:"),
    ]:
        assert row in rows
    # Only dead code sends these (-[InAppPurchaseManager makePurchase:]).
    assert ("NSError", "+errorWithDomain:code:userInfo:") not in rows
