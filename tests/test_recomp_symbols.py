"""The guest symbol table of the runtime's diagnostics (``recomp/symbols.py``, ADR 0030, #53),
on synthetic inputs: names from the names contract win over ``f_XXXXXXXX``, stubs are sized
by their neighbours, the text round-trips and the runtime's parser reads it."""

from __future__ import annotations

import json
from pathlib import Path

from recomp import symbols
from recomp.__main__ import main

FUNCTIONS = {
    "functions": [
        {"ptr": "0x280D", "start": "0x280C", "end": "0x2878"},
        {"ptr": "0x27E0", "start": "0x27E0", "end": "0x280C"},
        {"ptr": "0x2879", "start": "0x2878", "end": "0x28AC"},
        {"ptr": "0x3001", "start": "0x3000", "end": "0x3000"},  # empty: left out
    ]
}
LOADER = {
    "sections": [{"name": "__symbolstub1", "addr": "0xF0C90", "size": 12}],
    "imports": [
        {"name": "_memcpy", "stubs": ["0xF0C94"]},
        {"name": "_AudioServicesPlaySystemSound", "stubs": ["0xF0C90"]},
        {"name": "_OBJC_CLASS_$_NSObject", "stubs": []},
        {"name": "_free", "stubs": ["0xF0C98"]},
    ],
}


def write_names(directory: Path) -> None:
    directory.mkdir()
    (directory / "objc.json").write_text(
        json.dumps(
            {
                "source": "objc",
                "names": [
                    {
                        "address": "0x2879",
                        "name": "-[MapleThiefAppDelegate setWindow:]",
                        "kind": "function",
                    },
                    {"address": "0xF1000", "name": "someData", "kind": "data"},
                ],
            }
        ),
        encoding="utf-8",
    )
    (directory / "flow.json").write_text(
        json.dumps(
            {"source": "flow", "names": [{"address": "0x280D", "name": "main", "kind": "function"}]}
        ),
        encoding="utf-8",
    )


def test_build_names_functions_and_stubs(tmp_path: Path) -> None:
    write_names(tmp_path / "names")
    names = symbols.function_names(tmp_path / "names")
    assert names == {0x2878: "-[MapleThiefAppDelegate setWindow:]", 0x280C: "main"}
    table = symbols.build(FUNCTIONS, names, LOADER)
    assert [s.line() for s in table] == [
        "000027E0 0000002C f_000027E0",
        "0000280D 0000006C main",
        "00002879 00000034 -[MapleThiefAppDelegate setWindow:]",
        "000F0C90 00000004 _AudioServicesPlaySystemSound (stub)",
        "000F0C94 00000004 _memcpy (stub)",
        "000F0C98 00000004 _free (stub)",
    ]
    text = symbols.render(table)
    assert text.startswith(symbols.HEADER + "\n")
    assert symbols.parse(text) == table


def test_names_are_one_line() -> None:
    table = symbols.function_symbols(FUNCTIONS, {0x27E0: "  odd\tname\nwith lines "})
    assert table[1].name == "odd name with lines"


def test_without_names_or_loader(tmp_path: Path) -> None:
    assert symbols.function_names(tmp_path / "missing") == {}
    assert symbols.function_names(None) == {}
    table = symbols.build(FUNCTIONS, {})
    assert [s.name for s in table] == ["f_000027E0", "f_0000280D", "f_00002879"]


def test_command_line_writes_only_on_change(tmp_path: Path, capsys) -> None:
    functions = tmp_path / "functions.json"
    functions.write_text(json.dumps(FUNCTIONS), encoding="utf-8")
    loader = tmp_path / "loader.json"
    loader.write_text(json.dumps(LOADER), encoding="utf-8")
    write_names(tmp_path / "names")
    out = tmp_path / "build" / "maplethief.sym"
    argv = ["symbols", "--functions", str(functions), "--names", str(tmp_path / "names")]
    argv += ["--loader", str(loader), "--out", str(out)]
    assert main(argv) == 0
    assert "6 symbols (2 named)" in capsys.readouterr().out
    first = out.stat().st_mtime_ns
    text = out.read_text(encoding="utf-8")
    assert len(symbols.parse(text)) == 6
    assert main(argv) == 0
    assert out.stat().st_mtime_ns == first
    assert main(["symbols", "--functions", str(tmp_path / "nope.json")]) == 1
