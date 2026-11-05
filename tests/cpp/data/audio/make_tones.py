"""Makes the synthetic MP3 fixtures of tests/cpp/mp3_test.cpp (#63, #125) from our own tones.

    python3 -I tests/cpp/data/audio/make_tones.py   (needs LAME and FFmpeg on the PATH)

Each fixture is 0.3 s of a stereo tone (left 440 Hz at half scale, right 660 Hz at quarter scale)
encoded by LAME the way the game's two batches of MP3s were made (docs/re/audio.md): CBR joint
stereo, a CRC after every frame header (-p), an ID3v2 tag in front and an ID3v1 tag at the end
(--add-id3v2), and no Xing/LAME info frame (-t), so a decoder knows nothing of the encoder's
delay and padding.

    tone44.mp3   MPEG-1 Layer III, 44,100 Hz, 128 kbit/s (as 0-6, 30, 31, 34, 42)
    tone22.mp3   MPEG-2 Layer III, 22,050 Hz, 80 kbit/s (as 35-41)
    gap44.mp3    as tone44.mp3, but 0.1 s of digital silence, 0.2 s of the tone, then 0.25 s of
                 digital silence: a file with silence of its own at both ends, as most of the
                 game's tracks (#125, ADR 0062: the gapless option trims it)
    silence44.mp3  0.1 s of digital silence and nothing else (the gapless option keeps it whole)

The FLAC fixtures of tests/cpp/audio_pack_test.cpp (#188, audio packs) are lossless, so the test
compares their decode with the samples themselves: triangle waves in integers (no libm), which
the test computes again (its tone()), encoded by FFmpeg's FLAC encoder:

    ramp44.flac    16-bit stereo, 44,100 Hz, 13,230 frames (0.3 s): tone(13230, 2)
    ramp48_24.flac 24-bit mono, 48,000 Hz, 4,800 frames: tone(4800, 1) x 256 + 0x5A (the low
                   byte is what a 16-bit decode drops)

The MP3s and FLACs are committed (a few KB each, nothing of the game in them); this script only
documents and repeats how they were made. Another LAME or FFmpeg version may give other bytes:
the tests only rely on what they decode to, through dr_mp3 and dr_flac, never on the bytes.
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
# name, rate, kbit/s, seconds of silence before the tone, of the tone, of silence after it
FIXTURES = (
    ("tone44.mp3", 44100, 128, 0.0, 0.3, 0.0),
    ("tone22.mp3", 22050, 80, 0.0, 0.3, 0.0),
    ("gap44.mp3", 44100, 128, 0.1, 0.2, 0.25),
    ("silence44.mp3", 44100, 128, 0.1, 0.0, 0.0),
)


def tone(rate: int, before: float, seconds: float, after: float) -> bytes:
    frames = round(seconds * rate)
    out = bytearray(4 * round(before * rate))
    for i in range(frames):
        t = i / rate
        left = round(16383 * math.sin(2 * math.pi * 440 * t))
        right = round(8191 * math.sin(2 * math.pi * 660 * t))
        out += struct.pack("<hh", left, right)
    out += bytes(4 * round(after * rate))
    return bytes(out)


def triangle(frames: int, channels: int) -> list[int]:
    """audio_pack_test.cpp's tone(frames, channels): periods 100 and 74, from -10,000."""
    out = []
    for i in range(frames):
        for ch in range(channels):
            period = 100 if ch == 0 else 74
            t = i % period
            out.append((t if t < period // 2 else period - t) * 40000 // period - 10000)
    return out


def write_flacs(ffmpeg: str, tmp: str) -> None:
    for name, rate, channels, frames, bits in (
        ("ramp44.flac", 44100, 2, 13230, 16),
        ("ramp48_24.flac", 48000, 1, 4800, 24),
    ):
        wav = Path(tmp) / f"{name}.wav"
        data = bytearray()
        for v in triangle(frames, channels):
            data += struct.pack("<h", v) if bits == 16 else struct.pack("<i", v * 256 + 0x5A)[:3]
        with wave.open(str(wav), "wb") as w:
            w.setnchannels(channels)
            w.setsampwidth(bits // 8)
            w.setframerate(rate)
            w.writeframes(bytes(data))
        out = HERE / name
        fmt = "s16" if bits == 16 else "s32"
        subprocess.run(
            [ffmpeg, "-loglevel", "error", "-y", "-i", str(wav), "-c:a", "flac",
             "-sample_fmt", fmt, "-bits_per_raw_sample", str(bits), "-map_metadata", "-1",
             "-fflags", "+bitexact", str(out)],
            check=True,
        )  # fmt: skip
        print(f"wrote {out} ({out.stat().st_size} bytes)")


def main() -> int:
    lame = shutil.which("lame")
    ffmpeg = shutil.which("ffmpeg")
    if lame is None or ffmpeg is None:
        print("lame or ffmpeg not found", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        for name, rate, kbps, before, seconds, after in FIXTURES:
            wav = Path(tmp) / f"{name}.wav"
            with wave.open(str(wav), "wb") as w:
                w.setnchannels(2)
                w.setsampwidth(2)
                w.setframerate(rate)
                w.writeframes(tone(rate, before, seconds, after))
            out = HERE / name
            subprocess.run(
                [lame, "--quiet", "--cbr", "-b", str(kbps), "-m", "j", "-p", "-t",
                 "--add-id3v2", "--tt", "synthetic tone", "--ta", "maplethief-port tests",
                 str(wav), str(out)],
                check=True,
            )  # fmt: skip
            print(f"wrote {out} ({out.stat().st_size} bytes)")
        write_flacs(ffmpeg, tmp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
