"""Which format each file in MapleThief.app has, and which parser reads it.

The M1 exit criterion is "every file type in the .app has a spec in docs/formats/ and a
parser that decodes all shipped files". This module is the single table behind that check:
``classify(rel)`` maps a bundle-relative path to a ``Format``, and ``Format.decode`` parses
the file's bytes with the format's own parser (raising on any error). The real-asset test
``tests/test_formats_real.py`` walks the whole bundle through it.

Three kinds of format:

- ``spec``: reverse-engineered here, with a spec in ``docs/formats/`` and a parser in
  ``tools/mtport/``. Every shipped file must decode.
- ``standard``: a published format (PNG, MP3, WAV, binary plist, Mach-O, ...). It gets a
  framing check here; the full decoders are the pinned libraries in ADR 0003. PNGs are the
  exception: ``mtport.png`` decodes every pixel.
- ``deferred``: specified by a later milestone, named by its issue. Files are framing-checked.
  (None is left: CgBI PNGs, the last one, are a spec format since #24.)

Command line::

    python3 -m mtport.formats [APP_DIR]   # per-format file counts; exit 1 on any failure
"""

from __future__ import annotations

import argparse
import plistlib
import sys
from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from mtport import (
    attfile,
    audio,
    bootstrap,
    cgbi,
    datatables,
    macho,
    mapfile,
    mapprj,
    nib,
    objfile,
    png,
    pngdat,
    pnginfo,
    scriptout,
    sprprj,
)
from mtport.bytereader import FormatError

SPEC, STANDARD, DEFERRED = "spec", "standard", "deferred"


@dataclass(frozen=True)
class Format:
    name: str
    kind: str
    doc: str  # docs/formats/<doc> for a spec, else what defines the format
    decode: Callable[[bytes, str], object]


def _png(data: bytes, name: str) -> png.Image:
    if cgbi.is_cgbi(data):
        raise FormatError(f"{name}: CgBI PNG where a standard PNG was expected")
    return png.decode(data, name)


def _cgbi(data: bytes, name: str) -> png.Image:
    if not cgbi.is_cgbi(data):
        raise FormatError(f"{name}: expected a CgBI PNG")
    return cgbi.decode(data, name)


def _plist(data: bytes, name: str) -> object:
    try:
        return plistlib.loads(data)
    except Exception as e:  # plistlib raises several types for malformed input
        raise FormatError(f"{name}: not a property list ({e})") from e


def _mp3(data: bytes, name: str) -> audio.Mp3Info:
    return audio.read_mp3(data, name)  # every frame header walked (#29)


def _wav(data: bytes, name: str) -> audio.WavInfo:
    return audio.read_wav(data, name)  # RIFF chunks, fmt and data (#29)


def _pkginfo(data: bytes, name: str) -> bytes:
    if len(data) != 8 or not data.startswith(b"APPL"):
        raise FormatError(f"{name}: PkgInfo is not 'APPL' + 4-byte creator")
    return data


def _empty(data: bytes, name: str) -> bytes:
    if data:
        raise FormatError(f"{name}: expected an empty file, got {len(data)} bytes")
    return data


def _text(data: bytes, name: str) -> str:
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as e:
        raise FormatError(f"{name}: not UTF-8 text") from e


def _fat_macho(data: bytes, name: str) -> list:
    from mtport import fat

    return [macho.MachO(fat.extract(data, a)) for a in fat.parse_fat(data)]


def _datatable(data: bytes, name: str) -> object:
    stem = PurePosixPath(name).stem
    if stem == "buildup":
        return datatables.parse_buildup(data)
    return datatables.parse(stem, data)


def _att(data: bytes, name: str) -> attfile.AttFile:
    # 106.att is the one headerless raw grid (docs/formats/att.md); every other file is strict.
    if PurePosixPath(name).name == "106.att":
        return attfile.decode_any(data, name)
    return attfile.decode(data, name)


FORMATS = {
    f.name: f
    for f in (
        Format("map", SPEC, "map.md", mapfile.decode),
        Format("att", SPEC, "att.md", _att),
        Format("obj", SPEC, "obj.md", objfile.decode),
        Format("mapprj", SPEC, "mapprj.md", mapprj.decode),
        Format("sprprj", SPEC, "sprprj.md", lambda d, n: sprprj.parse(d)),
        Format("png-dat", SPEC, "png-dat.md", pngdat.split),
        Format("data-table", SPEC, "data-tables.md", _datatable),
        Format("script-out", SPEC, "script-out.md", scriptout.decode),
        Format("nib", SPEC, "nibarchive.md", nib.parse),
        Format("png", STANDARD, "PNG (ISO/IEC 15948), mtport.png", _png),
        Format("cgbi-png", SPEC, "cgbi.md", _cgbi),
        Format(
            "mp3", STANDARD, "MPEG-1/2 Layer III (headers: mtport.audio; dr_mp3, ADR 0003)", _mp3
        ),
        Format("wav", STANDARD, "RIFF WAVE PCM (headers: mtport.audio)", _wav),
        Format("plist", STANDARD, "Apple property list (plistlib)", _plist),
        Format("macho", STANDARD, "fat Mach-O (mtport.fat, mtport.macho)", _fat_macho),
        Format("pkginfo", STANDARD, "bundle PkgInfo", _pkginfo),
        Format("text", STANDARD, "UTF-8 text (Chartboost's README)", _text),
        Format("empty", STANDARD, "0-byte MapleThief_armv6 (Clutch artifact, ANALYSIS.md)", _empty),
    )
}

# Code-signing files: plists Apple writes; not game data, but they still have to parse.
_PLIST_NAMES = {"Info.plist", "ResourceRules.plist", "CodeResources"}


def classify(rel: str, data: bytes) -> Format | None:
    """The format of the bundle file at ``rel`` (relative to MapleThief.app), or None.

    PNGs are told apart by content (a ``CgBI`` chunk), not by folder: three root PNGs are
    standard ones.
    """
    path = PurePosixPath(rel)
    top, suffix, name = path.parts[0], path.suffix, path.name
    if suffix in (".map", ".att", ".obj", ".mapprj", ".sprprj"):
        return FORMATS[suffix[1:]]
    if suffix == ".out" and top == "script":
        return FORMATS["script-out"]
    if suffix == ".nib":
        return FORMATS["nib"]
    if suffix == ".dat" and top == "png":
        return FORMATS["png-dat"]
    if suffix == ".dat" and len(path.parts) == 3 and path.parts[1] == "data":
        return FORMATS["data-table"]
    if suffix == ".png":
        return FORMATS["cgbi-png" if pnginfo.read_info(data, rel).cgbi else "png"]
    if suffix in (".mp3", ".wav"):
        return FORMATS[suffix[1:]]
    if suffix == ".strings" or name in _PLIST_NAMES:
        return FORMATS["plist"]
    if rel == "MapleThief":
        return FORMATS["macho"]
    if rel == "MapleThief_armv6":
        return FORMATS["empty"]
    if name == "PkgInfo":
        return FORMATS["pkginfo"]
    if name == "README.md" and len(path.parts) == 1:
        return FORMATS["text"]
    return None


@dataclass
class Report:
    counts: Counter
    unclassified: list[str]
    failures: list[str]


def check(app_dir: Path) -> Report:
    """Classify and decode every file under ``app_dir``."""
    report = Report(Counter(), [], [])
    for path in sorted(p for p in app_dir.rglob("*") if p.is_file()):
        rel = path.relative_to(app_dir).as_posix()
        data = path.read_bytes()
        try:
            fmt = classify(rel, data)
        except FormatError as e:
            report.failures.append(f"{rel}: {e}")
            continue
        if fmt is None:
            report.unclassified.append(rel)
            continue
        report.counts[fmt.name] += 1
        try:
            fmt.decode(data, rel)
        except FormatError as e:
            report.failures.append(f"{rel}: {e}")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.formats", description=__doc__.splitlines()[0]
    )
    parser.add_argument("app_dir", nargs="?", default=bootstrap.APP_DIR)
    args = parser.parse_args(argv)
    report = check(Path(args.app_dir))
    for name, n in sorted(report.counts.items()):
        fmt = FORMATS[name]
        print(f"{name:<12} {fmt.kind:<9} {n:>5}  {fmt.doc}")
    for rel in report.unclassified:
        print(f"unclassified: {rel}", file=sys.stderr)
    for line in report.failures:
        print(f"failed: {line}", file=sys.stderr)
    total = sum(report.counts.values())
    print(f"files={total} unclassified={len(report.unclassified)} failures={len(report.failures)}")
    return 1 if report.unclassified or report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
