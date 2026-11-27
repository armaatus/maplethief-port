"""Writes the guest program's test sound (#385, ADR 0213): tone.wav, a 16-bit mono PCM WAV at
22,050 Hz, 0.25 s of a 441 Hz square wave at a quarter of full scale. Synthetic and the same
bytes every run; scripts/guest-image puts it in the guest bundle.

    python3 tests/guest/make_sound.py OUT.wav
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

RATE = 22050
SAMPLES = RATE // 4
PERIOD = RATE // 441  # 50 samples
LEVEL = 8192


def tone() -> bytes:
    """The WAV file's bytes."""
    pcm = b"".join(
        struct.pack("<h", LEVEL if (i % PERIOD) < PERIOD // 2 else -LEVEL) for i in range(SAMPLES)
    )
    fmt = struct.pack("<HHIIHH", 1, 1, RATE, RATE * 2, 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"data" + struct.pack("<I", len(pcm)) + pcm
    return b"RIFF" + struct.pack("<I", len(body)) + body


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    Path(argv[0]).write_bytes(tone())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
