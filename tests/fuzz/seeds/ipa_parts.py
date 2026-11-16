"""Seeds for fuzz_ipa_parts: byte 0 picks the reader (0 plist, 1 Mach-O, 2 entry name; for a
plist, byte 0 / 3 picks the key), the rest is its input."""

from __future__ import annotations

from _ipas import fat, info_plist, macho

MAX_LEN = 8192


def seeds() -> dict[str, bytes]:
    out = {
        "plist-binary": b"\x00" + info_plist(),
        "plist-xml": b"\x00" + info_plist(binary=False),
        "plist-key2": b"\x06" + info_plist(),
        "macho-plain": b"\x01" + macho(0),
        "macho-encrypted": b"\x01" + macho(1),
        "macho-none": b"\x01" + macho(None),
        "macho-fat": b"\x01" + fat([macho(1), macho(0)]),
    }
    names = [
        "Payload/MapleThief.app/Info.plist",
        "Payload/MapleThief.app/",
        "a/../b",
        "C:/x",
        "a\\b",
        "con.txt",
        "Payload//x",
        "\u00e9t\u00e9/\u00e9",
    ]
    for i, name in enumerate(names):
        out[f"name-{i}"] = b"\x02" + name.encode("utf-8")
    return out
