"""Data-table browser: static HTML pages for the language data tables (issue #28, ADR 0010).

Renders every table of ``<Language>/data/`` (docs/formats/data-tables.md) as one sortable page,
English and Korean text side by side, with the inventory icons from the ``png/**/*.dat`` packs
(docs/formats/png-dat.md) where the game's mapping is known. It exists to check the parsers
against the real game. Everything comes from :mod:`mtport.datatables` and :mod:`mtport.pngdat`;
nothing is decoded here.

The 8 tables are the 8 files of each language: the seven record tables of
:data:`mtport.datatables.TABLES` and ``buildup.dat`` (one integer). Japanese is checked to be a
byte-identical copy of English and is not shown separately.

Output (``assets-out/viewers/tables/``, owned by this command alone)::

    index.html                 every table with its record counts
    <table>.html               one page per table (hero, mob, ..., buildup)
    icons/<pack>/<NN>.png      icon images, copied byte for byte from the packs

Command line (``PYTHONPATH=tools python3 -m mtport.tablebrowser ...``)::

    build [--app DIR] [--out DIR]   write the pages
    check [--app DIR] [--out DIR]   re-render in memory, compare with the pages on disk and
                                    count each page's rows against the parser

Both print one line per error and exit non-zero on any error. The output is deterministic.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from mtport import datatables as dt
from mtport import htmlpage as hp
from mtport import pngdat
from mtport.bytereader import FormatError

VIEWER = "tables"
LANGS = ("English", "Korean")  # shown side by side
LANG_CODES = {"English": "en", "Korean": "ko"}
COPY_OF_ENGLISH = "Japanese"  # checked byte-identical to English, not shown

_REPO = Path(__file__).resolve().parents[2]
DEFAULT_APP = _REPO / "work" / "ipa" / "Payload" / "MapleThief.app"
DEFAULT_OUT = _REPO / "assets-out" / "viewers" / VIEWER

PAGES = (*dt.TABLES, "buildup")

TITLES = {
    "hero": "Hero levels",
    "mob": "Monsters",
    "npc": "NPCs",
    "item_equip": "Equipment",
    "item_etc": "Other items",
    "skill": "Skills",
    "market": "Shop listings",
    "buildup": "buildup.dat",
}


@dataclass(frozen=True)
class IconPack:
    """How a table's ``icon`` column picks an image: image ``icon`` of ``pack``."""

    pack: str  # path of the pack inside the .app (a key of pngdat.BY_PATH)
    evidence: str


# The icon index is the image's position in the pack (flat array), established from code in
# docs/formats/png-dat.md: the loaders fill flat arrays, and the drawing code indexes them with
# the table's icon column.
ICONS = {
    "item_equip": IconPack(
        "png/item/equip.dat",
        "item_equip.icon (0x45) indexes the flat array the loader 0x16AF1 fills from "
        "png/item/equip.dat (resource manager +0x2D8); drawn at 0x31EC1, 0x3243D, 0x69A51",
    ),
    "item_etc": IconPack(
        "png/item/etc.dat",
        "item_etc.icon (0x58) indexes the flat array filled from png/item/etc.dat "
        "(+0x2DC); drawn at 0x31EC1, 0x3243D, 0x4CAED, 0x699A1",
    ),
    "skill": IconPack(
        "png/skill/skillico.dat",
        "skill.icon (0x11) indexes the flat array filled from png/skill/skillico.dat "
        "(+0x2B0); the entry is icon x 0x24 at 0x7D1AD",
    ),
}

# Enumerated columns: value -> meaning (docs/formats/data-tables.md).
ENUMS: dict[tuple[str, str], dict[int, str]] = {
    ("mob", "unknown_24"): {1: "normal", 2: "boss", 3: "Malady Bomb"},
    ("item_equip", "slot"): {0: "hat", 1: "weapon", 2: "overall", 3: "earrings", 4: "shoes"},
    ("item_equip", "weapon_type"): {0: "none", 1: "claw", 2: "dagger"},
    ("item_equip", "job"): {4: "thief"},
    ("item_equip", "bonus_stat"): {
        0: "none",
        1: "ATT",
        2: "DEF",
        3: "ACC",
        4: "AVOID",
        5: "HP",
        6: "MP",
    },
    ("item_etc", "type"): {
        0: "meso",
        1: "potion",
        2: "throwing star",
        3: "scroll",
        4: "return scroll",
        5: "etc",
        6: "quest",
        7: "special",
        8: "100 % scroll",
    },
    ("skill", "branch"): {0: "Rogue", 1: "Assassin", 2: "Bandit"},
    ("skill", "type"): {0: "basic attack", 1: "passive", 2: "buff", 3: "active attack"},
    ("market", "table"): {0: "item_equip", 1: "item_etc"},
}

# Columns holding an item id: (table, column) -> the table the id points into.
REFS = {
    ("mob", "equip_drop"): "item_equip",
    ("mob", "etc_drop_1"): "item_etc",
    ("mob", "etc_drop_2"): "item_etc",
    ("mob", "etc_drop_3"): "item_etc",
}
PER_MILLE = re.compile(r"_chance$")


@dataclass
class Data:
    """Everything the pages are rendered from, plus the errors met while loading it."""

    tables: dict[str, dict[str, dt.Table]] = field(default_factory=dict)  # lang -> name -> table
    buildup: dict[str, int] = field(default_factory=dict)  # lang -> value
    icons: dict[str, list[bytes]] = field(default_factory=dict)  # table -> PNG per pack index
    japanese: str = "not checked"
    errors: list[str] = field(default_factory=list)


def load(app: Path) -> Data:
    """Parse every table of each shown language and the icon packs; errors are collected."""
    data = Data()
    for lang in LANGS:
        data.tables[lang] = {}
        d = app / lang / "data"
        for name in dt.TABLES:
            try:
                data.tables[lang][name] = dt.parse(name, (d / f"{name}.dat").read_bytes())
            except (OSError, dt.DataTableError) as e:
                data.errors.append(f"{lang}/data/{name}.dat: {e}")
        try:
            data.buildup[lang] = dt.parse_buildup((d / "buildup.dat").read_bytes())
        except (OSError, dt.DataTableError) as e:
            data.errors.append(f"{lang}/data/buildup.dat: {e}")
    data.japanese = _check_copy(app, data.errors)
    for name, ip in ICONS.items():
        pack = pngdat.BY_PATH[ip.pack]
        try:
            images = pngdat.split((app / ip.pack).read_bytes(), ip.pack)
        except (OSError, FormatError) as e:
            data.errors.append(f"{ip.pack}: {e}")
            continue
        if len(images) != pack.count:
            data.errors.append(f"{ip.pack}: {len(images)} images, the loader expects {pack.count}")
        data.icons[name] = [im.data for im in images]
    _check_languages(data)
    return data


def _check_copy(app: Path, errors: list[str]) -> str:
    src, dup = app / "English" / "data", app / COPY_OF_ENGLISH / "data"
    if not dup.is_dir():
        return f"{COPY_OF_ENGLISH}/data absent"
    differ = []
    for name in PAGES:
        a, b = src / f"{name}.dat", dup / f"{name}.dat"
        if not (a.is_file() and b.is_file()) or a.read_bytes() != b.read_bytes():
            differ.append(name)
    if differ:
        errors.append(f"{COPY_OF_ENGLISH}/data differs from English/data: {', '.join(differ)}")
        return f"differs from English in {', '.join(differ)} (not shown)"
    return f"byte-identical to English in all {len(PAGES)} files (checked), so not shown"


def _numbers(record: Any) -> dict[str, Any]:
    return {c.name: getattr(record, c.name) for c in dt.layout(type(record)) if c.kind != "str"}


def _check_languages(data: Data) -> None:
    """English and Korean must have the same rows and the same numeric columns."""
    en, ko = data.tables.get("English", {}), data.tables.get("Korean", {})
    for name in dt.TABLES:
        if name not in en or name not in ko:
            continue
        a, b = en[name].records, ko[name].records
        if len(a) != len(b):
            data.errors.append(f"{name}: English has {len(a)} rows, Korean {len(b)}")
            continue
        for row, (x, y) in enumerate(zip(a, b, strict=True)):
            if _numbers(x) != _numbers(y):
                data.errors.append(f"{name} row {row}: numeric columns differ between EN and KO")
    if len(set(data.buildup.values())) > 1:
        data.errors.append(f"buildup.dat differs between languages: {data.buildup}")


# --- Rendering ------------------------------------------------------------------------------


def icon_file(table: str, index: int) -> str:
    """Relative path of the copied icon image ``index`` of ``table``'s pack."""
    pack = pngdat.BY_PATH[ICONS[table].pack]
    return f"icons/{Path(pack.path).stem}/{pngdat.image_name(pack.shape, index)}.png"


def _nav(current: str) -> str:
    items = ['<a href="index.html">Index</a>']
    for name in PAGES:
        if name == current:
            items.append(f"<strong>{hp.esc(name)}</strong>")
        else:
            items.append(hp.link(f"{name}.html", name))
    return " · ".join(items)


def _name_index(data: Data) -> dict[str, dict[int, Any]]:
    """English and Korean names of the items by table and key, for the reference columns."""
    out: dict[str, dict[int, Any]] = {}
    for name in ("item_equip", "item_etc"):
        en = data.tables.get("English", {}).get(name)
        ko = data.tables.get("Korean", {}).get(name)
        if en is None:
            continue
        ko_names = {r.id: r.name for r in ko.records} if ko else {}
        out[name] = {r.id: (r.name, ko_names.get(r.id, "")) for r in en.records}
    return out


def _ref_cell(target: str, key: int, names: dict, errors: list[str], where: str) -> hp.Cell:
    found = names.get(target, {}).get(key)
    if found is None:
        errors.append(f"{where}: {target} id {key} does not exist")
        return hp.Cell(f'{key} <span class="error">missing {hp.esc(target)}</span>', key)
    en, ko = found
    return hp.Cell(f"{key} " + hp.link(f"{target}.html#r{key}", en, ko), key)


def _value_cell(name: str, col: dt.Column, value: int) -> hp.Cell:
    label = ENUMS.get((name, col.name), {}).get(value, "")
    if PER_MILLE.search(col.name) and value:
        label = f"{value / 10:g} %"
    return hp.num(value, label)


def render_table(name: str, data: Data, names: dict, errors: list[str]) -> str:
    """The page of record table ``name``; ``errors`` collects dangling ids and bad icons."""
    langs = [lang for lang in LANGS if name in data.tables.get(lang, {})]
    base = data.tables[langs[0]][name]
    # A language whose row count differs is left out (the mismatch is already an error).
    langs = [lang for lang in langs if len(data.tables[lang][name].records) == len(base.records)]
    tables = {lang: data.tables[lang][name] for lang in langs}
    cols = dt.layout(dt.TABLES[name].record)
    has_text = any(c.kind == "str" for c in cols)
    columns: list[hp.Column] = []
    icon = name in ICONS and name in data.icons
    if icon:
        columns.append(hp.Column("icon", f"image `icon` of {ICONS[name].pack}", numeric=True))
    for c in cols:
        tip = f"0x{c.offset:02X} {c.kind}{f'[{c.size}]' if c.kind == 'str' else ''}: {c.doc}"
        if c.kind == "str":
            for lang in langs:
                columns.append(hp.Column(f"{c.name} ({LANG_CODES[lang]})", tip))
        else:
            columns.append(hp.Column(c.name, tip, numeric=True))
    rows, ids = [], []
    for row, record in enumerate(base.records):
        cells: list[hp.Cell] = []
        where = f"{name} row {row}"
        if icon:
            index = record.icon
            if 0 <= index < len(data.icons[name]):
                src = icon_file(name, index)
                cells.append(
                    hp.Cell(f'<img class="icon" src="{hp.esc(src)}" alt="icon {index}">', index)
                )
            else:
                errors.append(f"{where}: icon {index} outside {ICONS[name].pack}")
                cells.append(hp.Cell('<span class="error">missing</span>', index))
        for c in cols:
            if c.kind == "str":
                for lang in langs:
                    value = getattr(tables[lang].records[row], c.name)
                    code = LANG_CODES[lang]
                    cells.append(hp.text(value, "" if code == "en" else code))
                continue
            value = getattr(record, c.name)
            target = REFS.get((name, c.name))
            if name == "market" and c.name == "item":
                target = "item_equip" if record.table == 0 else "item_etc"
            if target and getattr(record, f"{c.name}_chance", 1) == 0:
                cells.append(hp.num(value, "none (chance 0)"))  # a drop slot that never rolls
            elif target:
                cells.append(_ref_cell(target, value, names, errors, where))
            else:
                cells.append(_value_cell(name, c, value))
        rows.append(cells)
        key = record.id if hasattr(record, "id") else row
        ids.append(f"r{key}")
    langs_note = " and ".join(langs) if has_text else "all languages (no text columns)"
    head = [
        f"<p>{hp.esc(dt.TABLES[name].record.__doc__.splitlines()[0])}</p>",
        f'<p><strong data-records="{len(base.records)}">{len(base.records)} records</strong>'
        f" ({base.columns} columns, {base.stride} bytes each), parsed by "
        f"<code>mtport.datatables</code> from {hp.esc(langs_note)}. "
        f"Hover a header for the byte offset, type and meaning; click to sort.</p>",
    ]
    if icon:
        head.append(f'<p class="muted">Icons: {hp.esc(ICONS[name].evidence)}.</p>')
    elif name in ICONS:
        head.append(f'<p class="error">Icons: {hp.esc(ICONS[name].pack)} did not load.</p>')
    else:
        head.append('<p class="muted">Icons: none (no icon column in this table).</p>')
    body = "\n".join(head) + "\n" + hp.table(columns, rows, ids)
    return hp.page(f"{TITLES[name]} ({name}.dat)", body, _nav(name))


def render_buildup(data: Data) -> str:
    rows = [[hp.text(lang), hp.num(data.buildup[lang])] for lang in LANGS if lang in data.buildup]
    body = (
        "<p>Not a table: one ASCII decimal integer read with <code>atoi</code> by "
        "<code>0x980B5</code> (title scene, state 3). What it controls is unknown "
        "(docs/formats/data-tables.md).</p>\n"
        f'<p><strong data-records="{len(rows)}">{len(rows)} rows</strong> '
        "(one value per language).</p>\n"
        + hp.table([hp.Column("language"), hp.Column("value", numeric=True)], rows)
    )
    return hp.page(TITLES["buildup"], body, _nav("buildup"))


def render_index(data: Data, errors: list[str]) -> str:
    columns = [
        hp.Column("table"),
        hp.Column("contents"),
        *(hp.Column(f"records ({LANG_CODES[lang]})", numeric=True) for lang in LANGS),
        hp.Column("columns", numeric=True),
        hp.Column("stride", "bytes per record", numeric=True),
        hp.Column("icons", "pack images used by this table / in the pack"),
    ]
    rows = []
    for name in PAGES:
        counts = []
        shape = ["", ""]
        for lang in LANGS:
            if name == "buildup":
                counts.append(hp.num(1) if lang in data.buildup else hp.text("error"))
                continue
            table = data.tables.get(lang, {}).get(name)
            counts.append(hp.num(len(table.records)) if table else hp.text("error"))
            if table:
                shape = [table.columns, table.stride]
        icons = ""
        if name in data.icons and name in data.tables.get("English", {}):
            used = {r.icon for r in data.tables["English"][name].records}
            icons = f"{len(used)} of {len(data.icons[name])} ({ICONS[name].pack})"
        rows.append(
            [
                hp.Cell(hp.link(f"{name}.html", f"{name}.dat"), name),
                hp.text(TITLES[name]),
                *counts,
                hp.num(shape[0]) if shape[0] != "" else hp.text(""),
                hp.num(shape[1]) if shape[1] != "" else hp.text(""),
                hp.text(icons),
            ]
        )
    problems = (
        "<p>No errors.</p>"
        if not errors
        else "<ul>" + "".join(f'<li class="error">{hp.esc(e)}</li>' for e in errors) + "</ul>"
    )
    body = (
        "<p>The game's static data tables (<code>&lt;Language&gt;/data/*.dat</code>, "
        "docs/formats/data-tables.md), decoded by <code>mtport.datatables</code>. "
        "Each language has these 8 files. English and Korean are shown side by side; "
        f"Japanese: {hp.esc(data.japanese)}. Record counts come from the parser.</p>\n"
        + hp.table(columns, rows)
        + "<h2>Errors</h2>\n"
        + problems
        + "\n"
    )
    return hp.page("MapleStory Thief data tables", body, _nav("index"))


def render(data: Data) -> tuple[dict[str, bytes], list[str]]:
    """All output files (relative path -> bytes) and every error, load errors included."""
    errors = list(data.errors)
    names = _name_index(data)
    files: dict[str, bytes] = {}
    for name in dt.TABLES:
        if not any(name in data.tables.get(lang, {}) for lang in LANGS):
            continue
        files[f"{name}.html"] = render_table(name, data, names, errors).encode()
        if name in data.icons:
            en = data.tables.get("English", {}).get(name)
            for index in sorted({r.icon for r in en.records} if en else ()):
                if 0 <= index < len(data.icons[name]):
                    files[icon_file(name, index)] = data.icons[name][index]
    if data.buildup:
        files["buildup.html"] = render_buildup(data).encode()
    files["index.html"] = render_index(data, errors).encode()
    return files, errors


ROW = re.compile(rb'<tr data-i="')
RECORDS = re.compile(rb'data-records="(\d+)"')


def page_rows(page: bytes) -> tuple[int, int | None]:
    """Body rows in a rendered page and the record count it states."""
    stated = RECORDS.search(page)
    return len(ROW.findall(page)), int(stated.group(1)) if stated else None


def expected_rows(data: Data) -> dict[str, int]:
    """Record count per page, straight from the parser (all shown languages agree)."""
    out = {}
    for name in dt.TABLES:
        counts = {
            len(data.tables[lang][name].records) for lang in LANGS if name in data.tables[lang]
        }
        if len(counts) == 1:
            out[name] = counts.pop()
    out["buildup"] = len(data.buildup)
    return out


def check(app: Path, out: Path) -> list[str]:
    """Errors of a fresh render, plus every difference between it and the pages in ``out``,
    plus every page whose row count differs from the parser's record count."""
    data = load(app)
    files, errors = render(data)
    errors += hp.diff_dir(out, files)
    for name, want in expected_rows(data).items():
        path = out / f"{name}.html"
        if not path.is_file():
            continue  # reported by diff_dir
        rows, stated = page_rows(path.read_bytes())
        if rows != want or stated != want:
            errors.append(f"{name}.html: {rows} rows, states {stated}; the parser has {want}")
    return errors


def build(app: Path, out: Path) -> tuple[dict[str, bytes], list[str]]:
    files, errors = render(load(app))
    hp.write_dir(out, files, VIEWER)
    return files, errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.tablebrowser", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    for cmd, help_ in (("build", "write the pages"), ("check", "compare the pages on disk")):
        p = sub.add_parser(cmd, help=help_)
        p.add_argument("--app", type=Path, default=DEFAULT_APP, help="MapleThief.app directory")
        p.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args(argv)
    if not args.app.is_dir():
        print(f"error: {args.app}: not a directory (run scripts/bootstrap)", file=sys.stderr)
        return 1
    if args.cmd == "build":
        try:
            files, errors = build(args.app, args.out)
        except OSError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        summary = f"{len(files)} files -> {args.out}"
    else:
        errors = check(args.app, args.out)
        summary = f"checked {args.out}"
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    print(f"{summary}, {len(errors)} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
