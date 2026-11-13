"""Seeds for fuzz_mp3: the synthetic MP3s of tests/cpp/data/audio/ (make_tones.py), whole and
cut."""

from __future__ import annotations

from pathlib import Path

MAX_LEN = 16384
AUDIO = Path(__file__).resolve().parents[2] / "cpp" / "data" / "audio"


def seeds() -> dict[str, bytes]:
    out = {p.name: p.read_bytes() for p in sorted(AUDIO.glob("*.mp3"))}
    tone = out["tone22.mp3"]
    out["tone22-cut.mp3"] = tone[: len(tone) // 3]
    out["tone22-id3.mp3"] = b"ID3\x03\x00\x00\x00\x00\x00\x10" + bytes(16) + tone
    return out
