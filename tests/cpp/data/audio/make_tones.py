"""Makes the synthetic MP3 fixtures of tests/cpp/mp3_test.cpp (#63) from our own tones.

    python3 -I tests/cpp/data/audio/make_tones.py   (needs LAME on the PATH)

Each fixture is 0.3 s of a stereo tone (left 440 Hz at half scale, right 660 Hz at quarter scale)
encoded by LAME the way the game's two batches of MP3s were made (docs/re/audio.md): CBR joint
stereo, a CRC after every frame header (-p), an ID3v2 tag in front and an ID3v1 tag at the end
(--add-id3v2), and no Xing/LAME info frame (-t), so a decoder knows nothing of the encoder's
delay and padding.

    tone44.mp3   MPEG-1 Layer III, 44,100 Hz, 128 kbit/s (as 0-6, 30, 31, 34, 42)
    tone22.mp3   MPEG-2 Layer III, 22,050 Hz, 80 kbit/s (as 35-41)

The MP3s are committed (a few KB each, nothing of the game in them); this script only documents
and repeats how they were made. Another LAME version may give other bytes: the tests only rely on
what they decode to, through dr_mp3, never on the exact bytes.
"""

from __future__ import annotations

import math
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
SECONDS = 0.3
FIXTURES = (("tone44.mp3", 44100, 128), ("tone22.mp3", 22050, 80))


def tone(rate: int) -> bytes:
    frames = round(SECONDS * rate)
    out = bytearray()
    for i in range(frames):
        t = i / rate
        left = round(16383 * math.sin(2 * math.pi * 440 * t))
        right = round(8191 * math.sin(2 * math.pi * 660 * t))
        out += struct.pack("<hh", left, right)
    return bytes(out)


def main() -> int:
    lame = shutil.which("lame")
    if lame is None:
        print("lame not found", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        for name, rate, kbps in FIXTURES:
            wav = Path(tmp) / f"{rate}.wav"
            with wave.open(str(wav), "wb") as w:
                w.setnchannels(2)
                w.setsampwidth(2)
                w.setframerate(rate)
                w.writeframes(tone(rate))
            out = HERE / name
            subprocess.run(
                [lame, "--quiet", "--cbr", "-b", str(kbps), "-m", "j", "-p", "-t",
                 "--add-id3v2", "--tt", "synthetic tone", "--ta", "maplethief-port tests",
                 str(wav), str(out)],
                check=True,
            )  # fmt: skip
            print(f"wrote {out} ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
