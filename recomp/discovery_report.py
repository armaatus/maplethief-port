"""The discovery report and its Ghidra cross-check (#32): ``work/recomp/discovery-report.md``.

``python -m recomp discover`` writes ``work/recomp/functions.json`` (the discovery itself,
schema in :mod:`recomp.discover`) and this report: the summary and timing, every function
discovery could not finish with its reason, the seeds and those outside
``LC_FUNCTION_STARTS``, the jump tables, the noreturn functions, and the comparison with
Ghidra's ``work/ghidra/functions.json`` when that export exists.

The Ghidra export is a cross-check only: nothing in ``functions.json`` comes from it, since
Ghidra's results depend on its analysis settings and host (#112). The comparison is per
function and per halfword (Ghidra's ``body`` ranges are split at the function pointer's
Thumb bit, so bytes are compared as halfwords). Each difference gets a reason:

Ghidra has code that discovery has not:
- ``after-noreturn``: bytes after a call that never returns (dead code or alignment padding;
  Ghidra does not know every noreturn function);
- ``data``: bytes discovery marks as a literal pool or jump table, decoded by Ghidra as code.

Discovery has code that Ghidra has not:
- ``sjlj-entry``: code only an internal entry reaches: the SjLj resume code and the
  landing pads its dispatch table jumps to. No branch from the function start reaches it,
  so Ghidra never disassembles it.

Anything else is ``unexplained`` and counts against the < 1% target of #32. Functions Ghidra
has and discovery has not are explained as import stubs, Ghidra's external block, or data
inside a discovered function (the three false positives of ANALYSIS.md).
"""

from __future__ import annotations

import json
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from mtport import ghidra_common as gc
from recomp import REPO
from recomp.decode import decode_thumb
from recomp.discover import SEED_SOURCES, STRONG_SOURCES, Discovery, Function

FUNCTIONS_OUT = REPO / "work" / "recomp" / "functions.json"
REPORT_OUT = REPO / "work" / "recomp" / "discovery-report.md"
GHIDRA_FUNCTIONS = REPO / "work" / "ghidra" / "functions.json"
UNEXPLAINED_LIMIT = 0.01  # #32: < 1% of the functions may differ from Ghidra unexplained

# Functions #112 found Ghidra's switch analysis cutting short on slower hosts.
KEY_FUNCTIONS = (
    (0x3ED19, "CDataMgr::OnServerPacket", "nested tbb/tbh, 29,427 bytes"),
    (0x87501, "CStateGame::Paint", "large switch"),
    (0x8D4C9, "CStateGame::OnKeyPressed", "large switch"),
)


@dataclass
class FunctionDiff:
    """Halfword differences of one function: reason -> halfwords."""

    ptr: int
    ghidra_bytes: int
    our_bytes: int
    ghidra_only: Counter = field(default_factory=Counter)
    ours_only: Counter = field(default_factory=Counter)
    first_unexplained: int | None = None

    @property
    def unexplained(self) -> bool:
        return bool(self.ghidra_only["unexplained"] or self.ours_only["unexplained"])


@dataclass
class GhidraDiff:
    compared: int
    diffs: list[FunctionDiff]
    only_ghidra: list[tuple[int, str]]  # Ghidra functions discovery lacks, with the reason
    only_ours: list[int]
    noreturn_only_ours: list[int]
    noreturn_only_ghidra: list[int]
    # Jump tables against Ghidra's COMPUTED_JUMP xrefs (None without xrefs.json):
    # tables whose targets agree, and (function, site, only discovery, only Ghidra) for the rest.
    tables_agree: int | None = None
    tables_differ: list[tuple[int, int, list[int], list[int]]] = field(default_factory=list)

    @property
    def unexplained(self) -> list[FunctionDiff]:
        return [d for d in self.diffs if d.unexplained]

    def unexplained_share(self) -> float:
        """Functions with an unexplained difference (body, extra function or jump table
        targets), as a share of the functions compared."""
        bad = {d.ptr for d in self.unexplained}
        bad |= {p for p, r in self.only_ghidra if r == "unexplained"}
        bad |= {fn for fn, _, _, _ in self.tables_differ}
        return len(bad) / max(self.compared, 1)


def _halfwords(ranges) -> set[int]:
    out: set[int] = set()
    for lo, hi in ranges:
        out.update(range(lo & ~1, hi, 2))
    return out


def _code_halfwords(fn: Function) -> set[int]:
    out: set[int] = set()
    for insn in fn.insns.values():
        out.update(range(insn.address, insn.end, 2))
    return out


def _data_halfwords(fn: Function) -> set[int]:
    out: set[int] = set()
    for a, (size, _) in fn.data.items():
        out.update(range(a & ~1, a + size, 2))
    return out


def _after_noreturn(fn: Function, h: int, code: set[int]) -> bool:
    """Whether halfword ``h`` lies in a gap that starts right after a noreturn call."""
    ends = {fn.insns[a].end: a for a in fn.noreturn_calls}
    a = h
    while a > fn.start and a not in code:
        if a in ends:
            return True
        a -= 2
    return False


def compare(disc: Discovery, export: dict, image) -> GhidraDiff:
    """Compare discovery with a Ghidra ``functions.json`` export (already loaded)."""
    ghidra = {int(f["address"], 16): f for f in export["functions"]}
    diffs: list[FunctionDiff] = []
    compared = 0
    for ptr, fn in sorted(disc.functions.items()):
        g = ghidra.get(ptr)
        if g is None:
            continue
        compared += 1
        G = _halfwords((int(lo, 16), int(hi, 16)) for lo, hi in g["body"])
        code = _code_halfwords(fn)
        data = _data_halfwords(fn)
        d = FunctionDiff(ptr, 2 * len(G), 2 * len(code))
        for h in sorted(G - code):
            if h in data:
                why = "data"
            elif _after_noreturn(fn, h, code):
                why = "after-noreturn"
            else:
                why = "unexplained"
            d.ghidra_only[why] += 1
            if why == "unexplained" and d.first_unexplained is None:
                d.first_unexplained = h
        entries = set(fn.entries)
        reached = _entry_code(fn, entries)
        for h in sorted(code - G):
            why = "sjlj-entry" if h in reached else "unexplained"
            d.ours_only[why] += 1
            if why == "unexplained" and d.first_unexplained is None:
                d.first_unexplained = h
        if d.ghidra_only or d.ours_only:
            diffs.append(d)
    only_ghidra = []
    text = image.text
    for ptr, g in sorted(ghidra.items()):
        if ptr in disc.functions:
            continue
        addr = ptr & ~1
        section = g.get("section") or ""
        if image.import_at(addr) is not None and section == "__TEXT,__symbolstub1":
            why = "import stub (imports, #36)"
        elif g.get("block") == "EXTERNAL" or not section:
            why = "Ghidra external symbol"
        elif text.contains(addr):
            owner = disc.containing(addr)
            if owner is not None and addr in _data_halfwords(owner):
                why = f"data of {_h(owner.ptr)} (literal pool or table)"
            elif owner is not None and addr not in _code_halfwords(owner):
                why = f"bytes inside {_h(owner.ptr)} that no path reaches"
            else:
                why = "unexplained"
        else:
            why = "unexplained"
        only_ghidra.append((ptr, why))
    only_ours = [p for p in sorted(disc.functions) if p not in ghidra]
    ours_nr = {p for p, f in disc.functions.items() if f.noreturn}
    ghidra_nr = {p for p, g in ghidra.items() if g.get("no_return") and p in disc.functions}
    return GhidraDiff(
        compared, diffs, only_ghidra, only_ours, sorted(ours_nr - ghidra_nr),
        sorted(ghidra_nr - ours_nr),
    )  # fmt: skip


def compare_tables(disc: Discovery, xrefs: dict, diff: GhidraDiff) -> None:
    """Check every jump table's targets against Ghidra's ``COMPUTED_JUMP`` xrefs from its
    site (Ghidra's own switch recovery), filling ``diff.tables_*``."""
    tables = {s: t for f in disc.functions.values() for s, t in f.tables.items()}
    ghidra: dict[int, set[int]] = {s: set() for s in tables}
    for x in xrefs["xrefs"]:
        site = int(x["from"], 16)
        if site in ghidra and x["type"] == "COMPUTED_JUMP" and x["to"].startswith("0x"):
            ghidra[site].add(int(x["to"], 16))
    diff.tables_agree = 0
    for site, table in sorted(tables.items()):
        ours = set(table.targets)
        if ours == ghidra[site]:
            diff.tables_agree += 1
        else:
            only_ours, only_ghidra = sorted(ours - ghidra[site]), sorted(ghidra[site] - ours)
            diff.tables_differ.append((disc.containing(site).ptr, site, only_ours, only_ghidra))


PADDING = frozenset({0xBF00, 0x46C0, 0x0000})  # nop, mov r8, r8, zero: alignment


def gaps(fn: Function, code: bytes, base: int) -> list[tuple[int, int, str]]:
    """``(start, end, reason)`` of every run of bytes in ``[start, end)`` that is neither an
    instruction nor data: ``padding`` (nop / ``mov r8, r8`` / zero halfwords),
    ``after-noreturn`` (follows a noreturn call), ``addressed-data`` (a PC-relative address
    computed in the function points into it), else ``unreached`` (dead code: no path and
    no pointer leads there)."""
    if not fn.thumb:
        return []
    covered: set[int] = set()
    for insn in fn.insns.values():
        covered.update(range(insn.address, insn.end, 2))
    covered |= _data_halfwords(fn)
    ends = {fn.insns[a].end for a in fn.noreturn_calls}
    out = []
    h = fn.start
    while h < fn.end:
        if h in covered:
            h += 2
            continue
        lo = h
        while h < fn.end and h not in covered:
            h += 2
        halfwords = [
            int.from_bytes(code[x - base : x - base + 2], "little") for x in range(lo, h, 2)
        ]
        if all(x in PADDING for x in halfwords):
            why = "padding"
        elif lo in ends:
            why = "after-noreturn"
        elif any(lo <= r < h for r in fn.data_refs.values()):
            why = "addressed-data"
        else:
            why = "unreached"
        out.append((lo, h, why))
    return out


def _entry_code(fn: Function, entries: set[int]) -> set[int]:
    """Halfwords of the code only the internal entries reach: everything the function
    start does not reach along successor edges (landing pads and their dispatch)."""
    del entries  # the complement of what the start reaches
    seen: set[int] = set()
    work = [fn.start]
    while work:
        a = work.pop()
        if a in seen or a not in fn.insns:
            continue
        seen.add(a)
        work.extend(fn.succ.get(a, ()))
    out: set[int] = set()
    for a, insn in fn.insns.items():
        if a not in seen:
            out.update(range(a, insn.end, 2))
    return out


# -- markdown ---------------------------------------------------------------------------------


def _h(v: int) -> str:
    return f"0x{v:X}"


def _names() -> dict[int, str]:
    """Function names from the RE names files, if present (for readability only)."""
    out: dict[int, str] = {}
    for path in sorted((REPO / "work" / "re" / "names").glob("*.json")):
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        for e in doc.get("names", []):
            if e.get("kind") == "function":
                out.setdefault(int(e["address"], 16) & ~1, e["name"])
    return out


def render(disc: Discovery, image, ghidra: GhidraDiff | None) -> str:
    names = _names()

    def name(ptr: int) -> str:
        n = names.get(ptr & ~1)
        return f"`{_h(ptr)}` {n}" if n else f"`{_h(ptr)}`"

    c = disc.counts()
    lines = ["# Function discovery report (#32)", ""]
    lines += [
        "Generated by `python -m recomp discover` from the armv7 slice; git-ignored, never",
        "committed. The discovery itself is `work/recomp/functions.json` (schema in",
        "`recomp/discover.py`); Ghidra is a cross-check only.",
        "",
        "## Summary",
        "",
        "| | |",
        "|---|---|",
    ]
    for key, value in c.items():
        lines.append(f"| {key.replace('_', ' ')} | {value:,} |")
    lines.append(f"| discovery time | {disc.seconds:.2f} s |")
    lines.append("")

    lines += ["## Functions discovery could not finish", ""]
    failures = disc.failures()
    if not failures:
        lines.append("None.")
    for ptr, problems in failures:
        for p in problems:
            lines.append(f"- {name(ptr)}: {p}")
    lines.append("")

    lines += ["## Seeds", "", "| source | seeds | outside LC_FUNCTION_STARTS |", "|---|---|---|"]
    outside = disc.outside_starts()
    for src in SEED_SOURCES:
        n = sum(src in s for s in disc.seeds.values())
        o = sum(src in s for s in outside.values())
        lines.append(f"| {src} | {n:,} | {o} |")
    lines.append("")
    lines.append(
        "Seeds from `" + "`, `".join(sorted(STRONG_SOURCES)) + "` outside the starts would be"
        " discovered as functions; `data` (odd words in data sections that point into"
        " `__text`) and `code` (addresses computed in code) only count when they are starts."
    )
    lines.append("")
    if outside:
        lines += ["| seed | sources | where | verdict |", "|---|---|---|---|"]
        for ptr, srcs in outside.items():
            owner = disc.containing(ptr & ~1)
            if owner is None:
                where = "outside every function"
            elif (ptr & ~1) in owner.insns:
                where = f"instruction inside {_h(owner.ptr)}"
            elif any(a <= (ptr & ~1) < a + s for a, (s, _) in owner.data.items()):
                where = f"data of {_h(owner.ptr)}"
            else:
                where = f"inside {_h(owner.ptr)}, not an instruction start"
            verdict = "discovered as a function" if srcs & STRONG_SOURCES else "not a code pointer"
            lines.append(f"| `{_h(ptr)}` | {', '.join(sorted(srcs))} | {where} | {verdict} |")
        lines.append("")

    lines += ["## Jump tables", ""]
    tables = [t for f in disc.functions.values() for t in f.tables.values()]
    kinds = Counter(t.kind for t in tables)
    lines.append(
        f"{len(tables)} tables ({', '.join(f'{n} {k}' for k, n in sorted(kinds.items()))}), "
        f"{sum(len(t.targets) for t in tables):,} distinct targets, all bounded by a `cmp`."
    )
    lines += ["", "| site | function | kind | entries | bound (cmp) | targets |"]
    lines.append("|---|---|---|---|---|---|")
    for t in sorted(tables, key=lambda t: t.site):
        owner = disc.containing(t.site)
        fname = name(owner.ptr) if owner else "?"
        lines.append(
            f"| `{_h(t.site)}` | {fname} | {t.kind} | {t.count} | `{_h(t.bound)}` "
            f"| {len(t.targets)} |"
        )
    lines.append("")

    lines += ["## Noreturn", ""]
    for addr, why in sorted(disc.noreturn.items()):
        fn = disc.containing(addr)
        label = name(fn.ptr) if fn is not None and fn.start == addr else f"`{_h(addr)}`"
        lines.append(f"- {label}: {why}")
    lines.append("")

    lines += ["## Bytes no path reaches", ""]
    all_gaps = [
        (f.ptr, g) for f in disc.functions.values() for g in gaps(f, image.code, image.text_base)
    ]
    by_reason: Counter = Counter()
    sizes: Counter = Counter()
    for _, (lo, hi, why) in all_gaps:
        by_reason[why] += 1
        sizes[why] += hi - lo
    lines.append(
        "Inside the functions' bounds, bytes that are neither an instruction discovery reached"
        " nor data it marked:"
    )
    lines += ["", "| reason | runs | bytes |", "|---|---|---|"]
    for why, n in sorted(by_reason.items()):
        lines.append(f"| {why} | {n} | {sizes[why]:,} |")
    lines.append("")
    for ptr, (lo, hi, why) in sorted(all_gaps, key=lambda g: g[1][0]):
        if why == "unreached":
            insn = decode_thumb(image.code, lo - image.text_base, lo)
            lines.append(f"- {name(ptr)}: `{_h(lo)}`-`{_h(hi)}` ({insn.text()}, ...)")
    lines.append("")

    lines += ["## Ghidra cross-check", ""]
    if ghidra is None:
        lines.append("`work/ghidra/functions.json` not found (run `scripts/re-analyze`).")
        return "\n".join(lines) + "\n"
    unexplained = ghidra.unexplained
    share = ghidra.unexplained_share()
    ok = "within" if share < UNEXPLAINED_LIMIT else "**over**"
    lines.append(
        f"{ghidra.compared:,} functions compared; {len(ghidra.diffs)} differ, "
        f"{len(unexplained)} unexplained ({share:.2%}, {ok} the 1% target)."
    )
    lines.append("")
    reasons_g: Counter = Counter()
    reasons_o: Counter = Counter()
    fn_g: Counter = Counter()
    fn_o: Counter = Counter()
    for d in ghidra.diffs:
        reasons_g.update(d.ghidra_only)
        reasons_o.update(d.ours_only)
        fn_g.update(set(d.ghidra_only))
        fn_o.update(set(d.ours_only))
    lines += ["| difference | reason | functions | bytes |", "|---|---|---|---|"]
    for why, n in sorted(reasons_g.items()):
        lines.append(f"| Ghidra has code, discovery not | {why} | {fn_g[why]} | {2 * n:,} |")
    for why, n in sorted(reasons_o.items()):
        lines.append(f"| discovery has code, Ghidra not | {why} | {fn_o[why]} | {2 * n:,} |")
    lines.append("")
    if ghidra.tables_agree is not None:
        n = ghidra.tables_agree + len(ghidra.tables_differ)
        lines.append(
            f"Jump tables against Ghidra's `COMPUTED_JUMP` xrefs (`work/ghidra/xrefs.json`): "
            f"{ghidra.tables_agree} of {n} have exactly the same targets."
        )
        for _, site, ours, theirs in ghidra.tables_differ:
            lines.append(
                f"- `{_h(site)}`: discovery only {', '.join(map(_h, ours)) or '-'}; "
                f"Ghidra only {', '.join(map(_h, theirs)) or '-'}"
            )
        lines.append("")
    lines += ["### Functions #112 found cut short on slower hosts", ""]
    lines += ["| function | what | discovery bytes | Ghidra bytes | difference |"]
    lines.append("|---|---|---|---|---|")
    by_ptr = {d.ptr: d for d in ghidra.diffs}
    for ptr, label, what in KEY_FUNCTIONS:
        fn = disc.functions.get(ptr)
        if fn is None:
            lines.append(f"| `{_h(ptr)}` {label} | {what} | - | - | not discovered |")
            continue
        d = by_ptr.get(ptr)
        ours = 2 * len(_code_halfwords(fn))
        theirs = d.ghidra_bytes if d else ours
        what_diff = "none"
        if d:
            parts = [f"Ghidra +{2 * n} {w}" for w, n in d.ghidra_only.items()]
            parts += [f"discovery +{2 * n} {w}" for w, n in d.ours_only.items()]
            what_diff = ", ".join(parts)
        lines.append(f"| `{_h(ptr)}` {label} | {what} | {ours:,} | {theirs:,} | {what_diff} |")
    lines.append("")
    lines += ["### Unexplained differences", ""]
    if not unexplained:
        lines.append("None.")
    for d in unexplained:
        lines.append(
            f"- {name(d.ptr)}: Ghidra {d.ghidra_bytes} B, discovery {d.our_bytes} B, first at "
            f"`{_h(d.first_unexplained or d.ptr)}`"
        )
    lines.append("")
    lines += ["### Functions only Ghidra has", ""]
    reasons = Counter(why for _, why in ghidra.only_ghidra)
    for why, n in sorted(reasons.items()):
        lines.append(f"- {n} x {why}")
    for ptr, why in ghidra.only_ghidra:
        if not why.startswith(("import stub", "Ghidra external")):
            lines.append(f"  - `{_h(ptr)}`: {why}")
    if ghidra.only_ours:
        lines.append("")
        lines.append(
            "Functions only discovery has: " + ", ".join(f"`{_h(p)}`" for p in ghidra.only_ours)
        )
    lines.append("")
    lines += ["### Noreturn functions", ""]
    lines.append(
        "Discovery only: "
        + (", ".join(name(p) for p in ghidra.noreturn_only_ours) or "none")
        + ". Ghidra only: "
        + (", ".join(name(p) for p in ghidra.noreturn_only_ghidra) or "none")
        + "."
    )
    lines.append("")
    lines += ["### Every function that differs", ""]
    lines += ["| function | Ghidra B | discovery B | reasons |", "|---|---|---|---|"]
    for d in ghidra.diffs:
        parts = [f"G+{2 * n} {w}" for w, n in sorted(d.ghidra_only.items())]
        parts += [f"D+{2 * n} {w}" for w, n in sorted(d.ours_only.items())]
        lines.append(f"| {name(d.ptr)} | {d.ghidra_bytes} | {d.our_bytes} | {', '.join(parts)} |")
    return "\n".join(lines) + "\n"


def write(
    disc: Discovery,
    image,
    source: dict,
    functions_out: Path = FUNCTIONS_OUT,
    report_out: Path = REPORT_OUT,
    ghidra_path: Path = GHIDRA_FUNCTIONS,
) -> GhidraDiff | None:
    """Write ``functions.json`` and the report; returns the Ghidra comparison (None without
    an export)."""
    gc.write_text(functions_out, disc.to_json(source))
    ghidra = None
    if ghidra_path.is_file():
        ghidra = compare(disc, gc.load_export(ghidra_path), image)
        xrefs = ghidra_path.with_name("xrefs.json")
        if xrefs.is_file():
            compare_tables(disc, gc.load_export(xrefs), ghidra)
    gc.write_text(report_out, render(disc, image, ghidra))
    return ghidra


__all__ = [
    "FunctionDiff", "GhidraDiff", "compare", "compare_tables", "gaps", "render", "write",
]  # fmt: skip
