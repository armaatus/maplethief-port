"""Asset pipeline: convert the extracted ``MapleThief.app`` into an inspectable ``assets-out/``.

Issue #25, ADR 0010 (the pipeline owns ``assets-out/``) and ADR 0011 (manifest, incremental
runs). The runtime may read the original files directly; ``assets-out/`` is for inspection,
the viewers and enhancement work.

Every file of the bundle is classified by :func:`mtport.formats.classify` and gets exactly one
outcome from :data:`RULES` (or :data:`OVERRIDES` for a few paths):

- ``converted``: parsed and written in a readable form (table below);
- ``copied``: already a standard format; checked by its decoder, then copied unchanged;
- ``ignored``: not an asset; the rule says why.

A file that no format matches, or a format without a rule, is an error. So the manifest lists
every file of the ``.app`` by construction. Output paths are the source path plus a new
extension, so every output names its source::

    *.png (CgBI)           -> same path, a standard PNG (mtport.cgbi.convert)
    *.png, *.mp3, *.wav    -> same path, copied (PNG pixels decoded first, audio framing checked)
    map/*.map|att|obj      -> <path>.json   tile layers / attribute grid / objects
    map/*.mapprj           -> <path>.json   tile size and sheet names
    sprite/**/*.sprprj     -> <path>.json   images, actions, frames, layers
    <Language>/data/*.dat  -> <path>.json   records with the column layout (UTF-8, all languages)
    script/**/*.out        -> <path>.dis    disassembly, en.lproj/Scripts.strings text as comments
    *.strings, Info.plist  -> <path>.json
    MainWindow.nib         -> <path>.json   the NIBArchive objects and values
    png/**/<stem>.dat      -> png/**/<stem>/NN.png (+ parts) and index.json, checked against
                              the length table in the bundle's own MapleThief binary

``assets-out/manifest.json`` records, per source file: sha256, size, format, outcome, the
reason (copied/ignored/error), the rule key, the hashes of the files the conversion also
read (``deps``) and every output with its sha256. A rule key is the rule name plus a hash of
the source code of this module and of the parser modules the rule uses (with every
``mtport`` module they import), so editing a parser invalidates exactly its outputs.

A run skips a file when its hash, format, rule key and dependency hashes match the manifest
and every recorded output is on disk with the recorded hash. Outputs of removed, changed or
failed inputs that are no longer produced are deleted. Files (and the manifest) are written
only when their bytes change, so a second run writes nothing. The output is deterministic:
sorted keys, ``\\n`` line ends, no timestamps or absolute paths, fixed PNG settings.
``assets-out/viewers/`` belongs to the viewer commands and is never touched.

Command line (``scripts/pipeline`` or ``PYTHONPATH=tools python3 -m mtport.pipeline``)::

    run   [--app DIR] [--out DIR]   convert (the default command)
    check [--app DIR] [--out DIR]   dry run: report what run would change; writes nothing

Both print one ``error:`` line per failed file and a summary line
``files=... converted=... copied=... ignored=... skipped(unchanged)=... errors=... removed=...``
and exit 1 on any error (``check`` also when anything is out of date). The input files are
untrusted data: they are parsed, never executed.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime
import hashlib
import json
import os
import plistlib
import sys
import time
import types
from collections import Counter
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

from mtport import (
    attfile,
    cgbi,
    datatables,
    fat,
    formats,
    macho,
    mapfile,
    mapprj,
    nib,
    objfile,
    pngdat,
    scriptout,
    sprprj,
)
from mtport.bytereader import FormatError

SCHEMA = 1  # manifest layout version
MANIFEST = "manifest.json"
RESERVED = ("viewers",)  # top-level directories of assets-out/ owned by other commands
CONVERTED, COPIED, IGNORED, ERROR = "converted", "copied", "ignored", "error"
OUTCOMES = (CONVERTED, COPIED, IGNORED, ERROR)

BINARY = "MapleThief"  # the fat Mach-O; png-dat packs are checked against its length table
SCRIPT_STRINGS = "en.lproj/Scripts.strings"  # dialogue text for the script listings

_REPO = Path(__file__).resolve().parents[2]
DEFAULT_APP = _REPO / "work" / "ipa" / "Payload" / "MapleThief.app"
DEFAULT_OUT = _REPO / "assets-out"


class PipelineError(Exception):
    """A file the pipeline cannot place (unclassified, no rule, output conflict)."""


# --- JSON ------------------------------------------------------------------------------------


def jsonable(value: object) -> object:
    """``value`` as plain JSON data: dataclasses become dicts of their fields, tuples lists,
    bytes lowercase hex, dates ISO 8601 text, nib references ``{"ref": index}``."""
    if isinstance(value, nib.Ref):
        return {"ref": value.index}
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        return {f.name: jsonable(getattr(value, f.name)) for f in dataclasses.fields(value)}
    if isinstance(value, dict):
        return {str(k): jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [jsonable(v) for v in value]
    if isinstance(value, (bytes, bytearray)):
        return bytes(value).hex()
    if isinstance(value, datetime.datetime):
        return value.isoformat()
    if isinstance(value, plistlib.UID):
        return {"uid": value.data}
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    raise TypeError(f"no JSON form for {type(value).__name__}")


def _scalar(v: object) -> bool:
    return v is None or isinstance(v, (bool, int, float, str))


def _emit(v: object, depth: int, out: list[str]) -> None:
    pad, inner = " " * depth, " " * (depth + 1)
    if isinstance(v, dict):
        if not v:
            out.append("{}")
            return
        out.append("{\n")
        keys = sorted(v)
        for i, k in enumerate(keys):
            if not isinstance(k, str):
                raise TypeError(f"JSON key {k!r} is not a string")
            out.append(inner + json.dumps(k, ensure_ascii=False) + ": ")
            _emit(v[k], depth + 1, out)
            out.append(",\n" if i + 1 < len(keys) else "\n")
        out.append(pad + "}")
    elif isinstance(v, list):
        if all(_scalar(x) for x in v):  # one line: map rows, palettes, coordinates
            out.append(json.dumps(v, ensure_ascii=False, allow_nan=False))
            return
        out.append("[\n")
        for i, x in enumerate(v):
            out.append(inner)
            _emit(x, depth + 1, out)
            out.append(",\n" if i + 1 < len(v) else "\n")
        out.append(pad + "]")
    elif _scalar(v):
        out.append(json.dumps(v, ensure_ascii=False, allow_nan=False))
    else:
        raise TypeError(f"not JSON data: {type(v).__name__}")


def dumps(value: object) -> bytes:
    """Deterministic UTF-8 JSON: sorted keys, one-space indent, lists of scalars on one line,
    a final newline. NaN and infinities are refused."""
    out: list[str] = []
    _emit(jsonable(value), 0, out)
    return ("".join(out) + "\n").encode("utf-8")


# --- Sources and rules -----------------------------------------------------------------------


@dataclass(frozen=True)
class Source:
    rel: str  # path relative to the .app, '/'-separated
    data: bytes
    sha256: str
    format: formats.Format | None = None


@dataclass
class Context:
    """What a conversion may read besides its own file: its declared dependencies."""

    deps: Mapping[str, Source]
    cache: dict = field(default_factory=dict)  # shared by all files of one run

    def dep(self, rel: str) -> bytes | None:
        if rel not in self.deps:
            return None
        return self.deps[rel].data


Converter = Callable[[Source, Context], dict[str, bytes]]  # -> {output path: bytes}


def _no_deps(rel: str) -> tuple[str, ...]:
    return ()


@dataclass(frozen=True)
class Rule:
    name: str
    outcome: str
    convert: Converter | None = None  # None for an ignored file
    reason: str = ""
    modules: tuple[types.ModuleType, ...] = ()  # parser code the outputs depend on
    deps: Callable[[str], tuple[str, ...]] = _no_deps  # other bundle files the conversion reads


def _out(src: Source, ext: str, data: bytes) -> dict[str, bytes]:
    return {src.rel + ext: data}


def _json(src: Source, value: Mapping[str, object]) -> dict[str, bytes]:
    return _out(src, ".json", dumps({"source": src.rel, **value}))


def _copy(src: Source, ctx: Context) -> dict[str, bytes]:
    assert src.format is not None
    src.format.decode(src.data, src.rel)  # pixels / framing must check out before copying
    return {src.rel: src.data}


def _cgbi(src: Source, ctx: Context) -> dict[str, bytes]:
    return {src.rel: cgbi.convert(src.data, src.rel)}


def _map_of(rel: str) -> tuple[str, ...]:
    return (str(PurePosixPath(rel).with_suffix(".map")),)


def _map_for(src: Source, ctx: Context) -> mapfile.MapFile | None:
    (rel,) = _map_of(src.rel)
    data = ctx.dep(rel)
    return None if data is None else mapfile.decode(data, rel)


def _map(src: Source, ctx: Context) -> dict[str, bytes]:
    m = mapfile.decode(src.data, src.rel)
    layers = [[list(row) for row in m.rows(i)] for i in range(len(m.layers))]
    return _json(
        src, {"width": m.width, "height": m.height, "empty": mapfile.EMPTY, "layers": layers}
    )


ATT_FLAGS = {
    "AIR": attfile.AIR,
    "FOOTHOLD": attfile.FOOTHOLD,
    "LADDER": attfile.LADDER,
    "LADDER_TOP": attfile.LADDER_TOP,
    "WALL": attfile.WALL,
}


def _att(src: Source, ctx: Context) -> dict[str, bytes]:
    att = formats.FORMATS["att"].decode(src.data, src.rel)  # strict, but 106.att is a raw grid
    value: dict[str, object] = {"packed": att.packed, "cells": len(att.cells), "flags": ATT_FLAGS}
    m = _map_for(src, ctx)
    if m is None:  # no .map: the grid width is unknown (106.att, docs/formats/att.md)
        value.update(grid_width=None, grid_height=None, values=list(att.cells))
    else:
        w, h = attfile.grid_size(m.width, m.height)
        if w * h != len(att.cells):
            raise FormatError(
                f"{src.rel}: {len(att.cells)} cells, the {m.width}x{m.height} map needs {w}x{h}"
            )
        value.update(grid_width=w, grid_height=h, rows=[list(r) for r in att.rows(w)])
    return _json(src, value)


def _obj(src: Source, ctx: Context) -> dict[str, bytes]:
    obj = objfile.decode(src.data, src.rel)
    m = _map_for(src, ctx)
    records = []
    for r in obj.records:
        row: dict[str, object] = {
            "cell": r.cell,
            "kind": r.kind,
            "name": r.name,
            "category": r.category,
        }
        if m is not None and not r.is_map_script:
            row["tile"] = list(r.tile(m.width))
            row["position"] = list(r.position(m.width))
        records.append(row)
    return _json(src, {"map_script": obj.map_script, "counts": obj.counts(), "records": records})


def _mapprj(src: Source, ctx: Context) -> dict[str, bytes]:
    p = mapprj.decode(src.data, src.rel)
    return _json(
        src,
        {
            "tile_width": p.tile_width,
            "tile_height": p.tile_height,
            "tilesets": list(p.tilesets),
            "tileset_paths": list(p.tileset_paths()),
        },
    )


def _sprprj(src: Source, ctx: Context) -> dict[str, bytes]:
    p = sprprj.parse(src.data, src.rel)
    res = PurePosixPath(src.rel).parent / sprprj.RES_DIR
    images = [{**jsonable(im), "file": str(res / im.name)} for im in p.images]
    return _json(src, {"images": images, "actions": jsonable(p.actions)})


def _datatable(src: Source, ctx: Context) -> dict[str, bytes]:
    stem = PurePosixPath(src.rel).stem
    if stem == "buildup":
        return _json(src, {"table": stem, "buildup": datatables.parse_buildup(src.data)})
    table = datatables.parse(stem, src.data)
    columns = datatables.layout(datatables.TABLES[stem].record)
    return _json(
        src,
        {
            "table": table.name,
            "encoding": datatables.ENCODING,
            "stride": table.stride,
            "columns": jsonable(columns),
            "records": jsonable(table.records),
        },
    )


def _strings(data: bytes, rel: str) -> dict[str, str]:
    table = formats.FORMATS["plist"].decode(data, rel)
    if not isinstance(table, dict) or not all(
        isinstance(k, str) and isinstance(v, str) for k, v in table.items()
    ):
        raise FormatError(f"{rel}: not a table of strings")
    return table


def _script(src: Source, ctx: Context) -> dict[str, bytes]:
    chunk = scriptout.decode(src.data, src.rel)
    raw = ctx.dep(SCRIPT_STRINGS)
    strings = None if raw is None else _strings(raw, SCRIPT_STRINGS)
    return _out(src, ".dis", scriptout.listing(chunk, strings).encode("utf-8"))


def _plist(src: Source, ctx: Context) -> dict[str, bytes]:
    if src.rel.endswith(".strings"):
        return _json(src, {"strings": _strings(src.data, src.rel)})
    return _json(src, {"plist": formats.FORMATS["plist"].decode(src.data, src.rel)})


def _nib(src: Source, ctx: Context) -> dict[str, bytes]:
    archive = nib.parse(src.data, src.rel)
    objects = []
    for o in archive.objects:
        values = []
        for v in o.values:
            row = {"key": v.key, "type": nib.TYPE_NAMES[v.type], "value": jsonable(v.value)}
            floats = v.floats()
            if floats is not None:
                row["floats"] = list(floats)
            values.append(row)
        objects.append({"index": o.index, "class": archive.class_name(o), "values": values})
    return _json(
        src,
        {
            "format_version": archive.format_version,
            "coder_version": archive.coder_version,
            "classes": jsonable(archive.classes),
            "objects": objects,
        },
    )


def armv7_image(data: bytes) -> macho.MachO:
    """The armv7 slice of the fat binary, for the png-dat length table."""
    archs = fat.parse_fat(data)
    return macho.MachO(fat.extract(data, fat.find_arch(archs, "armv7")))


def _pack(src: Source, ctx: Context) -> dict[str, bytes]:
    pack = pngdat.BY_PATH.get(src.rel)
    if pack is None:
        raise FormatError(f"{src.rel}: not a pack the game loads (mtport.pngdat.PACKS)")
    lengths = None
    binary = ctx.deps.get(BINARY)
    if binary is not None:
        key = ("armv7", binary.sha256)
        if key not in ctx.cache:
            ctx.cache[key] = armv7_image(binary.data)
        lengths = pngdat.length_row(ctx.cache[key], pack.id)
    files, rows, _ = pngdat.pack_files(pack, src.data, lengths)
    base = str(PurePosixPath(src.rel).with_suffix(""))
    out = {f"{base}/{name}": data for name, data in files.items()}
    entry = {**pngdat.pack_entry(pack, src.data, rows), "length_table_checked": lengths is not None}
    out[f"{base}/index.json"] = dumps(entry)
    return out


def _binary_dep(rel: str) -> tuple[str, ...]:
    return (BINARY,)


def _strings_dep(rel: str) -> tuple[str, ...]:
    return (SCRIPT_STRINGS,)


_COPY = "standard format, already readable: copied unchanged after its decoder checked it"
RULES: dict[str, Rule] = {
    r.name: r
    for r in (
        Rule("map", CONVERTED, _map, modules=(mapfile,)),
        Rule("att", CONVERTED, _att, modules=(attfile, mapfile, formats), deps=_map_of),
        Rule("obj", CONVERTED, _obj, modules=(objfile, mapfile), deps=_map_of),
        Rule("mapprj", CONVERTED, _mapprj, modules=(mapprj,)),
        Rule("sprprj", CONVERTED, _sprprj, modules=(sprprj,)),
        Rule("png-dat", CONVERTED, _pack, modules=(pngdat, fat, macho), deps=_binary_dep),
        Rule("data-table", CONVERTED, _datatable, modules=(datatables,)),
        Rule("script-out", CONVERTED, _script, modules=(scriptout, formats), deps=_strings_dep),
        Rule("nib", CONVERTED, _nib, modules=(nib,)),
        Rule("cgbi-png", CONVERTED, _cgbi, modules=(cgbi,)),
        Rule("plist", CONVERTED, _plist, modules=(formats,)),
        Rule("png", COPIED, _copy, _COPY + " (every pixel decoded)", modules=(formats,)),
        Rule("mp3", COPIED, _copy, _COPY + " (framing only)", modules=(formats,)),
        Rule("wav", COPIED, _copy, _COPY + " (framing only)", modules=(formats,)),
        Rule(
            "macho",
            IGNORED,
            reason="the game binary: input of the RE tools (work/slices/, scripts/re-analyze), "
            "not an asset",
        ),
        Rule("empty", IGNORED, reason="0-byte armv6 stub left by the decryption (ANALYSIS.md)"),
        Rule("pkginfo", IGNORED, reason="bundle metadata ('APPL' + creator code)"),
        Rule("text", IGNORED, reason="the Chartboost SDK's README, not game data"),
    )
}

_SIGNATURE = Rule("code-signing", IGNORED, reason="code-signing data written by Apple's tools")
OVERRIDES: dict[str, Rule] = {
    "_CodeSignature/CodeResources": _SIGNATURE,
    "CodeResources": _SIGNATURE,  # a byte-identical copy of the one above (ANALYSIS.md)
    "ResourceRules.plist": _SIGNATURE,
}


def rule_for(rel: str, fmt: formats.Format) -> Rule:
    if rel in OVERRIDES:
        return OVERRIDES[rel]
    if fmt.name not in RULES:
        raise PipelineError(f"format {fmt.name!r} has no pipeline rule (mtport.pipeline.RULES)")
    return RULES[fmt.name]


# --- Rule keys: invalidate outputs when the code that made them changes -----------------------


def _mtport_modules(mod: types.ModuleType) -> set[types.ModuleType]:
    """``mod`` and every ``mtport`` module it imports, directly or through names it imported."""
    seen: set[types.ModuleType] = set()
    todo = [mod]
    while todo:
        m = todo.pop()
        if m in seen:
            continue
        seen.add(m)
        for v in vars(m).values():
            if isinstance(v, types.ModuleType):
                name = v.__name__
            else:
                name = getattr(v, "__module__", None)
                if not isinstance(name, str):
                    continue
            if name.startswith("mtport.") and name in sys.modules:
                todo.append(sys.modules[name])
    return seen


_KEYS: dict[str, str] = {}


def rule_key(rule: Rule) -> str:
    """``<rule name>:<hash>`` of this module's source and the rule's modules (with imports).

    Files are keyed by file name, not module name, so ``python3 -m mtport.pipeline`` (where
    this module is ``__main__``) and an import give the same keys."""
    if rule.name not in _KEYS:
        files = {Path(__file__)}
        for m in rule.modules:
            files |= {Path(x.__file__ or "") for x in _mtport_modules(m)}
        h = hashlib.sha256(f"{SCHEMA}:{rule.name}:{rule.outcome}:{rule.reason}".encode())
        for path in sorted(files, key=lambda p: p.name):
            h.update(b"\0" + path.name.encode() + b"\0" + path.read_bytes())
        _KEYS[rule.name] = f"{rule.name}:{h.hexdigest()[:16]}"
    return _KEYS[rule.name]


# --- Planning --------------------------------------------------------------------------------


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def check_output_path(rel: str) -> str:
    """``rel`` if it is a safe output path inside assets-out/ that the pipeline may own."""
    p = PurePosixPath(rel)
    if (
        not rel
        or "\\" in rel
        or p.is_absolute()
        or any(part in ("", ".", "..") for part in rel.split("/"))
        or p.parts[0] in RESERVED
        or rel == MANIFEST
    ):
        raise PipelineError(f"output path {rel!r} is outside the pipeline's part of assets-out/")
    return rel


def scan(app: Path) -> list[Source]:
    """Every file under ``app``, sorted by path, read and hashed (classified later)."""
    out = []
    for path in sorted(app.rglob("*")):
        if path.is_dir() and not path.is_symlink():
            continue
        rel = path.relative_to(app).as_posix()
        if path.is_symlink() or not path.is_file():
            out.append(Source(rel, b"", ""))  # reported as an error by plan()
            continue
        data = path.read_bytes()
        out.append(Source(rel, data, _sha(data)))
    return out


@dataclass
class Plan:
    """What a run produces: the new manifest entries and the files to write."""

    entries: dict[str, dict] = field(default_factory=dict)  # source rel -> manifest entry
    files: dict[str, bytes] = field(default_factory=dict)  # output rel -> bytes (new work only)
    counts: Counter = field(default_factory=Counter)  # converted/copied/ignored/skipped/error
    errors: list[str] = field(default_factory=list)

    @property
    def outputs(self) -> dict[str, str]:
        """Every output path of the new manifest -> its source."""
        return {o: rel for rel, e in self.entries.items() for o in e["outputs"]}


def load_manifest(out: Path) -> dict[str, dict]:
    """The ``files`` table of an existing manifest, or {} if there is none we can use."""
    try:
        data = json.loads((out / MANIFEST).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if not isinstance(data, dict) or data.get("pipeline", {}).get("schema") != SCHEMA:
        return {}
    files = data.get("files")
    return files if isinstance(files, dict) else {}


def _old_outputs(old: Mapping[str, dict]) -> set[str]:
    out = set()
    for entry in old.values():
        outputs = entry.get("outputs") if isinstance(entry, dict) else None
        for rel in outputs if isinstance(outputs, dict) else ():
            try:
                out.add(check_output_path(rel))
            except PipelineError:
                pass  # never delete anything outside our part of assets-out/
    return out


def _file_sha(path: Path) -> str | None:
    try:
        return _sha(path.read_bytes()) if path.is_file() and not path.is_symlink() else None
    except OSError:
        return None


def _up_to_date(prev: object, entry: dict, out: Path) -> bool:
    if not isinstance(prev, dict) or prev.get("outcome") == ERROR:
        return False
    keys = ("sha256", "size", "format", "outcome", "rule", "deps", "reason")
    if any(prev.get(k) != entry[k] for k in keys):
        return False
    outputs = prev.get("outputs")
    if not isinstance(outputs, dict):
        return False
    try:
        return all(_file_sha(out / check_output_path(o)) == h for o, h in outputs.items())
    except PipelineError:
        return False


def _error(plan: Plan, rel: str, sha256: str, size: int, fmt: str | None, reason: str) -> None:
    plan.entries[rel] = {
        "sha256": sha256,
        "size": size,
        "format": fmt,
        "outcome": ERROR,
        "rule": None,
        "deps": {},
        "reason": reason,
        "outputs": {},
    }
    plan.counts[ERROR] += 1
    plan.errors.append(f"{rel}: {reason}")


def plan(app: Path, out: Path) -> Plan:
    """Classify and (where needed) convert every file in memory. Writes nothing."""
    sources = scan(app)
    by_rel = {s.rel: s for s in sources if s.sha256}
    old = load_manifest(out)
    result = Plan()
    produced: dict[str, dict[str, bytes]] = {}
    cache: dict = {}
    for src in sources:
        if not src.sha256:
            _error(result, src.rel, "", 0, None, "not a regular file (symlinks are not followed)")
            continue
        fmt = None
        try:
            fmt = formats.classify(src.rel, src.data)
            if fmt is None:
                raise PipelineError("unclassified: no format in mtport.formats matches it")
            src = dataclasses.replace(src, format=fmt)
            rule = rule_for(src.rel, fmt)
            deps = {d: by_rel[d] for d in rule.deps(src.rel) if d in by_rel and d != src.rel}
            entry = {
                "sha256": src.sha256,
                "size": len(src.data),
                "format": fmt.name,
                "outcome": rule.outcome,
                "rule": rule_key(rule),
                "deps": {d: s.sha256 for d, s in sorted(deps.items())},
                "reason": rule.reason,
            }
            prev = old.get(src.rel)
            if _up_to_date(prev, entry, out):
                result.entries[src.rel] = {**entry, "outputs": dict(prev["outputs"])}
                result.counts["skipped"] += 1
                continue
            files = rule.convert(src, Context(deps, cache)) if rule.convert else {}
            for o in files:
                check_output_path(o)
        except Exception as e:  # noqa: BLE001 - untrusted input: report every failure, go on
            reason = str(e) if isinstance(e, (FormatError, PipelineError)) else repr(e)
            _error(result, src.rel, src.sha256, len(src.data), fmt.name if fmt else None, reason)
            continue
        entry["outputs"] = {o: _sha(d) for o, d in sorted(files.items())}
        result.entries[src.rel] = entry
        produced[src.rel] = files
        result.counts[rule.outcome] += 1
    _resolve_conflicts(result, produced)
    for files in produced.values():
        result.files.update(files)
    return result


def _resolve_conflicts(result: Plan, produced: dict[str, dict[str, bytes]]) -> None:
    """Fail the later source (by path) of any two whose outputs clash: the same path (ignoring
    case, for macOS and Windows), or one output a directory prefix of another."""
    owner: dict[str, str] = {}  # lowercased output -> source
    dirs: dict[str, str] = {}  # lowercased directory of some output -> source
    for rel in sorted(result.entries):
        entry = result.entries[rel]
        clash = None
        for o in entry["outputs"]:
            low = o.lower()
            parents = [str(p).lower() for p in PurePosixPath(o).parents if str(p) != "."]
            if low in owner or low in dirs:
                clash = f"output {o} clashes with an output of {owner.get(low) or dirs[low]}"
            elif any(p in owner for p in parents):
                p = next(p for p in parents if p in owner)
                clash = f"output {o} would be inside the output file {p} of {owner[p]}"
            if clash:
                break
        if clash:
            result.counts["skipped" if produced.pop(rel, None) is None else entry["outcome"]] -= 1
            _error(result, rel, entry["sha256"], entry["size"], entry["format"], clash)
            continue
        for o in entry["outputs"]:
            owner[o.lower()] = rel
            for p in PurePosixPath(o).parents:
                if str(p) != ".":
                    dirs.setdefault(str(p).lower(), rel)


def manifest_bytes(result: Plan) -> bytes:
    rules = {r.name: rule_key(r) for r in (*RULES.values(), *OVERRIDES.values())}
    totals = Counter(e["outcome"] for e in result.entries.values())
    return dumps(
        {
            "pipeline": {"schema": SCHEMA, "rules": rules},
            "outcomes": {k: totals.get(k, 0) for k in OUTCOMES},
            "files": result.entries,
        }
    )


# --- Running ---------------------------------------------------------------------------------


def _write(path: Path, data: bytes) -> bool:
    """Write ``data`` atomically unless the file already holds exactly it."""
    if path.is_file() and not path.is_symlink() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)
    return True


def _prune(path: Path, top: Path) -> None:
    """Remove empty directories from ``path`` up to (not including) ``top``."""
    while path != top and top in path.parents:
        try:
            path.rmdir()
        except OSError:
            return
        path = path.parent


@dataclass
class Result:
    plan: Plan
    stale: list[str]  # outputs of the old manifest that are no longer produced
    written: list[str]  # outputs (and the manifest) whose bytes changed
    seconds: float = 0.0

    @property
    def ok(self) -> bool:
        return not self.plan.errors

    def summary(self) -> str:
        c = self.plan.counts
        return (
            f"files={len(self.plan.entries)} converted={c[CONVERTED]} copied={c[COPIED]} "
            f"ignored={c[IGNORED]} skipped(unchanged)={c['skipped']} errors={c[ERROR]} "
            f"removed={len(self.stale)}"
        )


def _check_dirs(app: Path, out: Path) -> None:
    if not app.is_dir():
        raise PipelineError(f"{app}: no such directory (run scripts/bootstrap)")
    a, o = app.resolve(), out.resolve()
    if a == o or a in o.parents or o in a.parents:
        raise PipelineError(f"{out}: the output directory must not overlap the bundle {app}")


def run(app: Path, out: Path, *, write: bool = True) -> Result:
    """Bring ``out`` up to date with ``app`` (``write=False``: only work out what would change)."""
    start = time.monotonic()
    app, out = Path(app), Path(out)
    _check_dirs(app, out)
    old = load_manifest(out)
    result = plan(app, out)
    new_outputs = set(result.outputs)
    stale = sorted(o for o in _old_outputs(old) - new_outputs if (out / o).is_file())
    manifest = manifest_bytes(result)
    written = []
    if write:
        for o in stale:  # before writing: on a case-insensitive disk a stale a/B.png is a/b.png
            (out / o).unlink()
            _prune((out / o).parent, out)
        for o in sorted(result.files):
            if _write(out / o, result.files[o]):
                written.append(o)
        if _write(out / MANIFEST, manifest):
            written.append(MANIFEST)
    else:
        for o in sorted(result.files):
            if _file_sha(out / o) != _sha(result.files[o]):
                written.append(o)
        if _file_sha(out / MANIFEST) != _sha(manifest):
            written.append(MANIFEST)
    return Result(result, stale, written, time.monotonic() - start)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.pipeline", description=__doc__.splitlines()[0]
    )
    parser.add_argument("command", nargs="?", choices=("run", "check"), default="run")
    parser.add_argument("--app", type=Path, default=DEFAULT_APP, help="MapleThief.app directory")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output directory")
    args = parser.parse_args(argv)
    write = args.command == "run"
    try:
        result = run(args.app, args.out, write=write)
    except (OSError, PipelineError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for line in result.plan.errors:
        print(f"error: {line}", file=sys.stderr)
    if not write:
        for o in result.stale:
            print(f"stale: {o}")
        for o in result.written:
            print(f"out of date: {o}")
    done = "would write" if not write else "wrote"
    print(f"{result.summary()} ({done} {len(result.written)} files in {result.seconds:.1f}s)")
    if not result.ok:
        return 1
    return 1 if not write and (result.stale or result.written) else 0


if __name__ == "__main__":
    sys.exit(main())
