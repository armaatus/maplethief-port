"""Seeds for fuzz_zip: save exports as the port writes them and as other tools would."""

from __future__ import annotations

from _zips import make_zip

MAX_LEN = 16384


def seeds() -> dict[str, bytes]:
    save = bytes(range(256)) * 4  # a stand-in for a save file's bytes
    return {
        "empty.zip": make_zip([]),
        "export.zip": make_zip(
            [
                ("Documents/theif0", save),
                ("Documents/theif1", save[::-1]),
                ("Library/Preferences/settings.plist", b"<plist/>"),
            ]
        ),
        "deflated.zip": make_zip(
            [("Documents/theif0", save), ("a.txt", b"hello " * 50)], deflate=True
        ),
        "folders.zip": make_zip([("Documents/", b""), ("Documents/theif2", b"x" * 40)], unix=True),
        "backslash.zip": make_zip([("Documents\\theif0", save[:64])]),
        "unicode.zip": make_zip([("Documents/caf\u00e9", b"\x00\x01\x02")]),
        "one-byte.zip": make_zip([("b", b"\xff")], deflate=True),
    }
