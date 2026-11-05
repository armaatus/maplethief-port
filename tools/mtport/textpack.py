"""Text packs: translations of the game's text and the port's own, in UTF-8 (#189, ADR 0108).

A text pack is a folder (``text_pack = DIR`` in settings.ini, or ``--text-pack DIR``;
``maplethief/textpack.h``, ``docs/formats/text-pack.md``) of ``.strings`` files in Apple's
text syntax, strict UTF-8:

- ``textpack.strings``: the manifest (``format`` "1", ``name``, ``language``; ``version``,
  ``game_version`` and ``author`` optional). Required.
- ``Scripts.strings``, ``ScriptExtra.strings``: the dialogue, by the game's own keys.
- ``Tables.strings``: names and descriptions of the data tables, ``<table>.<row>.<column>``.
- ``Port.strings``: the port's menus, by ``platform/src/port_text.def``'s ids and the derived
  keys ``setting.<key>.label|help`` and ``action.<key>.label|help|button|confirm``.

``python3 -m mtport.textpack export DIR [--bundle APP] [--language en|ko]`` writes every text
of the game's language (from your own copy, ``work/ipa/Payload/MapleThief.app`` by default)
and the port's, ready to translate; DIR holds the game's text, so it is yours to keep outside
the repository. ``python3 -m mtport.textpack check DIR`` reads a pack as the runtime does and
reports what the runtime would refuse, ignore or cut.

The parser here follows the runtime's (``runtime/src/textpack/strings_file.cpp``) rule for
rule, with the same reasons, so a pack that passes ``check`` opens. The bundle's files are
untrusted data: property lists and tables are parsed, nothing else.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

from mtport import datatables
from mtport.scriptout import load_strings

DEFAULT_BUNDLE = Path("work/ipa/Payload/MapleThief.app")
REPO = Path(__file__).resolve().parents[2]

MAX_FILE = 4 << 20
MAX_KEY = 256
MAX_VALUE = 4096
MAX_ENTRIES = 65536

MANIFEST = "textpack.strings"
FILES = {
    "textpack.strings": "textpack",
    "scripts.strings": "Scripts",
    "scriptextra.strings": "ScriptExtra",
    "tables.strings": "Tables",
    "port.strings": "Port",
}

# The tables' text columns the runtime replaces: (table, column) -> (offset, width).
TEXT_COLUMNS: dict[tuple[str, str], tuple[int, int]] = {
    ("mob", "name"): (1, 16),
    ("npc", "name"): (1, 32),
    ("item_equip", "name"): (1, 32),
    ("item_etc", "name"): (1, 64),
    ("item_etc", "description"): (90, 128),
    ("skill", "name"): (1, 16),
    ("skill", "description"): (41, 128),
    ("skill", "detail"): (169, 128),
}

LANGUAGES = {"en": ("en.lproj", "English"), "ko": ("ko.lproj", "Korean")}
_LANGUAGE_TAG = re.compile(r"^[A-Za-z]{2,3}(-[A-Za-z0-9]{1,8})*$")
_PLACE = re.compile(r"\{(\d{1,2})\}")


class StringsError(ValueError):
    """A ``.strings`` file the runtime refuses whole: ``line N: reason``."""


@dataclass
class StringsFile:
    entries: dict[str, str] = field(default_factory=dict)
    duplicates: int = 0


# -- the parser ----------------------------------------------------------------------------------


def _check_utf8(data: bytes) -> None:
    try:
        data.decode("utf-8", errors="strict")
    except UnicodeDecodeError as e:
        line = data.count(b"\n", 0, e.start) + 1
        raise StringsError(
            f"line {line}: invalid UTF-8 (byte 0x{data[e.start]:02X} at offset {e.start})"
        ) from None


def parse_strings(data: bytes) -> StringsFile:
    """Parse one ``.strings`` file; raises StringsError with the runtime's reason."""
    if len(data) > MAX_FILE:
        raise StringsError(f"larger than {MAX_FILE >> 20} MiB")
    _check_utf8(data)
    text = data.decode("utf-8")
    if text.startswith("﻿"):
        text = text[1:]
    out = StringsFile()
    i = 0
    line = 1
    n = len(text)

    def fail(reason: str) -> StringsError:
        return StringsError(f"line {line}: {reason}")

    def skip_space() -> None:
        nonlocal i, line
        while i < n:
            c = text[i]
            if c == "\n":
                line += 1
                i += 1
            elif c in " \t\r":
                i += 1
            elif text.startswith("//", i):
                while i < n and text[i] != "\n":
                    i += 1
            elif text.startswith("/*", i):
                end = text.find("*/", i + 2)
                if end < 0:
                    raise fail("a comment without its end (*/)")
                line += text.count("\n", i, end)
                i = end + 2
            else:
                return

    def quoted(what: str) -> str:
        nonlocal i, line
        if i >= n or text[i] != '"':
            raise fail(what)
        start_line = line
        i += 1
        buf: list[str] = []
        while True:
            if i >= n:
                line = start_line
                raise fail("a string without its closing quote")
            c = text[i]
            if c == '"':
                i += 1
                return "".join(buf)
            if c == "\n":
                line += 1
            if c == "\0":
                raise fail("a NUL character")
            if c != "\\":
                buf.append(c)
                i += 1
                continue
            i += 1
            if i >= n:
                line = start_line
                raise fail("a string without its closing quote")
            e = text[i]
            simple = {'"': '"', "\\": "\\", "'": "'", "n": "\n", "t": "\t", "r": "\r"}
            if e in simple:
                buf.append(simple[e])
                i += 1
                continue
            if e not in "Uu":
                shown = e if " " <= e < "\x7f" else f"x{e.encode()[0]:02X}"
                raise fail(f"an unknown escape \\{shown}")
            cp = _hex4(text, i + 1)
            if cp is None:
                raise fail("\\U needs four hex digits")
            i += 5
            if 0xD800 <= cp < 0xDC00:
                low = _hex4(text, i + 2) if text.startswith(("\\U", "\\u"), i) else None
                if low is None or not 0xDC00 <= low < 0xE000:
                    raise fail(f"a lone surrogate (\\U{cp:04X})")
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00)
                i += 6
            elif 0xDC00 <= cp < 0xE000:
                raise fail(f"a lone surrogate (\\U{cp:04X})")
            if cp == 0:
                raise fail("a NUL character")
            buf.append(chr(cp))

    while True:
        skip_space()
        if i >= n:
            return out
        key = quoted("expected a quoted key")
        skip_space()
        if i >= n or text[i] != "=":
            raise fail("expected '=' after the key")
        i += 1
        skip_space()
        value_line = line
        value = quoted("expected a quoted value after '='")
        skip_space()
        if i >= n or text[i] != ";":
            raise fail("expected ';' after the value")
        i += 1
        line_now, line = line, value_line  # the checks below name the value's line
        if not key:
            raise fail("an empty key")
        if len(key.encode()) > MAX_KEY:
            raise fail(f"a key longer than {MAX_KEY} bytes")
        if len(value.encode()) > MAX_VALUE:
            raise fail(f"a value longer than {MAX_VALUE} bytes")
        if key in out.entries:
            out.duplicates += 1
        elif len(out.entries) >= MAX_ENTRIES:
            raise fail(f"more than {MAX_ENTRIES} entries")
        line = line_now
        out.entries[key] = value


def _hex4(text: str, at: int) -> int | None:
    digits = text[at : at + 4]
    if len(digits) != 4 or any(c not in "0123456789abcdefABCDEF" for c in digits):
        return None
    return int(digits, 16)


def quote(s: str) -> str:
    """``s`` as a ``.strings`` literal the parser reads back as ``s``."""
    out = ['"']
    for c in s:
        if c == '"':
            out.append('\\"')
        elif c == "\\":
            out.append("\\\\")
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif c == "\r":
            out.append("\\r")
        elif ord(c) < 0x20 or ord(c) == 0x7F:
            out.append(f"\\U{ord(c):04X}")
        else:
            out.append(c)
    out.append('"')
    return "".join(out)


def utf8_cut(s: str, limit: int) -> str:
    """The longest prefix of ``s`` of whole characters in at most ``limit`` UTF-8 bytes."""
    raw = s.encode()
    if len(raw) <= limit:
        return s
    return raw[:limit].decode("utf-8", errors="ignore")


def places(s: str) -> set[int]:
    return {int(m) for m in _PLACE.findall(s)}


# -- the port's own text ---------------------------------------------------------------------------

_C_STRING = r'"(?:[^"\\]|\\.)*"'
_DEF = re.compile(r"MT_TEXT\(\s*(\w+)\s*,\s*((?:" + _C_STRING + r"\s*)+)\)")
_FIELD = re.compile(r"\.(key|label|help|button|confirm)\s*=\s*((?:" + _C_STRING + r"\s*)+)")


def _c_literal(chunk: str) -> str:
    """Adjacent C string literals (escapes: \\" \\\\ \\n only) joined."""
    out = []
    for lit in re.findall(_C_STRING, chunk):
        body = lit[1:-1]
        out.append(re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m[1], m[1]), body))
    return "".join(out)


def _rows(source: str, array: str, fields: Sequence[str], prefix: str) -> dict[str, str]:
    start = source.find(array)
    if start < 0:
        return {}
    end = source.find("\n};", start)
    body = source[start:end]
    out: dict[str, str] = {}
    key = None
    for m in _FIELD.finditer(body):
        name, value = m[1], _c_literal(m[2])
        if name == "key":
            key = value
        elif key is not None and name in fields:
            out[f"{prefix}.{key}.{name}"] = value
    return out


def port_english(repo: Path = REPO) -> dict[str, str]:
    """Every key of Port.strings with its English: the table, the registry, the menu actions."""
    out: dict[str, str] = {}
    src = repo / "platform" / "src"
    for m in _DEF.finditer((src / "port_text.def").read_text(encoding="utf-8")):
        out[m[1]] = _c_literal(m[2])
    settings = (src / "settings.cpp").read_text(encoding="utf-8")
    out.update(_rows(settings, "kRegistry[] = {", ("label", "help"), "setting"))
    menu = (src / "settings_menu.cpp").read_text(encoding="utf-8")
    out.update(_rows(menu, "kActions[] = {", ("label", "help", "button", "confirm"), "action"))
    return out


# -- check -----------------------------------------------------------------------------------------


@dataclass
class Report:
    entries: int = 0
    refused: list[str] = field(default_factory=list)  # files the runtime refuses whole
    ignored: list[str] = field(default_factory=list)  # entries it ignores
    cut: list[str] = field(default_factory=list)  # table values cut to their field
    notes: list[str] = field(default_factory=list)  # duplicates, unknown files

    @property
    def ok(self) -> bool:
        return not self.refused


def table_key(key: str) -> tuple[str, int, str] | None:
    parts = key.split(".")
    if len(parts) != 3 or not parts[1].isdigit() or (parts[0], parts[2]) not in TEXT_COLUMNS:
        return None
    return parts[0], int(parts[1]), parts[2]


def check(pack: Path, port: dict[str, str] | None = None) -> Report:
    """What the runtime does with ``pack``: refused files, ignored entries, cut values."""
    report = Report()
    if not pack.is_dir():
        report.refused.append(f"{pack}: is no directory")
        return report
    found = {p.name.lower(): p for p in pack.iterdir() if p.is_file()}
    for name, p in sorted(found.items()):
        if name.endswith(".strings") and name not in FILES:
            report.notes.append(f"{p.name}: not a text pack file: ignored")
    if MANIFEST not in found:
        report.refused.append(f"{pack}: has no {MANIFEST} (the manifest)")
        return report
    for name, table in FILES.items():
        if name not in found:
            continue
        p = found[name]
        try:
            parsed = parse_strings(p.read_bytes())
        except StringsError as e:
            report.refused.append(f"{p.name}: {e}")
            continue
        if parsed.duplicates:
            report.notes.append(f"{p.name}: {parsed.duplicates} keys twice: the last one kept")
        if table == "textpack":
            _check_manifest(parsed.entries, report)
            continue
        report.entries += len(parsed.entries)
        if table == "Tables":
            for key, value in parsed.entries.items():
                tk = table_key(key)
                if tk is None:
                    report.ignored.append(f'Tables.strings "{key}": not a table text column')
                    continue
                # A full field needs no NUL: the game's accessor 0x37999 ends its copy.
                width = TEXT_COLUMNS[(tk[0], tk[2])][1]
                if len(value.encode()) > width:
                    report.cut.append(
                        f'Tables.strings "{key}": {len(value.encode())} bytes, its field holds '
                        f"{width}: cut to {utf8_cut(value, width)!r}"
                    )
        elif table == "Port" and port is not None:
            for key, value in parsed.entries.items():
                if key not in port:
                    report.ignored.append(f'Port.strings "{key}": no such text')
                elif places(value) != places(port[key]):
                    report.ignored.append(
                        f'Port.strings "{key}": its places are not the English\'s '
                        f"({sorted(places(port[key]))}): the English kept"
                    )
    return report


def _check_manifest(m: dict[str, str], report: Report) -> None:
    if m.get("format") != "1":
        report.refused.append(f'{MANIFEST}: its "format" is not "1"')
    if not m.get("name"):
        report.refused.append(f'{MANIFEST}: it has no "name"')
    if not _LANGUAGE_TAG.match(m.get("language", "")):
        report.refused.append(f'{MANIFEST}: its "language" is not a language tag (en, pt-BR)')


# -- export ----------------------------------------------------------------------------------------


def _write(path: Path, header: str, entries: Sequence[tuple[str, str, str]]) -> None:
    lines = [f"/* {header} */", ""]
    for key, value, comment in entries:
        if comment:
            lines.append(f"/* {comment} */")
        lines.append(f"{quote(key)} = {quote(value)};")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def export(pack: Path, bundle: Path, language: str = "en") -> dict[str, int]:
    """Write a pack of the game's text in ``language`` and the port's, to translate."""
    lproj, data_dir = LANGUAGES[language]
    pack.mkdir(parents=True, exist_ok=True)
    counts: dict[str, int] = {}
    _write(
        pack / MANIFEST,
        "The pack: rename it, set its language (en, de, pt-BR, ...) and your name.",
        [
            ("format", "1", ""),
            ("name", f"MapleThief text ({language})", ""),
            ("language", language, ""),
            ("version", "1", ""),
            ("game_version", "1.5", ""),
            ("author", "", ""),
        ],
    )
    for table in ("Scripts", "ScriptExtra"):
        strings = load_strings(bundle / lproj / f"{table}.strings")
        rows = [(k, v, "") for k, v in sorted(strings.items())]
        _write(pack / f"{table}.strings", f"{table}: the game's dialogue, by its keys.", rows)
        counts[table] = len(rows)
    tables = datatables.load_dir(bundle / data_dir / "data")
    rows = []
    for (name, column), (_offset, width) in TEXT_COLUMNS.items():
        for i, record in enumerate(tables[name].records):
            value = getattr(record, column)
            if value:
                rows.append((f"{name}.{i}.{column}", value, f"at most {width} bytes"))
    _write(
        pack / "Tables.strings",
        "Tables: names and descriptions, <table>.<row>.<column>; '$' is a line break. A value "
        "longer than its room is cut at a whole character.",
        rows,
    )
    counts["Tables"] = len(rows)
    port = port_english()
    _write(
        pack / "Port.strings",
        "Port: the port's menus. Keep each {0}, {1}, ... (the menus show plain letters only: "
        "accents are dropped).",
        [(k, v, "") for k, v in port.items()],
    )
    counts["Port"] = len(port)
    return counts


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.textpack", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    e = sub.add_parser("export", help="write the game's and the port's text to translate")
    e.add_argument("pack", type=Path, metavar="DIR")
    e.add_argument("--bundle", type=Path, default=DEFAULT_BUNDLE, metavar="APP")
    e.add_argument("--language", choices=sorted(LANGUAGES), default="en")
    c = sub.add_parser("check", help="read a pack as the runtime does")
    c.add_argument("pack", type=Path, metavar="DIR")
    args = parser.parse_args(argv)
    if args.command == "export":
        if not args.bundle.is_dir():
            print(f"{args.bundle}: no such app directory (scripts/bootstrap)", file=sys.stderr)
            return 2
        counts = export(args.pack, args.bundle, args.language)
        print(f"{args.pack}: " + ", ".join(f"{k} {v}" for k, v in counts.items()))
        print("Your own copy's text: keep this folder outside the repository.")
        return 0
    report = check(args.pack, port_english())
    for title, lines in (
        ("refused", report.refused),
        ("ignored", report.ignored),
        ("cut", report.cut),
        ("note", report.notes),
    ):
        for line in lines:
            print(f"{title}: {line}")
    print(f"{args.pack}: {report.entries} entries, {len(report.refused)} refused")
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())
