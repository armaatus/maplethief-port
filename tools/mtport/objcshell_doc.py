"""The outputs of :mod:`mtport.objcshell`: ``work/re/objcshell.json`` (:func:`to_json`), the
generated blocks of ``docs/re/objc-shell.md`` (:func:`blocks`, :func:`render`) and the
``trace`` listing of one function. ``mtport.objcshell`` re-exports every name defined here.
"""

from __future__ import annotations

import json
from collections import Counter, defaultdict

from mtport import ghidra_common as gc
from mtport import nib
from mtport.names import hexaddr
from mtport.objcshell_analysis import (
    KEY_EVENT,
    MARKDOWN,
    SCHEMA_VERSION,
    Program,
    Shell,
    ShellClass,
    ShellError,
)

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


def render(doc: str, generated: dict[str, str]) -> str:
    """``doc`` with each generated block's body replaced (:func:`ghidra_common.render_blocks`).
    Every key must have exactly one block, and every block a key."""
    return gc.render_blocks(
        doc, generated, "python3 -m mtport.objcshell generate", MARKDOWN, ShellError
    )


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
