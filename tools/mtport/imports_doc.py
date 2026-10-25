"""The outputs of :mod:`mtport.imports`: ``work/re/imports.json`` (:func:`to_json`), the names
file ``work/re/names/imports.json`` (:func:`names_json`) and ``docs/re/imports.md``
(:func:`to_markdown`). ``mtport.imports`` re-exports every name defined here.
"""

from __future__ import annotations

from collections import Counter

from mtport.imports_analysis import (
    BOTH,
    CHARTBOOST,
    GAME,
    SCHEMA_VERSION,
    UNREFERENCED,
    Analysis,
    Ghidra,
    _function_name,
)
from mtport.names import hexaddr, parse_address

# -- outputs ------------------------------------------------------------------------------


def import_rows(a: Analysis) -> list[dict]:
    """One record per import, in ``nlist`` order (the body of ``imports.json``)."""
    rows = []
    for imp in a.imports:
        sites = a.sites.get(imp.name, [])
        per_function = Counter(f for _, f, _ in sites)
        functions = [
            {
                "function": hexaddr(f),
                "name": _function_name(a.names, f),
                "class": a.function_classes.get(f, GAME),
                "sites": n,
            }
            for f, n in sorted(per_function.items(), key=lambda kv: (-kv[1], kv[0]))
        ]
        check = a.ghidra_check[imp.name]
        rows.append(
            {
                "name": imp.name,
                "library": imp.library,
                "framework": imp.framework,
                "kind": imp.kind,
                "weak": imp.weak,
                "class": a.classes[imp.name],
                "code_sites": len(sites),
                "data_uses": len(a.owners.get(imp.name, [])),
                "stubs": [hexaddr(x) for x in imp.stubs],
                "lazy_pointers": [hexaddr(x) for x in imp.lazy],
                "non_lazy_pointers": [hexaddr(x) for x in imp.non_lazy],
                "binds": [{"address": hexaddr(x), "section": s} for x, s in imp.binds],
                "functions": functions,
                "sites": [[hexaddr(s), hexaddr(f), k] for s, f, k in sites],
                "data": a.owners.get(imp.name, []),
                "ghidra": {
                    "sites": check["ghidra"],
                    "capstone_sites": check["capstone"],
                    "only_capstone": [hexaddr(x) for x in check["only_capstone"]],
                    "only_ghidra": [hexaddr(x) for x in check["only_ghidra"]],
                },
            }
        )
    return rows


def message_rows(a: Analysis) -> list[dict]:
    """Sends grouped by (implementing class, class method?, selector), sorted."""
    groups: dict[tuple, dict] = {}
    for e in a.sends:
        if e["selector"] is None:
            continue
        key = (e["implemented_by"], e["class_method"], e["selector"], e["in_image"])
        g = groups.setdefault(
            key,
            {
                "class": e["implemented_by"],
                "class_method": e["class_method"],
                "selector": e["selector"],
                "in_image": e["in_image"],
                "game": 0,
                "chartboost": 0,
                "unreferenced": 0,
            },
        )
        side = e["class"] if e["class"] in (CHARTBOOST, UNREFERENCED) else GAME
        g[side] += 1
    return sorted(
        groups.values(),
        key=lambda g: (
            g["class"] is None,
            g["class"] or "",
            not g["class_method"],
            g["selector"],
            bool(g["in_image"]),
        ),
    )


def counts(a: Analysis) -> dict:
    by_class = Counter(a.classes.values())
    sends = a.sends
    msgs = message_rows(a)
    framework = [m for m in msgs if m["class"] is not None and m["in_image"] is False]
    unknown_fw = [m for m in msgs if m["class"] is None and m["in_image"] is False]
    return {
        "imports": len(a.imports),
        "game": by_class[GAME],
        "both": by_class[BOTH],
        "chartboost": by_class[CHARTBOOST],
        "unreferenced": by_class[UNREFERENCED],
        "hle": by_class[GAME] + by_class[BOTH],
        "stubs": sum(len(i.stubs) for i in a.imports),
        "lazy_pointers": sum(len(i.lazy) for i in a.imports),
        "non_lazy_pointers": sum(len(i.non_lazy) for i in a.imports),
        "code_sites": sum(len(v) for v in a.sites.values()),
        "functions": len(a.function_classes),
        "game_functions": sum(1 for c in a.function_classes.values() if c == GAME),
        "chartboost_functions": sum(1 for c in a.function_classes.values() if c == CHARTBOOST),
        "both_functions": sum(1 for c in a.function_classes.values() if c == BOTH),
        "unreferenced_functions": sum(1 for c in a.function_classes.values() if c == UNREFERENCED),
        "sends": len(sends),
        "sends_with_selector": sum(1 for e in sends if e["selector"] is not None),
        "sends_with_receiver": sum(1 for e in sends if e["receiver"] is not None),
        "framework_messages": len(framework),
        "framework_messages_game": sum(1 for m in framework if m["game"]),
        "unknown_receiver_framework_selectors": len(unknown_fw),
        "ghidra_agree": sum(
            1 for v in a.ghidra_check.values() if not v["only_capstone"] and not v["only_ghidra"]
        ),
    }


def to_json(a: Analysis, source: dict | None = None) -> dict:
    sends = [
        {
            "site": hexaddr(e["site"]),
            "function": hexaddr(e["function"]),
            "stub": e["stub"],
            "selector": e["selector"],
            "receiver": e["receiver"],
            "class_method": e["class_method"],
            "how": e["how"],
            "implemented_by": e["implemented_by"],
            "in_image": e["in_image"],
            "class": e["class"],
        }
        for e in a.sends
    ]
    return {
        "tool": "mtport.imports",
        "schema_version": SCHEMA_VERSION,
        "source": source or {},
        "counts": counts(a),
        "scan": a.scan_stats,
        "stub_problems": a.stub_problems,
        "category_classes": a.category_classes,
        "imports": import_rows(a),
        "messages": message_rows(a),
        "sends": sends,
        "functions": [
            {"function": hexaddr(f), "name": _function_name(a.names, f), "class": c}
            for f, c in sorted(a.function_classes.items())
        ],
    }


def names_json(a: Analysis, ghidra: Ghidra) -> dict:
    """``work/re/names/imports.json``: a name for each stub Ghidra left unnamed.

    Ghidra's Mach-O loader names the stubs itself (``_strcmp``; the C++ ones demangled, such
    as ``operator.new``), so this is normally empty. An entry is kept for a stub whose Ghidra
    name is missing, a default ``FUN_…`` name, or one this file set (``name_source`` names),
    so applying the file and regenerating it gives the same file.
    """
    out = []
    for imp in a.imports:
        for stub in imp.stubs:
            name, source = ghidra.names.get(stub), ghidra.name_sources.get(stub)
            unnamed = name is None or name.startswith(("FUN_", "thunk_FUN_"))
            if unnamed or (source == "names" and name == imp.name):
                out.append(
                    {
                        "address": hexaddr(stub),
                        "name": imp.name,
                        "kind": "function",
                        "comment": f"stub for {imp.name} ({imp.framework})",
                    }
                )
    return {"source": "imports", "names": sorted(out, key=lambda e: parse_address(e["address"]))}


# -- Markdown -----------------------------------------------------------------------------


_CLASS_TEXT = {
    GAME: "game",
    BOTH: "both",
    CHARTBOOST: "Chartboost only",
    UNREFERENCED: "unreferenced",
}


def _code(text: str) -> str:
    text = text.replace("|", "\\|")
    return f"`` {text} ``" if "`" in text else f"`{text}`"


def _anchor(text: str) -> str:
    return "".join(ch for ch in text.lower().replace(" ", "-") if ch.isalnum() or ch in "-_")


def _framework_order(rows: list[dict]) -> list[str]:
    sites = Counter()
    for r in rows:
        sites[r["framework"]] += r["code_sites"]
    return sorted(sites, key=lambda f: (-sites[f], f))


def _callers(r: dict, limit: int = 3) -> str:
    fs = r["functions"]
    if not fs:
        return "–"
    shown = ", ".join(f"{_code(f['name'])} ({f['sites']})" for f in fs[:limit])
    return shown + (f" +{len(fs) - limit} more" if len(fs) > limit else "")


def _data_text(r: dict) -> str:
    if not r["data"]:
        return "–"
    owners = Counter(d["owner"].split(" at ", 1)[0].split(" ", 1)[0] for d in r["data"])
    return ", ".join(f"{n} {kind}" for kind, n in sorted(owners.items()))


def to_markdown(a: Analysis, source: dict | None = None) -> str:
    c = counts(a)
    rows = import_rows(a)
    src = source or {}
    lines = [
        "# Imports",
        "",
        "<!-- Generated by `python3 -m mtport.imports generate` (tools/mtport/imports.py). "
        "Do not edit by hand: rerun the generator. -->",
        "",
        f"Every symbol the `{src.get('arch', 'armv7')}` slice imports"
        + (f" (`{src['path']}`, sha256 `{src['sha256']}`)" if "sha256" in src else "")
        + ", with its library, its code sites (calls of its stub, loads of its pointer slot), "
        "the functions that use it and whether the game, Chartboost or both need it. This is "
        "the HLE work list. Machine-readable: `work/re/imports.json` (every site, every "
        "calling function, every send). Method, cross-checks and caveats are at the "
        "[end](#method).",
        "",
        f"**The HLE layer must implement {c['hle']} of the {c['imports']} imports**: "
        f"{c['game']} used only by game code and {c['both']} used by both the game and "
        f"Chartboost. {c['chartboost']} are used only by Chartboost and can be stubbed once ads "
        f"are, and {c['unreferenced']} {'is' if c['unreferenced'] == 1 else 'are'} never used by "
        f"code ({', '.join(_code(r['name']) for r in rows if r['class'] == UNREFERENCED) or '–'}"
        + (
            "; `dyld_stub_binder` is called only by the lazy-binding `__stub_helper`, which a "
            "static recompiler does not need)."
            if any(r["name"] == "dyld_stub_binder" and r["class"] == UNREFERENCED for r in rows)
            else ")."
        ),
        "",
        "| Class | Imports | Meaning |",
        "|---|---|---|",
        f"| game | {c['game']} | only game functions use it |",
        f"| both | {c['both']} | game and Chartboost functions use it |",
        f"| Chartboost only | {c['chartboost']} | only Chartboost functions use it |",
        f"| unreferenced | {c['unreferenced']} | no function uses it |",
        f"| **HLE (game + both)** | **{c['hle']}** | |",
        "",
        "## By framework",
        "",
        "| Framework | Imports | game | both | Chartboost only | unreferenced | HLE | Code sites |",
        "|---|---|---|---|---|---|---|---|",
    ]
    order = _framework_order(rows)
    for fw in order:
        fr = [r for r in rows if r["framework"] == fw]
        k = Counter(r["class"] for r in fr)
        lines.append(
            f"| [{fw}](#{_anchor(fw)}) | {len(fr)} | {k[GAME]} | {k[BOTH]} | {k[CHARTBOOST]} "
            f"| {k[UNREFERENCED]} | {k[GAME] + k[BOTH]} | {sum(r['code_sites'] for r in fr)} |"
        )
    lines += [
        f"| **Total** | **{c['imports']}** | {c['game']} | {c['both']} | {c['chartboost']} "
        f"| {c['unreferenced']} | **{c['hle']}** | {c['code_sites']} |",
        "",
        "## Imports",
        "",
        "Per framework, sorted by code sites. *Code sites*: `bl`/`blx`/`b` to the stub, and "
        "loads of the import's pointer slot (`__nl_symbol_ptr`, `__objc_classrefs`, a bound "
        "`__data` word). *Data uses*: binds inside a structure (a class's superclass or isa, "
        "a CFString's isa, a vtable entry, a block's isa); the structure's owner decides the "
        "class. *Kind*: `function` (called through its stub), `class`/`metaclass` (ObjC), "
        "`address` (no stub, only its address is bound: data, or a function such as "
        "`___cxa_pure_virtual` reached through a pointer). *Functions*: distinct functions "
        "with a code site, the busiest named with their site count.",
        "",
    ]
    for fw in order:
        fr = sorted(
            (r for r in rows if r["framework"] == fw),
            key=lambda r: (-r["code_sites"], -r["data_uses"], r["name"]),
        )
        lib = fr[0]["library"]
        lines += [
            f"### {fw}",
            "",
            f"`{lib}`{' (weak)' if all(r['weak'] for r in fr) else ''}: {len(fr)} imports, "
            f"{sum(1 for r in fr if r['class'] in (GAME, BOTH))} for the HLE layer.",
            "",
            "| Import | Kind | Class | Code sites | Data uses | Functions | Busiest callers |",
            "|---|---|---|---|---|---|---|",
        ]
        for r in fr:
            lines.append(
                f"| {_code(r['name'])} | {r['kind']} | {_CLASS_TEXT[r['class']]} "
                f"| {r['code_sites']} | {_data_text(r)} | {len(r['functions'])} "
                f"| {_callers(r)} |"
            )
        lines.append("")

    msgs = message_rows(a)
    framework = [m for m in msgs if m["class"] is not None and m["in_image"] is False]
    unknown = [m for m in msgs if m["class"] is None and m["in_image"] is False]
    ambiguous = [m for m in msgs if m["class"] is None and m["in_image"]]
    lines += [
        "## Objective-C messages",
        "",
        f"{c['sends']} calls of the `objc_msgSend` family; {c['sends_with_selector']} with a "
        f"known selector (the `__objc_selrefs` slot loaded into r1, or r2 for `_stret`) and "
        f"{c['sends_with_receiver']} with a known receiving class: a class loaded from "
        "`__objc_classrefs` (a class method), the result of `+alloc`/`+new` or `-init…` on one "
        "(an instance method), or the superclass for `objc_msgSendSuper2`. A message to one of "
        "the image's own classes is listed under the framework class whose implementation it "
        "reaches (the first imported superclass without an override in the image).",
        "",
        "### By receiving framework class",
        "",
        f"{len(framework)} (class, selector) pairs, {c['framework_messages_game']} of them sent "
        "by game code. Counts are sends from game / Chartboost functions.",
        "",
        "| Class | Selector | Game | Chartboost |",
        "|---|---|---|---|",
    ]
    for m in framework:
        sel = ("+" if m["class_method"] else "-") + m["selector"]
        lines.append(f"| {_code(m['class'])} | {_code(sel)} | {m['game']} | {m['chartboost']} |")
    lines += [
        "",
        "### Receiver not determined",
        "",
        f"{len(unknown)} selectors that no class or category in the image implements, so "
        "every send of them reaches a framework method; the receiver is not known statically "
        f"(an ivar, an argument, a return value). Another {len(ambiguous)} selectors with an "
        "unknown receiver are also implemented in the image (`init`, `dealloc`, delegate "
        "methods), so they may reach either; they are in `work/re/imports.json` → `messages`.",
        "",
        "| Selector | Game | Chartboost |",
        "|---|---|---|",
    ]
    for m in sorted(unknown, key=lambda m: (-(m["game"] + m["chartboost"]), m["selector"])):
        lines.append(f"| {_code(m['selector'])} | {m['game']} | {m['chartboost']} |")
    lines += ["", *_method_section(a, c)]
    return "\n".join(lines) + "\n"


def _method_section(a: Analysis, c: dict) -> list[str]:
    st = a.scan_stats
    mism = {n: v for n, v in a.ghidra_check.items() if v["only_capstone"] or v["only_ghidra"]}
    lines = [
        "## Method",
        "",
        "Generated by `python3 -m mtport.imports generate` from the slice and the Ghidra "
        "exports of `tools/ghidra/import`.",
        "",
        f"1. **Import table.** {c['imports']} undefined `nlist` symbols (LC_SYMTAB), library "
        f"from the two-level-namespace ordinal in `n_desc`. The LC_DYSYMTAB indirect table maps "
        f"{c['stubs']} `__symbolstub1` stubs, {c['lazy_pointers']} `__lazy_symbol` pointers and "
        f"{c['non_lazy_pointers']} `__nl_symbol_ptr` import pointers (the other "
        "`__nl_symbol_ptr` slots point into the image). Every stub is `ldr pc, [pc, #n]` and "
        "loads the lazy pointer of the same symbol; every pointer agrees with the dyld "
        "bind/lazy-bind/weak-bind streams"
        + (" (no exceptions)." if not a.stub_problems else f" except: {a.stub_problems}."),
        f"2. **Code sites** (`tools/mtport/armscan.py`, Capstone): {st['functions']} "
        f"`LC_FUNCTION_STARTS` functions, {st['instructions']} instructions decoded "
        f"({st['skipped_bytes']} bytes of literal pools and jump tables skipped). A forward "
        "data-flow pass over each function's basic blocks follows registers and stack slots "
        "through `ldr rX, [pc, #n]`, `movw`/`movt` and `add rX, pc` to the absolute slot "
        f"address. {c['code_sites']} code sites in all.",
        f"3. **Cross-check with Ghidra** (`work/ghidra/xrefs.json`): for {c['ghidra_agree']} of "
        f"{c['imports']} imports the call and load sites are identical. Ghidra's "
        "`COMPUTED_CALL`s through a bound `__data` pointer (`free`, `malloc` via a function "
        "table) are the same accesses as the `READ`s of that pointer and are not counted twice.",
    ]
    for name, v in sorted(mism.items()):
        lines.append(
            f"   - {_code(name)}: Capstone {v['capstone']}, Ghidra {v['ghidra']}; only "
            f"Ghidra {', '.join(_code(hexaddr(x)) for x in v['only_ghidra']) or '–'}, only "
            f"Capstone {', '.join(_code(hexaddr(x)) for x in v['only_capstone']) or '–'}. "
            + (
                "Capstone has the same slot as an address-only site at "
                + ", ".join(_code(hexaddr(x)) for x in v["capstone_address_only"])
                + ": the load sits after a join where only one path holds the address, which "
                "Ghidra follows and the data-flow intersection does not."
                if v["capstone_address_only"]
                else ""
            )
        )
    lines += [
        f"4. **Function classes** (first pass, `classify_functions`; #10 replaces it): "
        f"{c['functions']} functions: {c['game_functions']} game, {c['chartboost_functions']} "
        f"Chartboost, {c['both_functions']} both, {c['unreferenced_functions']} unreferenced. "
        f"Chartboost entries are the IMPs of the 57 Chartboost classes ({len(a.chartboost_seeds)} "
        "entries with the category methods below); a function reachable only from them in the "
        "call graph (Ghidra's references between functions, plus function pointers in block "
        "descriptors, block literals and vtables, attributed to the code that references the "
        "structure) is Chartboost's. Every other function without a caller is a game root "
        "(ObjC IMPs, C++ virtuals, callbacks, `start`).",
        "   Category methods on framework classes are called only by code that sends their "
        "selector, so they are classified by those senders: "
        + "; ".join(f"{_code(k)} {_CLASS_TEXT[v]}" for k, v in sorted(a.category_classes.items()))
        + ". Methods whose selector nothing sends are unreferenced.",
        "5. **Import classes.** An import is used by the game when a game function has a code "
        "site for it or a game structure binds it, and by Chartboost likewise. A class's "
        "superclass/isa binds belong to that class; a CFString's or block's isa to the "
        "functions that reference it; other binds in `__const`/`__data` (C++ vtables and RTTI) "
        "to the game.",
        "",
        "Caveats: selectors reached through `performSelector:`/`NSSelectorFromString` and "
        "classes through `NSClassFromString` are invisible here; both functions are imported "
        "(`_NSSelectorFromString` only by Chartboost). A value whose address escapes to a "
        "callee is not tracked through that callee.",
    ]
    return lines
