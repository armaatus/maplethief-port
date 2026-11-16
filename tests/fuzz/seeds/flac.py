"""Seeds for fuzz_flac: the synthetic FLACs of tests/cpp/data/audio/ (make_tones.py), cut to a
few frames, and a FLAC built here whose header gives no length."""

from __future__ import annotations

from pathlib import Path

MAX_LEN = 16384
AUDIO = Path(__file__).resolve().parents[2] / "cpp" / "data" / "audio"


def seeds() -> dict[str, bytes]:
    out = {}
    for p in sorted(AUDIO.glob("*.flac")):
        data = p.read_bytes()
        out[p.stem + "-head.flac"] = data[:MAX_LEN]
        out[p.stem + "-short.flac"] = data[:4096]
    # STREAMINFO with total samples 0 ("unknown"): refused before decoding.
    head = bytearray(out["ramp44-head.flac"][:42])
    head[4] |= 0x80  # the last metadata block
    head[8 + 13] &= 0xF0
    head[8 + 14 : 8 + 18] = b"\x00\x00\x00\x00"
    out["no-length.flac"] = bytes(head)
    return out
