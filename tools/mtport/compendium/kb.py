"""Compendium knowledge base: every entity of the game as cross-linked JSON (issue #213).

The knowledge base (KB) is the one dataset the compendium site is generated from. It is built
from the extracted bundle by *stages*, each of which adds entities and links to a :class:`KB`:

- :mod:`mtport.compendium.tables`: monster, item, npc, shop, listing, skill, level
  (the language data tables, docs/formats/data-tables.md);
- :mod:`mtport.compendium.world`: area and map (the ``map/`` files, the spot names of
  ``ScriptExtra.strings`` and the portal scripts; docs/re/map-names.md).

A later stage (the quests child, #217) adds its kinds the same way: register its relations
with :func:`relation`, add a function ``stage(kb, src)`` to :data:`STAGES`.

Schema (docs/compendium/kb.md): one file per kind, ``<kind>.json`` =
``{"schema": 1, "kind": K, "entities": [...]}`` sorted by id in natural order, plus
``index.json`` with the kinds, their counts and the relation pairs. Every entity has ``id``
(a string), ``kind``, ``name`` (``{"en": ..., "ko": ...}``), its other fields and
``confidence`` (``{field: "code" | "data" | "probable" | "unknown"}`` for every field but
``id``, ``kind`` and ``confidence``). A relation is a list of references
``{"kind": ..., "id": ..., **attrs}`` and is stored on both sides: :meth:`KB.link` writes
the reference and its inverse at once, from the pairs registered with :func:`relation`.

Command line (``PYTHONPATH=tools python3 -m mtport.compendium.kb ...``)::

    build [--app DIR] [--out DIR]   write the KB (default build/compendium/kb/)
    check [--app DIR] [--out DIR]   rebuild in memory, compare with the files on disk and
                                    check every reference, inverse and confidence

Both print one line per problem and exit non-zero on any. The output is deterministic: two
builds from the same bundle are byte-identical. The bundle is untrusted data: it is parsed,
never executed.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, field
from functools import cached_property
from pathlib import Path
from typing import Any

SCHEMA = 1
CONFIDENCE = ("code", "data", "probable", "unknown")
LANGS = ("en", "ko")
INDEX = "index.json"

_REPO = Path(__file__).resolve().parents[3]
DEFAULT_APP = _REPO / "work" / "ipa" / "Payload" / "MapleThief.app"
DEFAULT_OUT = _REPO / "build" / "compendium" / "kb"

# Fields every entity has that carry no confidence of their own.
_BARE = ("id", "kind", "confidence")


class KBError(Exception):
    """A malformed entity or a misuse of the builder (a bug in a stage, not in the data)."""


# -- relations ------------------------------------------------------------------------------


@dataclass(frozen=True)
class Relation:
    """One direction of a relation: ``kind.field`` holds refs to ``target`` whose ``inverse``
    field holds the refs back. ``confidence`` is the confidence of both fields."""

    kind: str
    field: str
    target: str
    inverse: str
    confidence: str = "code"


RELATIONS: dict[tuple[str, str], Relation] = {}


def relation(kind: str, fld: str, target: str, inverse: str, confidence: str = "code") -> None:
    """Register the pair ``kind.fld`` <-> ``target.inverse`` (idempotent).

    A relation of a kind with itself (map exits and entrances) is two different fields."""
    if confidence not in CONFIDENCE:
        raise KBError(f"relation {kind}.{fld}: confidence {confidence!r}")
    pair = (
        Relation(kind, fld, target, inverse, confidence),
        Relation(target, inverse, kind, fld, confidence),
    )
    for r in pair:
        old = RELATIONS.get((r.kind, r.field))
        if old is not None and old != r:
            raise KBError(f"relation {r.kind}.{r.field} registered twice: {old} and {r}")
    for r in pair:
        RELATIONS[(r.kind, r.field)] = r


def relations_of(kind: str) -> list[Relation]:
    return [r for (k, _), r in sorted(RELATIONS.items()) if k == kind]


# -- ids and ordering -----------------------------------------------------------------------


def natural_key(text: str) -> tuple:
    """Sort key that compares digit runs as numbers: ``"2" < "10"``, ``"etc:3" < "etc:12"``."""
    return tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in re.split(r"(\d+)", text))


def ref(kind: str, ident: str, **attrs: Any) -> dict[str, Any]:
    return {"kind": kind, "id": ident, **attrs}


def ref_text(kind: str, ident: str) -> str:
    """The reference syntax of the site's prose: ``{{kind:id}}``."""
    return "{{" + f"{kind}:{ident}" + "}}"


def _ref_key(r: Mapping[str, Any]) -> tuple:
    return (r["kind"], natural_key(r["id"]), json.dumps(r, sort_keys=True, ensure_ascii=False))


def names(en: str | None, ko: str | None) -> dict[str, str | None]:
    return {"en": en, "ko": ko}


# -- the builder ----------------------------------------------------------------------------


@dataclass
class KB:
    """Entities by kind and id, plus the problems found while building."""

    entities: dict[str, dict[str, dict[str, Any]]] = field(default_factory=dict)
    problems: list[str] = field(default_factory=list)

    def add(
        self,
        kind: str,
        ident: str,
        name: Mapping[str, str | None],
        fields: Mapping[str, tuple[Any, str]],
        name_confidence: str = "data",
    ) -> dict:
        """Add one entity. ``fields`` maps each field to ``(value, confidence)``."""
        if not isinstance(ident, str) or not ident:
            raise KBError(f"{kind}: id {ident!r} is not a non-empty string")
        table = self.entities.setdefault(kind, {})
        if ident in table:
            raise KBError(f"{kind} {ident}: added twice")
        entity: dict[str, Any] = {"id": ident, "kind": kind, "name": dict(name)}
        conf = {"name": name_confidence}
        for key, (value, level) in fields.items():
            if key in _BARE or key == "name":
                raise KBError(f"{kind} {ident}: field {key!r} is reserved")
            entity[key] = value
            conf[key] = level
        entity["confidence"] = conf
        self._check_confidence(kind, ident, conf)
        table[ident] = entity
        return entity

    def get(self, kind: str, ident: str) -> dict[str, Any] | None:
        return self.entities.get(kind, {}).get(ident)

    def set(self, kind: str, ident: str, key: str, value: Any, confidence: str) -> None:
        """Set (or replace) one field of an existing entity, for a stage that adds to a kind
        another stage made."""
        entity = self.entities[kind][ident]
        if key in _BARE:
            raise KBError(f"{kind} {ident}: field {key!r} is reserved")
        entity[key] = value
        entity["confidence"][key] = confidence
        self._check_confidence(kind, ident, entity["confidence"])

    def link(
        self, kind: str, ident: str, fld: str, target: str, target_id: str, **attrs: Any
    ) -> bool:
        """Store ``kind:ident.fld -> target:target_id`` and its inverse, both with ``attrs``.

        A reference to an entity that does not exist is not stored: it is reported in
        :attr:`problems` (as is one from a missing entity) and False is returned."""
        rel = RELATIONS.get((kind, fld))
        if rel is None or rel.target != target:
            raise KBError(f"no relation {kind}.{fld} -> {target} is registered")
        here = self.get(kind, ident)
        there = self.get(target, target_id)
        if here is None or there is None:
            missing = f"{kind} {ident}" if here is None else f"{target} {target_id}"
            self.problems.append(f"{kind} {ident}.{fld} -> {target} {target_id}: no {missing}")
            return False
        here.setdefault(fld, []).append(ref(target, target_id, **attrs))
        there.setdefault(rel.inverse, []).append(ref(kind, ident, **attrs))
        return True

    def finish(self) -> None:
        """Give every entity every relation field of its kind (empty if unlinked), with the
        relation's confidence, and sort every relation list."""
        for kind, table in self.entities.items():
            rels = relations_of(kind)
            for entity in table.values():
                for r in rels:
                    refs = entity.setdefault(r.field, [])
                    refs.sort(key=_ref_key)
                    entity["confidence"].setdefault(r.field, r.confidence)

    def documents(self) -> dict[str, str]:
        """file name -> text for every file of the KB (index.json included)."""
        self.finish()
        out = {}
        for kind in sorted(self.entities):
            table = self.entities[kind]
            body = {
                "schema": SCHEMA,
                "kind": kind,
                "entities": [table[i] for i in sorted(table, key=natural_key)],
            }
            out[f"{kind}.json"] = dump(body)
        out[INDEX] = dump(index_document(self.entities))
        return out

    @staticmethod
    def _check_confidence(kind: str, ident: str, conf: Mapping[str, str]) -> None:
        for key, level in conf.items():
            if level not in CONFIDENCE:
                raise KBError(f"{kind} {ident}: confidence of {key} is {level!r}")


def index_document(entities: Mapping[str, Mapping[str, Any]]) -> dict[str, Any]:
    kinds = sorted(entities)
    return {
        "schema": SCHEMA,
        "kinds": {k: len(entities[k]) for k in kinds},
        "relations": [
            {
                "kind": r.kind,
                "field": r.field,
                "target": r.target,
                "inverse": r.inverse,
                "confidence": r.confidence,
            }
            for (_, _), r in sorted(RELATIONS.items())
            if r.kind in entities or r.target in entities
        ],
    }


def dump(obj: Any) -> str:
    return json.dumps(obj, sort_keys=True, ensure_ascii=False, indent=1) + "\n"


# -- sources --------------------------------------------------------------------------------


class Sources:
    """The bundle, read lazily and once. Stages take what they need from here."""

    def __init__(self, app: Path) -> None:
        self.app = Path(app)

    @cached_property
    def tables(self) -> dict[str, dict[str, Any]]:
        """``{"en": {table: Table}, "ko": {...}}`` from ``English/data`` and ``Korean/data``."""
        from mtport import datatables as dt

        return {
            "en": dt.load_dir(self.app / "English" / "data"),
            "ko": dt.load_dir(self.app / "Korean" / "data"),
        }

    @cached_property
    def script_extra(self) -> dict[str, dict[str, str]]:
        """``ScriptExtra.strings`` of ``en.lproj`` and ``ko.lproj``."""
        from mtport import scriptout

        return {
            lang: scriptout.load_strings(self.app / f"{lang}.lproj" / "ScriptExtra.strings")
            for lang in LANGS
        }

    def script_variants(self, name: str) -> list[tuple[str, Path]]:
        """Every file the game may run for script ``name``: ``chapter<n>/<name>.out`` for each
        chapter that has one, then ``common/<name>.out`` (``0x63191`` tries the chapter's
        directory first), as ``("chapter1/portal201", path)``."""
        root = self.app / "script"
        out = []
        dirs = sorted((p.name for p in root.glob("chapter*") if p.is_dir()), key=natural_key)
        for d in [*dirs, "common"]:
            path = root / d / f"{name}.out"
            if path.is_file():
                out.append((f"{d}/{name}", path))
        return out


Stage = Callable[[KB, Sources], None]


def stages() -> list[Stage]:
    """The stages in build order. A new stage goes at the end of this list."""
    from mtport.compendium import tables, world

    return [tables.stage, world.stage]


def build(app: Path, stage_list: Iterable[Stage] | None = None) -> KB:
    kb = KB()
    src = Sources(app)
    for st in stage_list if stage_list is not None else stages():
        st(kb, src)
    kb.finish()
    return kb


# -- reading and checking -------------------------------------------------------------------


def load_kb(path: str | Path) -> dict[str, dict[str, dict[str, Any]]]:
    """Read a KB directory: ``{kind: {id: entity}}`` for every kind ``index.json`` lists."""
    path = Path(path)
    index = json.loads((path / INDEX).read_text(encoding="utf-8"))
    if index.get("schema") != SCHEMA:
        raise KBError(f"{path / INDEX}: schema {index.get('schema')!r}, expected {SCHEMA}")
    out: dict[str, dict[str, dict[str, Any]]] = {}
    for kind in index["kinds"]:
        doc = json.loads((path / f"{kind}.json").read_text(encoding="utf-8"))
        if doc.get("schema") != SCHEMA or doc.get("kind") != kind:
            raise KBError(f"{path / f'{kind}.json'}: not a schema {SCHEMA} {kind} file")
        out[kind] = {e["id"]: e for e in doc["entities"]}
    return out


def load_relations(path: str | Path) -> list[Relation]:
    index = json.loads((Path(path) / INDEX).read_text(encoding="utf-8"))
    return [
        Relation(r["kind"], r["field"], r["target"], r["inverse"], r["confidence"])
        for r in index["relations"]
    ]


def check_kb(
    entities: Mapping[str, Mapping[str, Mapping[str, Any]]],
    relations: Iterable[Relation] | None = None,
) -> list[str]:
    """Every problem of a KB: dangling references, references without their inverse, fields
    without a valid confidence, names without both languages. Empty means sound."""
    rels = list(RELATIONS.values()) if relations is None else list(relations)
    by_field = {(r.kind, r.field): r for r in rels}
    out = []
    for kind in sorted(entities):
        for ident in sorted(entities[kind], key=natural_key):
            e = entities[kind][ident]
            where = f"{kind} {ident}"
            if e.get("id") != ident or e.get("kind") != kind:
                out.append(f"{where}: id/kind fields say {e.get('kind')} {e.get('id')}")
            name = e.get("name")
            if not isinstance(name, dict) or set(name) != set(LANGS):
                out.append(f"{where}: name is not {{en, ko}}: {name!r}")
            elif not all(isinstance(name[lang], str) and name[lang] for lang in LANGS):
                out.append(f"{where}: name lacks a language: {name!r}")
            conf = e.get("confidence", {})
            for key in e:
                if key in _BARE:
                    continue
                if conf.get(key) not in CONFIDENCE:
                    out.append(f"{where}: field {key} has confidence {conf.get(key)!r}")
            for key in conf:
                if key not in e:
                    out.append(f"{where}: confidence for missing field {key}")
            for (k, fld), r in by_field.items():
                if k != kind:
                    continue
                for rf in e.get(fld, []):
                    other = entities.get(r.target, {}).get(rf.get("id"))
                    if rf.get("kind") != r.target or other is None:
                        out.append(f"{where}.{fld}: dangling {rf.get('kind')} {rf.get('id')}")
                        continue
                    back = {**rf, "kind": kind, "id": ident}
                    if back not in other.get(r.inverse, []):
                        out.append(
                            f"{where}.{fld} -> {r.target} {rf['id']}: no inverse in {r.inverse}"
                        )
    return out


# -- command line ---------------------------------------------------------------------------


def write(kb: KB, out: Path) -> list[Path]:
    """Write the KB to ``out`` (owned by this command: other ``*.json`` files are removed)."""
    docs = kb.documents()
    out.mkdir(parents=True, exist_ok=True)
    for stale in sorted(out.glob("*.json")):
        if stale.name not in docs:
            stale.unlink()
    paths = []
    for name, text in docs.items():
        p = out / name
        p.write_text(text, encoding="utf-8", newline="\n")
        paths.append(p)
    return paths


def summary(entities: Mapping[str, Mapping[str, Any]]) -> str:
    return ", ".join(f"{k} {len(v)}" for k, v in sorted(entities.items()))


def report_lines(kb: KB) -> list[str]:
    """Notes for the build log: maps whose name is a fallback, and why (world stage)."""
    from mtport.compendium import world

    return world.name_report(kb.entities)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.compendium.kb", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    for cmd, text in (
        ("build", "write the knowledge base"),
        ("check", "compare the files on disk with a rebuild and check them"),
    ):
        p = sub.add_parser(cmd, help=text)
        p.add_argument("--app", type=Path, default=DEFAULT_APP, help="MapleThief.app directory")
        p.add_argument("--out", type=Path, default=DEFAULT_OUT, help="KB directory")
    args = parser.parse_args(argv)

    from mtport.bytereader import FormatError

    try:
        kb = build(args.app)
    except (OSError, FormatError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    problems = list(kb.problems)
    if args.cmd == "build":
        write(kb, args.out)
        problems += check_kb(kb.entities)
        print(f"wrote {args.out}: {summary(kb.entities)}")
    else:
        docs = kb.documents()
        for name, text in docs.items():
            path = args.out / name
            if not path.is_file():
                problems.append(f"{path}: missing")
            elif path.read_text(encoding="utf-8") != text:
                problems.append(f"{path}: differs from a rebuild")
        for path in sorted(args.out.glob("*.json")) if args.out.is_dir() else []:
            if path.name not in docs:
                problems.append(f"{path}: not part of the KB")
        if not problems:
            loaded = load_kb(args.out)
            problems += check_kb(loaded, load_relations(args.out))
        print(f"checked {args.out}: {summary(kb.entities)}")
    for line in report_lines(kb):
        print(f"note: {line}")
    for p in problems:
        print(f"error: {p}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    # Run the imported module's main: the stages register their relations in
    # mtport.compendium.kb, which is not this __main__ module.
    from mtport.compendium import kb as _kb

    sys.exit(_kb.main())
