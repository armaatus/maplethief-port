"""Functions no run has entered, each with a reason or a test (#305): ``docs/re/coverage-gaps.md``.

The run coverage (#295, ADR 0160) says which guest functions the automated runs entered. This
module takes every game function (Chartboost's 675 never run by design, docs/re/libraries.md)
that no run entered and gives it one class:

- ``dead``: no live code can reach it. Reachability is a fixpoint over the static references
  of the Ghidra export (``work/ghidra/xrefs.json``) and the ObjC and C++ metadata, seeded with
  every executed function and the roots below. A function is live when one of these is live:
  a function that calls it, jumps to it or takes its address; for an ObjC method, a function
  that loads its selector's ``__objc_selrefs`` slot (any class: the receiver is not known);
  for a virtual function, a vtable holding it whose address a live function loads (the
  constructors); for a script native's handler, a shipped script that calls the native (the
  dispatcher ``Lua::callNative`` calls every handler, so its calls are gated by the scripts).
  A selector that no code loads (most have no ``__objc_selrefs`` slot at all: no code in the
  image can send them; Chartboost's ``NSSelectorFromString`` is the only string lookup) is
  sent by nothing but the frameworks. Roots: the entry point, ``__mod_init_func``, and the
  ObjC methods that UIKit or Foundation may send: those the image also sends to an imported
  superclass (``[super dealloc]``: the method overrides the framework's), the delegate
  methods (the first keyword of a framework protocol a class adopts, ``alertView`` for
  ``UIAlertViewDelegate``, or one of :data:`DELEGATE_PREFIXES`, since a class need not
  declare the protocol of an object it is the delegate of), the setters of the nibs'
  outlets, and :data:`FRAMEWORK_SELECTORS`. The evidence names the dead references, or says
  there are none.
- ``difftest``: live, and it passes the differential test (ADR 0017,
  ``work/recomp/difftest.json``): 1,000 random inputs with zero mismatches against Unicorn.
- ``unreachable``: live, but only through a state the runs cannot produce (rules in
  :data:`RULES`, each with its reason): teardown, the system's events (memory warnings,
  audio interruptions), the App Store, Game Center. A rule can also class a live function
  ``dead`` when the port or the shipped data never takes its path (crt1's ``start``, the
  Monster Carnival that only unused scripts start), with the evidence.
- ``missed``: live, reachable by play, and no run has entered it yet (a run step is missing).

Every ``unreachable`` and ``missed`` row names the follow-up issue that adds its test or run
step (:data:`HOST_TESTS`, :data:`RUN_STEPS`).

``generate`` writes the doc from a coverage file (``mtport.runcoverage`` format, or a
directory of them; with ``--add``, the functions they entered are added to those the doc
already counts as executed, for a new run step); ``check`` regenerates it from the doc's own
list of unexecuted functions and the current ``work/`` and fails when the committed doc
differs, so a change to the evidence (a re-analysis, a difftest change) shows up without
rerunning every play test.

Inputs, all derived from your own copy of the game in git-ignored ``work/``: the function tags
(``work/re/function-tags.json``), the Ghidra cross-references, ``work/re/objc.json``,
``work/re/vtables.json``, the bundle's scripts and the difftest report. The doc holds
addresses, names and counts only.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from . import runcoverage
from .scriptout_isa import NATIVES

REPO = Path(__file__).resolve().parents[2]
WORK = REPO / "work"
DOC = REPO / "docs" / "re" / "coverage-gaps.md"
XREFS = WORK / "ghidra" / "xrefs.json"
OBJC = WORK / "re" / "objc.json"
VTABLES = WORK / "re" / "vtables.json"
DIFFTEST = WORK / "recomp" / "difftest.json"
SCRIPTS = runcoverage.APP / "script"
GENERATE = "python3 -m mtport.coveragegaps generate --coverage <coverage file or directory>"
CHECK = "python3 -m mtport.coveragegaps check"

TABLE_REACH = 16  # a block descriptor's dispose helper is 12 bytes in
CLASSES = ("dead", "difftest", "unreachable", "missed")
CALLNATIVE = 0x9B37C  # Lua::callNative 0x9B37D: the switch that calls every native's handler
# UIKit and Foundation callbacks the game also sends itself, without a super send.
FRAMEWORK_SELECTORS = frozenset(
    {"touchesBegan:withEvent:", "touchesMoved:withEvent:", "touchesEnded:withEvent:",
     "touchesCancelled:withEvent:", "drawRect:", "layoutSubviews", "awakeFromNib",
     "viewDidLoad", "viewDidUnload", "viewWillAppear:", "viewDidAppear:",
     "viewWillDisappear:", "viewDidDisappear:", "didReceiveMemoryWarning",
     "shouldAutorotateToInterfaceOrientation:", "observeValueForKeyPath:ofObject:change:context:",
     "dealloc", "copyWithZone:", "initWithCoder:", "encodeWithCoder:", "window",
     "applicationDidFinishLaunching:", "applicationWillTerminate:"}
)  # fmt: skip
# The first keyword of the delegate and data-source methods of the UIKit, Foundation, Game
# Kit, Store Kit and Media Player objects the game uses.
DELEGATE_PREFIXES = (
    "actionSheet", "alertView", "application", "connection", "leaderboardViewController",
    "achievementViewController", "matchmakerViewController", "moviePlayer", "paymentQueue",
    "productsRequest", "request", "requestDidFinish", "scrollView", "tableView",
    "numberOfSectionsInTableView", "textField", "textView", "webView", "audioPlayer",
    "animationDidStart", "animationDidStop",
)  # fmt: skip
ROW = re.compile(r"^\| `0x([0-9A-F]+)` \|")


@dataclass(frozen=True)
class Rule:
    """Live functions whose name matches ``pattern`` (a regex, the whole name): ``dead`` (the
    port or the shipped data never takes the path: the reason says why), ``unreachable`` (a
    state no run produces: the reason says which) or ``missed`` (no reason; a run step is
    missing). ``issue`` is the follow-up that adds the test or the run step."""

    pattern: str
    cls: str
    reason: str = ""
    issue: int = 0


HOST_TESTS = 432  # host-driven tests for the unreachable functions
RUN_STEPS = 435  # run steps for the missed functions (#433, then #435)

# Checked in order; the first match wins.
RULES: tuple[Rule, ...] = (
    Rule(r"start", "dead", "crt1's entry stub: the runtime does its work and calls `main` "
         "itself (ADR 0027)"),
    Rule(r"alBufferDataStaticProc", "dead", "reached only from `SoundPlayer_LoadEffects`'s "
         "branch for id 0, a music id, so it cannot run (re/audio.md)"),
    Rule(r"MonCa_.*|CStateGame::resultMonCa", "dead", "Monster Carnival: started only by "
         "`initMonCa` in `Spiegelman1`–`3`, scripts no map names (ANALYSIS.md, unused scripts)"),
    Rule(r"-\[(MoviePlayerController|MovieOverlayView) .*\]", "dead", "the movie player "
         "classes: nothing creates them (ANALYSIS.md)"),
    Rule(r"IStateAdaptor::(PushState|PopState|PopAndRestart|PopAllStates|GetRootState)|"
         r"CStateTitle::OnPointerEvent", "dead", "a state's virtual that no code calls through "
         "its slot: the state stack is never pushed or popped, and no caller sends the pointer "
         "event (re/program-flow.md, re/cpp-classes.md)"),
    Rule(r"-\[EAGLViewController viewDidLoad\]", "dead", "the controller is given its view "
         "by `setView:`, so UIKit never loads one and never sends `viewDidLoad`; it only "
         "presents Game Center, which the port draws itself (re/program-flow.md, ADR 0084)"),
    Rule(r"-\[TouchKeyPadView (release[4568]|click[58])Button:\]", "dead", "sent only by "
         "`setKeySequence:`'s 9-key chord mode, which only ever gets −1 (controls/keypad.md)"),
    Rule(r".*::~.*|-\[\w+ dealloc\]|SoundPlayer_ShutdownOpenAL|FUN_0000f000|FUN_000c0f30",
         "unreachable", "teardown: no run frees the object (the game keeps it until the "
         "process ends, and iOS ends it without unwinding)", HOST_TESTS),
    Rule(r"-\[\w+ (didReceiveMemoryWarning|viewDidUnload)\]", "unreachable",
         "a memory warning, which no run sends", HOST_TESTS),
    Rule(r"SoundPlayer_(InterruptionListener|BeginInterruption|EndInterruption)", "unreachable",
         "an audio-session interruption (a call, an alarm), which no run sends", HOST_TESTS),
    Rule(r"-\[MapleThiefAppDelegate application:didRegisterForRemoteNotifications"
         r"WithDeviceToken:\]", "unreachable", "a push-notification token, which the runtime "
         "never delivers", HOST_TESTS),
    Rule(r"-\[InAppPurchaseManager .*\]|-\[\w+ (release|click)(Exp|Meso|GradeRevival)\w*:\]",
         "unreachable", "the cash shop's purchases: they need the App Store", HOST_TESTS),
    Rule(r"-\[GameCenterManager .*\]", "unreachable", "Game Center, which the runtime does "
         "not provide", HOST_TESTS),
    Rule(r"CDataMgr::DeleteCoupon", "unreachable", "needs a coupon in the save, and nothing in "
         "1.5 adds one (formats/save.md)", HOST_TESTS),
    Rule(r".*", "missed", "", RUN_STEPS),
)  # fmt: skip


# -- inputs ----------------------------------------------------------------------------------


@dataclass
class Program:
    """What the classification reads, by plain address (Thumb bit cleared)."""

    functions: dict[int, runcoverage.Function]  # plain address -> tag and name
    # target -> [(kind, source)]: kind "code" (a function calls, jumps to or takes the address
    # of it; source = that function), "table" (its address is in a data structure; source = a
    # function that loads the structure), "data" (the same, but no code loads the structure:
    # a root; source = the word's address), "selector" (source = the selector), "vtable" (source =
    # the vtable's address point), "native" (source = the native's name), "entry"
    refs: dict[int, list[tuple[str, int | str]]] = field(default_factory=dict)
    selector_users: dict[str, set[int]] = field(default_factory=dict)
    vtable_users: dict[int, set[int]] = field(default_factory=dict)
    vtable_class: dict[int, str] = field(default_factory=dict)
    natives_used: set[str] = field(default_factory=set)
    # selectors UIKit or Foundation may send, with the reason
    framework: dict[str, str] = field(default_factory=dict)
    difftest_passed: set[int] = field(default_factory=set)

    def add_ref(self, target: int, kind: str, source: int | str) -> None:
        refs = self.refs.setdefault(target, [])
        if (kind, source) not in refs:
            refs.append((kind, source))


def _plain(text: str | int) -> int:
    return (int(text, 16) if isinstance(text, str) else text) & ~1


def build_program(
    tags: Mapping,
    xrefs: Sequence[Mapping],
    objc: Mapping,
    vtables: Mapping,
    natives_used: Iterable[str],
    difftest: Mapping | None = None,
    entry: Iterable[int] = (),
    sends: Iterable[Mapping] = (),
    data_words: Iterable[tuple[int, int]] = (),
    outlets: Iterable[tuple[str, str]] = (),
) -> Program:
    """The program from the parsed inputs (each the JSON document of its file; ``sends`` are
    the ``sends`` of ``work/re/imports.json``; ``data_words`` the (address, value) of every
    word of the data sections; ``outlets`` the (nib, outlet) of every outlet connection)."""
    p = Program({})
    for f in tags["functions"]:
        ptr = int(f["address"], 16)
        p.functions[ptr & ~1] = runcoverage.Function(
            ptr, f["tag"], f.get("name") or f"FUN_{ptr & ~1:08x}"
        )
    selrefs = {_plain(s["address"]): s["selector"] for s in objc.get("selrefs", [])}
    vt_points: dict[int, int] = {}  # any address of a vtable -> its address point
    for c in vtables.get("classes", []):
        for vt in c.get("vtables", []):
            point = _plain(vt["address_point"])
            p.vtable_class[point] = c["name"]
            vt_points[_plain(vt["address"])] = point
            vt_points[point] = point
    for f in vtables.get("functions", []):
        for use in f.get("uses", []):
            point = vt_points.get(_plain(use["vtable"]), _plain(use["vtable"]))
            p.vtable_class.setdefault(point, use["class"])
            p.add_ref(_plain(f["address"]), "vtable", point)
    handler_native = {n.handler & ~1: n.name for n in NATIVES if n.handler}
    data_users: dict[int, set[int]] = {}  # a data address -> the functions that load it
    for r in xrefs:
        to = _plain(r["to"])
        fn = _plain(r["function"]) if r.get("function") else None
        if fn is None:
            continue  # method lists, vtables and __mod_init_func: their own kinds
        if to not in p.functions:
            data_users.setdefault(to, set()).add(fn)
        if to in selrefs:
            p.selector_users.setdefault(selrefs[to], set()).add(fn)
        elif to in vt_points:
            p.vtable_users.setdefault(vt_points[to], set()).add(fn)
        elif to in p.functions and to != fn:
            if fn == CALLNATIVE and to in handler_native:
                p.add_ref(to, "native", handler_native[to])
            else:
                p.add_ref(to, "code", fn)
    for sel in FRAMEWORK_SELECTORS:
        p.framework[sel] = "a UIKit or Foundation callback"
    for send in sends:
        if send.get("how") == "super" and send.get("in_image") is False and send["selector"]:
            p.framework.setdefault(
                send["selector"], f"overrides `{send['implemented_by']}`'s (a super send)"
            )
    for nib, outlet in outlets:
        setter = f"set{outlet[:1].upper()}{outlet[1:]}:"
        p.framework.setdefault(setter, f"sets the outlet `{outlet}` of {nib}")
    for owner in (*objc.get("classes", []), *objc.get("categories", [])):
        prefixes = {
            proto: _protocol_prefix(proto)
            for proto in owner.get("protocols", [])
            if proto not in {pr["name"] for pr in objc.get("protocols", [])}
        }
        for kind in ("instance_methods", "class_methods"):
            for m in owner.get(kind, []):
                if not m.get("imp"):
                    continue
                p.add_ref(_plain(m["imp"]), "selector", m["selector"])
                for proto, prefix in prefixes.items():
                    if prefix and m["selector"].startswith(prefix):
                        p.framework.setdefault(m["selector"], f"`{proto}` method")
                # A delegate method takes the object it is the delegate of.
                if ":" in m["selector"] and m["selector"].startswith(DELEGATE_PREFIXES):
                    p.framework.setdefault(m["selector"], "a delegate method")
    for ptr in entry:
        p.add_ref(_plain(ptr), "entry", "")  # the entry point and __mod_init_func
    # Other data words that hold a function pointer (a block descriptor's copy and dispose
    # helpers, callback tables): live with the functions that load the structure holding it,
    # the nearest loaded address at most TABLE_REACH bytes below the word.
    listed = {t for t, refs in p.refs.items() if any(k in ("selector", "vtable") for k, _ in refs)}
    for addr, value in data_words:
        target = value & ~1
        if not value & 1 or target not in p.functions or target in listed:
            continue
        head = next((h for h in range(addr, addr - TABLE_REACH - 1, -4) if h in data_users), None)
        if head is None:
            p.add_ref(target, "data", addr)
        else:
            for fn in sorted(data_users[head]):
                p.add_ref(target, "table", fn)
    p.natives_used = set(natives_used)
    if difftest is not None:
        p.difftest_passed = {_plain(f["ptr"]) for f in difftest["functions"] if f["passed"]}
    return p


def _protocol_prefix(protocol: str) -> str:
    """The first keyword of a framework protocol's methods: ``UIAlertViewDelegate`` ->
    ``alertView``, ``SKPaymentTransactionObserver`` -> ``paymentQueue`` (the observer's
    methods are named after the queue)."""
    if protocol == "SKPaymentTransactionObserver":
        return "paymentQueue"
    m = re.fullmatch(r"[A-Z]{2}([A-Z][A-Za-z]*?)(Delegate|DataSource)", protocol)
    return m[1][0].lower() + m[1][1:] if m else ""


# -- the classification ----------------------------------------------------------------------


@dataclass(frozen=True)
class Gap:
    address: int  # the function pointer (Thumb bit as tagged)
    name: str
    tag: str
    cls: str
    evidence: str


def live_set(p: Program, executed: Iterable[int]) -> set[int]:
    """Every function some live reference reaches (plain addresses); ``executed`` are live."""
    live = {_plain(x) for x in executed}

    def alive(kind: str, source: int | str) -> bool:
        if kind in ("code", "table"):
            return source in live
        if kind == "selector":
            users = p.selector_users.get(str(source))
            return source in p.framework or any(u in live for u in users or ())
        if kind == "vtable":
            return any(u in live for u in p.vtable_users.get(int(source), ()))
        if kind == "native":
            return CALLNATIVE in live and source in p.natives_used
        return kind in ("entry", "data")

    changed = True
    while changed:
        changed = False
        for target, refs in p.refs.items():
            if target not in live and any(alive(k, s) for k, s in refs):
                live.add(target)
                changed = True
    return live


def _name(p: Program, ptr: int) -> str:
    f = p.functions.get(ptr)
    return f"`{f.name}`" if f else f"`0x{ptr:X}`"


def _list(items: list[str], limit: int = 3) -> str:
    more = f" and {len(items) - limit} more" if len(items) > limit else ""
    return ", ".join(items[:limit]) + more


def dead_evidence(p: Program, ptr: int) -> str:
    """Why nothing live reaches ``ptr``: its references, each dead."""
    refs = p.refs.get(ptr, [])
    if not refs:
        return "no reference: nothing calls it, takes its address or lists it"
    parts: list[str] = []
    callers = sorted(int(s) for k, s in refs if k == "code")
    if callers:
        parts.append(f"called only by dead {_list([_name(p, c) for c in callers])}")
    holders = sorted(int(s) for k, s in refs if k == "table")
    if holders:
        parts.append(
            f"its address is in data loaded only by dead {_list([_name(p, h) for h in holders])}"
        )
    for k, s in refs:
        if k == "selector":
            users = sorted(p.selector_users.get(str(s), ()))
            parts.append(
                f"selector `{s}` loaded only by dead {_list([_name(p, u) for u in users])}"
                if users
                else f"no code loads selector `{s}`, and no framework or nib sends it"
            )
        elif k == "vtable":
            users = sorted(p.vtable_users.get(int(s), ()))
            cls = p.vtable_class.get(int(s), f"0x{int(s):X}")
            parts.append(
                f"vtable of `{cls}` built only by dead {_list([_name(p, u) for u in users])}"
                if users
                else f"vtable of `{cls}`, which no function loads"
            )
        elif k == "native":
            parts.append(f"native `{s}`, which no shipped script calls")
    return "; ".join(parts)


def classify(p: Program, executed: Iterable[int], rules: Sequence[Rule] = RULES) -> list[Gap]:
    """Every unexecuted function that is not Chartboost's, with its class and evidence."""
    executed = {_plain(x) for x in executed}
    live = live_set(p, executed)
    compiled = [(re.compile(r.pattern), r) for r in rules]
    gaps: list[Gap] = []
    for ptr, f in sorted(p.functions.items()):
        if ptr in executed or f.tag == runcoverage.NOT_GAME:
            continue
        if ptr not in live:
            gaps.append(Gap(f.ptr, f.name, f.tag, "dead", dead_evidence(p, ptr)))
        elif ptr in p.difftest_passed:
            gaps.append(Gap(f.ptr, f.name, f.tag, "difftest", "passes 1,000 inputs (ADR 0017)"))
        else:
            rule = next((r for rx, r in compiled if rx.fullmatch(f.name)), None)
            cls = rule.cls if rule else "missed"
            evidence = rule.reason if rule and rule.reason else _live_evidence(p, ptr, live)
            if rule and rule.issue:
                evidence += f"; follow-up #{rule.issue}"
            gaps.append(Gap(f.ptr, f.name, f.tag, cls, evidence))
    return gaps


def _live_evidence(p: Program, ptr: int, live: set[int]) -> str:
    refs = p.refs.get(ptr, [])
    callers = sorted(int(s) for k, s in refs if k == "code" and int(s) in live)
    if callers:
        return f"called by live {_list([_name(p, c) for c in callers])}"
    holders = sorted(int(s) for k, s in refs if k == "table" and int(s) in live)
    if holders:
        return f"its address is in data loaded by live {_list([_name(p, h) for h in holders])}"
    for k, s in refs:
        if k == "selector":
            users = sorted(u for u in p.selector_users.get(str(s), ()) if u in live)
            if users:
                return f"selector `{s}` loaded by live {_list([_name(p, u) for u in users])}"
            if s in p.framework:
                return f"selector `{s}`: {p.framework[str(s)]}"
            return f"selector `{s}`"
        if k == "vtable":
            return f"virtual, in the live vtable of `{p.vtable_class.get(int(s), s)}`"
        if k == "native":
            return f"native `{s}`, called by shipped scripts"
        if k == "data":
            return f"a data word at `0x{int(s):X}` that no code loads holds its address"
        if k == "entry":
            return "the entry point or a static initializer"
    return "live"


# -- the doc ---------------------------------------------------------------------------------


def render(gaps: Sequence[Gap], p: Program, executed: int) -> str:
    game = [f for f in p.functions.values() if f.tag != runcoverage.NOT_GAME]
    counts = {c: sum(g.cls == c for g in gaps) for c in CLASSES}
    lines = [
        "# Coverage gaps",
        "",
        f"<!-- Generated by `{GENERATE}` (tools/mtport/coveragegaps.py); "
        f"`{CHECK}` checks it. Do not edit by hand. -->",
        "",
        "Every game function that no automated run has entered, with a reason or a test "
        "([#305](https://github.com/armaatus/maplethief-port/issues/305)). The runs are the "
        "whole real-asset test suite under the run coverage "
        "([testing/run-coverage.md](../testing/run-coverage.md), ADR 0160). Chartboost's "
        "functions are left out: they never run by design ([libraries.md](libraries.md)).",
        "",
        f"- **Game functions: {len(game):,}; executed by a run: {executed:,}; "
        f"not executed: {len(gaps):,}.**",
        f"- `dead`: {counts['dead']:,}. No live code reaches them: every reference to them "
        "(call, address taken, selector load, vtable, script native) comes from dead code, or "
        "there is none.",
        f"- `difftest`: {counts['difftest']:,}. Live, and the differential test (ADR 0017) "
        "runs them on 1,000 random inputs each against Unicorn with zero mismatches.",
        f"- `unreachable`: {counts['unreachable']:,}. Live, but only from a state no run can "
        "produce; the evidence says which. Host-driven tests for them: "
        f"[#{HOST_TESTS}](https://github.com/armaatus/maplethief-port/issues/{HOST_TESTS}).",
        f"- `missed`: {counts['missed']:,}. Live and reachable by play; no run enters them yet. "
        f"Run steps for them: [#{RUN_STEPS}](https://github.com/armaatus/maplethief-port/issues/"
        f"{RUN_STEPS}).",
        "- A `dead` row with a reason in words comes from a rule: the port or the shipped data "
        "never takes the path.",
        "",
        "How a function is classed is in the module docstring of `tools/mtport/coveragegaps.py`."
        " Names come from the binary's symbols, the ObjC and C++ metadata and the committed "
        "names files ([function-names.md](function-names.md)).",
        "",
        "| Address | Name | Tag | Class | Evidence |",
        "|---|---|---|---|---|",
    ]
    for g in gaps:
        name = g.name.replace("|", "\\|")
        ev = g.evidence.replace("|", "\\|")
        lines.append(f"| `0x{g.address:X}` | `{name}` | `{g.tag}` | {g.cls} | {ev} |")
    return "\n".join(lines) + "\n"


def doc_unexecuted(text: str) -> set[int]:
    """The functions a committed doc lists (plain addresses)."""
    return {int(m[1], 16) & ~1 for line in text.splitlines() if (m := ROW.match(line))}


# -- loading ---------------------------------------------------------------------------------


def _read(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def scripts_natives(root: Path) -> set[str]:
    """The natives some shipped script calls."""
    from .scriptout import scan

    stats, _ = scan(root)
    return set(stats.native_calls)


def nib_outlets(app: Path) -> list[tuple[str, str]]:
    """(nib, outlet name) of every outlet connection of the bundle's nibs."""
    from . import nib

    out = []
    for path in sorted(app.rglob("*.nib")):
        if path.is_file():
            out += [(path.name, name) for name, _, _ in nib.load(path).outlets()]
    return out


def _data_words(work: Path) -> list[tuple[int, int]]:
    from . import bootstrap, macho
    from .rtti_analysis import data_words

    slice_path = work.parent / bootstrap.slice_path("armv7")
    return list(data_words(macho.MachO(slice_path.read_bytes())))


def load_program(work: Path = WORK, app: Path = runcoverage.APP) -> Program:
    """The program from ``work/`` and the extracted bundle. Raises FileNotFoundError."""
    tags = _read(work / "re" / "function-tags.json")
    entry = [
        int(f["address"], 16)
        for f in tags["functions"]
        if f.get("rule") in ("entry-point", "static-init")
    ]
    difftest = work / "recomp" / "difftest.json"
    p = build_program(
        tags,
        _read(work / "ghidra" / "xrefs.json")["xrefs"],
        _read(work / "re" / "objc.json"),
        _read(work / "re" / "vtables.json"),
        scripts_natives(app / "script"),
        _read(difftest) if difftest.is_file() else None,
        entry,
        _read(work / "re" / "imports.json")["sends"],
        _data_words(work),
        nib_outlets(app),
    )
    return p


def executed_from_doc(p: Program, text: str) -> set[int]:
    """Every function a doc does not list (Chartboost's included)."""
    unrun = doc_unexecuted(text)
    return {a for a in p.functions if a not in unrun}


def added_to_doc(p: Program, text: str, executed: Iterable[int]) -> set[int]:
    """The functions a doc counts as executed and those of a new run (``generate --add``)."""
    return executed_from_doc(p, text) | {a & ~1 for a in executed}


def document(p: Program, executed: set[int]) -> tuple[str, list[Gap]]:
    """The doc for the functions ``executed`` (plain addresses), and its rows."""
    gaps = classify(p, executed)
    run = sum(1 for a, f in p.functions.items() if a in executed and f.tag != runcoverage.NOT_GAME)
    return render(gaps, p, run), gaps


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.coveragegaps", description=__doc__.split("\n\n")[0]
    )
    sub = parser.add_subparsers(dest="command", required=True)
    g = sub.add_parser("generate", help="write the doc from a run coverage")
    g.add_argument("--coverage", type=Path, nargs="+", required=True)
    g.add_argument("--out", type=Path, default=DOC)
    g.add_argument(
        "--add",
        action="store_true",
        help="add these runs' functions to the ones the doc already counts as executed (a new "
        "run step, without rerunning the whole suite)",
    )
    c = sub.add_parser("check", help="regenerate from the doc's own list; fail when stale")
    c.add_argument("--doc", type=Path, default=DOC)
    args = parser.parse_args(argv)
    try:
        p = load_program()
    except FileNotFoundError as e:
        print(f"error: {e.filename} is missing (scripts/re-analyze, scripts/difftest)")
        return 2
    if args.command == "generate":
        try:
            cov = runcoverage.load(args.coverage)
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        executed = {a & ~1 for a in cov.functions}
        out = args.out
        if args.add:
            executed = added_to_doc(p, out.read_text(encoding="utf-8"), executed)
    else:
        out = args.doc
        executed = executed_from_doc(p, out.read_text(encoding="utf-8"))
    text, gaps = document(p, executed)
    if args.command == "check":
        if out.read_text(encoding="utf-8") != text:
            print(f"FAIL   {out} is stale: run {GENERATE}")
            return 1
        print(f"ok     {out}: {len(gaps)} functions not executed")
        return 0
    out.write_text(text, encoding="utf-8")
    counts = ", ".join(f"{sum(x.cls == k for x in gaps)} {k}" for k in CLASSES)
    print(f"wrote {out}: {len(gaps)} functions not executed ({counts})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
