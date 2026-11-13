"""Seeds for fuzz_wav: PCM WAVs built in code, every accepted sample size, with and without a
smpl loop, odd chunks, and a cut data chunk."""

from __future__ import annotations

import struct

MAX_LEN = 16384


def chunk(cid: bytes, data: bytes) -> bytes:
    return cid + struct.pack("<I", len(data)) + data + (b"\x00" if len(data) % 2 else b"")


def wav(
    channels: int,
    rate: int,
    bits: int,
    frames: int,
    *,
    extra: bytes = b"",
    loop: tuple[int, int] | None = None,
    cut: int = 0,
) -> bytes:
    block = channels * bits // 8
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * block, block, bits)
    data = bytes((i * 7) & 0xFF for i in range(frames * block))
    body = b"WAVE" + chunk(b"fmt ", fmt) + extra
    if loop is not None:
        smpl = struct.pack("<9I", 0, 0, 0, 60, 0, 0, 0, 1, 0)
        smpl += struct.pack("<6I", 0, 0, loop[0], loop[1], 0, 0)
        body += chunk(b"smpl", smpl)
    body += chunk(b"data", data)
    out = b"RIFF" + struct.pack("<I", len(body)) + body
    return out[: len(out) - cut] if cut else out


def seeds() -> dict[str, bytes]:
    return {
        "mono8.wav": wav(1, 8000, 8, 64),
        "stereo16.wav": wav(2, 44100, 16, 64),
        "mono24.wav": wav(1, 22050, 24, 33),
        "quad32.wav": wav(4, 48000, 32, 16),
        "loop.wav": wav(2, 44100, 16, 128, loop=(16, 95)),
        "odd-chunk.wav": wav(1, 11025, 16, 20, extra=chunk(b"LIST", b"INFOx")),
        "cut.wav": wav(2, 44100, 16, 50, cut=37),
    }
