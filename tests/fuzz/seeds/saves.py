"""Seeds for fuzz_saves: synthetic save files (docs/formats/save.md), as saves_test.cpp has them.

An input's first byte is k_names' index (low 5 bits, saves_fuzz.cpp) and the reason buffer's
size (top 3 bits); the file follows.
"""

from __future__ import annotations

MAX_LEN = 16384

# saves_fuzz.cpp's k_names, in order.
NAMES = [
    "theif0",
    "theif1",
    "theif2",
    "quest0",
    "quest1",
    "quest2",
    "config",
    "cupon",
    "CTerms",
    "crestore",
    "riprestore1",
    "riprestore2",
    "totalrestore",
    "maplethief-cashshop0",
    "maplethief-cashshop1",
    "maplethief-cashshop2",
    "thief0",
    "theif3",
    "",
]


def new_character() -> bytes:
    """docs/formats/save.md, "A new character"."""
    b = bytearray(0x464)
    b[2] = 1
    for i in range(48):
        b[0x015 + 16 * i] = 0xFF
        b[0x325 + 3 * i] = 0xFF
    b[0x325], b[0x326], b[0x328], b[0x329] = 3, 5, 6, 5
    for i in range(5):
        b[0x3C2 + 16 * i] = 0xFF
    b[0x3C2 + 16] = 0x48
    b[0x41C], b[0x41D] = 2, 8
    return bytes(b)


def quests() -> bytes:
    """Ten quest records, the first active with empty keys and lists."""
    q = bytearray(0x12AD)
    for r in range(10):
        q[0xC9 + 0x1C9 * r] = 0xFF
    rec = 0xC9
    for k in range(5):
        q[rec + 0x143 + 14 * k] = 0xFF
        q[rec + 0x189 + 13 * k] = 0xFF
    q[rec] = 0
    return bytes(q)


def seed(name: str, data: bytes, why: int = 6) -> tuple[str, bytes]:
    return f"{name or 'noname'}-{why}", bytes([NAMES.index(name) | why << 5]) + data


def seeds() -> dict[str, bytes]:
    counters = b"# the cash shop's counters\nlevel = 12\ndeaths = 1\npaid_revives = 0\n"
    return dict(
        [
            seed("theif0", new_character()),
            seed("theif2", new_character(), 3),
            seed("quest1", quests()),
            seed("config", bytes([3, 1, 0, 1, 1, 1])),
            seed("config", bytes([0xFF, 7, 9, 9, 9, 5]), 1),
            seed("cupon", bytes(0x174A)),
            seed("CTerms", bytes(2)),
            seed("crestore", b"\x02\x00\x01\x03\x00abc\x02\x01\x00z"),
            seed("totalrestore", b"\x00\x00"),
            seed("maplethief-cashshop1", counters),
            seed("maplethief-cashshop0", b"# counters\r\nlevel = 7\n\tdeaths=4294967295  \n", 7),
            seed("theif3", b"x", 0),
            seed("", b"", 2),
        ]
    )
