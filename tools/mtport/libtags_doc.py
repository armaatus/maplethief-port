"""The outputs of :mod:`mtport.libtags`: ``work/re/function-tags.json`` (:func:`to_json`), the
read-only API over it (:class:`FunctionTags`, :func:`load`), the Ghidra readback
(:func:`readback_json`) and ``docs/re/libraries.md`` (:func:`to_markdown`). ``mtport.libtags``
re-exports every name defined here.
"""

from __future__ import annotations

from collections import defaultdict
from collections.abc import Iterable
from pathlib import Path

from mtport.libtags_program import (
    APP_OBJC,
    ENGINE,
    GENERATE_STEP,
    JSON_OUT,
    LIB_PREFIX,
    READBACK_SCHEMA,
    RULE_DOCS,
    SCHEMA_VERSION,
    UNKNOWN,
)
from mtport.libtags_rules import Tagger

from . import armscan
from . import ghidra_common as gc
from .names import parse_address, parse_hex

# -- output --------------------------------------------------------------------------------


def _sort_tags(tags: Iterable[str]) -> list[str]:
    order = {APP_OBJC: 0, ENGINE: 1}
    return sorted(set(tags), key=lambda s: (order.get(s, 3 if s == UNKNOWN else 2), s))


def to_json(t: Tagger, source: dict) -> str:
    prog = t.prog
    records = []
    counts: dict[str, dict[str, int]] = {
        tag: {"functions": 0, "bytes": 0, "body_bytes": 0} for tag in (APP_OBJC, ENGINE, UNKNOWN)
    }
    rules: dict[str, int] = defaultdict(int)
    for addr in prog.starts:
        f = prog.functions[addr]
        a = t.assigned[addr]
        records.append(
            {
                "address": f.pointer,
                "size": f.span,
                "body": f.body,
                "tag": a.tag,
                "rule": a.rule,
                "reason": a.reason,
                "name": f.name,
            }
        )
        c = counts.setdefault(a.tag, {"functions": 0, "bytes": 0, "body_bytes": 0})
        c["functions"] += 1
        c["bytes"] += f.span
        c["body_bytes"] += f.body
        rules[a.rule] += 1
    libraries = {
        tag: {"evidence": t.evidence.get(tag, []), **counts[tag]}
        for tag in _sort_tags(counts)
        if tag.startswith(LIB_PREFIX)
    }
    header = {
        "schema": SCHEMA_VERSION,
        "tool": "mtport.libtags",
        "program": source,
        "counts": {k: counts[k] for k in _sort_tags(counts)},
        "rules": [
            {"rule": r, "description": d, "functions": rules.get(r, 0)}
            for r, d in RULE_DOCS.items()
        ],
        "libraries": libraries,
        "signatures": t.signatures,
        "vtables": [
            {
                "address": f"0x{v.address:X}",
                "class": v.class_name,
                "typeinfo": f"0x{v.typeinfo:X}",
                "functions": len(v.functions),
            }
            for v in t.vtables
        ],
        "coverage": {"function_starts": len(prog.image.function_starts), "tagged": len(records)},
    }
    return gc.dumps(header, "functions", records)


# -- API -----------------------------------------------------------------------------------


class FunctionTags:
    """Read-only view of ``work/re/function-tags.json``."""

    def __init__(self, doc: dict) -> None:
        self.doc = doc
        self.records = {parse_address(r["address"]) & ~1: r for r in doc["functions"]}
        self._index = armscan.FunctionIndex(self.records)

    @staticmethod
    def _key(addr: int | str) -> int:
        return (parse_hex(addr) if isinstance(addr, str) else addr) & ~1

    def record(self, addr: int | str) -> dict | None:
        return self.records.get(self._key(addr))

    def tag_of(self, addr: int | str) -> str | None:
        """Tag of the function starting at ``addr`` (Thumb bit optional), or None."""
        r = self.record(addr)
        return r["tag"] if r else None

    def tag_containing(self, addr: int | str) -> str | None:
        """Tag of the function whose span holds the code address ``addr``."""
        key = self._key(addr)
        start = self._index.of(key)
        if start is None:
            return None
        r = self.records[start]
        return r["tag"] if key < start + r["size"] else None

    def functions_with(self, tag: str) -> list[str]:
        """Function pointers with ``tag`` (a ``lib:`` prefix alone matches every library)."""
        match = (lambda t: t.startswith(LIB_PREFIX)) if tag == LIB_PREFIX else tag.__eq__
        return [r["address"] for _, r in sorted(self.records.items()) if match(r["tag"])]

    def is_library(self, addr: int | str) -> bool:
        tag = self.tag_of(addr)
        return bool(tag and tag.startswith(LIB_PREFIX))

    def tags(self) -> list[str]:
        return list(self.doc["counts"])

    def counts(self) -> dict[str, dict[str, int]]:
        return self.doc["counts"]


TAG_DOCS = {
    APP_OBJC: "MapleThief ObjC shell: app class IMPs, main, code of their .m files",
    ENGINE: "MapleThief C/C++ game code",
    UNKNOWN: "no libtags rule applied",
}


def is_scheme_tag(name: str) -> bool:
    """Whether ``name`` is a tag of this scheme (and so managed by ``apply_tags.py``)."""
    return name in TAG_DOCS or (name.startswith(LIB_PREFIX) and len(name) > len(LIB_PREFIX))


def ghidra_plan(doc: dict) -> dict[int, str]:
    """Function entry (even VM address) -> the one Ghidra function tag it should carry."""
    return {parse_address(r["address"]) & ~1: r["tag"] for r in doc["functions"]}


def readback_json(doc: dict, held: dict[int, Iterable[str]]) -> str:
    """``READBACK_JSON``: the scheme tags Ghidra holds (``held``: even function entry -> its
    tag names), one record per function that has any, keyed by ``doc``'s function pointers
    (``gc.hexaddr`` of the entry for a function ``doc`` doesn't list). Deterministic."""
    pointers = {parse_address(r["address"]) & ~1: r["address"] for r in doc["functions"]}
    records = []
    for entry in sorted(held):
        tags = sorted(t for t in set(held[entry]) if is_scheme_tag(t))
        if tags:
            records.append({"address": pointers.get(entry, gc.hexaddr(entry)), "tags": tags})
    return gc.dumps({"schema": READBACK_SCHEMA, "count": len(records)}, "functions", records)


def readback_problems(doc: dict, readback: dict) -> list[str]:
    """Where the tags read back from Ghidra differ from ``doc`` (empty when they agree)."""
    if readback.get("schema") != READBACK_SCHEMA:
        return [f"readback schema {readback.get('schema')!r}, expected {READBACK_SCHEMA}"]
    plan = ghidra_plan(doc)
    held = {parse_address(r["address"]) & ~1: r["tags"] for r in readback["functions"]}
    problems = [
        f"{gc.hexaddr(a)}: Ghidra has {held.get(a, [])}, expected [{want!r}]"
        for a, want in sorted(plan.items())
        if held.get(a) != [want]
    ]
    problems += [
        f"{gc.hexaddr(a)}: Ghidra has {held[a]}, untagged"
        for a in sorted(held.keys() - plan.keys())
    ]
    return problems


def tag_comments(doc: dict) -> dict[str, str]:
    """Comment for each tag used in ``doc`` (Ghidra's function tag comment)."""
    out = {}
    for tag, c in doc["counts"].items():
        if not c["functions"]:
            continue
        if tag in TAG_DOCS:
            out[tag] = TAG_DOCS[tag]
        else:
            ev = doc["libraries"].get(tag, {}).get("evidence", [])
            first = ev[0]["reason"] if ev else ""
            out[tag] = f"statically linked library code (libtags #10): {first}"[:1000]
    return out


def load(path: str | Path | None = None) -> FunctionTags:
    p = Path(path) if path else Path(__file__).resolve().parents[2] / JSON_OUT
    return FunctionTags(gc.load_json(p, SCHEMA_VERSION, producer=GENERATE_STEP))


# -- Markdown ------------------------------------------------------------------------------


FINDINGS = """## Findings

These hold for MapleThief 1.5 (armv7); the tables below are the evidence.

- **Chartboost 3.0.4** (`lib:chartboost`) is linked as one block at the end of `__text`,
  from `0xC2D60` (after the last app class, `HelpScrollKeyPadView`) to the end at `0xD991C`.
  Besides the 57 `CB*` classes it holds the `NSArray`/`NSDictionary`
  `(ATFunctionalCollectionExtensions)` categories (linked inside the block and sent only from
  Chartboost code), an SBJson copy renamed to `CBJson*` (`org.brautaset.*` error domains), and
  `CBBSON` on top of the MongoDB C driver's `bson.c`, whose `_bson_*` functions are still in
  the export trie. The rest are blocks, their copy/dispose helpers and static C helpers.
  Chartboost is compiled with optimization (e.g. `-[CBAPIConnection sendRequest:]` at
  `0xC5375` keeps values in registers); the game's own code is not (below).
- **`UIDevice(IdentifierAddition)` + `NSString(MD5Addition)`** (`lib:uidevice-identifier`)
  are not Chartboost's: they are linked with the app, between `GameCenterManager` and
  `TermsView`, and the only sender of `-uniqueGlobalDeviceIdentifier` is the game function
  `0x98865` (engine). Chartboost has its own MAC-address code, `+[CBIdentity cbma]`
  (`0xCA23D`), which shares the `"en0"` string with `-macaddress`.
- **crt1** (`lib:crt1`) contributes only `start` (`0x27E0`, ARM); `main` (`0x280D`) is the
  app's.
- **Apple sample code**: `MyGetOpenALAudioData` and `alBufferDataStaticProc` from
  `MyOpenALSupport.h` (oalTouch) are `lib:apple-openal-support`. `GameCenterManager` is
  adapted from Apple's GKTapper sample (its assert names
  `-[GameCenterManager callDelegate:withArg:error:]`, the build path is the game's
  `Classes/GameCenterManager.mm`); it stays `app-objc` because it is part of the ObjC shell
  that gets reimplemented anyway.
- **No zlib, libpng, stock Lua or libstdc++ code.** None of the strong markers below occur. The one
  `0xEDB88320` (crc32 polynomial) is inside a C++ constructor (`0x24EA1`: it stores a vtable
  pointer, then fills a 256-entry table), so the engine has its own crc32, but there is no
  inflate/deflate, no crc table in data and no adler32 modulus. PNGs are decoded by
  UIKit/CoreGraphics. The script VM classes `Lua`, `TFunc`, `Table`, `Symbol`, `Undump` and
  `Build` are game C++ classes with RTTI and vtables, with no Lua 4/5 strings, opcode names or
  `"\\033Lua"` signature, so they are `engine`. They are a C++ rewrite of the **Lua 2.4**
  interpreter (its builtin names such as `setfallback` remain at `0x117950`; see
  [script-vm.md](script-vm.md)), not the Lua C library. libstdc++ is only imported for the C++ ABI
  (`operator new/delete`, `__cxa_*`, typeinfo vtables, `std::terminate`): no container or
  string members, no `std::` RTTI and no STL error strings, so no STL instantiations can be
  identified. Header-only templates such as `std::min` would be indistinguishable from game
  code without symbols.
- **The game is compiled without optimization** (`-O0` style: every value is stored to and
  reloaded from the stack, constants are built as `movs` + `movt #0`, e.g. `0x24EA1`). Each
  app `.m` file therefore carries out-of-line copies of SDK struct-maker inlines (44-byte
  two-float and 76/108-byte four-float constructors, `CGPointMake`/`CGRectMake`-style); the
  `objc-file` rule keeps them `app-objc`.

"""


def _kib(n: int) -> str:
    return f"{n:,} ({n / 1024:.1f} KiB)"


def to_markdown(doc: dict) -> str:
    counts = doc["counts"]
    total_f = sum(c["functions"] for c in counts.values())
    total_b = sum(c["bytes"] for c in counts.values())
    src = doc["program"]
    lines = [
        "# Statically linked libraries and function tags",
        "",
        "<!-- Generated by `python3 -m mtport.libtags generate` (tools/mtport/libtags.py). "
        "Do not edit by hand: rerun the generator. -->",
        "",
        f"Every one of the {total_f:,} `LC_FUNCTION_STARTS` functions of the armv7 slice "
        f"(sha256 `{src.get('sha256', '?')}`) has exactly one tag. The machine-readable result "
        "is `work/re/function-tags.json` (git-ignored, regenerate with "
        "`python3 -m mtport.libtags generate` after `tools/ghidra/import`); "
        "`tools/ghidra/run-script apply_tags.py` puts the tags on the Ghidra functions. "
        "Rules and their order are documented in `tools/mtport/libtags.py`.",
        "",
        "*Bytes* is each function's span from its start to the next start (so the column sums "
        "to the `__text` bytes from the first start on, literal pools and padding included); "
        "*body* is the size of Ghidra's function body.",
        "",
        "## Counts per tag",
        "",
        "| Tag | Functions | Bytes | Body bytes | Share of bytes |",
        "|---|---|---|---|---|",
    ]
    for tag, c in counts.items():
        lines.append(
            f"| `{tag}` | {c['functions']:,} | {_kib(c['bytes'])} | {c['body_bytes']:,} "
            f"| {100 * c['bytes'] / total_b:.1f}% |"
        )
    lines += [
        f"| **total** | **{total_f:,}** | **{_kib(total_b)}** "
        f"| **{sum(c['body_bytes'] for c in counts.values()):,}** | 100% |",
        "",
        FINDINGS,
        "## Rules",
        "",
        "Rules run in this order; the first one that claims a function wins.",
        "",
        "| Rule | Functions | Meaning |",
        "|---|---|---|",
    ]
    for r in doc["rules"]:
        lines.append(f"| `{r['rule']}` | {r['functions']:,} | {r['description']} |")
    by_rule: dict[tuple[str, str], int] = defaultdict(int)
    for rec in doc["functions"]:
        by_rule[(rec["tag"], rec["rule"])] += 1
    lines += [
        "",
        "Functions per tag and rule:",
        "",
        "| Tag | " + " | ".join(f"`{r['rule']}`" for r in doc["rules"]) + " |",
        "|---|" + "---|" * len(doc["rules"]),
    ]
    for tag in counts:
        cells = [str(by_rule.get((tag, r["rule"]), "")) or "–" for r in doc["rules"]]
        lines.append(f"| `{tag}` | " + " | ".join(cells) + " |")
    lines += ["", "## Libraries", ""]
    for tag, lib in doc["libraries"].items():
        lines += [
            f"### `{tag}`",
            "",
            f"{lib['functions']:,} function{'s' if lib['functions'] != 1 else ''}, "
            f"{_kib(lib['bytes'])} bytes.",
            "",
            "| Address | Evidence |",
            "|---|---|",
        ]
        for ev in lib["evidence"]:
            lines.append(f"| `{ev['address']}` | {ev['reason']} |")
        lines.append("")
    lines += [
        "## Signature searches",
        "",
        "Markers searched for in every section of the slice (*bytes*) and as 32-bit constants "
        "built with `movw`/`movt` or held in `__text` literal pools (*const*). A strong marker "
        "tags the functions that contain or reference it; a weak one is only reported.",
        "",
        "| Library | Marker | Strong | Hits | Functions |",
        "|---|---|---|---|---|",
    ]
    for s in doc["signatures"]:
        hits = ", ".join(f"`{h}`" for h in s["hits"][:4]) + (" …" if len(s["hits"]) > 4 else "")
        funcs = ", ".join(f"`{h}`" for h in s["functions"][:4])
        if len(s["functions"]) > 4:
            funcs += " …"
        lines.append(
            f"| `{s['library']}` | {s['marker']} | {'yes' if s['strong'] else 'no'} "
            f"| {hits or 'none'} | {funcs or '–'} |"
        )
    lines.append("")
    return "\n".join(lines)
