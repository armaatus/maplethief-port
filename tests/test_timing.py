"""Unit tests for mtport.timing, mtport.sitecheck and armscan.find_instructions (synthetic data)."""

import json
import subprocess
import sys

import importfixtures as fx
from mtport import armscan, macho, sitecheck
from mtport import names as nm
from mtport import timing as tm
from repo_paths import REPO


def synthetic_docs():
    imports_doc = {
        "imports": [
            {"name": "_gettimeofday", "sites": [["0xA0E6", "0xA0D5", "call"]]},
            {"name": "_dispatch_async", "sites": [["0xC01C0", "0xC0145", "call"]]},
            {"name": "_dispatch_once", "sites": [["0xC2EAC", "0xC2E05", "call"]]},
            {"name": "_strlen", "sites": [["0x1000", "0x2001", "call"]]},  # not timing
        ],
        "sends": [
            {
                "site": "0xC020C",
                "function": "0xC01D1",
                "selector": "isMainThread",
                "class_method": True,
            },
            {
                "site": "0xC94EA",
                "function": "0xC9421",
                "selector": "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:",
                "class_method": True,
            },
            {"site": "0x3000", "function": "0x2001", "selector": "init", "class_method": False},
        ],
        "functions": [{"function": "0xA0D5", "name": "GetTimeMs"}],
    }
    tags_doc = {
        "functions": [
            {"address": "0xA0D5", "tag": "engine"},
            {"address": "0xC0145", "tag": "app-objc"},
            {"address": "0xC01D1", "tag": "app-objc"},
            {"address": "0xC2E05", "tag": "lib:chartboost"},
            {"address": "0xC9421", "tag": "lib:chartboost"},
            {"address": "0x2001", "tag": "engine"},
        ]
    }
    return imports_doc, tags_doc


def test_collect_keeps_only_timing_sites():
    sites = tm.collect(*synthetic_docs())
    assert [(s.what, hex(s.site), s.tag) for s in sites] == [
        ("_gettimeofday", "0xa0e6", "engine"),
        ("_dispatch_async", "0xc01c0", "app-objc"),
        ("+isMainThread", "0xc020c", "app-objc"),
        ("_dispatch_once", "0xc2eac", "lib:chartboost"),
        (
            "+scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:",
            "0xc94ea",
            "lib:chartboost",
        ),
    ]
    assert sites[0].function_name == "GetTimeMs"


def test_check_doc_uses_the_shared_rule():
    sites = tm.collect(*synthetic_docs())
    doc = "`0xA0E6` `0xC01C0` `0xC020C`; Chartboost: `0xC2E05`, `0xC9421`"
    assert tm.check_doc(doc, sites) == []
    # A game site is not covered by its function's address.
    missing = tm.check_doc("`0xA0D5` `0xC0145` `0xC01D1` `0xC2E05`", sites)
    assert [hex(s.site) for s in missing] == ["0xa0e6", "0xc01c0", "0xc020c", "0xc94ea"]
    assert tm.check_doc is sitecheck.check_doc


def test_atomic_sites_carry_tags_and_names():
    insns = [
        armscan.Insn(0xC51F6, 0xC51E9, "dmb", "dmb ish"),
        armscan.Insn(0x1234, 0x1201, "ldrex", "ldrex r1, [r0]"),
    ]
    tags = {"functions": [{"address": "0xC51E9", "tag": "lib:chartboost"}]}
    sites = tm.atomic_sites(insns, tags, {0xC51E9: "-[Chartboost delegate]"})
    assert [(s.what, s.tag, s.chartboost, s.function_name) for s in sites] == [
        ("dmb", "lib:chartboost", True, "-[Chartboost delegate]"),
        ("ldrex", "unknown", False, ""),
    ]
    # The Chartboost barrier is covered by its accessor; the unknown ldrex needs its site.
    assert tm.check_doc("`0xC51E9`", sites) == sites[1:]


def test_find_instructions_sees_exclusives_and_barriers():
    t = fx.Thumb()
    t.push_r7_lr()
    t.h(0xE850, 0x1F00)  # ldrex r1, [r0]
    t.h(0xBF08)  # it eq
    t.h(0xE840, 0x1200)  # strexeq r2, r1, [r0]
    t.h(0xF3BF, 0x8F5B)  # dmb ish
    t.pop_r7_pc()
    image = macho.MachO(fx.build(bytes(t.code), [fx.TEXT | 1]))
    found = armscan.find_instructions(image, tm.ATOMIC_MNEMONICS)
    assert [(hex(i.site), i.function, i.mnemonic, i.text) for i in found] == [
        ("0x1802", fx.TEXT | 1, "ldrex", "ldrex r1, [r0]"),
        ("0x1808", fx.TEXT | 1, "strex", "strexeq r2, r1, [r0]"),
        ("0x180c", fx.TEXT | 1, "dmb", "dmb ish"),
    ]
    assert armscan.find_instructions(image, ["clrex"]) == []


def test_absent_selectors():
    objc_doc = {
        "selrefs": [
            {"address": "0x1", "selector": "isMainThread"},
            {"address": "0x2", "selector": "currentRunLoop"},
        ]
    }
    assert tm.absent_selectors(objc_doc) == ["currentRunLoop"]


def test_lists_are_consistent():
    for values in (
        tm.TIMING_IMPORTS,
        tm.TIMING_SELECTORS,
        tm.ABSENT_SELECTORS,
        tm.ATOMIC_MNEMONICS,
    ):
        assert len(set(values)) == len(values)
    assert not set(tm.TIMING_SELECTORS) & set(tm.ABSENT_SELECTORS)


def test_names_file_is_valid():
    doc = json.loads(tm.names_json())
    source, entries = nm.parse(doc, "timing.json")
    assert source == "timing"
    assert len(entries) == len(tm.NAMES)
    assert all(e.thumb for e in entries if e.kind == "function")  # the game is Thumb code
    assert {e.name for e in entries} >= {"CDataMgr::ApplyGameSpeed", "Rand_Next", "g_randSeed"}


def test_doc_names_every_named_address_and_absent_selector():
    text = (REPO / tm.DOC).read_text(encoding="utf-8")
    seen = sitecheck.doc_addresses(text)
    for address, _kind, name, _comment in tm.NAMES:
        assert int(address, 16) in seen, (address, name)
        assert name in text, name
    for selector in tm.ABSENT_SELECTORS:
        # timerWithTimeInterval:... is abbreviated in the doc; its first part is enough.
        assert f"`{selector.split(':')[0]}" in text, selector


def test_cli_names(tmp_path):
    out = tmp_path / "names" / "timing.json"
    env = {"PYTHONPATH": str(REPO / "tools")}
    subprocess.run(
        [sys.executable, "-m", "mtport.timing", "names", str(out)],
        check=True,
        capture_output=True,
        env=env,
    )
    assert json.loads(out.read_text(encoding="utf-8"))["source"] == "timing"
    res = subprocess.run(
        [sys.executable, "-m", "mtport.timing", "check", "--repo", str(tmp_path), "--no-atomics"],
        capture_output=True,
        text=True,
        env=env,
    )
    assert res.returncode == 1 and "error:" in res.stderr
