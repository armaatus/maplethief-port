"""The outputs of :mod:`mtport.rtti`: ``work/re/vtables.json`` (:func:`to_json`), the names
file ``work/re/names/rtti.json`` (:func:`names_json`) and ``docs/re/cpp-classes.md``
(:func:`to_markdown`). ``mtport.rtti`` re-exports every name defined here.
"""

from __future__ import annotations

from mtport import names
from mtport.names import hexaddr, parse_address
from mtport.rtti_analysis import (
    PURE_VIRTUAL,
    TYPEINFO_KIND_NAMES,
    Rtti,
    _order,
    layout_problems,
    slot_exceptions,
)

# Facts about MapleThief 1.5 that the generic output can't state; generate() adds them.
NOTES = (
    "**63 classes, not 64.** docs/ANALYSIS.md used to list 64 RTTI names. The 64th, `CPW`, "
    "is not a type name: the bytes `43 50 57` (`CPW`) occur only six times, all inside one "
    "Thumb function in `__text` (`0xBCED8`–`0xBCFE6`), where they are `str r3, [r0, r1]` "
    "(`43 50`) followed by the first byte of an `ldr.w r0, [r7, #-0x1c]` (`57 f8 1c 0c`). No "
    "`type_info` points at them. Every `type_info` object in the image is reached from one of "
    "the 63 `__cxxabiv1` vtable binds, and every class name they hold is listed below."
)


SCHEMA_VERSION = 1
JSON_OUT = "work/re/vtables.json"
NAMES_OUT = "work/re/names/rtti.json"
MARKDOWN_OUT = "docs/re/cpp-classes.md"


# -- JSON -----------------------------------------------------------------------------------


def counts(r: Rtti) -> dict[str, int]:
    slots = [s for v in r.vtables for s in v.slots]
    kinds = [f.kind for f in r.functions.values()]
    return {
        "classes": len(r.typeinfos),
        "class_type_info": sum(t.kind == "class" for t in r.typeinfos),
        "si_class_type_info": sum(t.kind == "si" for t in r.typeinfos),
        "vmi_class_type_info": sum(t.kind == "vmi" for t in r.typeinfos),
        "classes_with_vtable": len({v.cls for v in r.vtables}),
        "vtables": len(r.vtables),
        "secondary_vtables": sum(not v.primary for v in r.vtables),
        "slots": len(slots),
        "pure_slots": sum(s.pure for s in slots),
        "functions": len(r.functions),
        "virtual_functions": kinds.count("virtual"),
        "destructors": kinds.count("complete-dtor") + kinds.count("deleting-dtor"),
        "thunks": kinds.count("thunk"),
        "slot_exceptions": len(slot_exceptions(r)),
        "non_pure_slot_exceptions": sum(not e["pure"] for e in slot_exceptions(r)),
    }


def to_json(r: Rtti, source: dict | None = None) -> dict:
    classes = []
    for t in r.typeinfos:
        vts = []
        for v in r.vtables_of(t.name):
            vts.append(
                {
                    "address": hexaddr(v.address),
                    "address_point": hexaddr(v.address_point),
                    "offset_to_top": v.offset_to_top,
                    "subobject": r.subobject_of(v),
                    "end": v.end,
                    "slots": [
                        {
                            "index": s.index,
                            "slot": hexaddr(s.address),
                            "target": None if s.pure else hexaddr(s.target),
                            "pure": s.pure,
                            "name": None if s.pure else r.slot_name(s.target),
                            "in_function_starts": (not s.pure) and s.target in r.function_starts,
                        }
                        for s in v.slots
                    ],
                }
            )
        dtor = r.destructors.get(t.name)
        classes.append(
            {
                "name": t.name,
                "mangled": t.mangled,
                "typeinfo": hexaddr(t.address),
                "typeinfo_kind": TYPEINFO_KIND_NAMES[t.kind],
                "typeinfo_name": hexaddr(t.name_address),
                "vmi_flags": t.flags,
                "bases": [
                    {"name": n, "offset": b.offset, "public": b.public, "virtual": b.virtual}
                    for n, b in r.bases(t.name)
                ],
                "derived": r.derived(t.name),
                "destructor_slots": None if dtor is None else [dtor, dtor + 1],
                "vtables": vts,
                "evidence": r.evidence.get(t.name, []),
            }
        )
    functions = [
        {
            "address": hexaddr(f.address),
            "name": f.name,
            "owner": f.owner,
            "kind": f.kind,
            "index": f.index,
            "thunk_of": None if f.thunk_of is None else hexaddr(f.thunk_of),
            "adjust": f.adjust,
            "in_function_starts": f.address in r.function_starts,
            "size": f.size,
            "uses": [{"class": c, "vtable": hexaddr(a), "index": i} for c, a, i in f.uses],
            "note": f.note,
        }
        for f in sorted(r.functions.values(), key=lambda f: f.address)
    ]
    return {
        "schema": SCHEMA_VERSION,
        "source": source or {},
        "counts": counts(r),
        "classes": classes,
        "functions": functions,
        "slot_exceptions": slot_exceptions(r),
        "layout_problems": layout_problems(r),
    }


def names_json(r: Rtti) -> dict:
    """``work/re/names/rtti.json``: every slot function plus data labels for the type_info
    objects, their name strings and the vtables."""
    entries = []
    for f in sorted(r.functions.values(), key=lambda f: f.address):
        comment = {
            "virtual": f"virtual, slot {f.index} of {f.owner}'s vtable",
            "complete-dtor": "complete-object destructor (D1)",
            "deleting-dtor": "deleting destructor (D0)",
            "thunk": f"non-virtual thunk: this += {f.adjust}, then the call",
        }[f.kind]
        others = sorted({c for c, _, _ in f.uses} - {f.owner})
        if others:
            comment += "; also in the vtables of " + ", ".join(others)
        if f.note:
            comment += "; " + f.note
        entries.append(
            {"address": hexaddr(f.address), "name": f.name, "kind": "function", "comment": comment}
        )
    for t in r.typeinfos:
        bases = ", ".join(n for n, _ in r.bases(t.name)) or "none"
        entries.append(
            {
                "address": hexaddr(t.address),
                "name": f"{t.name}::typeinfo",
                "kind": "data",
                "comment": f"{TYPEINFO_KIND_NAMES[t.kind]} for {t.name}; bases: {bases}",
            }
        )
        entries.append(
            {
                "address": hexaddr(t.name_address),
                "name": f"{t.name}::typeinfo_name",
                "kind": "data",
                "comment": f"type_info name {t.mangled!r}",
            }
        )
    for v in r.vtables:
        sub = r.subobject_of(v)
        label = f"{v.cls}::vtable" if v.primary else f"{v.cls}::vtable_for_{sub.replace('::', '_')}"
        entries.append(
            {
                "address": hexaddr(v.address),
                "name": label,
                "kind": "data",
                "comment": f"offset_to_top {v.offset_to_top}, {len(v.slots)} slots from "
                f"{hexaddr(v.address_point)}",
            }
        )
    entries.sort(key=lambda e: parse_address(e["address"]))
    doc = {"source": "rtti", "names": entries}
    names.parse(doc, NAMES_OUT)
    return doc


# -- Markdown -------------------------------------------------------------------------------


def _bases_text(r: Rtti, cls: str) -> str:
    parts = []
    for name, b in r.bases(cls):
        extra = [] if b.public else ["non-public"]
        if b.virtual:
            extra.append("virtual")
        if b.offset:
            extra.append(f"at +{b.offset}")
        parts.append(f"`{name}`" + (f" ({', '.join(extra)})" if extra else ""))
    return ", ".join(parts) or "–"


def _anchor(name: str) -> str:
    return name.lower().replace("::", "")


def to_markdown(r: Rtti, source: dict | None = None, notes: str = "") -> str:
    c = counts(r)
    src = source or {}
    lines = [
        "# C++ classes, RTTI and vtables",
        "",
        "<!-- Generated by `python3 -m mtport.rtti generate` (tools/mtport/rtti.py). "
        "Do not edit by hand: rerun the generator. -->",
        "",
        f"Recovered from the `{src.get('arch', 'armv7')}` slice"
        + (f" (`{src['path']}`, sha256 `{src['sha256']}`)" if "sha256" in src else "")
        + " by `python3 -m mtport.rtti generate`, which also writes `work/re/vtables.json` and "
        "the Ghidra names `work/re/names/rtti.json`. Addresses are armv7 VM addresses; slot "
        "targets are function pointers (Thumb bit set). How the data is found and the naming "
        "rules are in the docstring of `tools/mtport/rtti.py`.",
        "",
        "| | Count |",
        "|---|---|",
        f"| Classes (`type_info` objects) | {c['classes']} ({c['class_type_info']} "
        f"`__class_type_info`, {c['si_class_type_info']} `__si_class_type_info`, "
        f"{c['vmi_class_type_info']} `__vmi_class_type_info`) |",
        f"| Classes with a vtable | {c['classes_with_vtable']} |",
        f"| Vtables | {c['vtables']} ({c['secondary_vtables']} secondary) |",
        f"| Slots | {c['slots']} ({c['pure_slots']} pure virtual) |",
        f"| Distinct slot functions | {c['functions']} ({c['virtual_functions']} virtual, "
        f"{c['destructors']} destructors, {c['thunks']} thunks) |",
        f"| Slots that are not an `LC_FUNCTION_STARTS` function | {c['slot_exceptions']} "
        f"(all pure virtual: {c['non_pure_slot_exceptions'] == 0}) |",
        "",
        "Function names: `Class::vfN` is slot N of the owning class's primary vtable (an "
        "override keeps its base's number), `Class::~Class` / `Class::~Class_deleting` are the "
        "complete-object (D1) and deleting (D0) destructors, and `Name@thunk-N` is a "
        "non-virtual thunk that adds -N to `this` and calls `Name`. Data labels: "
        "`Class::typeinfo`, `Class::typeinfo_name`, `Class::vtable` and "
        "`Class::vtable_for_Base` (a secondary vtable).",
        "",
    ]
    if notes:
        lines += [notes.rstrip(), ""]

    lines += ["## Hierarchy", "", "```mermaid", "classDiagram", "    direction LR"]
    for t in r.typeinfos:
        if not r.bases(t.name) and not r.derived(t.name):
            continue
        for name, b in r.bases(t.name):
            label = f" : +{b.offset}" if b.offset else ""
            lines.append(f"    {_mermaid(name)} <|-- {_mermaid(t.name)}{label}")
    lone = [t.name for t in r.typeinfos if not r.bases(t.name) and not r.derived(t.name)]
    lines += ["```", ""]
    if lone:
        lines += [
            f"Classes with no base and no derived class ({len(lone)}): "
            + ", ".join(f"`{n}`" for n in lone)
            + ".",
            "",
        ]

    lines += [
        "## Classes",
        "",
        "| Class | Bases | Derived | `type_info` | Vtable | Slots | Destructor slots |",
        "|---|---|---|---|---|---|---|",
    ]
    for t in r.typeinfos:
        vt = r.primary_vtable(t.name)
        derived = ", ".join(f"`{n}`" for n in r.derived(t.name)) or "–"
        dtor = r.destructors.get(t.name)
        lines.append(
            f"| [`{t.name}`](#{_anchor(t.name)}) | {_bases_text(r, t.name)} | {derived} "
            f"| `{hexaddr(t.address)}` ({TYPEINFO_KIND_NAMES[t.kind]}) "
            f"| {'–' if vt is None else f'`{hexaddr(vt.address)}`'} "
            f"| {'–' if vt is None else len(vt.slots)} "
            f"| {'–' if dtor is None else f'{dtor}, {dtor + 1}'} |"
        )
    lines.append("")

    _interfaces(lines, r)
    lines += ["## Vtables", ""]
    for t in r.typeinfos:
        lines += [f"### {t.name}", ""]
        facts = [
            f"`{TYPEINFO_KIND_NAMES[t.kind]}` at `{hexaddr(t.address)}`, name "
            f"`{t.mangled}` at `{hexaddr(t.name_address)}`",
            f"bases: {_bases_text(r, t.name)}",
        ]
        if t.kind == "vmi":
            facts.append(f"`__flags` {t.flags}")
        facts += r.evidence.get(t.name, [])
        lines += [f"- {f}" for f in facts] + [""]
        vts = r.vtables_of(t.name)
        if not vts:
            lines += ["No vtable in the image.", ""]
        for v in vts:
            title = "Primary vtable" if v.primary else f"Vtable for the `{r.subobject_of(v)}` base"
            lines += [
                f"{title} at `{hexaddr(v.address)}` (offset_to_top {v.offset_to_top}, address "
                f"point `{hexaddr(v.address_point)}`, {len(v.slots)} slots, followed by "
                f"{v.end}):",
                "",
                "| Slot | Target | Function | Bytes | Defined in |",
                "|---|---|---|---|---|",
            ]
            for s in v.slots:
                if s.pure:
                    method = r.method(t.name, s.index) if v.primary else None
                    what = f"pure virtual `{method}`" if method else "pure virtual"
                    lines.append(f"| {s.index} | – | {what} (`{PURE_VIRTUAL}`) | – | – |")
                    continue
                f = r.functions[s.target]
                own = "this class" if f.owner == t.name else f"`{f.owner}`"
                lines.append(
                    f"| {s.index} | `{hexaddr(s.target)}` | `{f.name}` | {f.size} | {own} |"
                )
            lines.append("")

    exc = slot_exceptions(r)
    lines += [
        "## Slot check",
        "",
        "Every slot of every vtable above is checked against `LC_FUNCTION_STARTS` "
        "(`tests/test_rtti_real.py`). The exceptions are the pure virtual slots: the compiler "
        f"leaves them 0 and dyld binds them to `{PURE_VIRTUAL}` from libstdc++, which "
        "aborts if a pure virtual function is ever called. There is no function to name.",
        "",
        f"{len(exc)} exceptions:",
        "",
        "| Class | Vtable | Slot | Slot address | Reason |",
        "|---|---|---|---|---|",
    ]
    for e in exc:
        lines.append(
            f"| `{e['class']}` | `{e['vtable']}` | {e['index']} | `{e['slot']}` | {e['reason']} |"
        )
    lines.append("")
    problems = layout_problems(r)
    lines += ["Layout cross-checks (base slot counts, secondary vtables): "]
    lines[-1] += "all consistent." if not problems else f"{len(problems)} problems:"
    lines.append("")
    lines += [f"- {p}" for p in problems]
    if problems:
        lines.append("")
    return "\n".join(lines)


def _interfaces(lines: list[str], r: Rtti) -> None:
    """One table per class that declares virtual functions besides its destructor: each
    slot it declares, against itself and every class that extends its vtable."""
    lines += [
        "## Virtual interfaces",
        "",
        "Each class below declares virtual functions besides its destructor. The table shows "
        "every slot it declares and what each derived class puts there (*inherited* = the "
        "function of the class in brackets; sizes in bytes). Method names come from "
        "localization keys the functions look up: a function of class `C` that loads "
        "`C.Method.N` keys (`ScriptExtra.strings`, see docs/ANALYSIS.md) is `C::Method`. "
        "Other slots keep their number.",
        "",
    ]
    for t in r.typeinfos:
        vt = r.primary_vtable(t.name)
        if vt is None:
            continue
        dtor = r.destructors.get(t.name)
        skip = set() if dtor is None else {dtor, dtor + 1}
        own = [
            s.index
            for s in vt.slots
            if s.index not in skip and r.introducer(t.name, s.index) == t.name
        ]
        if not own:
            continue
        family = [t.name] + [
            c
            for c in _order(r)
            if c != t.name and _primary_chain_has(r, c, t.name) and r.primary_vtable(c)
        ]
        lines += [
            f"### {t.name} interface",
            "",
            "| Slot | Method | " + " | ".join(f"`{c}`" for c in family) + " |",
            "|---|---|" + "---|" * len(family),
        ]
        for i in own:
            cells = []
            for c in family:
                s = r.primary_vtable(c).slots[i]
                if s.pure:
                    cells.append("pure")
                    continue
                f = r.functions[s.target]
                text = f"`{hexaddr(s.target)}` {f.size} B"
                cells.append(text if f.owner == c else f"*inherited* ({f.owner})")
            lines.append(f"| {i} | {r.method(t.name, i) or '–'} | " + " | ".join(cells) + " |")
        lines.append("")
        named = [(i, r.slot_names[(t.name, i)]) for i in own if (t.name, i) in r.slot_names]
        for i, (method, why) in named:
            lines.append(f"- slot {i} = `{method}`: {why}")
        if named:
            lines.append("")


def _primary_chain_has(r: Rtti, cls: str | None, ancestor: str) -> bool:
    while cls is not None:
        if cls == ancestor:
            return True
        cls = r.primary_base(cls)
    return False


def _mermaid(name: str) -> str:
    return name.replace("::", "_")
