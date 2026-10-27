"""The guest symbol table of the runtime's diagnostics (ADR 0030, #53).

The runtime names guest addresses in its HLE trace, its unimplemented report and its crash
reports from a compact text table, which it loads only from a configured path
(``MAPLETHIEF_SYMBOLS``) and never from ``work/`` by itself. This module writes it from the
recompiler's outputs:

* ``work/recomp/functions.json`` (``python -m recomp discover``): every function's pointer
  and code range, the ``f_XXXXXXXX`` name of its generated C function;
* ``work/re/names/*.json`` (the names contract, ``mtport.names``, #8/#9/#12), optional: the
  name a function gets instead, e.g. ``CStateGame::Update`` or ``-[EAGLView layoutSubviews]``;
* ``work/recomp/loader.json`` (``python -m recomp load``), optional: each import stub of
  ``__symbolstub1``, named ``_memcpy (stub)``.

Format (parsed by ``mt_symbols_load_text``, ``runtime/src/diag/symbols.cpp``)::

    # maplethief symbols 1
    000027E0 00000028 start
    0000280D 0000006C f_0000280D
    000F0C90 00000004 _AudioServicesPlaySystemSound (stub)

one line per symbol, sorted by address: the function pointer (Thumb bit set for Thumb code,
as everywhere in this project), the size in bytes, and the name (the rest of the line).

The file is derived from the IPA: it lives in ``work/`` or the build tree, is never committed
and never part of a release kit (ADR 0024). The build writes ``<build dir>/maplethief.sym``
when the recompiler's outputs exist (``cmake/GeneratedCode.cmake``).

Command line: ``python -m recomp symbols [--functions F] [--names DIR] [--loader F] [--out F]``.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from recomp import REPO

HEADER = "# maplethief symbols 1"
FUNCTIONS_IN = REPO / "work" / "recomp" / "functions.json"
NAMES_IN = REPO / "work" / "re" / "names"
LOADER_IN = REPO / "work" / "recomp" / "loader.json"
SYMBOLS_OUT = REPO / "work" / "recomp" / "symbols.txt"
STUB_SECTION = "__symbolstub1"
STUB_SIZE = 4  # an armv7 __symbolstub1 stub: one ldr pc, [pc, ...]


@dataclass(frozen=True)
class Symbol:
    ptr: int  # function pointer (Thumb bit for Thumb code)
    size: int  # bytes
    name: str

    @property
    def start(self) -> int:
        return self.ptr & ~1

    def line(self) -> str:
        return f"{self.ptr:08X} {self.size:08X} {self.name}"


def _int(text: str | int) -> int:
    return text if isinstance(text, int) else int(text, 16)


def _clean(name: str) -> str:
    """One line, no surrounding blanks: the runtime reads the name to the end of the line."""
    return " ".join(name.split())


def function_names(names_dir: Path | None) -> dict[int, str]:
    """Code address (Thumb bit cleared) -> name, for every ``function`` entry of the names
    contract; empty if there is no directory."""
    if names_dir is None or not names_dir.is_dir():
        return {}
    from mtport import names

    return {
        m.entry.location: m.entry.name
        for m in names.load_dir(names_dir)
        if m.entry.kind == "function"
    }


def function_symbols(functions: dict, names: dict[int, str]) -> list[Symbol]:
    """One symbol per discovered function: its contract name if it has one, else the name of
    its generated C function."""
    out = []
    for f in functions["functions"]:
        ptr = _int(f["ptr"])
        start, end = _int(f["start"]), _int(f["end"])
        if end <= start:
            continue
        out.append(Symbol(ptr, end - start, _clean(names.get(ptr & ~1) or f"f_{ptr:08X}")))
    return out


def stub_symbols(loader: dict) -> list[Symbol]:
    """One symbol per import stub, sized up to the next stub or the section end."""
    stubs = sorted(
        (_int(a), imp["name"]) for imp in loader.get("imports", []) for a in imp.get("stubs", [])
    )
    section_end = None
    for s in loader.get("sections", []):
        if s.get("name") == STUB_SECTION:
            section_end = _int(s["addr"]) + int(s["size"])
    out = []
    for i, (addr, name) in enumerate(stubs):
        limit = stubs[i + 1][0] if i + 1 < len(stubs) else section_end
        size = limit - addr if limit is not None and limit > addr else STUB_SIZE
        out.append(Symbol(addr, min(size, STUB_SIZE), _clean(f"{name} (stub)")))
    return out


def build(functions: dict, names: dict[int, str], loader: dict | None = None) -> list[Symbol]:
    """Every symbol, sorted by address."""
    symbols = function_symbols(functions, names)
    if loader is not None:
        symbols += stub_symbols(loader)
    return sorted(symbols, key=lambda s: (s.start, s.name))


def render(symbols: list[Symbol]) -> str:
    return "\n".join([HEADER, *(s.line() for s in symbols)]) + "\n"


def parse(text: str) -> list[Symbol]:
    """The reverse of :func:`render` (for tests and tools); malformed lines raise."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        ptr, size, name = line.split(maxsplit=2)
        out.append(Symbol(int(ptr, 16), int(size, 16), name))
    return out


def write(
    out: Path,
    functions_path: Path = FUNCTIONS_IN,
    names_dir: Path | None = NAMES_IN,
    loader_path: Path | None = LOADER_IN,
) -> list[Symbol]:
    """Builds the table from the files and writes it to ``out`` if it changed (so the build
    does not see a new file every time)."""
    functions = json.loads(functions_path.read_text(encoding="utf-8"))
    loader = None
    if loader_path is not None and loader_path.is_file():
        loader = json.loads(loader_path.read_text(encoding="utf-8"))
    symbols = build(functions, function_names(names_dir), loader)
    text = render(symbols)
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.is_file() or out.read_text(encoding="utf-8") != text:
        out.write_text(text, encoding="utf-8", newline="\n")
    return symbols
