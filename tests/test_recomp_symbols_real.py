"""The guest symbol table of the real binary (#53, ADR 0030; local only: needs the
recompiler's ``work/recomp/functions.json``, which ``python -m recomp all`` writes).

Every discovered function has a symbol covering exactly its code range, every import stub
has one, the symbols do not overlap, and the names contract names most functions."""

from __future__ import annotations

import json

import pytest

from recomp import symbols

pytestmark = pytest.mark.real_assets


def test_real_symbol_table() -> None:
    if not symbols.FUNCTIONS_IN.is_file():
        pytest.skip("needs work/recomp/functions.json (python -m recomp all)")
    functions = json.loads(symbols.FUNCTIONS_IN.read_text(encoding="utf-8"))
    loader = None
    if symbols.LOADER_IN.is_file():
        loader = json.loads(symbols.LOADER_IN.read_text(encoding="utf-8"))
    names = symbols.function_names(symbols.NAMES_IN)
    table = symbols.build(functions, names, loader)
    funcs = [s for s in table if not s.name.endswith(" (stub)")]
    assert len(funcs) == len(functions["functions"]) == 2655
    by_ptr = {s.ptr: s for s in funcs}
    for f in functions["functions"]:
        s = by_ptr[int(f["ptr"], 16)]
        assert s.start == int(f["start"], 16)
        assert s.start + s.size == int(f["end"], 16)
    if loader is not None:
        stubs = sum(len(i["stubs"]) for i in loader["imports"])
        assert len(table) - len(funcs) == stubs
    for a, b in zip(table, table[1:], strict=False):
        assert a.start + a.size <= b.start, f"{a.line()} overlaps {b.line()}"
    assert by_ptr[0x27E0].name == "start" or by_ptr[0x27E0].name.startswith("f_")
    if names:
        named = sum(not s.name.startswith("f_") for s in funcs)
        assert named > len(funcs) // 2
    assert symbols.parse(symbols.render(table)) == table
