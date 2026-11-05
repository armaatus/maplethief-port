"""mtport.audiopack (#188, ADR 0106): the list of replaceable names, on a synthetic sound/."""

from __future__ import annotations

import struct
from pathlib import Path

import pytest

from mtport import audio, audiopack

MPEG1 = 0xFFFB9040  # MPEG-1 Layer III, 128 kbit/s, 44.1 kHz, joint stereo: 1,152 samples


def mp3_frames(n: int, title: bytes | None = None) -> bytes:
    h = struct.pack(">I", MPEG1)
    size = audio.frame_header(h, 0).size
    tag = b""
    if title is not None:
        body = b"TIT2" + struct.pack(">I", len(title) + 1) + b"\0\0" + b"\0" + title
        tag = b"ID3\x03\x00\x00" + bytes((0, 0, 0, len(body))) + body
    return tag + (h + b"\x55" * (size - 4)) * n


def wav(frames: int, rate: int = 22050, channels: int = 1) -> bytes:
    align = 2 * channels
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * align, align, 16)
    data = b"\0" * (frames * align)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data"
    body += struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def write_bundle(app: Path) -> Path:
    sounds = app / "sound"
    sounds.mkdir(parents=True)
    for i in range(audio.SOUND_COUNT):
        if i in audio.BGM_IDS:
            (sounds / f"{i}.mp3").write_bytes(mp3_frames(3 + i, b"Track" if i == 35 else None))
        else:
            (sounds / f"{i}.wav").write_bytes(wav(100 * i, 8000 if i % 2 else 22050, 1 + i % 2))
    return sounds


def test_init_lists_every_name_with_role_length_and_format(tmp_path: Path) -> None:
    app = tmp_path / "MapleThief.app"
    write_bundle(app)
    out = audiopack.init(tmp_path / "pack", app)
    assert out == tmp_path / "pack" / "audiopack.txt"
    lines = [ln for ln in out.read_text(encoding="utf-8").splitlines() if not ln.startswith("#")]
    assert len(lines) == 44
    rows = {ln.split()[0]: ln.split() for ln in lines}
    assert rows["0.mp3"][1] == "BGM"
    assert rows["0.mp3"][3] == str(3 * 1152)  # frames: MPEG frames x samples per frame
    assert rows["0.mp3"][2] == f"{3 * 1152 / 44100:.2f}"
    assert rows["0.mp3"][4:] == ["MP3", "44100", "Hz", "stereo"]
    assert rows["35.mp3"][-1] == '"Track"'
    assert rows["7.wav"][1:] == ["SFX", f"{700 / 8000:.2f}", "700", "WAV", "8000", "Hz", "stereo"]
    assert rows["12.wav"][3:] == ["1200", "WAV", "22050", "Hz", "mono"]
    assert sum(r[1] == "BGM" for r in rows.values()) == 18
    # nothing but the list: no sound of the game is copied
    assert [p.name for p in (tmp_path / "pack").iterdir()] == ["audiopack.txt"]


def test_init_takes_the_sound_folder_and_keeps_the_players_files(tmp_path: Path) -> None:
    sounds = write_bundle(tmp_path / "app")
    pack = tmp_path / "pack"
    pack.mkdir()
    (pack / "31.wav").write_bytes(b"mine")
    assert audiopack.main(["init", str(pack), "--bundle", str(sounds)]) == 0
    assert (pack / "31.wav").read_bytes() == b"mine"
    assert (pack / "audiopack.txt").is_file()


def test_init_without_a_bundle_or_with_a_wrong_one(tmp_path: Path, capsys) -> None:
    assert audiopack.main(["init", str(tmp_path / "p"), "--bundle", str(tmp_path / "none")]) == 2
    assert "is no folder" in capsys.readouterr().err
    sounds = write_bundle(tmp_path / "app")
    (sounds / "3.mp3").unlink()
    assert audiopack.main(["init", str(tmp_path / "p"), "--bundle", str(sounds)]) == 2
    assert "expected 0-43" in capsys.readouterr().err


@pytest.mark.skipif(
    not (Path(__file__).resolve().parents[1] / audiopack.DEFAULT_BUNDLE).is_dir(),
    reason="needs work/ipa (scripts/bootstrap)",
)
def test_init_on_the_real_bundle(tmp_path: Path) -> None:
    bundle = Path(__file__).resolve().parents[1] / audiopack.DEFAULT_BUNDLE
    text = audiopack.init(tmp_path, bundle).read_text(encoding="utf-8")
    rows = [ln.split() for ln in text.splitlines() if not ln.startswith("#")]
    assert len(rows) == 44
    assert {r[0] for r in rows if r[1] == "BGM"} == {f"{i}.mp3" for i in audio.BGM_IDS}
