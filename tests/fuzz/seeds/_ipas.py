"""Synthetic IPAs, Info.plists and Mach-O headers for the IPA seeds (nothing from the game)."""

from __future__ import annotations

import plistlib
import struct

from _zips import make_zip

APP = "Payload/MapleThief.app/"


def info_plist(
    version: str = "1.5", binary: bool = True, ident: str = "com.example.thief"
) -> bytes:
    data = {
        "CFBundleVersion": version,
        "CFBundleShortVersionString": version,
        "CFBundleIdentifier": ident,
        "CFBundleExecutable": "MapleThief",
    }
    return plistlib.dumps(
        data, fmt=plistlib.FMT_BINARY if binary else plistlib.FMT_XML, sort_keys=True
    )


def macho(cryptid: int | None = 0) -> bytes:
    """A thin 32-bit ARM Mach-O header with LC_ENCRYPTION_INFO (or none) and a little code."""
    cmds = b""
    if cryptid is not None:
        cmds += struct.pack("<5I", 0x21, 20, 0x1000, 0x100, cryptid)  # LC_ENCRYPTION_INFO
    cmds += struct.pack("<2I", 0x26, 8)  # LC_FUNCTION_STARTS, empty: just another command
    header = struct.pack("<7I", 0xFEEDFACE, 12, 9, 2, 2 if cryptid is not None else 1, len(cmds), 0)
    return header + cmds + b"\x00\xbf" * 16


def fat(slices: list[bytes]) -> bytes:
    """A fat (universal) file of the slices, each at a 256-byte boundary (align 2^8)."""
    out = struct.pack(">2I", 0xCAFEBABE, len(slices))
    offset = 256
    body = b""
    for s in slices:
        out += struct.pack(">5I", 12, 9, offset, len(s), 8)
        body += s + b"\x00" * (-len(s) % 256)
        offset += len(s) + (-len(s) % 256)
    return out + b"\x00" * (256 - len(out)) + body


def ipa(
    plist: bytes,
    exe: bytes,
    *,
    app: str = APP,
    extra: list[tuple[str, bytes]] = (),
    deflate: bool = True,
) -> bytes:
    entries = [
        ("Payload/", b""),
        (app, b""),
        (app + "Info.plist", plist),
        (app + "MapleThief", exe),
        *extra,
    ]
    return make_zip(entries, deflate=deflate, unix=True)
