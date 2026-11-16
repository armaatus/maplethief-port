"""Seeds for fuzz_ipa: synthetic IPAs, each refused for its own reason or extracted."""

from __future__ import annotations

from _ipas import info_plist, ipa, macho
from _zips import make_zip

MAX_LEN = 16384


def seeds() -> dict[str, bytes]:
    return {
        "good-shape.ipa": ipa(
            info_plist(),
            macho(0),
            extra=[("Payload/MapleThief.app/a/b.png", b"\x89PNG" + bytes(40))],
        ),
        "xml-plist.ipa": ipa(info_plist(binary=False), macho(0), deflate=False),
        "encrypted-app.ipa": ipa(info_plist(), macho(1)),
        "other-version.ipa": ipa(info_plist("1.4"), macho(0)),
        "other-app.ipa": ipa(
            info_plist(ident="com.example.other"), macho(0), app="Payload/Other.app/"
        ),
        "unsafe-name.ipa": ipa(
            info_plist(), macho(0), extra=[("Payload/MapleThief.app/../x", b"no")]
        ),
        "no-payload.zip": make_zip([("readme.txt", b"not an ipa")]),
        "not-zip.bin": b"PK\x03\x04" + bytes(60),
    }
