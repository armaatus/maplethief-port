"""The names contract: ``work/re/names/*.json`` files that name addresses in the armv7 slice.

Analyses such as ObjC metadata (#8), RTTI/vtables (#9) and program flow (#12) each write one
file; ``tools/ghidra/apply-names`` applies all of them to the Ghidra project
(``tools/ghidra/scripts/apply_names.py``). This module is the Ghidra-free half: the schema,
validation, merging across files, and the mapping of a contract name to a legal Ghidra
symbol. Schema (one file)::

    {"source": "objc",                       # who wrote it; non-empty
     "names": [
       {"address": "0x2A3C1",                # armv7 VM address, hex, at most 32 bits
        "name": "-[Class sel:]",             # the name as the analysis spells it
        "kind": "function",                  # "function" | "data" | "label"
        "comment": "IMP from __objc_const"}  # optional
     ]}

Addresses follow CLAUDE.md: a ``function`` address is a function pointer, so its Thumb bit
picks the mode (``0x2A3C1`` = Thumb function at 0x2A3C0, ``0x27E0`` = ARM function). A
``label`` names a code location, and its Thumb bit, if set, is ignored. A ``data`` address is
used exactly as written.

Merging (:func:`load_dir`): files are read in sorted file-name order. Two entries for the
same location (Thumb bit cleared for code) must agree on kind and name; if they differ, that
is an error that names both files. Comments of agreeing entries are kept in file order,
without duplicates.

Ghidra symbols (:func:`ghidra_symbol`): Ghidra rejects names that are empty, longer than
2,000 characters, or contain whitespace, control, format, combining or unassigned code points,
and reserves its default names (``FUN_0002a3c0``, ``LAB_…``, ``DAT_…``)::

    -[Class sel:with:]     ObjC method: kept whole in the global namespace, space -> "_"
                           Ghidra symbol "-[Class_sel:with:]"
    +[Class(Cat) new]      the same: "+[Class(Cat)_new]"
    Class::vf3             C++: split at top-level "::" into namespaces
                           namespace "Class", symbol "vf3"
    ns::Tpl<a::b>::f       only top-level "::" splits: namespaces "ns", "Tpl<a::b>"; "f"
    Class::operator<       nothing after "operator" is split: namespace "Class", "operator<"
    anything else          one global symbol; each run of invalid characters -> "_"

Sanitizing can merge distinct names, so ``apply_names.py`` also records the exact contract
name for each address in the program (string property map ``mtport.name``), and the export
reports that name. Re-importing and re-applying reproduces the same names.

Command line::

    python3 -m mtport.names check [DIR]     # validate DIR (default work/re/names)
    python3 -m mtport.names show  [DIR]     # merged entries + their Ghidra symbols
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path

KINDS = ("function", "data", "label")
NAMES_DIR = "work/re/names"
MAX_NAME = 2000  # Ghidra's SymbolUtilities.MAX_SYMBOL_NAME_LENGTH

_ADDRESS = re.compile(r"0x[0-9A-Fa-f]{1,8}")
_RESERVED = re.compile(r"(?:SUB|LAB|DAT|UNK|EXT|FUN|OFF)_[0-9A-Fa-f]+")
_OBJC = re.compile(r"[+-]\[[^\s\[\]]+(?:\([^\s()]*\))? [^\s\[\]]+\]")
# Unicode categories Ghidra refuses in symbol names (SymbolUtilities.isInvalidCodePoint).
_INVALID_CATEGORIES = frozenset({"Zs", "Mc", "Cc", "Me", "Cf", "Zl", "Mn", "Zp", "Co", "Cs", "Cn"})


class NamesError(ValueError):
    """One or more names files are malformed or contradict each other."""

    def __init__(self, problems: list[str]) -> None:
        super().__init__("\n".join(problems))
        self.problems = problems


@dataclass(frozen=True)
class NameEntry:
    """One validated entry. ``address`` is as written (Thumb bit kept)."""

    address: int
    name: str
    kind: str
    comment: str | None = None

    @property
    def location(self) -> int:
        """The address the name is attached to: Thumb bit cleared for code."""
        return self.address & ~1 if self.kind in ("function", "label") else self.address

    @property
    def thumb(self) -> bool:
        """For a ``function``: True if it is Thumb code."""
        return self.kind == "function" and bool(self.address & 1)


@dataclass
class MergedName:
    """All agreeing entries for one location, merged."""

    entry: NameEntry
    sources: list[str] = field(default_factory=list)
    comments: list[str] = field(default_factory=list)

    @property
    def comment(self) -> str | None:
        return "\n".join(self.comments) if self.comments else None


def is_invalid_char(ch: str) -> bool:
    return unicodedata.category(ch) in _INVALID_CATEGORIES


def name_problem(name: object) -> str | None:
    """Why ``name`` can't be a contract name, or None if it can."""
    if not isinstance(name, str) or not name:
        return "name must be a non-empty string"
    if len(name) > MAX_NAME:
        return f"name longer than {MAX_NAME} characters"
    if name != name.strip():
        return "name has leading or trailing whitespace"
    if any(ch in "\r\n\t\0" for ch in name):
        return "name contains a control character"
    namespaces, symbol = ghidra_symbol(name)
    for part in (*namespaces, symbol):
        if not part:
            return "name has an empty '::' component"
        if _RESERVED.fullmatch(part):
            return f"{part!r} is one of Ghidra's reserved default names"
    return None


def sanitize(part: str) -> str:
    """``part`` with each run of characters Ghidra refuses replaced by one ``_``."""
    out: list[str] = []
    for ch in part:
        if is_invalid_char(ch):
            if not out or out[-1] != "_":
                out.append("_")
        else:
            out.append(ch)
    return "".join(out)


def split_qualified(name: str) -> list[str]:
    """Split a C++-style name at ``::`` outside ``<>``, ``()`` and ``[]``; nothing after an
    ``operator`` keyword is split."""
    parts: list[str] = []
    depth = 0
    start = i = 0
    while i < len(name):
        if depth == 0 and name.startswith("operator", i) and (i == start):
            break
        ch = name[i]
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth = max(0, depth - 1)
        elif depth == 0 and name.startswith("::", i):
            parts.append(name[start:i])
            i += 2
            start = i
            continue
        i += 1
    parts.append(name[start:])
    return parts


def ghidra_symbol(name: str) -> tuple[tuple[str, ...], str]:
    """``(namespace path, symbol name)`` of a contract name, both legal in Ghidra."""
    if _OBJC.fullmatch(name):
        return (), sanitize(name)
    parts = [sanitize(p) for p in split_qualified(name)]
    return tuple(parts[:-1]), parts[-1]


def parse_address(value: object) -> int:
    if not isinstance(value, str) or not _ADDRESS.fullmatch(value):
        raise ValueError(f"address must be a hex string like '0x2A3C1', got {value!r}")
    return int(value, 16)


def parse(doc: object, where: str) -> tuple[str, list[NameEntry]]:
    """Validate one decoded names file; ``where`` prefixes every problem."""
    problems: list[str] = []
    if not isinstance(doc, dict):
        raise NamesError([f"{where}: top level must be an object"])
    extra = set(doc) - {"source", "names"}
    if extra:
        problems.append(f"{where}: unknown top-level keys {sorted(extra)}")
    source = doc.get("source")
    if not isinstance(source, str) or not source.strip():
        problems.append(f"{where}: 'source' must be a non-empty string")
    names = doc.get("names")
    if not isinstance(names, list):
        problems.append(f"{where}: 'names' must be a list")
        names = []
    entries: list[NameEntry] = []
    for i, item in enumerate(names):
        at = f"{where}: names[{i}]"
        if not isinstance(item, dict):
            problems.append(f"{at}: must be an object")
            continue
        extra = set(item) - {"address", "name", "kind", "comment"}
        if extra:
            problems.append(f"{at}: unknown keys {sorted(extra)}")
        try:
            address = parse_address(item.get("address"))
        except ValueError as e:
            problems.append(f"{at}: {e}")
            continue
        kind = item.get("kind")
        if kind not in KINDS:
            problems.append(f"{at}: kind must be one of {list(KINDS)}, got {kind!r}")
            continue
        name = item.get("name")
        why = name_problem(name)
        if why:
            problems.append(f"{at}: {why}: {name!r}")
            continue
        comment = item.get("comment")
        if comment is not None and not isinstance(comment, str):
            problems.append(f"{at}: comment must be a string")
            continue
        entries.append(NameEntry(address, name, kind, comment or None))
    if problems:
        raise NamesError(problems)
    return source, entries


def load_file(path: Path) -> tuple[str, list[NameEntry]]:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as e:
        raise NamesError([f"{path.name}: cannot read JSON: {e}"]) from e
    return parse(doc, path.name)


def merge(files: list[tuple[str, str, list[NameEntry]]]) -> list[MergedName]:
    """Merge ``(file name, source, entries)`` in the given order; sorted by location."""
    merged: dict[int, MergedName] = {}
    first_file: dict[int, str] = {}
    problems: list[str] = []
    for file_name, source, entries in files:
        for e in entries:
            have = merged.get(e.location)
            if have is None:
                merged[e.location] = have = MergedName(e)
                first_file[e.location] = file_name
            elif (have.entry.kind, have.entry.name, have.entry.thumb) != (e.kind, e.name, e.thumb):
                problems.append(
                    f"0x{e.location:X}: {first_file[e.location]} says {have.entry.kind} "
                    f"{have.entry.name!r} (0x{have.entry.address:X}), {file_name} says "
                    f"{e.kind} {e.name!r} (0x{e.address:X})"
                )
                continue
            if source not in have.sources:
                have.sources.append(source)
            if e.comment and e.comment not in have.comments:
                have.comments.append(e.comment)
    if problems:
        raise NamesError(problems)
    return [merged[k] for k in sorted(merged)]


def load_dir(directory: Path) -> list[MergedName]:
    """Every ``*.json`` in ``directory`` (sorted by file name), validated and merged.
    A missing directory has no names."""
    if not directory.is_dir():
        return []
    files, problems = [], []
    for path in sorted(directory.glob("*.json")):
        try:
            source, entries = load_file(path)
        except NamesError as e:
            problems.extend(e.problems)
            continue
        files.append((path.name, source, entries))
    if problems:
        raise NamesError(problems)
    return merge(files)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.names", description=__doc__.splitlines()[0]
    )
    parser.add_argument("cmd", choices=("check", "show"))
    parser.add_argument("dir", nargs="?", type=Path, default=Path(NAMES_DIR))
    args = parser.parse_args(argv)
    try:
        merged = load_dir(args.dir)
    except NamesError as e:
        for problem in e.problems:
            print(f"error: {problem}", file=sys.stderr)
        return 1
    if args.cmd == "show":
        for m in merged:
            namespaces, symbol = ghidra_symbol(m.entry.name)
            qualified = "::".join((*namespaces, symbol))
            print(f"0x{m.entry.address:08X} {m.entry.kind:<8} {qualified}  [{','.join(m.sources)}]")
    print(f"{len(merged)} names OK in {args.dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
