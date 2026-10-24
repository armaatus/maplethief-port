"""The Objective-C shell (#22): what each of the 18 app classes uses and calls.

The mechanical half of ``docs/re/objc-shell.md``. For every app class (``objc.json``, #8) it
collects, from a Capstone sweep (:mod:`mtport.armscan`, receivers resolved as in
:mod:`mtport.imports`) and the Ghidra exports:

* **functions**: the class's IMPs plus its file-local helpers: the ``objc-file`` functions of
  ``libtags`` (#10) and the curated :data:`SHELL_NAMES` (inline ``CGRectMake``-style copies,
  block invoke/copy/dispose helpers, two C helpers);
* **framework classes**: classes loaded from ``__objc_classrefs`` and receivers of sends;
* **framework messages**: sends that reach an implementation outside the image (receiver
  class where it is known), the HLE surface of the class;
* **shell messages**: sends whose selector only app classes implement: the class's own methods
  and edges to other app classes;
* **engine calls** (ObjC → C++): direct calls from the class's functions to functions tagged
  ``engine``, with the constant arguments of the ``Clet_HandleEvent`` key events; and **engine
  sends** (C++ → ObjC): sends from ``engine`` functions that reach this class's methods;
* **selectors passed as arguments** (button and gesture wiring): a selector reference that a
  function loads but does not send, with the send it is passed to;
* **images**: strings the class's functions reference that name a root file of the app bundle
  (``work/ipa/Payload/MapleThief.app``), exactly or without the extension.

It also renders ``MainWindow.nib`` (:mod:`mtport.nib`). The judgement half (what each class is
for, the effort estimate) is hand-written in the same doc; this module only rewrites the blocks
between ``<!-- BEGIN generated:KEY -->`` and ``<!-- END generated:KEY -->``, and fails if the
doc lacks one.

A send's receiver is unknown at most sites (an ivar or an argument). Such a send counts as a
shell message when only app classes implement the selector and no send anywhere in the image
reaches a framework class with it; otherwise it is a framework message with receiver ``?``.

Command line::

    python3 -m mtport.objcshell names [--repo DIR]      # work/re/names/objcshell.json
    python3 -m mtport.objcshell generate [--repo DIR]   # work/re/objcshell.json + doc blocks
    python3 -m mtport.objcshell trace [--repo DIR] FUNCTION...   # one function's events

``names`` needs only the slice; ``generate`` and ``trace`` need the Ghidra export, the function
tags and the extracted bundle (``scripts/re-analyze``). ``trace`` prints a function's events in
site order (calls, sends with their arguments, ivar, class and string references) for a hex
address or every function whose name contains the text. It is a reading aid.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

from mtport import armscan, imports, libtags, macho, names, nib, objc
from mtport import ghidra_common as gc
from mtport.names import hexaddr

SCHEMA_VERSION = 1
SOURCE = "objcshell"
JSON_OUT = "work/re/objcshell.json"
NAMES_OUT = "work/re/names/objcshell.json"
NAMES_DIR = "work/re/names"
MARKDOWN = "docs/re/objc-shell.md"
GHIDRA_DIR = "work/ghidra"
TAGS_JSON = "work/re/function-tags.json"
LIBTAGS_STEP = "python3 -m mtport.libtags generate"
BUNDLE = "work/ipa/Payload/MapleThief.app"
NIB_NAME = "MainWindow.nib"

APP_OBJC, ENGINE = "app-objc", "engine"
KEY_EVENT = "Clet_HandleEvent"
ALLOC = ("alloc", "allocWithZone:", "new")

TOUCH_SELECTORS = (
    "touchesBegan:withEvent:",
    "touchesMoved:withEvent:",
    "touchesEnded:withEvent:",
    "touchesCancelled:withEvent:",
)
# UIControlEvents bits (UIControl.h).
CONTROL_EVENTS = {
    1 << 0: "TouchDown",
    1 << 1: "TouchDownRepeat",
    1 << 2: "TouchDragInside",
    1 << 3: "TouchDragOutside",
    1 << 4: "TouchDragEnter",
    1 << 5: "TouchDragExit",
    1 << 6: "TouchUpInside",
    1 << 7: "TouchUpOutside",
    1 << 8: "TouchCancel",
    1 << 12: "ValueChanged",
}

# (function pointer, owning class, name, comment). Each was read in the decompiler and in its
# callers. The inline CoreGraphics/UIKit constructors are -O0 copies, one per source file;
# a 2-field one is told apart (CGPointMake or CGSizeMake) by what its result is passed to.
SHELL_NAMES: tuple[tuple[int, str, str, str], ...] = (
    (0x55B5, "EAGLView", "EAGLView_CGPointMake", "inline copy; result -> setCenter:"),
    (0x55E1, "EAGLView", "EAGLView_CGRectMake", "inline copy"),
    (0x562D, "EAGLView", "EAGLView_CGAffineTransformMake", "inline copy; -> setTransform:"),
    (0x9FCE9, "MoviePlayerController", "MoviePlayerController_CGRectMake", "inline copy"),
    (
        0xA2579,
        "ChooseCharacterView",
        "ChooseCharacterView_AddStatusLabel",
        "(text, x, y, superview, alignment): Helvetica 12 UILabel, 183x24, clear background",
    ),
    (0xA7A6D, "ChooseCharacterView", "ChooseCharacterView_CGPointMake", "inline copy"),
    (0xA7A99, "ChooseCharacterView", "ChooseCharacterView_CGRectMake", "inline copy"),
    (0xA7AE5, "ChooseCharacterView", "ChooseCharacterView_UIEdgeInsetsMake", "inline copy"),
    (
        0xA7B51,
        "ChooseCharacterView",
        "ChooseCharacterView_AddStatusLabelLeft",
        "(text, point, superview): ChooseCharacterView_AddStatusLabel with alignment 0",
    ),
    (
        0xA7B79,
        "MainMenuView",
        "MainMenuView_RectOffset",
        "(rect, dx, dy) -> CGRectMake(x + dx, y + dy, w, h); first function of MainMenuView's "
        "file (calls its CGRectMake copy), also called by ChooseCharacterView",
    ),
    (0xADF81, "MainMenuView", "MainMenuView_CGPointMake", "inline copy"),
    (0xADFAD, "MainMenuView", "MainMenuView_CGSizeMake", "inline copy; -> setContentSize:"),
    (0xADFD9, "MainMenuView", "MainMenuView_CGRectMake", "inline copy"),
    (0xAE025, "MainMenuView", "MainMenuView_UIEdgeInsetsMake", "inline copy"),
    (0xAE6FD, "MapKeyPadView", "MapKeyPadView_CGPointMake", "inline copy; -> touchPoint"),
    (0xAFEA5, "MarketKeyPadView", "MarketKeyPadView_CGPointMake", "inline copy"),
    (0xAFED1, "MarketKeyPadView", "MarketKeyPadView_CGRectMake", "inline copy"),
    (0xAFF1D, "MarketKeyPadView", "MarketKeyPadView_UIEdgeInsetsMake", "inline copy"),
    (0xB1755, "NpcKeyPadView", "NpcKeyPadView_CGRectMake", "inline copy"),
    (0xB17A1, "NpcKeyPadView", "NpcKeyPadView_UIEdgeInsetsMake", "inline copy"),
    (0xB4545, "PopupKeyPadView", "PopupKeyPadView_CGRectMake", "inline copy"),
    (0xB4591, "PopupKeyPadView", "PopupKeyPadView_UIEdgeInsetsMake", "inline copy"),
    (
        0xB45FD,
        "TouchKeyPadView",
        "TouchKeyPadView_Distance",
        "(x1, y1, x2, y2) -> sqrtf(dx * dx + dy * dy): finger travel in the touch handlers",
    ),
    (0xBD135, "TouchKeyPadView", "TouchKeyPadView_CGPointMake", "inline copy"),
    (0xBD161, "TouchKeyPadView", "TouchKeyPadView_CGRectMake", "inline copy"),
    (0xBD1AD, "TouchKeyPadView", "TouchKeyPadView_UIEdgeInsetsMake", "inline copy"),
    (0xBDC1D, "CashShopKeyPadView", "CashShopKeyPadView_CGRectMake", "inline copy (C++ file)"),
    (
        0xBDC7D,
        "CashShopKeyPadView",
        "CashShopKeyPadView_UIEdgeInsetsMake",
        "inline copy (C++ file)",
    ),
    (0xC2095, "TermsView", "TermsView_CGSizeMake", "inline copy; -> setContentSize:"),
    (0xC20C1, "TermsView", "TermsView_CGRectMake", "inline copy"),
    (0xC210D, "TermsView", "TermsView_UIEdgeInsetsMake", "inline copy"),
    (0xC2D61, "HelpScrollKeyPadView", "HelpScrollKeyPadView_CGPointMake", "inline copy"),
    (0xC2D8D, "HelpScrollKeyPadView", "HelpScrollKeyPadView_CGSizeMake", "inline copy"),
    (0xC2DB9, "HelpScrollKeyPadView", "HelpScrollKeyPadView_CGRectMake", "inline copy"),
)

# GameCenterManager's completion-handler blocks: (invoke, copy helper, dispose helper, method).
# The invoke function's pointer sits in the block literal; copy and dispose in its descriptor.
GAMECENTER_BLOCKS: tuple[tuple[int, int | None, int | None, str], ...] = (
    (0xC0419, 0xC0499, 0xC04BD, "AuthenticationUser"),
    (0xC04D5, 0xC0519, 0xC0551, "ReloadHighScoreByCategory"),
    (0xC0571, 0xC05A9, 0xC05CD, "ReportScore"),
    (0xC05E5, 0xC07DD, 0xC0815, "SubmitAchievement_load"),
    (0xC0835, 0xC086D, 0xC0891, "SubmitAchievement_report"),
    (0xC08A9, 0xC08E1, 0xC0905, "ResetAchievements"),
    (0xC091D, 0xC0A71, 0xC0AA9, "FindPlayerByid"),
    (0xC03D5, 0xC0AC9, 0xC0B15, "callDelegateOnMainThread"),
)


# The engine's C bridge into the shell: each is ``[[[EAGLView_Instance Instance] objView] SEL]``
# (61 bytes) and is called from C++ game code. Not part of any class: they are engine
# functions, named here after the selector they send.
BRIDGES: tuple[tuple[int, str], ...] = (
    (0x6655, "enalbeHelpScrollKey"),
    (0x66A5, "enableCashShopKey"),
    (0x66F5, "enableMapKey"),
    (0x6745, "disableAlertKey"),
    (0x6795, "enableAlertKey"),
    (0x67E5, "enableMarketKey"),
    (0x6835, "enableNPCKey"),
    (0x6885, "enableAnimationKey"),
    (0x68D5, "disablePopupMenuKey"),
    (0x6925, "enablePopupMenuKey"),
    (0x6975, "disableTouchKeyPad"),
    (0x69C5, "enableTouchKeyPad"),
)


def bridge_name(selector: str) -> str:
    return f"Shell_{selector[0].upper()}{selector[1:]}"


def curated_names() -> list[tuple[int, str | None, str, str]]:
    """:data:`SHELL_NAMES`, the GameCenterManager block helpers and the :data:`BRIDGES`
    (class None: engine functions)."""
    out: list[tuple[int, str | None, str, str]] = list(SHELL_NAMES)
    for fp, sel in BRIDGES:
        out.append((fp, None, bridge_name(sel), f"engine -> shell bridge: [objView {sel}]"))
    for invoke, copy, dispose, method in GAMECENTER_BLOCKS:
        base = f"GameCenterManager_{method}"
        out.append((invoke, "GameCenterManager", f"{base}_block_invoke", "block body"))
        if copy is not None:
            out.append((copy, "GameCenterManager", f"{base}_block_copy", "block copy helper"))
        if dispose is not None:
            out.append(
                (dispose, "GameCenterManager", f"{base}_block_dispose", "block dispose helper")
            )
    return sorted(out)


class ShellError(ValueError):
    """An input is missing or inconsistent, or the doc's generated blocks are missing."""


# -- names --------------------------------------------------------------------------------


def build_names(image: macho.MachO, taken: dict[int, str]) -> list[dict]:
    """The names-file entries for :func:`curated_names`, checked against the image's function
    starts and against the other names files (which must not name these addresses)."""
    starts = set(image.function_starts)
    problems, entries, seen = [], [], set()
    for fp, cls, name, comment in curated_names():
        if fp not in starts:
            problems.append(f"0x{fp:X} {name}: not an LC_FUNCTION_STARTS entry (or wrong mode)")
        if fp & ~1 in seen:
            problems.append(f"0x{fp:X} {name}: listed twice")
        seen.add(fp & ~1)
        other = taken.get(fp & ~1)
        if other is not None and other != name:
            problems.append(f"0x{fp:X}: objcshell says {name!r}, another names file {other!r}")
        text = f"{cls}: {comment}" if cls else comment
        entries.append({"address": hexaddr(fp), "name": name, "kind": "function", "comment": text})
    if problems:
        raise ShellError("; ".join(problems))
    doc = {"source": SOURCE, "names": entries}
    names.parse(doc, NAMES_OUT)
    return entries


def names_json(entries: list[dict]) -> str:
    return gc.dump_json({"source": SOURCE, "names": entries})


# -- inputs -------------------------------------------------------------------------------


@dataclass
class Function:
    pointer: int
    name: str
    size: int
    tag: str
    rule: str
    reason: str


@dataclass
class Program:
    image: macho.MachO
    objc: objc.ObjCImage
    scan: armscan.Scan
    sends: list[dict]  # imports.resolve_sends records, plus "args" (the argument words)
    functions: dict[int, Function]  # LC_FUNCTION_STARTS pointer -> function
    stub_of: dict[int, str]  # stub address -> import name
    class_framework: dict[str, str]  # imported class -> framework
    strings: dict[int, tuple[str, str]]  # address -> (kind, value): cfstring / cstring
    bundle: list[str]  # root file names of the app bundle, sorted
    index: armscan.FunctionIndex = field(init=False)

    def __post_init__(self) -> None:
        self.index = armscan.FunctionIndex(self.functions)

    def start(self, addr: int) -> int | None:
        """The function pointer whose entry is ``addr`` (Thumb bit optional), else None."""
        ptr = self.index.of(addr)
        return ptr if ptr is not None and ptr & ~1 == addr & ~1 else None

    def name(self, addr: int) -> str:
        ptr = self.start(addr)
        if ptr is not None and ptr in self.functions:
            return self.functions[ptr].name
        if addr in self.stub_of:
            return self.stub_of[addr]
        return f"FUN_{addr & ~1:08x}"


def load_ghidra_functions(path: Path) -> dict[int, dict]:
    doc = gc.load_export(path)
    return {int(f["address"], 16): f for f in doc["functions"]}


def load_strings(path: Path) -> dict[int, tuple[str, str]]:
    doc = gc.load_export(path)
    out = {}
    for s in doc["strings"]:
        if s["kind"] in ("cfstring", "cstring") and s["encoding"] != "hex":
            out[int(s["address"], 16)] = (s["kind"], s["value"])
    return out


def bundle_files(bundle: Path) -> list[str]:
    """Root file names of the bundle (directories left out)."""
    return sorted(p.name for p in bundle.iterdir() if p.is_file())


def argument_words(o: objc.ObjCImage) -> dict[str, int]:
    """Selector -> argument words after ``self`` and ``_cmd``, from the image's method type
    strings (``v40@0:4@8...``: a 40-byte frame is 8 argument words). For a selector the image
    doesn't implement, a caller assumes one word per ``:``. Only the words a send really passes
    are kept, so stale stack slots are not mistaken for arguments."""
    out: dict[str, int] = {}
    methods = [m for k in o.classes for m in k.instance_methods + k.class_methods]
    methods += [m for c in o.categories for m in c.instance_methods + c.class_methods]
    for m in methods:
        frame = re.match(r"\D*(\d+)", m.types)
        if frame:
            out[m.selector] = max(out.get(m.selector, 0), (int(frame[1]) - 8 + 3) // 4)
    return out


def build_program(
    image: macho.MachO,
    ghidra_functions: dict[int, dict],
    tags: dict,
    strings: dict[int, tuple[str, str]],
    bundle: list[str],
) -> Program:
    table = imports.import_table(image)
    stub_of = {a: i.name for i in table for a in i.stubs}
    class_framework = {
        i.name[len(objc.CLASS_SYMBOL) :]: i.framework
        for i in table
        if i.name.startswith(objc.CLASS_SYMBOL)
    }
    dispatchers = [a for i in table if i.name in imports.DISPATCHERS for a in i.stubs]
    scan = armscan.scan(image, dispatchers)
    objc_image = objc.parse(image)
    resolved = imports.resolve_sends(image, objc_image, scan, stub_of, {})
    frames = argument_words(objc_image)
    for entry, send in zip(resolved, scan.sends, strict=True):
        stret = entry["stub"].endswith("_stret")
        words = list(send.regs[3 if stret else 2 :]) + list(send.stack)
        sel = entry["selector"]
        entry["args"] = words[: frames.get(sel, sel.count(":"))] if sel else []
    functions = {}
    for r in tags["functions"]:
        ptr = int(r["address"], 16)
        g = ghidra_functions.get(ptr, {})
        functions[ptr] = Function(
            ptr, g.get("name") or r["name"], r["size"], r["tag"], r["rule"], r["reason"]
        )
    return Program(
        image, objc_image, scan, resolved, functions, stub_of, class_framework, strings, bundle
    )


# -- classifying sends --------------------------------------------------------------------


FRAMEWORK, APP, CHARTBOOST, UNKNOWN = "framework", "app", "chartboost", "unknown"


@dataclass
class Selectors:
    """What the image knows about each selector."""

    app: dict[str, set[str]]  # selector -> app classes (or categories) implementing it
    framework: set[str]  # selectors some send sends to a framework receiver
    strict: set[str]  # ... to a class reference, +alloc result or super (firmly resolved)
    supers: dict[str, list[str]]  # class -> its superclass chain (imported part included)


def _misresolved(e: dict, owners: frozenset[str], info: Selectors) -> frozenset[str]:
    """The app classes a send really reaches when the resolver gave it the superclass.

    ``self = [super init…]`` makes the resolver see the superclass as the receiver of every
    later send on ``self`` (``how`` ``init``). Such a send reaches the app subclass when one
    implements the selector and no send with a firmly resolved framework receiver (a class
    reference, ``+alloc`` or ``super``) uses it."""
    if e["how"] != "init" or e["selector"] in info.strict:
        return frozenset()
    return frozenset(c for c in owners if e["receiver"] in info.supers.get(c, ()))


def selector_info(p: Program) -> Selectors:
    app: dict[str, set[str]] = defaultdict(set)
    cb: dict[str, set[str]] = defaultdict(set)
    for k in p.objc.classes:
        target = cb if k.group == "chartboost" else app
        for m in k.instance_methods + k.class_methods:
            target[m.selector].add(k.name)
    for c in p.objc.categories:
        label = f"{c.class_name}({c.name})"
        for m in c.instance_methods + c.class_methods:
            # Chartboost's own categories don't concern the game shell.
            (cb if c.name == "ATFunctionalCollectionExtensions" else app)[m.selector].add(label)
    own = {k.name: k for k in p.objc.classes}
    supers = {}
    for k in p.objc.classes:
        chain, cur = [], k
        while cur is not None and cur.superclass is not None:
            chain.append(cur.superclass.name)
            cur = own.get(cur.superclass.name)
        supers[k.name] = chain
    resolved = [
        e for e in p.sends if e["selector"] and e["receiver"] is not None and e["in_image"] is False
    ]
    strict = {e["selector"] for e in resolved if e["how"] != "init"}
    info = Selectors(dict(app), set(strict), strict, supers)
    for e in resolved:
        if not _misresolved(e, frozenset(app.get(e["selector"], ())), info):
            info.framework.add(e["selector"])
    return info


def classify(e: dict, info: Selectors) -> tuple[str, frozenset[str], str]:
    """``(kind, owners, shown)`` of one send. ``owners`` are the app classes that implement
    it (kind ``app``); ``shown`` is the framework receiver (``?`` when unknown)."""
    sel = e["selector"]
    if sel is None:
        return UNKNOWN, frozenset(), "?"
    owners = frozenset(info.app.get(sel, ()))
    recv, impl = e["receiver"], e["implemented_by"]
    if recv is not None:
        if e["in_image"]:
            if impl is not None and objc.is_chartboost(impl):
                return CHARTBOOST, frozenset(), impl
            return APP, frozenset({impl}) if impl else owners, impl or recv
        subs = _misresolved(e, owners, info)
        if subs:
            return APP, subs, recv
        return FRAMEWORK, frozenset(), impl or recv
    if owners and sel not in info.framework:
        return APP, owners, "?"
    # A selector only Chartboost implements (`tag`, `identifier`) is still a framework
    # selector when the receiver is unknown: game code reaches Chartboost only through a
    # resolved `Chartboost` class reference.
    return FRAMEWORK, frozenset(), "?"


# -- per-class analysis -------------------------------------------------------------------


@dataclass
class ShellClass:
    name: str
    superclass: str
    protocols: list[str]
    methods: int
    imps: list[int]
    helpers: list[int]
    bytes: int
    created_by: list[int] = field(default_factory=list)
    framework_classes: Counter = field(default_factory=Counter)  # class -> code sites
    framework_messages: Counter = field(default_factory=Counter)  # (receiver, sel) -> sends
    own_messages: Counter = field(default_factory=Counter)  # selector -> sends
    app_edges: Counter = field(default_factory=Counter)  # (owner classes, selector) -> sends
    chartboost_messages: Counter = field(default_factory=Counter)
    unknown_sends: int = 0
    engine_calls: dict[int, set[int]] = field(default_factory=dict)  # callee -> callers
    shell_calls: dict[int, set[int]] = field(default_factory=dict)  # other classes' helpers
    key_events: list[tuple[int, tuple]] = field(default_factory=list)  # (caller, args)
    engine_sends: dict[str, set[int]] = field(default_factory=dict)  # selector -> engine fns
    touches: list[str] = field(default_factory=list)
    delegates: list[str] = field(default_factory=list)
    actions: list[tuple[int, str, str]] = field(default_factory=list)  # (fn, action, via)
    images: dict[str, list[str]] = field(default_factory=dict)  # string -> bundle files
    unreferenced: list[str] = field(default_factory=list)  # methods with no selector ref


def app_classes(o: objc.ObjCImage) -> list[objc.ObjCClass]:
    return [k for k in o.classes if k.group == "app"]


def assign_functions(p: Program) -> dict[str, tuple[list[int], list[int]]]:
    """App class -> (IMPs, helpers). Helpers: libtags ``objc-file`` functions and the
    curated names."""
    out: dict[str, tuple[list[int], list[int]]] = {}
    owner: dict[int, str] = {}
    for k in app_classes(p.objc):
        imps = sorted({m.imp for m in k.instance_methods + k.class_methods})
        out[k.name] = (imps, [])
        for i in imps:
            owner[i] = k.name
    curated = {fp: cls for fp, cls, _, _ in curated_names() if cls is not None}
    for fp, cls in curated.items():
        if cls not in out:
            raise ShellError(f"{hexaddr(fp)}: curated helper of unknown class {cls!r}")
        if fp in owner:
            raise ShellError(f"{hexaddr(fp)}: curated helper is an IMP of {owner[fp]}")
    for ptr, f in sorted(p.functions.items()):
        cls = curated.get(ptr)
        if cls is None and f.tag == APP_OBJC and ptr not in owner and f.rule == "objc-file":
            cls = f.reason.split(":", 1)[0]
            if cls not in out:
                raise ShellError(f"{hexaddr(ptr)}: objc-file helper of unknown class {cls!r}")
        if cls is not None:
            out[cls][1].append(ptr)
            owner[ptr] = cls
    return out


def resource_files(text: str, bundle: list[str]) -> list[str]:
    """Bundle root files a string names, exactly or without the extension."""
    if not text or "/" in text or len(text) > 96:
        return []
    if text in bundle:
        return [text]
    return [b for b in bundle if "." in b and b.rsplit(".", 1)[0] == text]


def argument_selectors(p: Program, funcs: set[int]) -> list[tuple[int, str, str]]:
    """``(function, selector, passed to)`` for each selector reference a function loads
    without sending it. Each send consumes the closest earlier load of its selector, and a send
    whose selector the scan lost consumes the closest earlier load of any selector; a load
    left over is an argument of the next send after it (``addTarget:action:forControlEvents:``,
    a button helper, a delegate call)."""
    selrefs = dict(p.objc.selrefs)
    loads: dict[int, list[tuple[int, int]]] = defaultdict(list)  # function -> (site, selref)
    for r in p.scan.refs:
        if r.function in funcs and r.kind == "load" and r.target in selrefs:
            loads[r.function].append((r.site, r.target))
    sends: dict[int, list[dict]] = defaultdict(list)
    for e in p.sends:
        if e["function"] in funcs:
            sends[e["function"]].append(e)
    out = []
    for fn in sorted(loads):
        pending = sorted(loads[fn])
        fsends = sorted(sends.get(fn, []), key=lambda e: e["site"])
        used: set[int] = set()
        for e in fsends:
            for i in range(len(pending) - 1, -1, -1):
                site, ref = pending[i]
                if i not in used and site < e["site"] and selrefs[ref] == e["selector"]:
                    used.add(i)
                    break
        # A send whose selector the scan lost most likely used the closest load before it.
        for e in fsends:
            if e["selector"] is not None:
                continue
            for i in range(len(pending) - 1, -1, -1):
                if i not in used and pending[i][0] < e["site"]:
                    used.add(i)
                    break
        for i, (site, ref) in enumerate(pending):
            if i in used:
                continue
            nxt = next((e for e in fsends if e["site"] > site), None)
            via = nxt["selector"] if nxt and nxt["selector"] else "?"
            out.append((fn, selrefs[ref], via))
    return out


def analyse_class(
    p: Program,
    k: objc.ObjCClass,
    imps: list[int],
    helpers: list[int],
    info: Selectors,
    key_event: int | None,
    shell_helpers: dict[int, str],
) -> ShellClass:
    funcs = set(imps) | set(helpers)
    sc = ShellClass(
        name=k.name,
        superclass=k.superclass.name if k.superclass else "-",
        protocols=list(k.protocols),
        methods=len(k.instance_methods) + len(k.class_methods),
        imps=imps,
        helpers=sorted(helpers),
        bytes=sum(p.functions[f].size for f in funcs if f in p.functions),
    )
    classrefs = {slot: ref for slot, ref in p.objc.classrefs if ref is not None}
    creators = set()
    for r in p.scan.refs:
        if r.target not in classrefs or r.kind != "load":
            continue
        ref = classrefs[r.target]
        if r.function in funcs and ref.address is None:
            sc.framework_classes[ref.name] += 1
    for e in p.sends:
        if e["receiver"] == k.name and e["class_method"] and e["selector"] in ALLOC:
            if e["function"] not in funcs:
                creators.add(e["function"])
    sc.created_by = sorted(creators)
    for e in p.sends:
        if e["function"] not in funcs:
            continue
        kind, owners, shown = classify(e, info)
        sel = e["selector"]
        if kind == UNKNOWN:
            sc.unknown_sends += 1
        elif kind == FRAMEWORK:
            prefix = "" if e["receiver"] is None else ("+" if e["class_method"] else "-")
            sc.framework_messages[(shown, prefix + sel)] += 1
            if shown != "?":
                sc.framework_classes[shown] += 0
        elif kind == CHARTBOOST:
            sc.chartboost_messages[sel] += 1
        elif k.name in owners:
            sc.own_messages[sel] += 1
        else:
            sc.app_edges[(tuple(sorted(owners)), sel)] += 1
    for r in p.scan.refs:
        if r.function not in funcs or r.kind not in ("call", "tail"):
            continue
        callee = p.start(r.target)
        f = p.functions.get(callee) if callee is not None else None
        if callee is not None and callee in shell_helpers and callee not in funcs:
            sc.shell_calls.setdefault(callee, set()).add(r.function)
            continue
        if f is None or f.tag != ENGINE or callee in funcs:
            continue
        sc.engine_calls.setdefault(callee, set()).add(r.function)
        if callee == key_event:
            args = p.scan.call_args.get(r.site, ())
            consts = tuple(a[1] if a and a[0] == "k" else None for a in args[:3])
            sc.key_events.append((r.function, consts))
    own = {m.selector for m in k.instance_methods + k.class_methods}
    for e in p.sends:
        f = p.functions.get(e["function"])
        if f is None or f.tag != ENGINE or e["selector"] not in own:
            continue
        kind, owners, _ = classify(e, info)
        if kind == APP and k.name in owners:
            sc.engine_sends.setdefault(e["selector"], set()).add(e["function"])
    referenced = {sel for _, sel in p.objc.selrefs}
    sc.unreferenced = [
        f"{'+' if m in k.class_methods else '-'}{m.selector}"
        for m in k.instance_methods + k.class_methods
        if m.selector not in referenced
    ]
    sc.touches = [s for s in TOUCH_SELECTORS if s in own]
    protocol_methods = {
        m.selector
        for proto in p.objc.protocols
        if proto.name in k.protocols
        for m in proto.instance_methods + proto.optional_instance_methods
    }
    sc.delegates = sorted(own & protocol_methods)
    sc.actions = argument_selectors(p, funcs)
    for r in p.scan.refs:
        if r.function in funcs and r.target in p.strings:
            text = p.strings[r.target][1]
            hits = resource_files(text, p.bundle)
            if hits:
                sc.images[text] = hits
    return sc


@dataclass
class Shell:
    classes: list[ShellClass]
    bundle_png: list[str]
    shell_png: set[str]
    engine_png: set[str]
    engine_png_functions: dict[str, set[int]]
    engine_framework: Counter = field(default_factory=Counter)  # selector -> engine sends
    engine_framework_functions: set[int] = field(default_factory=set)


def analyse(p: Program) -> Shell:
    info = selector_info(p)
    assigned = assign_functions(p)
    key_event = next((f.pointer for f in p.functions.values() if f.name == KEY_EVENT), None)
    helpers = {h: cls for cls, (_, hs) in assigned.items() for h in hs}
    classes = [
        analyse_class(p, k, *assigned[k.name], info, key_event, helpers)
        for k in app_classes(p.objc)
    ]
    png = [b for b in p.bundle if b.lower().endswith(".png")]
    shell = {f for c in classes for files in c.images.values() for f in files}
    engine_png: set[str] = set()
    engine_fns: dict[str, set[int]] = defaultdict(set)
    for r in p.scan.refs:
        f = p.functions.get(r.function)
        if f is None or f.tag != ENGINE or r.target not in p.strings:
            continue
        for hit in resource_files(p.strings[r.target][1], png):
            engine_png.add(hit)
            engine_fns[hit].add(r.function)
    out = Shell(classes, png, shell & set(png), engine_png, dict(engine_fns))
    for e in p.sends:
        f = p.functions.get(e["function"])
        if f is not None and f.tag == ENGINE and classify(e, info)[0] == FRAMEWORK:
            out.engine_framework[e["selector"]] += 1
            out.engine_framework_functions.add(e["function"])
    return out


# -- JSON ---------------------------------------------------------------------------------


def _fn(p: Program, ptr: int) -> dict:
    return {"address": hexaddr(ptr), "name": p.name(ptr)}


def to_json(p: Program, s: Shell, source: dict) -> dict:
    classes = []
    for c in s.classes:
        classes.append(
            {
                "name": c.name,
                "superclass": c.superclass,
                "protocols": c.protocols,
                "methods": c.methods,
                "bytes": c.bytes,
                "imps": [hexaddr(i) for i in c.imps],
                "helpers": [_fn(p, h) for h in c.helpers],
                "created_by": [_fn(p, f) for f in c.created_by],
                "framework_classes": dict(sorted(c.framework_classes.items())),
                "framework_messages": [
                    {"receiver": r, "selector": sel, "sends": n}
                    for (r, sel), n in sorted(c.framework_messages.items())
                ],
                "own_messages": dict(sorted(c.own_messages.items())),
                "app_edges": [
                    {"classes": list(o), "selector": sel, "sends": n}
                    for (o, sel), n in sorted(c.app_edges.items())
                ],
                "chartboost_messages": dict(sorted(c.chartboost_messages.items())),
                "unknown_sends": c.unknown_sends,
                "engine_calls": [
                    {**_fn(p, callee), "from": [_fn(p, f) for f in sorted(callers)]}
                    for callee, callers in sorted(c.engine_calls.items())
                ],
                "shell_calls": [
                    {**_fn(p, callee), "from": [_fn(p, f) for f in sorted(callers)]}
                    for callee, callers in sorted(c.shell_calls.items())
                ],
                "key_events": [
                    {**_fn(p, f), "args": list(args)} for f, args in sorted(c.key_events)
                ],
                "engine_sends": [
                    {"selector": sel, "from": [_fn(p, f) for f in sorted(fns)]}
                    for sel, fns in sorted(c.engine_sends.items())
                ],
                "touches": c.touches,
                "delegates": c.delegates,
                "actions": [
                    {**_fn(p, f), "action": a, "passed_to": via} for f, a, via in c.actions
                ],
                "images": dict(sorted(c.images.items())),
                "unreferenced": c.unreferenced,
            }
        )
    return {
        "tool": "mtport.objcshell",
        "schema_version": SCHEMA_VERSION,
        "source": source,
        "bundle_png": len(s.bundle_png),
        "shell_png": sorted(s.shell_png),
        "engine_png": sorted(s.engine_png),
        "classes": classes,
    }


# -- Markdown -----------------------------------------------------------------------------


def _code(text: str) -> str:
    text = text.replace("|", "\\|")
    return f"`` {text} ``" if "`" in text else f"`{text}`"


def _short(p: Program, ptr: int, cls: str | None = None) -> str:
    """A function as ``name`` (``0x…``); a method of ``cls`` loses its class prefix."""
    name = p.name(ptr)
    if cls and name.startswith(("-[", "+[")) and name[2:].startswith(cls + " "):
        name = name[0] + name[len(cls) + 3 : -1]
    return f"{_code(name)} `{hexaddr(ptr)}`"


def _names(p: Program, ptrs, cls: str | None = None, limit: int = 4) -> str:
    ptrs = sorted(ptrs)
    shown = ", ".join(_short(p, f, cls) for f in ptrs[:limit])
    return shown + (f" +{len(ptrs) - limit} more" if len(ptrs) > limit else "")


def _counted(items: list[tuple[str, int]]) -> str:
    return ", ".join(f"{_code(t)}" + (f" ×{n}" if n > 1 else "") for t, n in items)


def _by_count(counter: Counter) -> list:
    return sorted(counter.items(), key=lambda kv: (-kv[1], kv[0]))


def _keycode(v: int | None) -> str:
    if v is None:
        return "?"
    v = v - (1 << 32) if v & 0x80000000 else v
    if 0x20 < v < 0x7F:
        return f"{v} `'{chr(v)}'`"
    return str(v)


def summary_block(p: Program, s: Shell) -> str:
    lines = [
        "| Class | Superclass | Methods | Helpers | Bytes | Framework classes | "
        "Framework selectors | ObjC → C++ | C++ → ObjC | Images | Created by |",
        "|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for c in s.classes:
        sels = {sel for (_, sel) in c.framework_messages}
        created = _names(p, c.created_by, limit=2) if c.created_by else "–"
        if c.name in ("EAGLView", "MapleThiefAppDelegate"):
            created = "`MainWindow.nib`"
        lines.append(
            f"| [`{c.name}`](#{c.name.lower()}) | `{c.superclass}` | {c.methods} | "
            f"{len(c.helpers)} | {c.bytes:,} | {len(c.framework_classes)} | {len(sels)} | "
            f"{len(c.engine_calls)} | {len(c.engine_sends)} | {len(c.images)} | {created} |"
        )
    tot = s.classes
    lines.append(
        f"| **Total** | | {sum(c.methods for c in tot)} | {sum(len(c.helpers) for c in tot)} "
        f"| {sum(c.bytes for c in tot):,} | "
        f"{len({k for c in tot for k in c.framework_classes})} | "
        f"{len({sel for c in tot for (_, sel) in c.framework_messages})} | "
        f"{len({f for c in tot for f in c.engine_calls})} | "
        f"{sum(len(c.engine_sends) for c in tot)} | "
        f"{len({i for c in tot for i in c.images})} | |"
    )
    lines.append("")
    lines.append(
        f"*Framework classes*: distinct imported classes loaded or messaged. *Framework "
        f"selectors*: distinct selectors of framework messages. *ObjC → C++*: distinct `engine` "
        f"functions called. *C++ → ObjC*: distinct selectors of this class that `engine` code "
        f"sends. *Images*: distinct strings naming a bundle file. The bundle has "
        f"{len(s.bundle_png)} root PNGs: {len(s.shell_png)} are named by shell code, "
        f"{len(s.engine_png - s.shell_png)} only by engine code, "
        f"{len(set(s.bundle_png) - s.shell_png - s.engine_png)} by neither (see "
        f"[Images](#images-and-the-bundle))."
    )
    to_shell = {sel for c in s.classes for sel in c.engine_sends}
    senders = {f for c in s.classes for fns in c.engine_sends.values() for f in fns}
    callees = {f for c in s.classes for f in c.engine_calls}
    lines += [
        "",
        f"Across the boundary: shell code calls {len(callees)} distinct `engine` functions "
        f"directly. `engine` code sends {len(to_shell)} distinct selectors that reach shell "
        f"classes, from {len(senders)} functions, and {len(s.engine_framework)} distinct "
        f"selectors that reach framework classes, from {len(s.engine_framework_functions)} "
        f"functions ({sum(s.engine_framework.values())} sends). A selector that several app "
        "classes implement is counted for each of them when the receiver is unknown.",
    ]
    return "\n".join(lines)


ADD_BUTTON = (
    "addButtonWithImageName:selectedImageName:withFrame:withEdgeInsets:"
    "actionForTouchDown:actionForTouchUpInside:"
)


def _prefix(p: Program, owners, selector: str) -> str:
    """``+`` when the selector is a class method of one of the app classes, else ``-``."""
    for k in p.objc.classes:
        if k.name in owners and any(m.selector == selector for m in k.class_methods):
            return "+"
    return "-"


def _via(selector: str) -> str:
    return "`addButtonWithImageName:…`" if selector == ADD_BUTTON else _code(selector)


def class_block(p: Program, c: ShellClass) -> str:
    out: list[str] = []
    imps = c.imps
    rng = f"`{hexaddr(imps[0])}`–`{hexaddr(imps[-1])}`" if imps else "none"
    helpers = f", {len(c.helpers)} helpers: {_names(p, c.helpers, limit=99)}" if c.helpers else ""
    out.append(f"- **Code**: {c.methods} methods ({rng}){helpers}; {c.bytes:,} bytes.")
    if c.created_by:
        out.append(f"- **Created by**: {_names(p, c.created_by, limit=6)}.")
    by_fw: dict[str, list[str]] = defaultdict(list)
    for cls in sorted(c.framework_classes):
        by_fw[p.class_framework.get(cls, "?")].append(cls)
    if by_fw:
        parts = [f"{fw}: {', '.join(_code(k) for k in v)}" for fw, v in sorted(by_fw.items())]
        out.append(f"- **Framework classes**: {'; '.join(parts)}.")
    if c.framework_messages:
        msgs = _by_count(c.framework_messages)
        known = [(f"{r} {sel}", n) for (r, sel), n in msgs if r != "?"]
        unknown = [(sel, n) for (r, sel), n in msgs if r == "?"]
        text = f"- **Framework messages** ({sum(c.framework_messages.values())} sends)"
        if known:
            text += f": {_counted(known)}"
        if unknown:
            text += f"; receiver not determined: {_counted(unknown)}"
        out.append(text + ".")
    if c.app_edges:
        edges = [
            (f"{'/'.join(o)} {_prefix(p, o, sel)}{sel}", n)
            for (o, sel), n in sorted(c.app_edges.items(), key=lambda kv: (-kv[1], kv[0]))
        ]
        out.append(f"- **Messages to other shell classes**: {_counted(edges)}.")
    if c.own_messages:
        out.append(
            f"- **Messages to itself**: {sum(c.own_messages.values())} sends of "
            f"{len(c.own_messages)} of its own selectors."
        )
    if c.shell_calls:
        calls = [f"{_short(p, h)} from {_names(p, f, c.name)}" for h, f in c.shell_calls.items()]
        out.append(f"- **Calls helpers of other classes**: {'; '.join(sorted(calls))}.")
    if c.chartboost_messages:
        out.append(f"- **Chartboost**: {_counted(_by_count(c.chartboost_messages))}.")
    if c.unknown_sends:
        out.append(f"- **Sends with no known selector**: {c.unknown_sends}.")
    if c.touches:
        out.append(f"- **Touch overrides**: {', '.join(f'`{t}`' for t in c.touches)}.")
    if c.delegates:
        out.append(f"- **Protocol methods**: {', '.join(f'`{d}`' for d in c.delegates)}.")
    if c.actions:
        grouped: dict[str, dict[str, set[int]]] = defaultdict(lambda: defaultdict(set))
        for f, action, via in c.actions:
            grouped[via][action].add(f)
        for via, acts in sorted(grouped.items()):
            items = ", ".join(
                f"`{a}` ({_names(p, fns, c.name, limit=2)})" for a, fns in sorted(acts.items())
            )
            out.append(f"- **Selectors passed to** {_via(via)} ({len(acts)}): {items}.")
    if c.images:
        out.append(
            f"- **Images** ({len(c.images)}): {', '.join(_code(i) for i in sorted(c.images))}."
        )
    if c.unreferenced:
        out.append(
            f"- **No selector reference** ({len(c.unreferenced)}; called only by the frameworks, "
            f"or dead): {', '.join(_code(m) for m in c.unreferenced)}."
        )
    if c.engine_calls:
        out += ["", "ObjC → C++ (direct calls into `engine` functions):", ""]
        out += ["| Engine function | Called from |", "|---|---|"]
        for callee, callers in sorted(c.engine_calls.items()):
            out.append(f"| {_short(p, callee)} | {_names(p, callers, c.name)} |")
    if c.key_events:
        out += ["", f"`{KEY_EVENT}(type, code, arg)` per method (type 2 press, 3 release):", ""]
        out += ["| Method | Type | Code | Arg |", "|---|---|---|---|"]
        for f, args in sorted(c.key_events, key=lambda fa: p.name(fa[0])):
            t, code, arg = (list(args) + [None, None, None])[:3]
            out.append(f"| {_short(p, f, c.name)} | {t} | {_keycode(code)} | {_keycode(arg)} |")
    if c.engine_sends:
        out += ["", "C++ → ObjC (sends from `engine` functions that reach this class):", ""]
        out += ["| Selector | Sent by |", "|---|---|"]
        for sel, fns in sorted(c.engine_sends.items()):
            out.append(f"| `{_prefix(p, {c.name}, sel)}{sel}` | {_names(p, fns)} |")
    return "\n".join(out)


def images_block(p: Program, s: Shell) -> str:
    engine_only = sorted(s.engine_png - s.shell_png)
    neither = sorted(set(s.bundle_png) - s.shell_png - s.engine_png)
    lines = [
        f"- {len(s.shell_png)} of the {len(s.bundle_png)} root PNGs are named by shell code "
        "(listed per class above).",
        f"- {len(engine_only)} only by `engine` code: "
        + (
            ", ".join(
                f"{_code(f)} ({_names(p, s.engine_png_functions[f], limit=2)})" for f in engine_only
            )
            or "none"
        )
        + ".",
        f"- {len(neither)} by no string in the binary: "
        + (", ".join(_code(f) for f in neither) or "none")
        + ".",
    ]
    return "\n".join(lines)


def _nib_value(n: nib.NibArchive, v: nib.Value) -> str:
    if isinstance(v.value, nib.Ref):
        target = n.objects[v.value.index]
        if n.class_name(target) == "NSString":
            return f"#{target.index} {json.dumps(n.string(target))}"
        return f"#{target.index} {n.label(target)}"
    floats = v.floats()
    if floats is not None:
        return "(" + ", ".join(f"{f:g}" for f in floats) + ")"
    if v.type in (nib.TRUE, nib.FALSE):
        return "YES" if v.type == nib.TRUE else "NO"
    if isinstance(v.value, float):
        return f"{v.value:g}"
    return repr(v.value)


def nib_block(n: nib.NibArchive) -> str:
    lines = [
        f"`NIBArchive` format {n.format_version}, coder {n.coder_version}: {len(n.objects)} "
        f"objects, {len(n.keys)} keys, {sum(len(o.values) for o in n.objects)} values, "
        f"{len(n.classes)} classes.",
        "",
        "| # | Class | Label | Values |",
        "|---|---|---|---|",
    ]
    for o in n.objects:
        if n.class_name(o) == "NSString":
            vals = json.dumps(n.string(o))
        else:
            vals = "; ".join(f"`{v.key}` = {_nib_value(n, v)}" for v in o.values)
        lines.append(f"| {o.index} | `{n.class_name(o)}` | `{n.label(o)}` | {vals} |")
    root = n.root()
    lines += ["", "| Root key | Contents |", "|---|---|"]
    for v in root.values:
        arr = n.deref(v.value)
        items = ", ".join(f"#{x.index} `{n.label(x)}`" for x in n.array(arr)) or "empty"
        lines.append(f"| `{v.key}` | #{arr.index}: {items} |")
    lines += ["", "| Connection | Source | Outlet | Destination |", "|---|---|---|---|"]
    for c in n.array(n.deref(n.get(root, "UINibConnectionsKey"))):
        src, dst = n.deref(n.get(c, "UISource")), n.deref(n.get(c, "UIDestination"))
        label = n.string(n.deref(n.get(c, "UILabel")))
        lines.append(
            f"| #{c.index} `{n.class_name(c)}` | #{src.index} `{n.label(src)}` | `{label}` | "
            f"#{dst.index} `{n.label(dst)}` |"
        )
    return "\n".join(lines)


def blocks(p: Program, s: Shell, n: nib.NibArchive) -> dict[str, str]:
    out = {"summary": summary_block(p, s), "nib": nib_block(n), "images": images_block(p, s)}
    for c in s.classes:
        out[f"class:{c.name}"] = class_block(p, c)
    return out


_BLOCK = re.compile(
    r"(<!-- BEGIN generated:(?P<key>[\w:]+) -->\n)(?P<body>.*?)(<!-- END generated:(?P=key) -->)",
    re.S,
)


def render(doc: str, generated: dict[str, str]) -> str:
    """``doc`` with each generated block's body replaced. Every key must have exactly one
    block, and every block a key."""
    found = Counter(m["key"] for m in _BLOCK.finditer(doc))
    problems = [f"no block for {k}" for k in generated if found[k] == 0]
    problems += [f"block {k} appears {n} times" for k, n in found.items() if n > 1]
    problems += [f"unknown block {k}" for k in found if k not in generated]
    if problems:
        raise ShellError(f"{MARKDOWN}: " + "; ".join(problems))
    note = "<!-- Generated by `python3 -m mtport.objcshell generate`: do not edit by hand. -->\n"
    return _BLOCK.sub(lambda m: m[1] + note + generated[m["key"]].rstrip("\n") + "\n" + m[4], doc)


# -- trace --------------------------------------------------------------------------------


def _value_text(p: Program, v, selrefs, classrefs, ivars) -> str:
    if v is None:
        return "?"
    if v[0] == "ret":
        return f"ret@{hexaddr(v[1])}"
    if v[0] == "m":
        a = v[1]
        if a in selrefs:
            return f"@selector({selrefs[a]})"
        if classrefs.get(a) is not None:
            return f"[{classrefs[a].name} class]"
        if a in ivars:
            return f"ivar {ivars[a]}"
        return f"*{hexaddr(a)}"
    n = v[1]
    if n in p.strings:
        k, text = p.strings[n]
        return ("@" if k == "cfstring" else "") + json.dumps(text, ensure_ascii=False)[:80]
    return str(n) if n < 0x10000 else hexaddr(n)


def trace(p: Program, ptr: int) -> list[str]:
    selrefs = dict(p.objc.selrefs)
    classrefs = dict(p.objc.classrefs)
    ivars = {
        i.offset_address: f"{k.name}.{i.name}"
        for k in p.objc.classes
        for i in k.ivars
        if i.offset_address
    }
    events: list[tuple[int, str]] = []
    sends = {e["site"]: e for e in p.sends if e["function"] == ptr}
    for r in p.scan.refs:
        if r.function != ptr or r.target in selrefs:
            continue
        if r.kind in ("call", "tail"):
            if r.site in sends:
                continue
            args = p.scan.call_args.get(r.site, ())
            shown = ", ".join(_value_text(p, a, selrefs, classrefs, ivars) for a in args)
            events.append((r.site, f"{r.kind} {p.name(r.target)}({shown})"))
        elif r.target in ivars:
            events.append((r.site, f"ivar {ivars[r.target]}"))
        elif classrefs.get(r.target) is not None:
            events.append((r.site, f"class {classrefs[r.target].name}"))
        elif r.target in p.strings:
            k, text = p.strings[r.target]
            events.append((r.site, f"{k} {json.dumps(text, ensure_ascii=False)[:80]}"))
        else:
            events.append((r.site, f"{r.kind} {hexaddr(r.target)}"))
    for site, e in sends.items():
        args = " ".join(_value_text(p, a, selrefs, classrefs, ivars) for a in e["args"])
        events.append((site, f"send [{e['receiver'] or '?'} {e['selector']}] {args}".rstrip()))
    return [f"  {hexaddr(s)}  {t}" for s, t in sorted(events)]


# -- driver -------------------------------------------------------------------------------


def load_program(repo: Path) -> Program:
    from mtport import bootstrap

    slice_path = repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH)
    ghidra = repo / GHIDRA_DIR
    for path in (slice_path, ghidra / "functions.json", repo / TAGS_JSON, repo / BUNDLE):
        if not path.exists():
            raise ShellError(f"{path} is missing (run scripts/bootstrap and scripts/re-analyze)")
    return build_program(
        macho.load(slice_path),
        load_ghidra_functions(ghidra / "functions.json"),
        gc.load_json(repo / TAGS_JSON, libtags.SCHEMA_VERSION, producer=LIBTAGS_STEP),
        load_strings(ghidra / "strings.json"),
        bundle_files(repo / BUNDLE),
    )


def build(repo: Path) -> tuple[Program, Shell, dict, str]:
    """Everything ``generate`` writes: the program, the analysis, the JSON and the doc."""
    from mtport import bootstrap

    p = load_program(repo)
    s = analyse(p)
    n = nib.load(repo / BUNDLE / NIB_NAME)
    slice_path = repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH)
    source = gc.source_info(slice_path, slice_path.read_bytes(), repo)
    doc = render((repo / MARKDOWN).read_text(encoding="utf-8"), blocks(p, s, n))
    return p, s, to_json(p, s, source), doc


def generate(repo: Path) -> Shell:
    _, s, doc_json, doc = build(repo)
    gc.write_text(repo / JSON_OUT, gc.dump_json(doc_json))
    gc.write_text(repo / MARKDOWN, doc)
    print(f"wrote {JSON_OUT} and the generated blocks of {MARKDOWN}")
    return s


def write_names(repo: Path) -> list[dict]:
    from mtport import bootstrap

    image = macho.load(repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH))
    entries = build_names(image, names.other_names(repo / NAMES_DIR, Path(NAMES_OUT).name))
    gc.write_text(repo / NAMES_OUT, names_json(entries))
    print(f"wrote {NAMES_OUT} ({len(entries)} names)")
    return entries


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.objcshell", description=__doc__.splitlines()[0]
    )
    parser.add_argument("command", choices=("generate", "names", "trace"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("functions", nargs="*", help="trace: hex addresses or name substrings")
    args = parser.parse_args(argv)
    try:
        if args.command == "names":
            write_names(args.repo)
        elif args.command == "generate":
            generate(args.repo)
        else:
            p = load_program(args.repo)
            for text in args.functions:
                if re.fullmatch(r"0x[0-9A-Fa-f]+", text):
                    found = [p.index.of(int(text, 16))]
                else:
                    found = [f.pointer for f in p.functions.values() if text in f.name]
                for ptr in found:
                    if ptr is not None:
                        print(f"{hexaddr(ptr)} {p.name(ptr)}")
                        print("\n".join(trace(p, ptr)))
    except (OSError, ShellError, macho.MachOError, names.NamesError, nib.FormatError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
