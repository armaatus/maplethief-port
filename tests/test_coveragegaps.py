"""The coverage-gap classification (#305) on a synthetic program: dead, difftest, unreachable
and missed functions, the evidence, the framework selectors and the doc round trip. No game
data."""

from __future__ import annotations

from mtport import coveragegaps as cg

MAIN, CALLED, DEAD_CALLER, DEAD_CALLEE, LEAF = 0x1001, 0x1101, 0x1201, 0x1301, 0x1401
SENDER, METHOD, ORPHAN_METHOD, LONELY, UI_METHOD = 0x1501, 0x1601, 0x1701, 0x1801, 0x1901
CTOR, VIRTUAL, DEAD_VIRTUAL, HANDLER_USED, HANDLER_UNUSED = 0x2001, 0x2101, 0x2201, 0x2301, 0x2401
STORE, HELPER, CB = 0x2501, 0x2601, 0x2701
CALLNATIVE = cg.CALLNATIVE | 1


def fn(ptr: int, name: str, tag: str = "engine", rule: str = "reach") -> dict:
    return {"address": f"0x{ptr:X}", "name": name, "tag": tag, "rule": rule}


def call(frm: int, to: int, kind: str = "UNCONDITIONAL_CALL") -> dict:
    return {"from": "0x0", "function": f"0x{frm:X}", "to": f"0x{to & ~1:X}", "type": kind}


def load(frm: int, to: int) -> dict:
    return {"from": "0x0", "function": f"0x{frm:X}", "to": f"0x{to:X}", "type": "READ"}


TAGS = {
    "functions": [
        fn(MAIN, "main", rule="entry-point"),
        fn(CALLED, "Called"),
        fn(DEAD_CALLER, "DeadCaller"),
        fn(DEAD_CALLEE, "DeadCallee"),
        fn(LEAF, "Leaf"),
        fn(SENDER, "Sender"),
        fn(METHOD, "-[A go]", "app-objc"),
        fn(ORPHAN_METHOD, "-[A dead]", "app-objc"),
        fn(LONELY, "-[A lonely]", "app-objc"),
        fn(UI_METHOD, "-[A alertView:clickedButtonAtIndex:]", "app-objc"),
        fn(CTOR, "C::C"),
        fn(VIRTUAL, "C::run"),
        fn(DEAD_VIRTUAL, "D::run"),
        fn(HANDLER_USED, "Native_used"),
        fn(HANDLER_UNUSED, "Native_unused"),
        fn(CALLNATIVE, "Lua::callNative"),
        fn(STORE, "Store_buy"),
        fn(HELPER, "block_copy"),
        fn(CB, "UsesBlock"),
        fn(0x3001, "-[CBAd show]", "lib:chartboost"),
    ]
}
SELREF_GO, SELREF_DEAD, SELREF_ALERT = 0x5000, 0x5004, 0x5008
VT_C, VT_D, DESCRIPTOR = 0x6008, 0x6108, 0x7000
XREFS = [
    call(MAIN, CALLED),
    call(MAIN, SENDER),
    call(MAIN, CTOR),
    call(MAIN, CALLNATIVE),
    call(MAIN, STORE),
    call(MAIN, CB),
    call(DEAD_CALLER, DEAD_CALLEE),
    call(CALLED, LEAF),
    load(SENDER, SELREF_GO),
    load(DEAD_CALLER, SELREF_DEAD),
    load(DEAD_CALLER, SELREF_ALERT),
    load(CTOR, VT_C),
    call(CALLNATIVE, HANDLER_USED),
    call(CALLNATIVE, HANDLER_UNUSED),
    load(CB, DESCRIPTOR),
]
OBJC = {
    "selrefs": [
        {"address": f"0x{SELREF_GO:X}", "selector": "go"},
        {"address": f"0x{SELREF_DEAD:X}", "selector": "dead"},
        {"address": f"0x{SELREF_ALERT:X}", "selector": "alertView:clickedButtonAtIndex:"},
    ],
    "classes": [
        {
            "name": "A",
            "protocols": ["UIAlertViewDelegate"],
            "instance_methods": [
                {"selector": "go", "imp": f"0x{METHOD:X}"},
                {"selector": "dead", "imp": f"0x{ORPHAN_METHOD:X}"},
                {"selector": "lonely", "imp": f"0x{LONELY:X}"},
                {"selector": "alertView:clickedButtonAtIndex:", "imp": f"0x{UI_METHOD:X}"},
            ],
            "class_methods": [],
        }
    ],
    "categories": [],
    "protocols": [],
}
VTABLES = {
    "classes": [
        {"name": "C", "vtables": [{"address": "0x6000", "address_point": f"0x{VT_C:X}"}]},
        {"name": "D", "vtables": [{"address": "0x6100", "address_point": f"0x{VT_D:X}"}]},
    ],
    "functions": [
        {"address": f"0x{VIRTUAL:X}", "uses": [{"class": "C", "vtable": f"0x{VT_C:X}"}]},
        {"address": f"0x{DEAD_VIRTUAL:X}", "uses": [{"class": "D", "vtable": f"0x{VT_D:X}"}]},
    ],
}
DIFFTEST = {"functions": [{"ptr": f"0x{LEAF:X}", "passed": True}]}
DATA_WORDS = [(DESCRIPTOR + 8, HELPER)]


def program(monkeypatch) -> cg.Program:
    natives = [
        type("N", (), {"handler": HANDLER_USED, "name": "used"})(),
        type("N", (), {"handler": HANDLER_UNUSED, "name": "unused"})(),
    ]
    monkeypatch.setattr(cg, "NATIVES", natives)
    return cg.build_program(
        TAGS,
        XREFS,
        OBJC,
        VTABLES,
        {"used"},
        DIFFTEST,
        entry=[MAIN],
        data_words=DATA_WORDS,
    )


def by_address(gaps) -> dict[int, cg.Gap]:
    return {g.address: g for g in gaps}


def test_classes_and_evidence(monkeypatch) -> None:
    p = program(monkeypatch)
    rules = (cg.Rule("Store_.*", "unreachable", "needs the App Store", 7),)
    gaps = by_address(cg.classify(p, {MAIN, SENDER, CTOR, CALLNATIVE}, rules))
    assert 0x3001 not in gaps  # Chartboost: not game code
    assert MAIN not in gaps and SENDER not in gaps  # executed
    assert {a: g.cls for a, g in gaps.items()} == {
        CALLED: "missed",
        DEAD_CALLER: "dead",
        DEAD_CALLEE: "dead",
        LEAF: "difftest",
        METHOD: "missed",
        ORPHAN_METHOD: "dead",
        LONELY: "dead",
        UI_METHOD: "missed",
        VIRTUAL: "missed",
        DEAD_VIRTUAL: "dead",
        HANDLER_USED: "missed",
        HANDLER_UNUSED: "dead",
        STORE: "unreachable",
        HELPER: "missed",
        CB: "missed",
    }
    assert (
        gaps[DEAD_CALLER].evidence
        == "no reference: nothing calls it, takes its address or lists it"
    )
    assert gaps[DEAD_CALLEE].evidence == "called only by dead `DeadCaller`"
    assert gaps[ORPHAN_METHOD].evidence == "selector `dead` loaded only by dead `DeadCaller`"
    assert gaps[DEAD_VIRTUAL].evidence == "vtable of `D`, which no function loads"
    assert gaps[HANDLER_UNUSED].evidence == "native `unused`, which no shipped script calls"
    assert gaps[STORE].evidence == "needs the App Store; follow-up #7"
    assert gaps[CALLED].evidence == "called by live `main`"
    assert gaps[METHOD].evidence == "selector `go` loaded by live `Sender`"
    assert gaps[LONELY].evidence == (
        "no code loads selector `lonely`, and no framework or nib sends it"
    )
    assert gaps[UI_METHOD].evidence == (
        "selector `alertView:clickedButtonAtIndex:`: `UIAlertViewDelegate` method"
    )
    assert gaps[VIRTUAL].evidence == "virtual, in the live vtable of `C`"
    assert gaps[HANDLER_USED].evidence == "native `used`, called by shipped scripts"
    assert gaps[HELPER].evidence == "its address is in data loaded by live `UsesBlock`"


def test_a_dead_block_helper_goes_with_its_user(monkeypatch) -> None:
    p = program(monkeypatch)
    p.refs[CB & ~1] = [("code", DEAD_CALLER & ~1)]
    gaps = by_address(cg.classify(p, {MAIN}, ()))
    assert gaps[CB].cls == gaps[HELPER].cls == "dead"
    assert gaps[HELPER].evidence == "its address is in data loaded only by dead `UsesBlock`"


def test_super_sends_make_framework_selectors(monkeypatch) -> None:
    sends = [{"how": "super", "in_image": False, "selector": "dead", "implemented_by": "UIView"}]
    p = cg.build_program(TAGS, XREFS, OBJC, VTABLES, set(), None, [MAIN], sends)
    assert p.framework["dead"] == "overrides `UIView`'s (a super send)"
    gaps = by_address(cg.classify(p, {MAIN}, ()))
    assert gaps[ORPHAN_METHOD].cls == "missed"


def test_outlet_setters_and_delegate_methods_are_framework_selectors(monkeypatch) -> None:
    p = cg.build_program(
        TAGS, XREFS, OBJC, VTABLES, set(), None, [MAIN], outlets=[("Main.nib", "lonely")]
    )
    assert p.framework["setLonely:"] == "sets the outlet `lonely` of Main.nib"
    assert p.framework["alertView:clickedButtonAtIndex:"] == "`UIAlertViewDelegate` method"
    objc = {**OBJC, "classes": [{**OBJC["classes"][0], "protocols": []}]}
    p = cg.build_program(TAGS, XREFS, objc, VTABLES, set(), None, [MAIN])
    assert p.framework["alertView:clickedButtonAtIndex:"] == "a delegate method"
    assert "lonely" not in p.framework


def test_protocol_prefix() -> None:
    assert cg._protocol_prefix("UIAlertViewDelegate") == "alertView"
    assert cg._protocol_prefix("UIScrollViewDelegate") == "scrollView"
    assert cg._protocol_prefix("SKProductsRequestDelegate") == "productsRequest"
    assert cg._protocol_prefix("SKPaymentTransactionObserver") == "paymentQueue"
    assert cg._protocol_prefix("NSCoding") == ""


def test_doc_round_trip(monkeypatch) -> None:
    p = program(monkeypatch)
    executed = {MAIN, SENDER, CTOR, CALLNATIVE}
    gaps = cg.classify(p, executed, ())
    text = cg.render(gaps, p, len(executed))
    assert "- **Game functions: 19; executed by a run: 4; not executed: 15.**" in text
    assert "| `0x1201` | `DeadCaller` | `engine` | dead | no reference:" in text
    assert cg.doc_unexecuted(text) == {g.address & ~1 for g in gaps}
    again = cg.executed_from_doc(p, text)
    assert again == {a & ~1 for a in executed} | {0x3000}  # Chartboost: never listed
    assert cg.render(cg.classify(p, again, ()), p, len(executed)) == text


def test_a_new_run_adds_to_the_doc(monkeypatch) -> None:
    """update (#432, #433): a run step's coverage joins what the doc already counts."""
    p = program(monkeypatch)
    text = cg.render(cg.classify(p, {MAIN, SENDER}, ()), p, 2)
    executed = cg.added_to_doc(p, text, {CTOR, CALLNATIVE})
    assert executed == {a & ~1 for a in (MAIN, SENDER, CTOR, CALLNATIVE)} | {0x3000}
    gaps = by_address(cg.classify(p, executed, ()))
    assert CTOR not in gaps and gaps[VIRTUAL].cls == "missed"


def test_a_rule_can_class_dead_and_name_a_follow_up(monkeypatch) -> None:
    p = program(monkeypatch)
    rules = (cg.Rule("Called", "dead", "only the port skips it"), cg.Rule(".*", "missed", "", 9))
    gaps = by_address(cg.classify(p, {MAIN, SENDER, CTOR, CALLNATIVE}, rules))
    assert (gaps[CALLED].cls, gaps[CALLED].evidence) == ("dead", "only the port skips it")
    assert gaps[METHOD].evidence == "selector `go` loaded by live `Sender`; follow-up #9"
    assert gaps[LEAF].cls == "difftest" and gaps[DEAD_CALLER].cls == "dead"


def test_update_adds_a_runs_coverage_to_the_docs_executed_functions(monkeypatch, tmp_path) -> None:
    """``update`` (#432): what a new run entered leaves the doc; the rest stays as it was."""
    p = program(monkeypatch)
    monkeypatch.setattr(cg, "load_program", lambda: p)
    executed = {MAIN, SENDER, CTOR, CALLNATIVE}
    doc = tmp_path / "gaps.md"
    doc.write_text(cg.document(p, {a & ~1 for a in executed})[0], encoding="utf-8")
    cov = tmp_path / "coverage-1.txt"
    cov.write_text(
        "# maplethief coverage 1: 1 guest functions entered (pointer, entries, name)\n"
        f"0x{CALLED:08X} 1 Called\n",
        encoding="utf-8",
    )
    assert cg.main(["update", "--coverage", str(cov), "--doc", str(doc)]) == 0
    text = doc.read_text(encoding="utf-8")
    assert "executed by a run: 5; not executed: 14." in text
    assert CALLED & ~1 not in cg.doc_unexecuted(text)
    assert cg.doc_unexecuted(text) | {CALLED & ~1} == {
        g.address & ~1 for g in cg.classify(p, {a & ~1 for a in executed}, cg.RULES)
    }
    assert cg.main(["check", "--doc", str(doc)]) == 0
