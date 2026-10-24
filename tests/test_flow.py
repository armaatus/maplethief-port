"""The program-flow names generator (mtport.flow) and its doc, without the IPA."""

from __future__ import annotations

import re

import pytest

from mtport import flow, names
from repo_paths import REPO

DOC = REPO / "docs" / "re" / "program-flow.md"
ADDR = re.compile(r"`(0x[0-9A-F]{4,6})`")


def test_curated_names_follow_the_contract():
    locations = [fp & ~1 for fp, _, _ in flow.FLOW_NAMES]
    assert len(locations) == len(set(locations))
    assert len({n for _, n, _ in flow.FLOW_NAMES}) == len(flow.FLOW_NAMES)
    for fp, name, comment in flow.FLOW_NAMES:
        assert names.name_problem(name) is None, name
        assert comment and fp < 0x100000
        # Every game function is Thumb; only crt1's start is ARM.
        assert (fp & 1) == (0 if name == "start" else 1), name


class _Img:
    def __init__(self, refs):
        self.starts = tuple(refs)
        self._refs = refs

    def string_refs(self, fp):
        return self._refs[fp]


def test_key_names():
    img = _Img(
        {
            0x101: ["CHero.setMP.1", "CHero.setMP.2", "unrelated"],
            0x201: ["CHero.setMP.3"],
            0x301: ["CA.f.1", "CB.g.1"],  # two methods: no name
            0x401: ["CHero.setExp.1"],  # skipped below
            0x501: ["not.a.key", "CHero"],
            0x601: ["CHero.setMP.4"],
        }
    )
    got = flow.key_names(img, skip={0x400})
    assert [(fp, n) for fp, n, _ in got] == [
        (0x101, "CHero::setMP"),
        (0x201, "CHero::setMP_2"),
        (0x601, "CHero::setMP_3"),
    ]
    assert got[0][2] == "looks up 2 CHero.setMP.N key(s)"


class _Image:
    def __init__(self, starts):
        self.function_starts = tuple(starts)


def test_build_checks_and_skips(monkeypatch):
    monkeypatch.setattr(flow, "FLOW_NAMES", ((0x101, "A::f", "x"), (0x201, "B::g", "y")))
    monkeypatch.setattr(
        flow.rtti, "Image", lambda image: _Img({0x101: [], 0x201: [], 0x301: ["C.h.1"]})
    )
    entries = flow.build(_Image([0x101, 0x201, 0x301]), {0x200: "B::g"})
    assert [(e["address"], e["name"]) for e in entries] == [("0x101", "A::f"), ("0x301", "C::h")]
    with pytest.raises(flow.FlowError, match="another names file says 'Other'"):
        flow.build(_Image([0x101, 0x201]), {0x100: "Other"})
    with pytest.raises(flow.FlowError, match="not an LC_FUNCTION_STARTS entry"):
        flow.build(_Image([0x100, 0x201]), {})


def test_doc_uses_the_curated_names():
    """Every curated function the doc mentions by address is called by its curated name."""
    text = DOC.read_text(encoding="utf-8")
    mentioned = {int(a, 16) for a in ADDR.findall(text)}
    used = [(fp, n) for fp, n, _ in flow.FLOW_NAMES if fp in mentioned]
    assert len(used) >= 60
    for fp, name in used:
        assert f"`{name}`" in text or f"`{name}(" in text, f"0x{fp:X} {name}"
    assert text.rstrip().splitlines()[-1].startswith("8. ")  # the unknowns end the page
    assert "## Open unknowns" in text
