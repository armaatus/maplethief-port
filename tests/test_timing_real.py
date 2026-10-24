"""Real-asset checks for docs/re/timing-threading.md (#21) against the binary and the RE outputs.

Skipped when work/ipa/ is absent (CI), and per test when the generated inputs are missing
(run scripts/bootstrap, then scripts/re-analyze).
"""

from __future__ import annotations

import json
import struct

import pytest

from mtport import macho
from mtport import timing as tm
from repo_paths import REPO

pytestmark = pytest.mark.real_assets


def _json(rel: str) -> dict:
    path = REPO / rel
    if not path.is_file():
        pytest.skip(f"{rel} absent (run scripts/bootstrap and scripts/re-analyze)")
    return json.loads(path.read_text(encoding="utf-8"))


@pytest.fixture(scope="module")
def image() -> macho.MachO:
    path = REPO / tm.SLICE
    if not path.is_file():
        pytest.skip(f"{tm.SLICE} absent (run scripts/bootstrap)")
    return macho.load(path)


@pytest.fixture(scope="module")
def sites() -> list[tm.Site]:
    return tm.collect(_json(tm.IMPORTS_JSON), _json(tm.TAGS_JSON))


@pytest.fixture(scope="module")
def atomics(image) -> list[tm.Site]:
    _json(tm.IMPORTS_JSON)
    _json(tm.TAGS_JSON)
    return tm.atomics(REPO)


@pytest.fixture(scope="module")
def functions() -> dict[int, dict]:
    return {int(f["address"], 16): f for f in _json("work/ghidra/functions.json")["functions"]}


def test_every_timing_site_is_in_the_doc(sites, atomics):
    """Each site is named in the doc, or sits in a Chartboost function the doc lists."""
    missing = tm.check_doc((REPO / tm.DOC).read_text(encoding="utf-8"), sites + atomics)
    assert missing == [], "\n".join(tm.site_rows(missing))


def test_lists_name_things_that_exist(sites):
    """Typo guard: every listed import is in the binary and every listed selector is sent."""
    present = {r["name"] for r in _json(tm.IMPORTS_JSON)["imports"]}
    assert set(tm.TIMING_IMPORTS) <= present
    sent = {s.what[1:] for s in sites if s.what[0] in "+-"}
    assert set(tm.TIMING_SELECTORS) == sent


def test_absent_selectors_have_no_selector_reference():
    assert tm.absent_selectors(_json(tm.OBJC_JSON)) == []


def test_site_counts(sites, atomics):
    """211 sites: 55 in game code, 156 in Chartboost (66 of them dmb)."""
    everything = sites + atomics
    assert len(everything) == 211
    assert sum(not s.chartboost for s in everything) == 55
    # The game's only GCD use: GameCenterManager's dispatch_async to the main queue.
    gcd = [s for s in sites if "dispatch" in s.what and not s.chartboost]
    assert {(s.site, s.what) for s in gcd} == {
        (0xC01BC, "__dispatch_main_q"),
        (0xC01C0, "_dispatch_async"),
    }
    # Run-loop timers and both clocks: engine only, in RunLoopTimer_Arm and GetTimeMs.
    timer = [s for s in sites if s.what.startswith(("_CFRunLoopTimer", "_CFAbsolute", "_getti"))]
    assert {s.function for s in timer} == {0x8E29, 0xA0D5}
    # Atomics: 66 dmb, all Chartboost; no exclusive load/store anywhere.
    assert len(atomics) == 66
    assert {s.what for s in atomics} == {"dmb"} and all(s.chartboost for s in atomics)


def test_speed_table_and_timer_constants(image):
    """{65, 50, 40, -1} ms at 0x108C58; RunLoopTimer_Arm's 1000.0f and 0.001 s."""
    assert struct.unpack("<4i", image.read(0x108C58, 16)) == (65, 50, 40, -1)
    assert struct.unpack("<f", image.read(0x8F64, 4)) == (1000.0,)
    assert struct.unpack("<d", image.read(0x8F68, 8))[0] == pytest.approx(0.001)


def test_get_time_ms_wraps_to_32_bits(image):
    """tv_sec * 1000 + tv_usec / 1000 is one 32-bit mla, then sign-extended to 64 bits."""
    import capstone

    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    insns = {
        i.address: f"{i.mnemonic} {i.op_str}" for i in md.disasm(image.read(0xA0D4, 76), 0xA0D4)
    }
    assert insns[0xA106] == "mla r1, r2, r1, r3"
    assert insns[0xA10C] == "asrs r2, r1, #0x1f"


def test_names_are_applied_in_ghidra(functions):
    """After scripts/re-analyze, every timing.json function name is on its function."""
    for address, kind, name, _comment in tm.NAMES:
        if kind != "function":
            continue
        f = functions.get(int(address, 16))
        assert f is not None, (address, name)
        assert f["name"] == name and f["name_source"] == "names", (address, f["name"])


def test_call_graph_matches_the_doc(functions):
    names = {n: int(a, 16) for a, k, n, _c in tm.NAMES if k == "function"}

    def callers(ptr: int) -> set[int]:
        return {int(c, 16) for c in functions[ptr]["callers"] if c.startswith("0x")}

    def callees(ptr: int) -> set[int]:
        return {int(c, 16) for c in functions[ptr]["callees"] if c.startswith("0x")}

    get_time_ms, set_interval = 0xA0D5, 0x21DA1
    # The interval is set only by ApplyGameSpeed and (dead branch) CStateTitle::LoadSettings.
    assert callers(set_interval) == {names["CDataMgr::ApplyGameSpeed"], 0x980B5}
    assert callers(names["CDataMgr::ApplyGameSpeed"]) == {0x38611, 0x46A29}
    # The RNG: seeded from the clock by CMUtil::CMUtil, stepped by the three helpers.
    assert get_time_ms in callees(names["Rand_SeedFromTime"])
    assert callers(names["Rand_SeedFromTime"]) == {names["CMUtil::CMUtil"]}
    assert callers(names["Rand_Next"]) == {
        names["Rand_Range"],
        names["Rand_Chance"],
        names["Rand_Percent"],
    }
    for touch in ("TouchWidget_RepeatDue", "TouchWidget_StartRepeat", "TouchWidget_HandleTouch"):
        assert get_time_ms in callees(names[touch]), touch
    assert callers(names["TouchWidget_HandleTouch"]) == {0x8B85}
    # CMAction update (one sprite tick) is called from Paint-side code, never from Update.
    assert callers(0x1E585) == {
        0x5E6C1,
        0x5E7B1,
        0x61895,
        0x61A19,
        0x62ADD,
        0x6AF09,
        0x728E1,
        0x7458D,
        0x87501,
    }
    assert callees(0x874E1) == {0x55C85}  # CStateGame::Update -> CDataMgr::Update only
