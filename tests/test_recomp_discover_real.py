"""Function discovery (#32) on the real armv7 slice (local only: needs the IPA).

* Every one of the 2,655 ``LC_FUNCTION_STARTS`` seeds is analysed without a problem, the
  ARM entry stub included (A32 decoder, #33: it ends in a tail call to ``_exit``, so it
  never returns). No seed from ObjC, vtables, init functions, calls or tail calls lies
  outside the starts.
* Every ``tbb``/``tbh`` site the linear census finds (``mtport.armscan``'s Capstone sweep,
  ANALYSIS.md: 132 + 52) is a resolved table; the other 16 tables are the ``mov pc`` branch
  tables of Chartboost's SjLj dispatch. Every table has exactly the targets Ghidra's switch
  recovery found (``COMPUTED_JUMP`` xrefs; skipped without the export).
* The cross-check with ``work/ghidra/functions.json`` has no unexplained difference (#32
  allows < 1%), and the three functions #112 saw cut short have all of Ghidra's code.
* Inside the function bounds, every byte is code, data, padding or after a noreturn call,
  except three dead ``b`` instructions.
* The lifter consumes the result: no jump table or fall-through problem is left, and the
  whole binary lifts with at least the yield measured for #32.
* Discovery stays far inside ADR 0002's 60 s budget for a full recompile.
"""

from __future__ import annotations

import json

import pytest

from mtport import ghidra_common as gc
from recomp import discovery_report, prototype
from recomp.__main__ import main
from recomp.decode import decode_thumb
from recomp.discover import discover
from recomp.loader import Image
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / "work" / "slices" / "MapleThief.armv7"
GHIDRA = REPO / "work" / "ghidra"
ENTRY_STUB = 0x27E0


@pytest.fixture(scope="module")
def image() -> Image:
    return Image.load(SLICE)


@pytest.fixture(scope="module")
def found(image: Image):
    return discover(image)


def _ghidra(name: str) -> dict:
    path = GHIDRA / name
    if not path.is_file():
        pytest.skip(f"{path} not found (scripts/re-analyze)")
    return gc.load_export(path)


def test_every_seed_is_analysed(image: Image, found) -> None:
    assert len(image.function_starts) == 2655
    assert set(image.function_starts) <= set(found.functions)
    assert found.failures() == []
    stub = found.functions[ENTRY_STUB]
    assert stub.mode == "arm" and len(stub.insns) == 11 and stub.noreturn
    assert list(stub.calls.values()) == [0x280D]  # blx #imm into Thumb code
    assert len(stub.tail_calls) == 1  # b to the _exit stub
    strong = {p: s for p, s in found.outside_starts().items() if s - {"data", "code"}}
    assert strong == {}
    assert len(found.functions) == 2655
    c = found.counts()
    assert (c["thumb"], c["arm"]) == (2654, 1)


def test_every_census_jump_table_is_resolved(found) -> None:
    pytest.importorskip("capstone")
    from mtport import armscan, macho

    scanner = armscan.Scanner(macho.load(SLICE))
    census = {}
    for ptr, start, end in scanner.functions():
        insns, _, _ = scanner.decode(start, end, bool(ptr & 1))
        for insn in insns:
            if insn.mnemonic in ("tbb", "tbh"):
                census[insn.address] = insn.mnemonic
    tables = {s: t for f in found.functions.values() for s, t in f.tables.items()}
    assert sorted(census.values()).count("tbb") == 132
    assert sorted(census.values()).count("tbh") == 52
    assert {s: tables[s].kind for s in census if s in tables} == census
    # The other tables are the 16 branch tables of Chartboost's SjLj dispatch (mov pc).
    others = {s: t.kind for s, t in tables.items() if s not in census}
    assert sorted(set(others.values())) == ["branch"] and len(others) == 16


def test_jump_tables_match_ghidra_switch_recovery(found, image: Image) -> None:
    diff = discovery_report.compare(found, _ghidra("functions.json"), image)
    discovery_report.compare_tables(found, _ghidra("xrefs.json"), diff)
    assert diff.tables_agree == 200 and diff.tables_differ == []


def test_ghidra_cross_check_has_no_unexplained_difference(found, image: Image) -> None:
    diff = discovery_report.compare(found, _ghidra("functions.json"), image)
    assert diff.compared == 2655
    assert diff.unexplained_share() < discovery_report.UNEXPLAINED_LIMIT
    assert diff.unexplained == []
    assert all(why != "unexplained" for _, why in diff.only_ghidra)
    by = {d.ptr: d for d in diff.diffs}
    for ptr, name, _ in discovery_report.KEY_FUNCTIONS:
        d = by.get(ptr)
        assert d is None or not d.ghidra_only, f"{name}: Ghidra has code discovery lacks"
    # The only functions Ghidra knows as noreturn, discovery knows too.
    assert diff.noreturn_only_ghidra == []


def test_no_code_is_left_unreached(image: Image, found) -> None:
    gaps = [g for f in found.functions.values() for g in discovery_report.gaps(f, image.code,
            image.text_base)]  # fmt: skip
    unreached = sorted(lo for lo, _, why in gaps if why == "unreached")
    # Three dead -O0 branches (a `b` right after another unconditional branch).
    assert unreached == [0x297E6, 0x30376, 0xACE62]
    for lo in unreached:
        assert decode_thumb(image.code, lo - image.text_base, lo).mnemonic == "b"


def test_lifter_consumes_discovery(image: Image, found) -> None:
    lifted = prototype.lift(image, list(image.function_starts), discovery=found)
    problems = [p for f in lifted for p in f.problems if "unsupported" not in p]
    assert problems == []  # no jump table, fall-through or flag problem is left
    unsupported = {why.split()[0] for f in lifted for _, why in f.unsupported}
    assert unsupported <= {
        "undefined",
        "vcvt.s32.f32",
        "vcvt.f32.s32",
        "vcvt.f64.s32",
        "vcvt.s32.f64",
        "vcvt.u32.f64",
    }  # NEON and conversions: #34
    assert sum(f.clean for f in lifted) >= 2525  # #30: 2,375; #32: 2,524; #33: + the ARM stub


def test_discovery_is_fast(found) -> None:
    assert found.seconds < 15  # 1.4 s on an Apple M4; ADR 0002 budget: 60 s for everything


def test_discover_command_writes_functions_json(tmp_path) -> None:
    out, report = tmp_path / "functions.json", tmp_path / "discovery-report.md"
    ghidra = GHIDRA / "functions.json"
    status = main(["discover", "--slice", str(SLICE), "--out", str(out), "--report",
                   str(report), "--ghidra", str(ghidra)])  # fmt: skip
    assert status == 0
    doc = json.loads(out.read_text(encoding="utf-8"))
    assert doc["schema"] == 1 and len(doc["functions"]) == 2655
    assert doc["counts"]["tables"] == 200
    assert (doc["counts"]["tbb"], doc["counts"]["tbh"]) == (132, 52)
    assert doc["counts"]["entries"] == 205  # one SjLj resume point per SjLj function
    on_server_packet = next(f for f in doc["functions"] if f["ptr"] == "0x3ED19")
    assert len(on_server_packet["tables"]) >= 2  # nested tbb/tbh
    text = report.read_text(encoding="utf-8")
    assert "## Functions discovery could not finish" in text
    if ghidra.is_file():
        assert "0 unexplained" in text
