"""Nintendo Switch homebrew NROs and their NACP (docs/formats/nro.md, ADR 0138).

An NRO is the executable the Homebrew Menu starts: a header, the code, and after it an
optional *asset section* with the icon (a JPEG), the NACP (the application's name, author and
version) and a RomFS. This module reads both, following the public description on switchbrew
(https://switchbrew.org/wiki/NRO and https://switchbrew.org/wiki/NACP), and builds small ones
for the tests. It never runs anything: an NRO is untrusted data here.

The Switch build checks its own ``MapleThief.nro`` with it (CTest ``platform.switch_nro``,
platform/switch/switch.cmake), and the release guard splits an NRO into its code, icon, NACP
and RomFS with :func:`members` so that its rules see each part (mtport.release).

CLI::

    python3 -m mtport.nro info NRO
    python3 -m mtport.nro check NRO [--name N] [--author A] [--version V] [--icon-size PX]
    python3 -m mtport.nro unpack NRO DIR      code.bin, icon.jpg, control.nacp, romfs.bin
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from mtport import jpeg

NRO_MAGIC = b"NRO0"
ASSET_MAGIC = b"ASET"
HEADER_START = 0x10  # the NRO header follows the 0x10-byte NroStart
HEADER_END = 0x80
ASSET_HEADER_SIZE = 0x38
NACP_SIZE = 0x4000
NACP_LANGUAGES = 16
NACP_TITLE_SIZE = 0x300
NACP_NAME_SIZE = 0x200
NACP_AUTHOR_SIZE = 0x100
NACP_DISPLAY_VERSION = 0x3060
NACP_DISPLAY_VERSION_SIZE = 0x10
SEGMENT_NAMES = ("text", "ro", "data")


class NroError(ValueError):
    """The bytes are not an NRO (or an NACP) this reader understands."""


@dataclass(frozen=True)
class Segment:
    offset: int
    size: int


@dataclass(frozen=True)
class Title:
    name: str
    author: str


@dataclass(frozen=True)
class Nacp:
    titles: tuple[Title, ...]  # 16 languages, American English first
    display_version: str

    @property
    def title(self) -> Title:
        """The first language with a name: what the Homebrew Menu shows by default."""
        for t in self.titles:
            if t.name:
                return t
        return Title("", "")


@dataclass(frozen=True)
class Nro:
    version: int
    size: int  # the NRO proper, without the asset section
    flags: int
    segments: tuple[Segment, Segment, Segment]  # text, ro, data
    bss_size: int
    build_id: bytes
    code: bytes  # bytes [0, size)
    icon: bytes  # empty if none
    nacp: bytes  # empty if none
    romfs: bytes  # empty if none
    has_assets: bool


def _cstr(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("utf-8", "replace")


def parse_nacp(data: bytes) -> Nacp:
    if len(data) != NACP_SIZE:
        raise NroError(f"NACP is {len(data)} bytes, expected 0x{NACP_SIZE:X}")
    titles = []
    for i in range(NACP_LANGUAGES):
        entry = data[i * NACP_TITLE_SIZE : (i + 1) * NACP_TITLE_SIZE]
        titles.append(
            Title(_cstr(entry[:NACP_NAME_SIZE]), _cstr(entry[NACP_NAME_SIZE:NACP_TITLE_SIZE]))
        )
    version = _cstr(data[NACP_DISPLAY_VERSION : NACP_DISPLAY_VERSION + NACP_DISPLAY_VERSION_SIZE])
    return Nacp(tuple(titles), version)


def parse(data: bytes) -> Nro:
    """Read an NRO's header and asset section. Raises :class:`NroError`."""
    if len(data) < HEADER_END:
        raise NroError(f"{len(data)} bytes: too short for an NRO header")
    if data[HEADER_START : HEADER_START + 4] != NRO_MAGIC:
        raise NroError("no NRO0 magic at 0x10")
    version, size, flags = struct.unpack_from("<III", data, 0x14)
    seg = struct.unpack_from("<6I", data, 0x20)
    segments = (Segment(seg[0], seg[1]), Segment(seg[2], seg[3]), Segment(seg[4], seg[5]))
    (bss_size,) = struct.unpack_from("<I", data, 0x38)
    build_id = data[0x40:0x60]
    if size < HEADER_END or size > len(data):
        raise NroError(f"NRO size 0x{size:X} outside the file (0x{len(data):X} bytes)")
    for name, s in zip(SEGMENT_NAMES, segments, strict=True):
        if s.offset + s.size > size:
            raise NroError(f"{name} segment 0x{s.offset:X}+0x{s.size:X} past the NRO's end")
    icon = nacp = romfs = b""
    has_assets = False
    rest = data[size:]
    if len(rest) >= ASSET_HEADER_SIZE and rest[:4] == ASSET_MAGIC:
        has_assets = True
        sections = struct.unpack_from("<6Q", rest, 8)
        parts = []
        for which, (off, length) in zip(
            ("icon", "nacp", "romfs"), zip(sections[::2], sections[1::2], strict=True), strict=True
        ):
            if length and (off < ASSET_HEADER_SIZE or off + length > len(rest)):
                raise NroError(f"asset {which} 0x{off:X}+0x{length:X} outside the file")
            parts.append(rest[off : off + length] if length else b"")
        icon, nacp, romfs = parts
    elif rest:
        raise NroError(f"0x{len(rest):X} bytes after the NRO and no ASET header")
    return Nro(
        version, size, flags, segments, bss_size, build_id, data[:size], icon, nacp, romfs,
        has_assets,
    )  # fmt: skip


def members(data: bytes) -> list[tuple[str, bytes]]:
    """The parts of an NRO as files: ``code.bin`` (the NRO without its assets) and, when
    present, ``icon.jpg``, ``control.nacp`` and ``romfs.bin``."""
    nro = parse(data)
    out = [("code.bin", nro.code)]
    for name, part in (
        ("icon.jpg", nro.icon),
        ("control.nacp", nro.nacp),
        ("romfs.bin", nro.romfs),
    ):
        if part:
            out.append((name, part))
    return out


# --- Building (the tests' fixtures) --------------------------------------------------------


def build_nacp(name: str, author: str, version: str) -> bytes:
    """An NACP with ``name`` and ``author`` in every language, as nacptool --create writes."""
    out = bytearray(NACP_SIZE)
    n, a, v = name.encode(), author.encode(), version.encode()
    if (
        len(n) >= NACP_NAME_SIZE
        or len(a) >= NACP_AUTHOR_SIZE
        or len(v) >= NACP_DISPLAY_VERSION_SIZE
    ):
        raise ValueError("NACP field too long")
    for i in range(NACP_LANGUAGES):
        base = i * NACP_TITLE_SIZE
        out[base : base + len(n)] = n
        out[base + NACP_NAME_SIZE : base + NACP_NAME_SIZE + len(a)] = a
    out[NACP_DISPLAY_VERSION : NACP_DISPLAY_VERSION + len(v)] = v
    return bytes(out)


def build(code: bytes = b"", icon: bytes = b"", nacp: bytes = b"", romfs: bytes = b"") -> bytes:
    """A minimal NRO: the header, ``code`` as its text segment, and an asset section when any
    asset is given. Not runnable; enough for a reader."""
    text = code.ljust((len(code) + 0xFFF) & ~0xFFF, b"\0") if code else b""
    size = HEADER_END + len(text)
    head = bytearray(HEADER_END)
    head[HEADER_START : HEADER_START + 4] = NRO_MAGIC
    struct.pack_into("<III", head, 0x14, 0, size, 0)
    struct.pack_into("<6I", head, 0x20, HEADER_END, len(text), size, 0, size, 0)
    out = bytes(head) + text
    if icon or nacp or romfs:
        offset = ASSET_HEADER_SIZE
        sections = []
        for part in (icon, nacp, romfs):
            sections += [offset if part else 0, len(part)]
            offset += len(part)
        out += ASSET_MAGIC + struct.pack("<I6Q", 0, *sections) + icon + nacp + romfs
    return out


# --- Checks and CLI ------------------------------------------------------------------------


def check(
    data: bytes,
    name: str | None = None,
    author: str | None = None,
    version: str | None = None,
    icon_size: int | None = None,
) -> list[str]:
    """What is wrong with the NRO ``data`` against the expectations given (empty: nothing)."""
    try:
        nro = parse(data)
    except NroError as e:
        return [str(e)]
    problems = []
    if not nro.has_assets:
        return ["no asset section (ASET): no icon and no NACP"]
    if not nro.nacp:
        problems.append("no NACP in the asset section")
    else:
        try:
            nacp = parse_nacp(nro.nacp)
        except NroError as e:
            problems.append(str(e))
        else:
            title = nacp.title
            if name is not None and title.name != name:
                problems.append(f"NACP name {title.name!r}, expected {name!r}")
            if author is not None and title.author != author:
                problems.append(f"NACP author {title.author!r}, expected {author!r}")
            if version is not None and nacp.display_version != version:
                problems.append(f"NACP version {nacp.display_version!r}, expected {version!r}")
    if icon_size is not None:
        if not nro.icon:
            problems.append("no icon in the asset section")
        else:
            try:
                info = jpeg.info(nro.icon)
            except jpeg.JpegError as e:
                problems.append(f"icon: {e}")
            else:
                if (info.width, info.height) != (icon_size, icon_size):
                    problems.append(
                        f"icon is {info.width} x {info.height}, expected {icon_size} x {icon_size}"
                    )
                if not info.baseline:
                    problems.append("icon is not a baseline JPEG")
    return problems


def describe(data: bytes) -> str:
    nro = parse(data)
    lines = [f"NRO version {nro.version}, 0x{nro.size:X} bytes, bss 0x{nro.bss_size:X}"]
    for name, s in zip(SEGMENT_NAMES, nro.segments, strict=True):
        lines.append(f"  {name:5} 0x{s.offset:08X} 0x{s.size:08X}")
    lines.append(f"  build id {nro.build_id.hex()}")
    if nro.icon:
        try:
            info = jpeg.info(nro.icon)
            kind = f"JPEG {info.width} x {info.height}{'' if info.baseline else ' (not baseline)'}"
        except jpeg.JpegError as e:
            kind = f"not a JPEG ({e})"
        lines.append(f"icon: {len(nro.icon)} bytes, {kind}")
    if nro.nacp:
        nacp = parse_nacp(nro.nacp)
        lines.append(
            f"NACP: {nacp.title.name!r} by {nacp.title.author!r}, version {nacp.display_version!r}"
        )
    if nro.romfs:
        lines.append(f"RomFS: {len(nro.romfs)} bytes")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.nro", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info", help="print the header, the icon's size and the NACP")
    p.add_argument("nro", type=Path)
    p = sub.add_parser("check", help="exit 1 unless the NRO matches the expectations")
    p.add_argument("nro", type=Path)
    p.add_argument("--name")
    p.add_argument("--author")
    p.add_argument("--version")
    p.add_argument("--icon-size", type=int, metavar="PX")
    p = sub.add_parser("unpack", help="write the code, icon, NACP and RomFS to DIR")
    p.add_argument("nro", type=Path)
    p.add_argument("dir", type=Path)
    args = parser.parse_args(argv)
    data = args.nro.read_bytes()
    try:
        if args.cmd == "info":
            print(describe(data))
            return 0
        if args.cmd == "unpack":
            args.dir.mkdir(parents=True, exist_ok=True)
            for name, part in members(data):
                (args.dir / name).write_bytes(part)
                print(f"nro: {args.dir / name} ({len(part)} bytes)")
            return 0
    except NroError as e:
        print(f"nro: {args.nro}: {e}", file=sys.stderr)
        return 1
    problems = check(data, args.name, args.author, args.version, args.icon_size)
    for problem in problems:
        print(f"nro: {args.nro}: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"nro: {args.nro}: OK\n{describe(data)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
