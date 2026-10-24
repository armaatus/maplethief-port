"""Doc coverage of import and ``objc_msgSend`` sites (shared by #23 network and #21 timing).

A research doc in ``docs/re/`` that claims to account for every use of some imports and
messages is checked the same way:

* :func:`collect`: every code site of the given imports (from #11's ``work/re/imports.json``,
  ``imports[].sites``) and every send of the given selectors (``sends[]``), each with the #10
  tag of its function (``work/re/function-tags.json``).
* :func:`check_doc`: every collected site must be named in the doc by its address, except
  that a site inside a ``lib:chartboost`` function may instead be covered by that function's
  address: stubbing Chartboost at its entry points makes those unreachable.

The input documents are generated from untrusted data and only read as JSON.
"""

from __future__ import annotations

import re
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from mtport import ghidra_common as gc
from mtport import libtags
from mtport.imports import SCHEMA_VERSION as IMPORTS_SCHEMA_VERSION

IMPORTS_JSON = "work/re/imports.json"
TAGS_JSON = "work/re/function-tags.json"

CHARTBOOST_TAG = "lib:chartboost"


@dataclass(frozen=True)
class Site:
    """One code site of an import or message."""

    what: str  # import name, or "-selector" / "+selector" for a send
    site: int  # instruction address
    function: int  # containing function, as a function pointer
    function_name: str
    tag: str  # #10 tag of the function ("engine", "app-objc", "lib:chartboost", ...)

    @property
    def chartboost(self) -> bool:
        return self.tag == CHARTBOOST_TAG


def _int(text: str) -> int:
    return int(text, 16)


def collect(
    imports_doc: dict, tags_doc: dict, imports: Iterable[str], selectors: Iterable[str]
) -> list[Site]:
    """Every site of ``imports`` and ``selectors``, sorted by address."""
    tags = {_int(f["address"]): f["tag"] for f in tags_doc["functions"]}
    names = {_int(f["function"]): f["name"] for f in imports_doc.get("functions", [])}
    wanted = set(imports)
    out: set[Site] = set()
    for row in imports_doc["imports"]:
        if row["name"] not in wanted:
            continue
        for site, function, _kind in row["sites"]:
            f = _int(function)
            out.add(Site(row["name"], _int(site), f, names.get(f, ""), tags.get(f, "unknown")))
    wanted_selectors = set(selectors)
    for send in imports_doc["sends"]:
        if send["selector"] not in wanted_selectors:
            continue
        f = _int(send["function"])
        prefix = "+" if send.get("class_method") else "-"
        what = prefix + send["selector"]
        out.add(Site(what, _int(send["site"]), f, names.get(f, ""), tags.get(f, "unknown")))
    return sorted(out, key=lambda s: (s.site, s.what))


_HEX = re.compile(r"\b0x([0-9A-Fa-f]{1,8})\b")


def doc_addresses(text: str) -> set[int]:
    """Every hex number written in the doc."""
    return {int(m.group(1), 16) for m in _HEX.finditer(text)}


def check_doc(text: str, sites: Iterable[Site]) -> list[Site]:
    """The sites the doc does not cover (see the module docstring for the rule)."""
    seen = doc_addresses(text)
    missing = []
    for s in sites:
        if s.site in seen:
            continue
        if s.chartboost and s.function in seen:
            continue
        missing.append(s)
    return missing


def site_rows(sites: Iterable[Site]) -> list[str]:
    """Markdown rows ``| site | function | what | tag |`` (a starting point for the doc)."""
    rows = []
    for s in sites:
        name = s.function_name or f"0x{s.function:X}"
        rows.append(f"| `0x{s.site:X}` | `0x{s.function:X}` {name} | `{s.what}` | {s.tag} |")
    return rows


# Generated inputs: schema (key, version) and the step that writes each.
INPUTS = {
    IMPORTS_JSON: ("schema_version", IMPORTS_SCHEMA_VERSION, "python3 -m mtport.imports generate"),
    TAGS_JSON: ("schema", libtags.SCHEMA_VERSION, libtags.GENERATE_STEP),
}


def load_json(repo: Path, rel: str) -> dict:
    """``repo / rel`` through :func:`mtport.ghidra_common.load_json` (schema checked for the
    :data:`INPUTS`)."""
    key, schema, step = INPUTS.get(rel, ("schema", None, ""))
    return gc.load_json(repo / rel, schema, key=key, producer=step)


def load_sites(repo: Path, imports: Iterable[str], selectors: Iterable[str]) -> list[Site]:
    """:func:`collect` over the generated documents in ``repo``."""
    return collect(load_json(repo, IMPORTS_JSON), load_json(repo, TAGS_JSON), imports, selectors)
