"""Unit tests for mtport.audio (#29): WAV/MP3 headers built in code, a synthetic armscan result,
the map-music transcription, the names file and the doc's generated blocks."""

from __future__ import annotations

import json
import struct
from pathlib import Path

import pytest

from mtport import armscan, audio, flow, names
from mtport.bytereader import FormatError

# -- WAV ------------------------------------------------------------------------------------


def chunk(cid: bytes, body: bytes) -> bytes:
    return cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")


def fmt(channels=1, rate=22050, bits=16, tag=1) -> bytes:
    align = channels * bits // 8
    return chunk(b"fmt ", struct.pack("<HHIIHH", tag, channels, rate, rate * align, align, bits))


def wav(*chunks: bytes, extra: bytes = b"") -> bytes:
    body = b"WAVE" + b"".join(chunks)
    return b"RIFF" + struct.pack("<I", len(body)) + body + extra


def test_wav_header_and_duration():
    info = audio.read_wav(wav(fmt(), chunk(b"data", b"\0" * 44100), chunk(b"LIST", b"INFOx")))
    assert (info.format_name, info.channels, info.sample_rate, info.bits_per_sample) == (
        "PCM",
        1,
        22050,
        16,
    )
    assert info.frames == 22050 and info.duration == 1.0
    assert info.chunks == ("fmt ", "data", "LIST") and info.trailing == 0


def test_wav_odd_chunk_is_padded_and_trailing_bytes_counted():
    info = audio.read_wav(
        wav(fmt(2, 44100), chunk(b"junk", b"abc"), chunk(b"data", b"\0" * 8), extra=b"xyz")
    )
    assert info.chunks == ("fmt ", "junk", "data")
    assert (info.channels, info.frames, info.trailing) == (2, 2, 3)


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"RIFX\0\0\0\0WAVE", "not a RIFF/WAVE"),
        (b"RIFF\xff\0\0\0WAVE", "runs past the end"),
        (wav(chunk(b"data", b"\0\0")), "data chunk before fmt"),
        (wav(fmt()), "no data chunk"),
        (wav(chunk(b"data", b"")[:0]), "no fmt chunk"),
        (wav(fmt(), fmt(), chunk(b"data", b"")), "second fmt"),
        (wav(chunk(b"fmt ", b"\0" * 8)), "need 16"),
        (wav(fmt(1, 8000, 16)[:-2] + struct.pack("<H", 8), chunk(b"data", b"")), "inconsistent"),
        (wav(fmt(0, 8000), chunk(b"data", b"")), "0 channels"),
        (b"RIFF" + struct.pack("<I", 12) + b"WAVEdata\x10\0\0\0", "runs past the end"),
        (b"RIFF" + struct.pack("<I", 7) + b"WAVEdat", "truncated chunk header"),
    ],
)
def test_wav_errors(data, message):
    with pytest.raises(audio.AudioError, match=message):
        audio.read_wav(data, "x.wav")


def test_audio_error_is_a_format_error():
    assert issubclass(audio.AudioError, FormatError)


# -- MP3 ------------------------------------------------------------------------------------

# MPEG-1 Layer III, 128 kbit/s, 44.1 kHz, joint stereo, no CRC: 417 bytes (418 padded).
MPEG1 = 0xFFFB9040
# MPEG-2 Layer III, 80 kbit/s, 22.05 kHz, joint stereo, CRC: 261 bytes.
MPEG2 = 0xFFF29040


def frame(header: int, pad: bool = False, size: int | None = None, fill: bytes = b"") -> bytes:
    h = header | (0x200 if pad else 0)
    info = audio.frame_header(struct.pack(">I", h), 0)
    n = info.size if size is None else size
    body = fill + b"\x55" * (n - 4 - len(fill))
    return struct.pack(">I", h) + body


def syncsafe(n: int) -> bytes:
    return bytes(((n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F))


def id3v23(*frames: tuple[bytes, bytes]) -> bytes:
    body = b"".join(fid + struct.pack(">I", len(b)) + b"\0\0" + b for fid, b in frames)
    body += b"\0" * 16  # padding
    return b"ID3\x03\x00\x00" + syncsafe(len(body)) + body


def id3v22(*frames: tuple[bytes, bytes]) -> bytes:
    body = b"".join(fid + len(b).to_bytes(3, "big") + b for fid, b in frames)
    return b"ID3\x02\x00\x00" + syncsafe(len(body)) + body


def test_frame_header_fields():
    h = audio.frame_header(struct.pack(">I", MPEG1), 0)
    assert (h.version, h.layer, h.bitrate, h.sample_rate, h.channel_mode) == (
        "MPEG-1",
        3,
        128,
        44100,
        "joint stereo",
    )
    assert (h.size, h.samples, h.crc, h.side_info) == (417, 1152, False, 32)
    h2 = audio.frame_header(struct.pack(">I", MPEG2), 0)
    assert (h2.version, h2.bitrate, h2.sample_rate, h2.size, h2.samples, h2.crc) == (
        "MPEG-2",
        80,
        22050,
        261,
        576,
        True,
    )
    for bad in (0xFFE00000, 0xFFFB0040, 0xFFFBF040, 0xFFFB9C40, 0x12345678):
        assert audio.frame_header(struct.pack(">I", bad), 0) is None
    assert audio.frame_header(b"\xff\xfb", 0) is None


def test_mp3_with_tags_walks_every_frame():
    data = (
        id3v23((b"TIT2", b"\x00Song"), (b"TALB", b"\x01\xff\xfeA\x00b\x00"), (b"TXXX", b"\0x"))
        + b"\0\0"
        + b"".join(frame(MPEG1, pad=i % 3 == 0) for i in range(10))
        + b"TAG"
        + b"\0" * 125
    )
    info = audio.read_mp3(data, "a.mp3")
    assert info.id3v2 == "2.3.0" and info.id3v1
    assert (info.frames, info.samples, info.bitrates, info.vbr) == (10, 11520, (128,), False)
    assert info.duration == pytest.approx(11520 / 44100)
    assert (info.title, info.album, info.info_tag, info.encoder, info.trailing) == (
        "Song",
        "Ab",
        None,
        None,
        0,
    )
    assert "MPEG-1 Layer III, 44100 Hz, joint stereo, 128 kbit/s" in audio.describe(info)


def test_mp3_v22_tag_mpeg2_and_cut_off_last_frame():
    data = id3v22((b"TT2", b"\x00East"), (b"TP1", b"\x00Wizet")) + frame(MPEG2) * 4
    info = audio.read_mp3(data + frame(MPEG2)[:100], "b.mp3")
    assert (info.id3v2, info.title, info.album) == ("2.2.0", "East", None)
    assert (info.frames, info.samples, info.trailing, info.id3v1) == (4, 2304, 100, False)
    assert info.first.sample_rate == 22050


def test_mp3_xing_frame_is_not_audio():
    # MPEG-1 stereo, no CRC: the tag sits after 4 + 32 bytes; flags 0 -> LAME string next.
    xing = frame(MPEG1, fill=b"\0" * 32 + b"Info" + b"\0\0\0\0" + b"LAME3.99r")
    info = audio.read_mp3(xing + frame(MPEG1) * 3, "c.mp3")
    assert (info.info_tag, info.encoder, info.frames) == ("Info", "LAME3.99r", 3)


def test_mp3_vbri_and_vbr():
    vbri = frame(MPEG1, fill=b"\0" * 32 + b"VBRI")
    f160 = 0xFFFBA040  # 160 kbit/s
    info = audio.read_mp3(vbri + frame(MPEG1) + frame(f160), "d.mp3")
    assert (info.info_tag, info.frames, info.bitrates, info.vbr) == ("VBRI", 2, (128, 160), True)
    assert "VBR" in audio.describe(info)


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"\0" * 64, "no MPEG audio frame"),
        (b"ID3\x03\0\0\x80\0\0\0", "not syncsafe"),
        (b"ID3\x03\0\0\0\0\x10\0", "runs past the end"),
        (b"ID3\x03", "truncated ID3v2"),
        (frame(MPEG1) + b"\x12" * 500, "lost frame sync"),
        (frame(MPEG1) + frame(MPEG2), "changes mid-stream"),
        (frame(0xFFFD9040), "expected Layer III"),
        (frame(MPEG1, fill=b"\0" * 32 + b"Xing"), "no audio frames"),
    ],
)
def test_mp3_errors(data, message):
    with pytest.raises(audio.AudioError, match=message):
        audio.read_mp3(data, "e.mp3")


def test_id3v2_text_frames_edge_cases():
    assert audio.id3v2_text_frames(b"nope") == ()
    assert audio.id3v2_text_frames(b"ID3\x03\x00\x80" + syncsafe(0)) == ()  # unsynchronised
    tag = id3v23((b"TIT2", b"\x09bad"), (b"TPE1", b"\x03Ok\x00junk"), (b"TRCK", b""))
    assert audio.id3v2_text_frames(tag) == (("TPE1", "Ok"),)
    cut = id3v23((b"TIT2", b"\x00Title"))[:-20]  # a frame that runs past the tag
    assert audio.id3v2_text_frames(cut) == ()
    v24 = b"ID3\x04\0\0" + syncsafe(15) + b"TIT2" + syncsafe(5) + b"\0\0" + b"\x03Abcd"
    assert audio.id3v2_text_frames(v24) == (("TIT2", "Abcd"),)


def test_read_file_and_info_command(tmp_path: Path, capsys):
    (tmp_path / "7.wav").write_bytes(wav(fmt(), chunk(b"data", b"\0" * 4410)))
    (tmp_path / "1.mp3").write_bytes(frame(MPEG1) * 2)
    (tmp_path / "x.ogg").write_bytes(b"OggS")
    with pytest.raises(audio.AudioError, match="not a .wav or .mp3"):
        audio.read_file(tmp_path / "x.ogg")
    assert audio.main(["info", str(tmp_path / "7.wav"), str(tmp_path / "1.mp3")]) == 0
    out = capsys.readouterr().out
    assert "7.wav: WAV PCM, 22050 Hz, mono, 16-bit, 0.100 s" in out
    assert "1.mp3: MPEG-1 Layer III" in out


# -- inventory ------------------------------------------------------------------------------


def write_sounds(d: Path, skip: int | None = None, wrong: int | None = None) -> None:
    for i in range(audio.SOUND_COUNT):
        if i == skip:
            continue
        bgm = (i in audio.BGM_IDS) != (i == wrong)
        if bgm:
            (d / f"{i}.mp3").write_bytes(frame(MPEG1))
        else:
            (d / f"{i}.wav").write_bytes(wav(fmt(), chunk(b"data", b"\0\0")))


def test_inventory_classifies_by_code_list(tmp_path: Path):
    write_sounds(tmp_path)
    sounds = audio.inventory(tmp_path)
    assert [s.id for s in sounds] == list(range(44))
    assert sum(s.kind == "BGM" for s in sounds) == 18
    assert all((s.kind == "BGM") == s.file.endswith(".mp3") for s in sounds)


def test_inventory_rejects_wrong_extension_gaps_and_names(tmp_path: Path):
    write_sounds(tmp_path, wrong=7)
    with pytest.raises(audio.AudioError, match="7.mp3: id 7 is SFX"):
        audio.inventory(tmp_path)
    for p in tmp_path.iterdir():
        p.unlink()
    write_sounds(tmp_path, skip=43)
    with pytest.raises(audio.AudioError, match="expected 0-43"):
        audio.inventory(tmp_path)
    (tmp_path / "07.wav").write_bytes(b"")
    with pytest.raises(audio.AudioError, match="not named by a sound id"):
        audio.inventory(tmp_path)


# -- code facts -----------------------------------------------------------------------------


def insn(site: int, text: str) -> armscan.Insn:
    return armscan.Insn(site, audio.IS_BGM, text.split()[0], text)


def test_bgm_ids_from_the_compare_chain():
    chain = [
        insn(0x10, "movs r1, #0"),
        insn(0x12, "movt r1, #0"),
        insn(0x14, "ldr r0, [sp, #8]"),
        insn(0x16, "cmp r0, r1"),
        insn(0x18, "movs r0, #0x1e"),
        insn(0x1A, "movt r0, #0"),
        insn(0x1C, "ldr r1, [sp, #8]"),
        insn(0x1E, "cmp r1, r0"),
    ]
    assert audio.bgm_ids(chain) == {0, 30}
    with pytest.raises(audio.AudioError, match="cannot tell"):
        audio.bgm_ids([insn(0x10, "cmp r0, r1")])
    assert audio.bgm_ids(
        [
            insn(0x10, "movs r0, #5"),
            insn(0x12, "movt r0, #1"),
            insn(0x14, "mov r1, #2"),
            insn(0x16, "cmp r0, r1"),
        ]
    ) == {2}


def test_map_music_transcription():
    assert audio.map_bgm(0, 0) == (4, False)  # Kerning City town, restarted
    assert audio.map_bgm(0, 3) == (36, True)
    assert audio.map_bgm(2, 8) == (5, False)  # Henesys extra town maps
    assert audio.map_bgm(4, 17) == (34, True)  # boss
    assert audio.map_bgm(11, 0) == (9, False)  # outside the table: effect 9
    assert audio.map_bgm(11, 1) == (None, True)
    calls = audio.map_bgm_calls()
    assert (34, True) in calls and (9, False) in calls and len(calls) == 19
    assert all(i in audio.BGM_IDS for i, _ in calls if i != audio.TOWN_DEFAULT)


@pytest.mark.parametrize(
    ("stem", "expected"),
    [
        ("00", (0, 0)),
        ("29", (2, 9)),
        ("211", (2, 11)),
        ("418", (4, 18)),
        ("105", (10, 5)),
        ("910", (9, 10)),
    ],
)
def test_split_map_name(stem, expected):
    assert audio.split_map_name(stem) == expected


@pytest.mark.parametrize("stem", ["1", "1000", "ab", "0x1"])
def test_split_map_name_rejects(stem):
    with pytest.raises(audio.AudioError):
        audio.split_map_name(stem)


def test_names_file_is_valid_and_complete():
    doc = json.loads(audio.names_json())
    source, entries = names.parse(doc, "audio.json")
    assert source == "audio"
    locations = [e.location for e in entries]
    assert len(locations) == len(set(locations))
    by_name = {e.name: e for e in entries}
    for e in audio.ENTRY_POINTS:
        assert e.address & 1  # Thumb functions
        if e.address not in audio.NAMED_BY_FLOW:
            assert by_name[e.name].kind == "function" and by_name[e.name].location == e.address & ~1
    assert by_name["g_soundSources"].kind == "data"
    flow_names = {a: n for a, n, _c in flow.FLOW_NAMES}
    for address, name in audio.NAMED_BY_FLOW.items():
        assert flow_names[address] == name and name not in by_name


def test_names_command_writes_the_file(tmp_path: Path):
    out = tmp_path / "names" / "audio.json"
    assert audio.main(["names", str(out)]) == 0
    assert out.read_text(encoding="utf-8") == audio.names_json()


# -- call sites -----------------------------------------------------------------------------

K = lambda n: ("k", n, frozenset())  # noqa: E731


def scan_of(refs, call_args, sends=()) -> armscan.Scan:
    return armscan.Scan(refs=list(refs), sends=list(sends), call_args=dict(call_args))


def test_collect_calls_scene_commands_and_ghidra_check():
    caller, scene = 0x9462D, 0x2E5E1
    refs = [
        armscan.Ref(0x94720, caller, audio.PLAY & ~1, "call"),
        armscan.Ref(0x94874, caller, 0x1CF1C, "call"),
        armscan.Ref(0x1D428, audio.PLAY, 0x1D118, "call"),  # inside the player: skipped
        armscan.Ref(0x5000, caller, 0x1234, "call"),  # not an entry point
    ]
    args = {0x94720: (None, K(7), K(0), None), 0x94874: (None,) * 4}
    sends = [
        armscan.Send(0x2E922, scene, audio.SCENE_ADD & ~1, (None, K(1), K(55), K(34)), (K(1),)),
        armscan.Send(0x2E9EA, scene, audio.SCENE_ADD_P2 & ~1, (None, K(3), K(18), K(100))),
        armscan.Send(0x2ED20, scene, audio.SCENE_ADD_CMD & ~1, (None, K(100), K(56), None)),
        armscan.Send(0x2ED30, scene, audio.SCENE_ADD_CMD & ~1, (None, K(101), K(2), None)),
    ]
    xrefs = [
        {"from": "0x94720", "to": "0x1D3B8", "type": "UNCONDITIONAL_CALL", "function": "0x9462D"},
        {"from": "0x94874", "to": "0x1CF1C", "type": "UNCONDITIONAL_CALL", "function": "0x9462D"},
        {"from": "0x1D428", "to": "0x1D118", "type": "UNCONDITIONAL_CALL", "function": "0x1D3B9"},
    ]
    got = audio.collect(scan_of(refs, args, sends), xrefs)
    assert [(c.site, c.callee, c.arg, c.resume) for c in got.calls] == [
        (0x94720, audio.PLAY, 7, 0),
        (0x94874, 0x1CF1D, None, None),
    ]
    assert [(s.command, s.step, s.p1, s.p2) for s in got.scene] == [
        (55, 1, 34, 1),
        (18, 3, 0, 100),
        (56, 100, 0, 0),
    ]
    assert got.ghidra_only == [] and got.armscan_only == []
    assert audio.users_of(got) == {7: {caller}, 34: {scene}}
    xrefs.append(
        {"from": "0x9999A", "to": "0x1D3B8", "type": "UNCONDITIONAL_CALL", "function": "0x99999"}
    )
    assert audio.collect(scan_of(refs, args, sends), xrefs).ghidra_only == [0x9999A]


def test_collect_rejects_unknown_scene_command():
    sends = [armscan.Send(0x10, 0x2E5E1, audio.SCENE_ADD & ~1, (None, K(1), None, None), ())]
    with pytest.raises(audio.AudioError, match="not a constant"):
        audio.collect(scan_of([], {}, sends))


def map_calls() -> list[audio.Call]:
    return [
        audio.Call(0x81000 + 2 * n, audio.PLAY_MAP_BGM, audio.PLAY, i, int(r))
        for n, (i, r) in enumerate(sorted(audio.map_bgm_calls()))
    ]


def sounds() -> list[audio.Sound]:
    return [audio.Sound(i, f"{i}", "SFX", None, 0) for i in range(44)]


def test_check_accepts_consistent_facts_and_reports_problems():
    ok = audio.Collected(map_calls(), [], [], [])
    audio.check(sounds(), ok)
    bad = audio.Collected(
        map_calls()[1:]
        + [
            audio.Call(0x50202, 0x50131, audio.PLAY, None, 0),
            audio.Call(0x50204, 0x50131, audio.GAME_PLAY_SOUND, 44, None),
            audio.Call(0x81028, audio.GAME_PLAY_SOUND, audio.PLAY, None, 0),  # allowed
        ],
        [audio.SceneCommand(0x2E922, 0x2E5E1, 1, 55, None, 1)],
        [0x1],
        [],
    )
    with pytest.raises(audio.AudioError) as e:
        audio.check(sounds()[:-1], bad)
    text = str(e.value)
    for part in (
        "0x50202: id not a constant",
        "id 44 has no file",
        "scene play",
        "PlayMapBGM",
        "Ghidra-only",
        "sound ids",
    ):
        assert part in text
    assert "0x81028" not in text


# -- doc ------------------------------------------------------------------------------------


def test_render_replaces_blocks_and_checks_keys():
    doc = "# T\n<!-- BEGIN generated:a -->\nold\n<!-- END generated:a -->\ntail\n"
    out = audio.render(doc, {"a": "new\n"})
    assert "new\n<!-- END generated:a -->" in out and "old" not in out and "do not edit" in out
    assert audio.render(out, {"a": "new\n"}) == out
    with pytest.raises(audio.AudioError, match="no block for b"):
        audio.render(doc, {"a": "", "b": ""})
    with pytest.raises(audio.AudioError, match="unknown block a"):
        audio.render(doc, {})
    with pytest.raises(audio.AudioError, match="appears 2 times"):
        audio.render(doc + doc, {"a": ""})


def test_blocks_render_tables():
    snd = [
        audio.Sound(0, "0.mp3", "BGM", audio.read_mp3(frame(MPEG1) * 2), 836),
        audio.Sound(7, "7.wav", "SFX", audio.read_wav(wav(fmt(), chunk(b"data", b"\0\0"))), 46),
    ]
    names_ = {0x9462D: "CStateMainMenu::OnKeyPressed"}
    text = audio.files_block(snd, {7: {0x9462D}}, names_)
    assert "| 0 | `0.mp3` | BGM | MPEG-1 L3 joint stereo | 44100 | 2 | 128 kbit/s CBR |" in text
    assert "**never played**" in text and "`CStateMainMenu::OnKeyPressed`" in text
    got = audio.Collected(
        [
            audio.Call(0x94720, 0x9462D, audio.PLAY, 7, 0),
            audio.Call(0x93496, 0x93015, audio.FREE_BGM, 8, None),
            audio.Call(0x934B0, 0x93015, audio.FREE_BGM, 11, None),
            audio.Call(0x65C18, 0x65B55, 0x1D2E9, 100, None),
        ],
        [
            audio.SceneCommand(0x2E922, 0x2E5E1, 1, 55, 34, 1),
            audio.SceneCommand(0x2E9EA, 0x2E5E1, 3, 18, 0, 100),
            audio.SceneCommand(0x2ED20, 0x2E5E1, 100, 56, 0, 0),
        ],
        [],
        [],
    )
    calls = audio.calls_block(got, names_)
    assert (
        "| `CStateMainMenu::OnKeyPressed` `0x9462D` | `CMSoundPlayer::Play` | 7 | 0 | `0x94720` |"
        in calls
    )
    assert (
        "| `FUN_00093014` `0x93015` | `CMSoundPlayer::FreeBGM` | 8, 11 |  | `0x93496`, `0x934B0` |"
        in calls
    )
    assert "| 100 ms |" in calls
    scenes = audio.scene_block(got, names_)
    assert "| 1: **34**, 1 | 100 | 3: 100 |" in scenes
    maps = audio.maps_block([(0, 0), (0, 1), (4, 17)], {0: "Kerningcity.png"})
    assert "| 0 | `Kerningcity.png` | 0: 4 r, 1: 36 |" in maps and "| 4 | `?` | 17: 34 |" in maps
