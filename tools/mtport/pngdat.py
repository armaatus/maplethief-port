"""``png/**/*.dat`` image packs: several complete PNG files stored back to back.

Spec: docs/formats/png-dat.md. Each of the 40 ``.dat`` files under ``png/`` is a *pack*:
1 to 255 ordinary PNG files concatenated with no header, padding or trailer. The engine does
not parse the PNG framing to split a pack. It reads the whole file and cuts it with a table
of byte lengths compiled into the binary, ``u32 lengths[40][255]`` at ``0xFC740`` in
``__DATA,__data`` (row stride ``0x3FC``), one row per pack id. Each piece then becomes an
ordinary image object, exactly as if it had been loaded from its own ``.png`` file.

Layout::

    pack = png[0] png[1] ... png[n-1]            n = images in the pack, 1..255
    png  = 89 50 4E 47 0D 0A 1A 0A  IHDR ... IEND  (a complete standard PNG)
    lengths[id][i] = byte length of png[i];  lengths[id][n..254] = 0

This module splits packs (by the PNG framing, and optionally by the binary's length table, so
the two can be checked against each other), names the sub-images by how the game indexes them,
and extracts them. Every sub-image is written byte for byte as stored, so no pixel is re-encoded.
On top of that the two packs the game draws in parts are also cut into those parts: the
``num.dat`` digit strips (10 glyphs each) and the ``box.dat`` corner sheets (4 quadrants each).
Those crops are decoded and re-encoded with the stdlib-only indexed-PNG codec below.

The files are untrusted input: every offset is bounds-checked and every chunk CRC verified.

Command line (``PYTHONPATH=tools python3 -m mtport.pngdat ...``)::

    list     [--app DIR]                         one line per pack
    extract  [--app DIR] [--out DIR] [--binary FILE | --no-binary]
             writes assets-out/png-dat/<folder>/<stem>/<name>.png plus index.json
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

from mtport import pnginfo
from mtport.bytereader import FormatError
from mtport.names import hexaddr

# --- The engine's length table (docs/formats/png-dat.md -> "Length table") -----------------

TABLE_ADDR = 0xFC740  # __DATA,__data: u32 lengths[PACK_COUNT][ROW_LEN]
ROW_LEN = 255  # entries per row: a pack holds at most 255 images
ROW_STRIDE = ROW_LEN * 4  # 0x3FC, the `id * 0x3FC` in every pack loader
PACK_COUNT = 40

# Loader helpers on the resource manager `*(0xF143C)` (function pointers, Thumb bit set).
LOAD_FLAT = 0xF5A9  # (mgr, path, CImage[count], count, id)
LOAD_2D = 0xF6A5  # (mgr, path, CImage*[rows] -> CImage[cols], rows, cols, id)
LOAD_3D = 0xF445  # (mgr, path, CImage**[a] -> CImage*[b] -> CImage[c], a, b, c, id)
LOAD_ONE = 0xF34D  # (mgr, path, index, count, id, CImage*): only image `index`
# Each helper's name in the Ghidra call listing (tools/ghidra/scripts/pngdat_calls.py).
HELPER_NAMES = {
    LOAD_FLAT: "load_flat",
    LOAD_2D: "load_2d",
    LOAD_3D: "load_3d",
    LOAD_ONE: "load_one",
}

# Where each helper forms the table address: `ldr rN, [pc, #lit]` then `add rN, pc` at these
# addresses (0xF34D does it twice: once to skip images, once for the one it loads).
TABLE_REFS = {
    LOAD_FLAT: (0xF5F4,),
    LOAD_2D: (0xF6FE,),
    LOAD_3D: (0xF4AC,),
    LOAD_ONE: (0xF3A0, 0xF3D2),
}

PARTS_DIGITS = "digits"  # sub-image is a strip of 10 equal glyphs '0'..'9' (0x3A07D, 0x3A4D9)
PARTS_CORNERS = "corners"  # sub-image is a 2x2 sheet of box corners (0x3A899)


@dataclass(frozen=True)
class Pack:
    """One ``.dat`` pack: where the game loads it and how it indexes the images."""

    id: int  # row in the length table
    path: str  # relative to the .app
    shape: tuple[int, ...]  # array the loader fills; product = image count
    loader: int | None = None  # function that loads it (pointer), None = never loaded
    call: int | None = None  # address of the load call
    helper: int | None = None  # LOAD_FLAT / LOAD_2D / LOAD_3D
    field: int | None = None  # offset of the image array in the resource manager
    parts: str | None = None  # PARTS_* when the game draws parts of each image
    note: str = ""

    @property
    def count(self) -> int:
        n = 1
        for d in self.shape:
            n *= d
        return n

    @property
    def table_row(self) -> int:
        return TABLE_ADDR + self.id * ROW_STRIDE


_S, _T, _P, _M, _L = 0x1359D, 0x16AF1, 0x14C59, 0x12E6D, 0x1267D
_F, _2, _3 = LOAD_FLAT, LOAD_2D, LOAD_3D

# Pack id order is the table order: folder by folder, alphabetical inside a folder, except
# that popmenu/picon.dat comes last in popmenu (it was probably added later).
PACKS: tuple[Pack, ...] = (
    Pack(0, "png/static/acceptrefuse.dat", (2, 2), _S, 0x141D8, _2, 0xF0),
    Pack(1, "png/static/arrow.dat", (11, 2), _S, 0x141B2, _2, 0xEC),
    Pack(2, "png/static/box.dat", (49,), _S, 0x14154, _F, 0xC, PARTS_CORNERS),
    Pack(3, "png/static/keyinfo.dat", (4,), _S, 0x1421C, _F, 0x148),
    Pack(4, "png/static/num.dat", (14,), _S, 0x1418C, _F, 0x8, PARTS_DIGITS),
    Pack(5, "png/static/scrollbar.dat", (2,), _S, 0x14170, _F, 0x10),
    Pack(6, "png/static/yesno.dat", (3, 2), _S, 0x141FE, _2, 0xF4),
    Pack(7, "png/mainmenu/cmenu.dat", (10,), note="not loaded by 1.5"),
    Pack(8, "png/mainmenu/gameqa.dat", (4,), note="not loaded by 1.5"),
    Pack(9, "png/mainmenu/hmenu.dat", (10,), note="not loaded by 1.5"),
    Pack(10, "png/mainmenu/menu.dat", (10,), note="not loaded by 1.5"),
    Pack(11, "png/mainmenu/point.dat", (3,), note="not loaded by 1.5"),
    Pack(12, "png/skill/skillico.dat", (22,), _T, 0x17E62, _F, 0x2B0, note="skill.dat icon"),
    Pack(13, "png/item/equip.dat", (73,), _T, 0x17EB6, _F, 0x2D8, note="item_equip.dat icon"),
    Pack(14, "png/item/etc.dat", (109,), _T, 0x17EE8, _F, 0x2DC, note="item_etc.dat icon"),
    Pack(15, "png/game/miss.dat", (2,), _T, 0x17F6E, _F, 0x308),
    Pack(16, "png/game/spear.dat", (2,), _T, 0x17F1A, _F, 0x2E0),
    Pack(17, "png/game/storm.dat", (3, 2, 2), _T, 0x17FB0, _3, 0x30C),
    Pack(18, "png/game/yeti.dat", (4,), _T, 0x17FE2, _F, 0x310),
    Pack(19, "png/ui/bossface.dat", (5,), _T, 0x18162, _F, 0x408),
    Pack(20, "png/ui/mktext.dat", (4,), _T, 0x18130, _F, 0x394),
    Pack(21, "png/ui/npcnotify.dat", (5,), _T, 0x180CC, _F, 0x38C),
    Pack(22, "png/ui/qnline.dat", (2,), _T, 0x180FE, _F, 0x390),
    Pack(23, "png/ui/reqmark.dat", (2,), _T, 0x18068, _F, 0x33C),
    Pack(24, "png/ui/rewmark.dat", (2,), _T, 0x1809A, _F, 0x364),
    Pack(25, "png/ui/ui.dat", (11,), _T, 0x18036, _F, 0x338),
    Pack(26, "png/minimap/continent.dat", (4,), _M, 0x131B4, _F, 0x528),
    Pack(27, "png/minimap/worldmap.dat", (4,), _M, 0x13196, _F, 0x500),
    Pack(28, "png/popmenu/class.dat", (5,), _P, 0x161CC, _F, 0x460),
    Pack(29, "png/popmenu/classMark.dat", (2,), _P, 0x161F4, _F, 0x468),
    Pack(30, "png/popmenu/optIcon.dat", (4, 2), _P, 0x160D0, _2, 0x440),
    Pack(31, "png/popmenu/optText.dat", (4, 2), _P, 0x160A0, _2, 0x438),
    Pack(32, "png/popmenu/ptext.dat", (6,), _P, 0x16070, _F, 0x444),
    Pack(33, "png/popmenu/skillup.dat", (2,), _P, 0x16154, _F, 0x454),
    Pack(34, "png/popmenu/statInfo.dat", (8,), _P, 0x161A4, _F, 0x45C),
    Pack(35, "png/popmenu/stattext.dat", (3,), _P, 0x1617C, _F, 0x458),
    Pack(36, "png/popmenu/sticon.dat", (3,), _P, 0x16104, _F, 0x448),
    Pack(37, "png/popmenu/sttext.dat", (3,), _P, 0x1612C, _F, 0x44C),
    Pack(38, "png/popmenu/picon.dat", (6, 2), _P, 0x16038, _2, 0x430),
    Pack(39, "png/logo/logo.dat", (12,), _L, 0x12794, _F, 0x4),
)
BY_PATH = {p.path: p for p in PACKS}

# --- Splitting -------------------------------------------------------------------------------


@dataclass(frozen=True)
class SubImage:
    index: int
    offset: int  # byte offset of the PNG inside the pack
    data: bytes  # the complete PNG, as stored
    info: pnginfo.PngInfo = field(compare=False)

    @property
    def length(self) -> int:
        return len(self.data)


def split(data: bytes, name: str = "<pack>") -> list[SubImage]:
    """Split a pack by its PNG framing. Raises :class:`FormatError` on any malformed byte."""
    data = bytes(data)
    if not data:
        raise FormatError(f"{name}: empty pack")
    images: list[SubImage] = []
    pos = 0
    while pos < len(data):
        if len(images) == ROW_LEN:
            raise FormatError(f"{name}: more than {ROW_LEN} images")
        if data[pos : pos + len(pnginfo.SIGNATURE)] != pnginfo.SIGNATURE:
            raise FormatError(
                f"{name}: offset 0x{pos:X}: {len(data) - pos} bytes after image {len(images)} "
                "are not a PNG"
            )
        end = pnginfo.png_end(data, pos, name)
        png = data[pos:end]
        images.append(SubImage(len(images), pos, png, pnginfo.read_info(png, name)))
        pos = end
    return images


def split_by_lengths(data: bytes, lengths: Sequence[int], name: str = "<pack>") -> list[bytes]:
    """Cut a pack the way the engine does: consecutive pieces of the given byte lengths."""
    if sum(lengths) != len(data):
        raise FormatError(f"{name}: lengths add up to {sum(lengths)}, the file has {len(data)}")
    out, pos = [], 0
    for n in lengths:
        if n <= 0:
            raise FormatError(f"{name}: non-positive length {n}")
        out.append(bytes(data[pos : pos + n]))
        pos += n
    return out


def length_row(image, pack_id: int) -> tuple[int, ...]:
    """The non-zero prefix of row ``pack_id`` of the length table, read from a Mach-O image
    (:class:`mtport.macho.MachO` of the armv7 slice)."""
    if not 0 <= pack_id < PACK_COUNT:
        raise ValueError(f"pack id {pack_id} outside 0..{PACK_COUNT - 1}")
    row = struct.unpack("<255I", image.read(TABLE_ADDR + pack_id * ROW_STRIDE, ROW_STRIDE))
    n = ROW_LEN
    while n and row[n - 1] == 0:
        n -= 1
    if 0 in row[:n]:
        raise FormatError(f"length table row {pack_id}: zero entry before the last image")
    return row[:n]


# --- Naming ----------------------------------------------------------------------------------


def coords(shape: Sequence[int], index: int) -> tuple[int, ...]:
    """Array coordinates of the ``index``-th image (row-major, the order the loaders use)."""
    out = []
    for d in reversed(shape):
        out.append(index % d)
        index //= d
    return tuple(reversed(out))


def image_name(shape: Sequence[int], index: int) -> str:
    """``NN`` for a flat array, ``NN_i_j`` (``NN_i_j_k``) for the 2-D (3-D) arrays."""
    total = 1
    for d in shape:
        total *= d
    width = max(2, len(str(total - 1)))
    stem = f"{index:0{width}d}"
    if len(shape) > 1:
        stem += "".join(f"_{c}" for c in coords(shape, index))
    return stem


# --- Indexed (palette) PNG decode / encode, stdlib only ---------------------------------------


@dataclass(frozen=True)
class Indexed:
    """A palette image: one palette index per pixel, ``rows[y][x]``."""

    width: int
    height: int
    rows: tuple[bytes, ...]
    plte: bytes
    trns: bytes | None

    def crop(self, x: int, y: int, w: int, h: int) -> Indexed:
        if w <= 0 or h <= 0 or x < 0 or y < 0 or x + w > self.width or y + h > self.height:
            raise ValueError(f"crop {x},{y} {w}x{h} outside {self.width}x{self.height}")
        return Indexed(
            w, h, tuple(r[x : x + w] for r in self.rows[y : y + h]), self.plte, self.trns
        )


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode_indexed(png: bytes, name: str = "<png>") -> Indexed:
    """Decode a non-interlaced palette PNG (bit depth 1, 2, 4 or 8)."""
    info = pnginfo.read_info(png, name)
    if info.cgbi or info.color_type != 3 or info.interlace != 0:
        raise FormatError(f"{name}: only non-interlaced palette PNGs are decoded ({info.color})")
    depth = info.bit_depth
    if depth not in (1, 2, 4, 8):
        raise FormatError(f"{name}: palette bit depth {depth}")
    plte, trns, idat = b"", None, bytearray()
    pos = len(pnginfo.SIGNATURE)
    while True:
        ctype, payload, pos = pnginfo.chunk_at(png, pos, name)
        if ctype == b"PLTE":
            plte = payload
        elif ctype == b"tRNS":
            trns = payload
        elif ctype == b"IDAT":
            idat += payload
        elif ctype == b"IEND":
            break
    if not plte or len(plte) % 3:
        raise FormatError(f"{name}: missing or bad PLTE ({len(plte)} bytes)")
    stride = (info.width * depth + 7) // 8
    try:
        raw = zlib.decompress(bytes(idat))
    except zlib.error as exc:
        raise FormatError(f"{name}: IDAT does not inflate: {exc}") from None
    need = info.height * (stride + 1)
    if len(raw) != need:
        raise FormatError(f"{name}: IDAT holds {len(raw)} bytes, {info.height} rows need {need}")
    colours = len(plte) // 3
    rows: list[bytes] = []
    prev = bytearray(stride)
    for y in range(info.height):
        ftype = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 1] if i else 0
            b = prev[i]
            c = prev[i - 1] if i else 0
            if ftype == 0:
                continue
            elif ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                line[i] = (line[i] + _paeth(a, b, c)) & 0xFF
            else:
                raise FormatError(f"{name}: row {y}: unknown filter type {ftype}")
        prev = line
        if depth == 8:
            px = bytes(line[: info.width])
        else:
            per = 8 // depth
            mask = (1 << depth) - 1
            px = bytes(
                (line[x // per] >> (8 - depth * (x % per + 1))) & mask for x in range(info.width)
            )
        if px and max(px) >= colours:
            raise FormatError(f"{name}: row {y}: index {max(px)} past a {colours}-colour palette")
        rows.append(px)
    return Indexed(info.width, info.height, tuple(rows), plte, trns)


def _png_chunk(ctype: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + ctype
        + payload
        + struct.pack(">I", zlib.crc32(ctype + payload))
    )


def encode_indexed(img: Indexed) -> bytes:
    """An 8-bit palette PNG (filter 0 on every row) with the image's PLTE and tRNS."""
    ihdr = struct.pack(">IIBBBBB", img.width, img.height, 8, 3, 0, 0, 0)
    raw = b"".join(b"\0" + r for r in img.rows)
    out = pnginfo.SIGNATURE + _png_chunk(b"IHDR", ihdr) + _png_chunk(b"PLTE", img.plte)
    if img.trns is not None:
        out += _png_chunk(b"tRNS", img.trns)
    return out + _png_chunk(b"IDAT", zlib.compress(raw, 9)) + _png_chunk(b"IEND", b"")


# --- Parts the game draws through a clip rect (docs/formats/png-dat.md -> "Parts") ----------


def parts(kind: str | None, img: Indexed) -> list[tuple[str, tuple[int, int, int, int]]]:
    """Named source rects ``(x, y, w, h)`` the game cuts from one sub-image."""
    if kind == PARTS_DIGITS:  # 0x3A07D: glyph width = image width / 10, digit d at x = d * w
        w = img.width // 10
        return [(f"d{d}", (d * w, 0, w, img.height)) for d in range(10)]
    if kind == PARTS_CORNERS:  # 0x3A899: s = width >> 1, the four s*s quadrants
        s = img.width >> 1
        return [
            ("tl", (0, 0, s, s)),
            ("tr", (s, 0, s, s)),
            ("bl", (0, s, s, s)),
            ("br", (s, s, s, s)),
        ]
    return []


# --- Extraction ------------------------------------------------------------------------------


@dataclass
class Report:
    files: int = 0
    images: int = 0
    parts: int = 0
    table_checked: int = 0
    errors: list[str] = field(default_factory=list)


def extract_pack(
    pack: Pack, data: bytes, out_dir: Path, lengths: Sequence[int] | None = None
) -> tuple[list[dict], int]:
    """Write one pack's sub-images (and parts) to ``out_dir``. Returns manifest rows and the
    number of parts written. Raises :class:`FormatError` if the pack does not match ``pack``
    or (when given) the engine's length row."""
    name = pack.path
    images = split(data, name)
    if len(images) != pack.count:
        raise FormatError(f"{name}: {len(images)} images, the loader expects {pack.count}")
    if lengths is not None:
        pieces = split_by_lengths(data, lengths, name)
        if [p for p in pieces] != [im.data for im in images]:
            raise FormatError(f"{name}: length table row {pack.id} does not match the PNG framing")
    out_dir.mkdir(parents=True, exist_ok=True)
    rows, nparts = [], 0
    for im in images:
        stem = image_name(pack.shape, im.index)
        (out_dir / f"{stem}.png").write_bytes(im.data)
        row = {
            "index": im.index,
            "file": f"{stem}.png",
            "coords": list(coords(pack.shape, im.index)),
            "offset": im.offset,
            "length": im.length,
            "width": im.info.width,
            "height": im.info.height,
            "bit_depth": im.info.bit_depth,
            "color": im.info.color,
        }
        if pack.parts:
            decoded = decode_indexed(im.data, f"{name}[{im.index}]")
            row["parts"] = {}
            for part, rect in parts(pack.parts, decoded):
                fname = f"{stem}_{part}.png"
                (out_dir / fname).write_bytes(encode_indexed(decoded.crop(*rect)))
                row["parts"][part] = {"file": fname, "rect": list(rect)}
                nparts += 1
        rows.append(row)
    return rows, nparts


def _opt_hex(v: int | None) -> str | None:
    return None if v is None else hexaddr(v)


def extract_all(app: Path, out: Path, image=None) -> Report:
    """Extract every pack in :data:`PACKS` from the ``.app`` directory ``app`` into ``out``.

    ``image`` (a :class:`mtport.macho.MachO` of the armv7 slice) adds the length-table check.
    Errors are collected per file, so one bad pack does not stop the others.
    """
    report = Report()
    manifest = []
    for pack in PACKS:
        src = app / pack.path
        try:
            data = src.read_bytes()
            lengths = length_row(image, pack.id) if image is not None else None
            folder = Path(pack.path).parent.relative_to("png")
            rows, nparts = extract_pack(pack, data, out / folder / Path(pack.path).stem, lengths)
        except (OSError, FormatError, ValueError) as exc:
            report.errors.append(f"{pack.path}: {exc}")
            continue
        report.files += 1
        report.images += len(rows)
        report.parts += nparts
        report.table_checked += lengths is not None
        manifest.append(
            {
                "id": pack.id,
                "path": pack.path,
                "size": len(data),
                "shape": list(pack.shape),
                "table_row": hexaddr(pack.table_row),
                "loader": _opt_hex(pack.loader),
                "call": _opt_hex(pack.call),
                "helper": _opt_hex(pack.helper),
                "field": _opt_hex(pack.field),
                "parts": pack.parts,
                "note": pack.note,
                "images": rows,
            }
        )
    out.mkdir(parents=True, exist_ok=True)
    (out / "index.json").write_text(json.dumps({"packs": manifest}, indent=1) + "\n")
    return report


# --- Command line ----------------------------------------------------------------------------

_REPO = Path(__file__).resolve().parents[2]
DEFAULT_APP = _REPO / "work" / "ipa" / "Payload" / "MapleThief.app"
DEFAULT_OUT = _REPO / "assets-out" / "png-dat"
DEFAULT_BINARY = _REPO / "work" / "slices" / "MapleThief.armv7"


def _list(app: Path) -> int:
    status = 0
    print("id  file                          n  shape    loader   call     field  sizes")
    for p in PACKS:
        try:
            images = split((app / p.path).read_bytes(), p.path)
        except (OSError, FormatError) as exc:
            print(f"{p.path}: error: {exc}", file=sys.stderr)
            status = 1
            continue
        sizes = " ".join(f"{i.info.width}x{i.info.height}" for i in images[:6])
        more = " ..." if len(images) > 6 else ""
        shape = "x".join(map(str, p.shape))
        print(
            f"{p.id:>2}  {p.path[4:]:<28} {len(images):>3}  {shape:<7}  "
            f"{_opt_hex(p.loader) or '-':<7}  {_opt_hex(p.call) or '-':<7}  "
            f"{_opt_hex(p.field) or '-':<5}  "
            f"{sizes}{more}"
        )
    return status


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.pngdat", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    ls = sub.add_parser("list", help="one line per pack")
    ls.add_argument("--app", type=Path, default=DEFAULT_APP)
    ex = sub.add_parser("extract", help="write every sub-image to --out")
    ex.add_argument("--app", type=Path, default=DEFAULT_APP)
    ex.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ex.add_argument("--binary", type=Path, default=DEFAULT_BINARY, help="armv7 slice")
    ex.add_argument("--no-binary", action="store_true", help="skip the length-table check")
    args = parser.parse_args(argv)

    if args.command == "list":
        return _list(args.app)
    image = None
    if not args.no_binary:
        from mtport import macho

        try:
            image = macho.load(args.binary)
        except (OSError, ValueError) as exc:
            print(f"{args.binary}: error: {exc} (pass --no-binary to skip)", file=sys.stderr)
            return 1
    report = extract_all(args.app, args.out, image)
    for err in report.errors:
        print(f"error: {err}", file=sys.stderr)
    print(
        f"{report.files}/{len(PACKS)} packs, {report.images} sub-images, {report.parts} parts, "
        f"{report.table_checked} checked against the length table, "
        f"{len(report.errors)} errors -> {args.out}"
    )
    return 1 if report.errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
