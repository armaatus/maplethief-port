"""Audio inventory (#29): WAV/MP3 header readers, the sound player's code facts, its call sites,
the generated blocks of ``docs/re/audio.md`` and the Ghidra names of the player.

The game ships 44 sounds in ``sound/``, named by number (``0.mp3`` ... ``43.wav``). The number
is the sound id: ``CMSoundPlayer`` formats the id with ``@"%d"`` and asks the main bundle for
that resource with type ``@"mp3"`` (music, one ``AVAudioPlayer`` per id) or ``@"wav"`` (effects,
decoded by ``ExtAudioFile`` into one OpenAL buffer and source per id) in directory
``@"sound"``. Which of the two an id is comes from a hard-coded list in ``Sound_IsBGM``
(:data:`BGM_IDS`), not from the file. The prose of the analysis is in the doc; this module keeps
the parts that can be recomputed:

* :func:`read_wav` and :func:`read_mp3`: standard-library header readers. A WAV is walked chunk
  by chunk (``fmt `` and ``data`` give rate, channels, sample size and duration). An MP3 has its
  ID3v2 tags skipped and every MPEG audio frame header walked, so the duration and the bitrates
  come from the frames themselves; a Xing/Info, VBRI or LAME tag in the first frame is noted.
* :func:`inventory`: every file of ``sound/`` with its header and its class (``BGM`` / ``SFX``).
* :func:`bgm_ids_in_code`: the ids that ``Sound_IsBGM`` compares against, read back from its
  ``cmp`` instructions, so :data:`BGM_IDS` is checked against the binary.
* :func:`collect`: every direct call of the player's entry points (:data:`ENTRY_POINTS`) with
  the id argument, every call of the game's wrapper ``Game_PlaySound`` and every sound command
  that the scene builders (``CAniManager::makeAni*``) add to a scene, from one
  :mod:`mtport.armscan` sweep; the call sites are cross-checked with Ghidra's xrefs.
* :func:`render`: the doc with its ``<!-- BEGIN generated:KEY -->`` blocks rewritten (ADR 0009).

Command line::

    python3 -m mtport.audio names [OUT]          # work/re/names/audio.json
    python3 -m mtport.audio info FILE...          # one line per WAV/MP3 file
    python3 -m mtport.audio generate [--repo D]   # the generated blocks of docs/re/audio.md

``names`` and ``info`` need nothing else; ``generate`` needs the extracted bundle, the armv7
slice and the Ghidra export (``scripts/bootstrap``, ``scripts/re-analyze``) and Capstone. The
sound files and the slice are untrusted data: they are parsed, never executed.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from collections import Counter, defaultdict
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

from mtport import ghidra_common as gc
from mtport.bytereader import FormatError
from mtport.names import hexaddr, parse_address

NAMES_OUT = "work/re/names/audio.json"
SLICE = "work/slices/MapleThief.armv7"
BUNDLE = "work/ipa/Payload/MapleThief.app"
SOUND_DIR = f"{BUNDLE}/sound"
MAP_DIR = f"{BUNDLE}/map"
FUNCTIONS_JSON = "work/ghidra/functions.json"
XREFS_JSON = "work/ghidra/xrefs.json"
DOC = "docs/re/audio.md"


class AudioError(FormatError):
    """A sound file that is not the WAV or MP3 it claims to be, or a code fact that no longer
    holds (a generated doc would be wrong)."""


# -- WAV ------------------------------------------------------------------------------------

WAVE_FORMATS = {1: "PCM", 2: "MS ADPCM", 3: "IEEE float", 0x11: "IMA ADPCM", 0xFFFE: "extensible"}


@dataclass(frozen=True)
class WavInfo:
    format_tag: int
    channels: int
    sample_rate: int
    byte_rate: int
    block_align: int
    bits_per_sample: int
    data_bytes: int
    chunks: tuple[str, ...]  # chunk ids in file order
    trailing: int  # bytes after the RIFF chunk

    @property
    def format_name(self) -> str:
        return WAVE_FORMATS.get(self.format_tag, f"0x{self.format_tag:04X}")

    @property
    def frames(self) -> int:
        return self.data_bytes // self.block_align

    @property
    def duration(self) -> float:
        return self.frames / self.sample_rate


def read_wav(data: bytes, name: str = "<wav>") -> WavInfo:
    """The ``fmt `` and ``data`` chunks of a RIFF/WAVE file. Raises :class:`AudioError`."""
    data = bytes(data)
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise AudioError(f"{name}: not a RIFF/WAVE file")
    (riff_size,) = struct.unpack_from("<I", data, 4)
    end = 8 + riff_size
    if end > len(data):
        raise AudioError(f"{name}: RIFF size {riff_size} runs past the end ({len(data)} bytes)")
    pos = 12
    fmt: tuple | None = None
    data_bytes: int | None = None
    chunks: list[str] = []
    while pos < end:
        if pos + 8 > end:
            raise AudioError(f"{name}: offset 0x{pos:X}: truncated chunk header")
        cid, size = struct.unpack_from("<4sI", data, pos)
        body = pos + 8
        if body + size > end:
            raise AudioError(
                f"{name}: offset 0x{pos:X}: chunk {cid!r} of {size} bytes runs past the end"
            )
        chunks.append(cid.decode("latin-1"))
        if cid == b"fmt ":
            if fmt is not None:
                raise AudioError(f"{name}: offset 0x{pos:X}: second fmt chunk")
            if size < 16:
                raise AudioError(f"{name}: offset 0x{pos:X}: fmt chunk of {size} bytes, need 16")
            fmt = struct.unpack_from("<HHIIHH", data, body)
        elif cid == b"data":
            if data_bytes is not None:
                raise AudioError(f"{name}: offset 0x{pos:X}: second data chunk")
            if fmt is None:
                raise AudioError(f"{name}: offset 0x{pos:X}: data chunk before fmt")
            data_bytes = size
        pos = min(end, body + size + (size & 1))  # chunks are padded to an even size
    if fmt is None:
        raise AudioError(f"{name}: no fmt chunk")
    if data_bytes is None:
        raise AudioError(f"{name}: no data chunk")
    tag, channels, rate, byte_rate, align, bits = fmt
    if channels == 0 or rate == 0 or align == 0:
        raise AudioError(f"{name}: fmt has {channels} channels, {rate} Hz, block {align}")
    if tag == 1 and (align != channels * ((bits + 7) // 8) or byte_rate != rate * align):
        raise AudioError(
            f"{name}: inconsistent PCM fmt: {channels} ch x {bits} bit, block {align}, "
            f"{byte_rate} B/s at {rate} Hz"
        )
    return WavInfo(
        tag, channels, rate, byte_rate, align, bits, data_bytes, tuple(chunks), len(data) - end
    )


# -- MP3 ------------------------------------------------------------------------------------

# Bitrates in kbit/s by (MPEG-1?, layer)[index]; index 0 (free format) and 15 are invalid.
_BITRATES = {
    (True, 1): (0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448),
    (True, 2): (0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384),
    (True, 3): (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320),
    (False, 1): (0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256),
    (False, 2): (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160),
    (False, 3): (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160),
}
_RATES = {3: (44100, 48000, 32000), 2: (22050, 24000, 16000), 0: (11025, 12000, 8000)}
_VERSIONS = {3: "MPEG-1", 2: "MPEG-2", 0: "MPEG-2.5"}
_LAYERS = {3: 1, 2: 2, 1: 3}
CHANNEL_MODES = ("stereo", "joint stereo", "dual channel", "mono")


@dataclass(frozen=True)
class FrameHeader:
    version: str  # "MPEG-1", "MPEG-2", "MPEG-2.5"
    layer: int  # 1, 2, 3
    bitrate: int  # kbit/s
    sample_rate: int
    padding: bool
    channel_mode: str
    crc: bool  # a 16-bit CRC follows the header (protection bit 0)

    @property
    def mpeg1(self) -> bool:
        return self.version == "MPEG-1"

    @property
    def samples(self) -> int:
        if self.layer == 1:
            return 384
        if self.layer == 2 or self.mpeg1:
            return 1152
        return 576

    @property
    def size(self) -> int:
        """Frame length in bytes, header included."""
        if self.layer == 1:
            return (12 * self.bitrate * 1000 // self.sample_rate + self.padding) * 4
        return self.samples // 8 * self.bitrate * 1000 // self.sample_rate + self.padding

    @property
    def channels(self) -> int:
        return 1 if self.channel_mode == "mono" else 2

    @property
    def side_info(self) -> int:
        """Size of the Layer III side information (a Xing/Info tag starts after it)."""
        if self.mpeg1:
            return 17 if self.channels == 1 else 32
        return 9 if self.channels == 1 else 17


def frame_header(data: bytes, pos: int) -> FrameHeader | None:
    """The MPEG audio frame header at ``pos``, or None if there is no valid one there."""
    if pos < 0 or pos + 4 > len(data):
        return None
    (h,) = struct.unpack_from(">I", data, pos)
    if h >> 21 != 0x7FF:
        return None
    vbits, lbits = (h >> 19) & 3, (h >> 17) & 3
    bindex, rindex = (h >> 12) & 15, (h >> 10) & 3
    if vbits == 1 or lbits == 0 or bindex in (0, 15) or rindex == 3:
        return None  # reserved version/layer, free format or a bad index
    mpeg1 = vbits == 3
    layer = _LAYERS[lbits]
    return FrameHeader(
        version=_VERSIONS[vbits],
        layer=layer,
        bitrate=_BITRATES[(mpeg1, layer)][bindex],
        sample_rate=_RATES[vbits][rindex],
        padding=bool((h >> 9) & 1),
        channel_mode=CHANNEL_MODES[(h >> 6) & 3],
        crc=not (h >> 16) & 1,
    )


@dataclass(frozen=True)
class Mp3Info:
    id3v2: str | None  # version of the first ID3v2 tag ("2.3.0"), or None
    audio_start: int  # offset of the first frame
    first: FrameHeader
    frames: int  # audio frames (an info-tag frame is not counted)
    samples: int
    audio_bytes: int
    bitrates: tuple[int, ...]  # the distinct frame bitrates, sorted
    info_tag: str | None  # "Xing", "Info" or "VBRI" if the first frame carries one
    encoder: str | None  # the LAME/Lavf encoder string after a Xing/Info tag
    id3v1: bool
    trailing: int  # bytes after the last whole frame (not counting an ID3v1 tag)
    tags: tuple[tuple[str, str], ...] = ()  # ID3v2 text frames of the first tag: (id, text)

    @property
    def duration(self) -> float:
        return self.samples / self.first.sample_rate

    def tag(self, *ids: str) -> str | None:
        """The first text frame with one of ``ids`` (``"TIT2"``, ``"TT2"``, ...)."""
        for k, v in self.tags:
            if k in ids:
                return v
        return None

    @property
    def title(self) -> str | None:
        return self.tag("TIT2", "TT2")

    @property
    def album(self) -> str | None:
        return self.tag("TALB", "TAL")

    @property
    def vbr(self) -> bool:
        return len(self.bitrates) > 1


def _syncsafe(b: bytes) -> int | None:
    if any(x & 0x80 for x in b):
        return None
    return (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]


_TEXT_ENCODINGS = {0: "latin-1", 1: "utf-16", 2: "utf-16-be", 3: "utf-8"}


def _text_frame(body: bytes) -> str | None:
    if not body or body[0] not in _TEXT_ENCODINGS:
        return None
    raw = body[1:]
    try:
        text = raw.decode(_TEXT_ENCODINGS[body[0]])
    except UnicodeDecodeError:
        return None
    return text.split("\0", 1)[0].strip() or None


def id3v2_text_frames(tag: bytes) -> tuple[tuple[str, str], ...]:
    """The text frames (``T...`` ids, except ``TXXX``) of one ID3v2.2/2.3/2.4 tag, header
    included. Frames that do not decode are skipped; the walk stops at padding or at a frame
    that runs past the tag. Unsynchronised and compressed tags are not decoded."""
    if len(tag) < 10 or tag[:3] != b"ID3":
        return ()
    major, flags = tag[3], tag[5]
    if flags & 0x80 or major not in (2, 3, 4):
        return ()
    pos, out = 10, []
    head = 6 if major == 2 else 10
    while pos + head <= len(tag) and tag[pos] != 0:
        if major == 2:
            fid = tag[pos : pos + 3]
            size = int.from_bytes(tag[pos + 3 : pos + 6], "big")
        else:
            fid = tag[pos : pos + 4]
            raw = tag[pos + 4 : pos + 8]
            size = _syncsafe(raw) if major == 4 else int.from_bytes(raw, "big")
        if size is None or pos + head + size > len(tag):
            break
        body = tag[pos + head : pos + head + size]
        key = fid.decode("latin-1")
        if key.startswith("T") and key not in ("TXX", "TXXX") and key.isalnum():
            text = _text_frame(body)
            if text is not None:
                out.append((key, text))
        pos += head + size
    return tuple(out)


def read_mp3(data: bytes, name: str = "<mp3>") -> Mp3Info:
    """Walk an MP3's frames. Raises :class:`AudioError` unless the file is ID3v2 tags (optional),
    then a chain of MPEG Layer III frames of one version and sample rate, then an optional ID3v1
    tag. A cut-off last frame is allowed and reported as ``trailing``."""
    data = bytes(data)
    pos = 0
    id3v2 = None
    tags: tuple[tuple[str, str], ...] = ()
    while data[pos : pos + 3] == b"ID3":  # some encoders write more than one tag
        if pos + 10 > len(data):
            raise AudioError(f"{name}: offset 0x{pos:X}: truncated ID3v2 header")
        major, rev, flags = data[pos + 3], data[pos + 4], data[pos + 5]
        size = _syncsafe(data[pos + 6 : pos + 10])
        if size is None:
            raise AudioError(f"{name}: offset 0x{pos:X}: ID3v2 size is not syncsafe")
        if id3v2 is None:
            id3v2 = f"2.{major}.{rev}"
            tags = id3v2_text_frames(data[pos : pos + 10 + size])
        pos += 10 + size + (10 if flags & 0x10 else 0)
        if pos > len(data):
            raise AudioError(f"{name}: ID3v2 tag of {size} bytes runs past the end")
    id3v1 = len(data) - pos >= 128 and data[-128:-125] == b"TAG"
    end = len(data) - (128 if id3v1 else 0)
    while pos < end and data[pos] == 0:  # zero padding between the tag and the audio
        pos += 1
    first = frame_header(data, pos)
    if first is None:
        raise AudioError(f"{name}: offset 0x{pos:X}: no MPEG audio frame after the tags")
    if first.layer != 3:
        raise AudioError(f"{name}: {first.version} Layer {first.layer}, expected Layer III")
    audio_start = pos
    info_tag, encoder = _info_tag(data, pos, first)
    frames = samples = audio_bytes = 0
    rates: set[int] = set()
    while end - pos >= 4:  # fewer bytes than a header: a cut-off last frame
        h = frame_header(data, pos)
        if h is None:
            raise AudioError(f"{name}: offset 0x{pos:X}: lost frame sync")
        if (h.version, h.layer, h.sample_rate) != (first.version, first.layer, first.sample_rate):
            raise AudioError(f"{name}: offset 0x{pos:X}: frame format changes mid-stream")
        if pos + h.size > end:
            break  # a cut-off last frame
        if not (info_tag is not None and pos == audio_start):  # the tag frame holds no audio
            frames += 1
            samples += h.samples
            audio_bytes += h.size
            rates.add(h.bitrate)
        pos += h.size
    if frames == 0:
        raise AudioError(f"{name}: no audio frames")
    return Mp3Info(
        id3v2=id3v2,
        audio_start=audio_start,
        first=first,
        frames=frames,
        samples=samples,
        audio_bytes=audio_bytes,
        bitrates=tuple(sorted(rates)),
        info_tag=info_tag,
        encoder=encoder,
        id3v1=id3v1,
        trailing=end - pos,
        tags=tags,
    )


def _info_tag(data: bytes, pos: int, h: FrameHeader) -> tuple[str | None, str | None]:
    """A Xing/Info tag after the side info, or a VBRI tag 32 bytes after the header."""
    at = pos + 4 + (2 if h.crc else 0) + h.side_info
    tag = data[at : at + 4]
    if tag in (b"Xing", b"Info"):
        flags = struct.unpack_from(">I", data, at + 4)[0] if at + 8 <= len(data) else 0
        enc_at = at + 8 + 4 * bool(flags & 1) + 4 * bool(flags & 2) + 100 * bool(flags & 4)
        enc_at += 4 * bool(flags & 8)
        enc = data[enc_at : enc_at + 9]
        encoder = (
            enc.decode("latin-1").rstrip("\0 ") if enc[:4] in (b"LAME", b"Lavf", b"Lavc") else None
        )
        return tag.decode(), encoder
    if data[pos + 36 : pos + 40] == b"VBRI":
        return "VBRI", None
    return None, None


def read_file(path: Path) -> WavInfo | Mp3Info:
    data = path.read_bytes()
    suffix = path.suffix.lower()
    if suffix == ".wav":
        return read_wav(data, path.name)
    if suffix == ".mp3":
        return read_mp3(data, path.name)
    raise AudioError(f"{path.name}: not a .wav or .mp3 file")


def describe(info: WavInfo | Mp3Info) -> str:
    """One line: format, rate, channels, sample size or bitrate, duration."""
    if isinstance(info, WavInfo):
        ch = {1: "mono", 2: "stereo"}.get(info.channels, f"{info.channels} ch")
        return (
            f"WAV {info.format_name}, {info.sample_rate} Hz, {ch}, {info.bits_per_sample}-bit, "
            f"{info.duration:.3f} s"
        )
    f = info.first
    rate = f"{info.bitrates[0]}" if not info.vbr else f"{info.bitrates[0]}-{info.bitrates[-1]}"
    return (
        f"{f.version} Layer III, {f.sample_rate} Hz, {f.channel_mode}, {rate} kbit/s"
        f"{' VBR' if info.vbr else ''}, {info.duration:.3f} s"
    )


# -- what the code says ---------------------------------------------------------------------

SOUND_COUNT = 44  # the player's loops and alGenBuffers/alGenSources all use 0x2C
# Sound_IsBGM (0x1CC79): these ids are music (AVAudioPlayer, .mp3); every other id is an effect.
BGM_IDS = frozenset((*range(0, 7), 30, 31, *range(34, 43)))
IS_BGM = 0x1CC79


# The direct entry points of the sound player that game code calls, and what their arguments
# mean. ``arg`` is the register index of the id (or of the vibration time), ``resume`` that of
# the resume flag; None where the function takes neither.
@dataclass(frozen=True)
class Entry:
    address: int  # function pointer
    name: str
    arg: int | None = None
    arg_kind: str = ""  # "id" or "ms"
    resume: int | None = None


PLAY = 0x1D3B9
FREE_BGM = 0x1D011
GAME_PLAY_SOUND = 0x80F91
PLAY_MAP_BGM = 0x81031
SCENE_RUNNER = 0x83C71  # CStateGame::PaintExitToMenu (#12's name), runs the scene commands

ENTRY_POINTS: tuple[Entry, ...] = (
    Entry(0x1E099, "CMSoundPlayer::GetInstance"),
    Entry(PLAY, "CMSoundPlayer::Play", arg=1, arg_kind="id", resume=2),
    Entry(0x1D53D, "CMSoundPlayer::ResumeBGM"),
    Entry(0x1CF1D, "CMSoundPlayer::StopAllBGM"),
    Entry(FREE_BGM, "CMSoundPlayer::FreeBGM", arg=1, arg_kind="id"),
    Entry(0x1CD75, "CMSoundPlayer::MusicOff"),
    Entry(0x1CE45, "CMSoundPlayer::MusicOn"),
    Entry(0x1D351, "CMSoundPlayer::EffectsOff"),
    Entry(0x1D371, "CMSoundPlayer::EffectsOn"),
    Entry(0x1D2E9, "CMSoundPlayer::Vibrate", arg=1, arg_kind="ms"),
    Entry(0x1D309, "CMSoundPlayer::VibratePreview"),
    Entry(0x1CC71, "CMSoundPlayer::StopVibrate"),
    Entry(0x1CC61, "CMSoundPlayer::SetVibrate", arg=1, arg_kind="flag"),
    Entry(0x1CC11, "CMSoundPlayer::SetVibrateParam0C"),
    Entry(0x1CC39, "CMSoundPlayer::SetVibrateParam10"),
    Entry(GAME_PLAY_SOUND, "Game_PlaySound", arg=1, arg_kind="id"),
)

# The scene builders (CAniManager::makeAni*) append commands to a scene list with these;
# CStateGame's scene runner (0x83C71) executes them. Arguments: (list, step, command, p1, p2).
SCENE_ADD = 0x26809  # p1 in r3, p2 on the stack
SCENE_ADD_P2 = 0x26895  # (list, step, command, p2): p1 = 0, p2 in r3
SCENE_ADD_CMD = 0x268C5  # (list, step, command): p1 = p2 = 0
SCENE_ADDERS = (SCENE_ADD, SCENE_ADD_P2, SCENE_ADD_CMD)
# Sound commands of the scene runner (its switch on command - 1).
SCENE_VIBRATE, SCENE_PLAY, SCENE_STOP_BGM, SCENE_STOP_VIBRATE = 18, 55, 56, 57
SCENE_COMMANDS = {
    SCENE_VIBRATE: "vibrate (p2 ms)",
    SCENE_PLAY: "play (id p1, resume p2 == 1)",
    SCENE_STOP_BGM: "stop all music",
    SCENE_STOP_VIBRATE: "stop vibrating (no-op)",
}

# CStateGame::PlayMapBGM (0x81031), transcribed. Town maps restart their track (resume 0);
# every other map resumes it (resume 1), so field music continues where it stopped.
TOWN_EXTRA_MAPS = frozenset({(2, 8), (2, 9)})  # besides map 0 of every area
TOWN_BGM = {0: 4, 1: 6, 2: 5, 3: 2, 4: 1, 5: 41, 6: 2, 7: 2, 8: 2, 9: 3, 10: 40}
TOWN_DEFAULT = 9  # area outside 0-10: plays effect 9 (the warp sound) instead
BOSS_MAPS = frozenset({(3, 8), (4, 17), (7, 4), (9, 5), (10, 5)})
BOSS_BGM = 34
FIELD_BGM = {0: 36, 1: 37, 2: 35, 3: 38, 4: 1, 5: 41, 6: 2, 7: 2, 8: 2, 9: 39, 10: 40}


def map_bgm(area: int, map_no: int) -> tuple[int | None, bool]:
    """(sound id, resume) that ``CStateGame::PlayMapBGM`` plays on (area, map); id None if it
    plays nothing."""
    if map_no == 0 or (area, map_no) in TOWN_EXTRA_MAPS:
        return TOWN_BGM.get(area, TOWN_DEFAULT), False
    if (area, map_no) in BOSS_MAPS:
        return BOSS_BGM, True
    return FIELD_BGM.get(area), True


def map_bgm_calls() -> set[tuple[int, bool]]:
    """Every (id, resume) pair the transcription can produce: one per call in 0x81031."""
    out = {(i, False) for i in TOWN_BGM.values()} | {(TOWN_DEFAULT, False), (BOSS_BGM, True)}
    return out | {(i, True) for i in FIELD_BGM.values()}


def split_map_name(stem: str) -> tuple[int, int]:
    """(area, map) of a ``map/<area><map>.map`` file name (docs/formats/map.md): the numbers
    are not padded, and in the shipped set a 3-digit name starting with 10 is area 10."""
    if not stem.isdigit() or len(stem) not in (2, 3):
        raise AudioError(f"{stem}.map: not an <area><map> name")
    if len(stem) == 3 and stem.startswith("10"):
        return 10, int(stem[2])
    return int(stem[0]), int(stem[1:])


# -- Ghidra names (work/re/names/audio.json) ------------------------------------------------

# (address, kind, name, comment). Function pointers carry the Thumb bit. The entry points are
# in ENTRY_POINTS; these are the rest of the player and the helpers the doc names.
_MORE_NAMES: tuple[tuple[str, str, str, str], ...] = (
    ("0x1CBDD", "function", "SoundPlayer_InterruptionListener", "AudioSession: 1 begin, 0 end"),
    ("0x1CC79", "function", "Sound_IsBGM", "id in {0-6, 30, 31, 34-42}: AVAudioPlayer + .mp3"),
    ("0x1D119", "function", "CMSoundPlayer::LoadBGM", "sound/<id>.mp3 -> AVAudioPlayer, loops -1"),
    ("0x1D329", "function", "SoundPlayer_SetListenerGain", "alListenerf(AL_GAIN, gain)"),
    ("0x1D391", "function", "SoundPlayer_PlayEffect", "alSourcePlay(g_soundSources[id])"),
    ("0x1D555", "function", "SoundPlayer_AttachBuffers", "AL_BUFFER of source i = buffer i"),
    ("0x1D5F5", "function", "alBufferDataStaticProc", "MyOpenALSupport.h; only for id 0 (dead)"),
    ("0x1D67D", "function", "SoundPlayer_LoadEffects", "every non-BGM id: sound/<id>.wav"),
    ("0x1D8B1", "function", "SoundPlayer_InitOpenAL", "device, context, 44 buffers + sources"),
    ("0x1E1DD", "function", "SoundPlayer_ShutdownOpenAL", "delete sources/buffers, context"),
    ("0x1E259", "function", "SoundPlayer_EndInterruption", "ambient category, context current"),
    ("0x1E315", "function", "SoundPlayer_BeginInterruption", "session inactive, suspend context"),
    ("0xA0045", "function", "MyGetOpenALAudioData", "Apple oalTouch: ExtAudioFile -> 16-bit PCM"),
    ("0x81031", "function", "CStateGame::PlayMapBGM", "music of (area +0x84, map +0x88)"),
    ("0x26809", "function", "Scene_AddCommand", "(list, step, cmd, p1, p2)"),
    ("0x26895", "function", "Scene_AddCommandP2", "(list, step, cmd, p2): p1 = 0"),
    ("0x268C5", "function", "Scene_AddCommand0", "(list, step, cmd): p1 = p2 = 0"),
    ("0x26981", "function", "Scene_ArgP1", "p1 of the current command"),
    ("0x269C9", "function", "Scene_ArgP2", "p2 of the current command"),
    ("0x11AAA0", "data", "g_soundSources", "ALuint[44], one OpenAL source per id"),
    ("0x11AB50", "data", "g_soundBuffers", "ALuint[44], one OpenAL buffer per id"),
    ("0x11AC00", "data", "g_alBufferDataStatic", "alcGetProcAddress result, cached"),
    ("0x11AC10", "data", "g_soundNames", "char*[44] for error messages, never written"),
    ("0x11ACC0", "data", "g_soundStaticData", "malloc'd PCM for alBufferDataStatic"),
    ("0x11ACC4", "data", "g_alContext", "the OpenAL context"),
    ("0x11A458", "data", "g_soundLastResume", "resume flag of the last Play, never read"),
    ("0x11A460", "data", "g_soundPlayer", "CMSoundPlayer singleton"),
)

_ENTRY_COMMENTS = {
    0x1E099: "new(0x18) on first use",
    PLAY: "(this, id, resume): BGM via AVAudioPlayer, else SoundPlayer_PlayEffect",
    0x1D53D: "Play(this, this+4 = last BGM id, 0)",
    0x1CF1D: "stop every playing BGM player",
    0x1D011: "(this, id): stop and release the BGM player",
    0x1CD75: "if music is on (+0x17): every player volume 0, +0x17 = 0",
    0x1CE45: "if music is off: every player volume 0.7, +0x17 = 1",
    0x1D351: "if effects are on (+0x16): listener gain 0, +0x16 = 0",
    0x1D371: "if effects are off: listener gain 1, +0x16 = 1",
    0x1D2E9: "(this, ms): kSystemSoundID_Vibrate if +0x14; ms unused",
    0x1D309: "Vibrate(this, 500) if +0x14",
    0x1CC71: "empty",
    0x1CC61: "+0x14 = flag",
    0x1CC11: "+0x0C = v; v == 0 clears +0x14",
    0x1CC39: "+0x10 = v; v == 0 clears +0x14",
    GAME_PLAY_SOUND: "(game, id): Play(player, id, 0)",
}

# Named by #12's flow.json (mtport.flow.FLOW_NAMES) with the same names; flow skips addresses
# that another names file covers, so these stay out of ours.
NAMED_BY_FLOW = {0x1E099: "CMSoundPlayer::GetInstance", 0x1D9B1: "CMSoundPlayer::CMSoundPlayer"}

NAMES: tuple[tuple[str, str, str, str], ...] = tuple(
    sorted(
        [
            (hexaddr(e.address), "function", e.name, _ENTRY_COMMENTS[e.address])
            for e in ENTRY_POINTS
            if e.address not in NAMED_BY_FLOW
        ]
        + list(_MORE_NAMES),
        key=lambda n: parse_address(n[0]),
    )
)


def names_json() -> str:
    return gc.names_json("audio", NAMES)


# The player's own functions: calls between them are the player's internals, not uses.
PLAYER_FUNCTIONS = frozenset(
    {parse_address(a) for a, k, _n, _c in NAMES if k == "function"} | set(NAMED_BY_FLOW)
) - {
    GAME_PLAY_SOUND,
    PLAY_MAP_BGM,
    *SCENE_ADDERS,
    0x26981,
    0x269C9,
}


# -- inventory ------------------------------------------------------------------------------


@dataclass(frozen=True)
class Sound:
    id: int
    file: str
    kind: str  # "BGM" or "SFX"
    info: WavInfo | Mp3Info
    size: int


def inventory(sound_dir: Path) -> list[Sound]:
    """Every file of ``sound/``, by id. Raises :class:`AudioError` unless the files are exactly
    ``0``-``43`` with ``.mp3`` for the ids in :data:`BGM_IDS` and ``.wav`` for the rest (the
    paths the code builds)."""
    files = sorted(p for p in sound_dir.iterdir() if p.is_file())
    out = []
    for p in files:
        if not p.stem.isdigit() or str(int(p.stem)) != p.stem:
            raise AudioError(f"{p.name}: not named by a sound id")
        i = int(p.stem)
        kind = "BGM" if i in BGM_IDS else "SFX"
        want = ".mp3" if kind == "BGM" else ".wav"
        if p.suffix != want:
            raise AudioError(f"{p.name}: id {i} is {kind}, the code loads sound/{i}{want}")
        out.append(Sound(i, p.name, kind, read_file(p), p.stat().st_size))
    out.sort(key=lambda s: s.id)
    if [s.id for s in out] != list(range(SOUND_COUNT)):
        raise AudioError(f"sound/: ids {[s.id for s in out]}, expected 0-{SOUND_COUNT - 1}")
    return out


# -- call sites -----------------------------------------------------------------------------


@dataclass(frozen=True, order=True)
class Call:
    """A direct call of an :data:`ENTRY_POINTS` function from outside the player."""

    site: int
    caller: int  # function pointer
    callee: int  # function pointer
    arg: int | None  # the id (or ms, flag) if constant
    resume: int | None


@dataclass(frozen=True, order=True)
class SceneCommand:
    """A sound command added to a scene by a ``CAniManager::makeAni*`` builder."""

    site: int
    builder: int  # function pointer
    step: int | None
    command: int
    p1: int | None
    p2: int | None


@dataclass
class Collected:
    calls: list[Call]
    scene: list[SceneCommand]
    ghidra_only: list[int]  # call sites Ghidra has and armscan has not
    armscan_only: list[int]


def _const(v) -> int | None:
    return v[1] if v is not None and v[0] == "k" else None


def _signed(v: int | None) -> int | None:
    return v - (1 << 32) if v is not None and v & 0x80000000 else v


def collect(scan, xrefs: Iterable[dict] = ()) -> Collected:
    """Calls and scene commands from an :func:`mtport.armscan.scan` of the slice that was run
    with :data:`SCENE_ADDERS` (Thumb bit cleared) as dispatchers; ``xrefs`` are Ghidra's
    ``xrefs.json`` records for the cross-check."""
    entries = {e.address & ~1: e for e in ENTRY_POINTS}
    calls = []
    for r in scan.refs:
        if r.kind != "call" or r.target not in entries or r.function in PLAYER_FUNCTIONS:
            continue
        e = entries[r.target]
        regs = scan.call_args.get(r.site, (None,) * 4)
        arg = _signed(_const(regs[e.arg])) if e.arg is not None else None
        resume = _const(regs[e.resume]) if e.resume is not None else None
        calls.append(Call(r.site, r.function, e.address, arg, resume))
    adders = {a & ~1 for a in SCENE_ADDERS}
    scene = []
    for s in scan.sends:
        if s.stub not in adders or s.function in SCENE_ADDERS:
            continue
        command = _const(s.regs[2])
        if command is None:
            raise AudioError(f"0x{s.site:X}: scene command not a constant")
        if command not in SCENE_COMMANDS:
            continue
        if s.stub == SCENE_ADD & ~1:
            p1, p2 = _const(s.regs[3]), _const(s.stack[0]) if s.stack else None
        elif s.stub == SCENE_ADD_P2 & ~1:
            p1, p2 = 0, _const(s.regs[3])
        else:
            p1 = p2 = 0
        scene.append(SceneCommand(s.site, s.function, _const(s.regs[1]), command, p1, p2))
    # Ghidra's view of the same calls.
    targets = {hexaddr(e.address & ~1) for e in ENTRY_POINTS}
    player = {hexaddr(a) for a in PLAYER_FUNCTIONS}
    ghidra = {
        parse_address(x["from"])
        for x in xrefs
        if x["to"] in targets and x["type"].endswith("CALL") and x["function"] not in player
    }
    ours = {c.site for c in calls}
    return Collected(
        sorted(calls),
        sorted(scene),
        sorted(ghidra - ours) if xrefs else [],
        sorted(ours - ghidra) if xrefs else [],
    )


def check(sounds: list[Sound], got: Collected) -> None:
    """Raise :class:`AudioError` if a code fact the doc states does not hold."""
    problems = []
    unknown_ok = {GAME_PLAY_SOUND, SCENE_RUNNER}  # pass an id they were given
    for c in got.calls:
        if c.callee in (PLAY, GAME_PLAY_SOUND) and c.arg is None and c.caller not in unknown_ok:
            problems.append(f"0x{c.site:X}: id not a constant")
        if (
            c.callee in (PLAY, GAME_PLAY_SOUND)
            and c.arg is not None
            and not 0 <= c.arg < SOUND_COUNT
        ):
            problems.append(f"0x{c.site:X}: id {c.arg} has no file")
    for s in got.scene:
        if s.command == SCENE_PLAY and (s.p1 is None or s.p2 is None):
            problems.append(f"0x{s.site:X}: scene play without constant id/resume")
    mine = {
        (c.arg, bool(c.resume)) for c in got.calls if c.caller == PLAY_MAP_BGM and c.callee == PLAY
    }
    if mine != map_bgm_calls():
        problems.append(
            f"PlayMapBGM: code plays {sorted(mine)}, transcription {sorted(map_bgm_calls())}"
        )
    if got.ghidra_only or got.armscan_only:
        problems.append(f"Ghidra-only sites {got.ghidra_only}, armscan-only {got.armscan_only}")
    if [s.id for s in sounds] != list(range(SOUND_COUNT)):
        problems.append("sound ids")
    if problems:
        raise AudioError("; ".join(problems))


_MOV_IMM = re.compile(r"(r\d+), #(0x[0-9a-f]+|\d+)$")
_CMP_REGS = re.compile(r"(r\d+), (r\d+)$")


def bgm_ids(insns: Iterable) -> frozenset[int]:
    """The constants ``Sound_IsBGM`` compares the id with. The -O0 code loads each one with
    ``movs rX, #n`` (+ ``movt rX, #0``), reloads the id with ``ldr``, then ``cmp``s the two
    registers; ``insns`` are its ``mov``/``movs``/``movt``/``ldr``/``cmp`` instructions."""
    regs: dict[str, int] = {}
    out = set()
    for insn in sorted(insns):
        dst = insn.text.split(None, 1)[1].split(",", 1)[0] if " " in insn.text else ""
        if insn.mnemonic in ("mov", "movs") and (m := _MOV_IMM.search(insn.text)):
            regs[m[1]] = int(m[2], 0)
        elif insn.mnemonic == "movt" and (m := _MOV_IMM.search(insn.text)):
            if int(m[2], 0) != 0:
                regs.pop(m[1], None)
        elif insn.mnemonic == "ldr":
            regs.pop(dst, None)
        elif insn.mnemonic == "cmp" and (m := _CMP_REGS.search(insn.text)):
            known = [regs[r] for r in (m[1], m[2]) if r in regs]
            if len(known) != 1:
                raise AudioError(f"0x{insn.site:X}: cannot tell the constant in {insn.text!r}")
            out.add(known[0])
    return frozenset(out)


def bgm_ids_in_code(image) -> frozenset[int]:
    """:func:`bgm_ids` of ``Sound_IsBGM`` in the slice (needs Capstone)."""
    from mtport import armscan

    found = armscan.find_instructions(image, ("mov", "movs", "movt", "ldr", "cmp"))
    return bgm_ids(i for i in found if i.function == IS_BGM)


# -- doc ------------------------------------------------------------------------------------


def render(doc: str, generated: dict[str, str]) -> str:
    """``doc`` with each generated block's body replaced (:func:`ghidra_common.render_blocks`).
    Every key must have exactly one block, and every block a key."""
    return gc.render_blocks(doc, generated, "python3 -m mtport.audio generate", DOC, AudioError)


def _hex(a: int) -> str:
    return f"`{hexaddr(a)}`"


def _fn(a: int, names: dict[int, str]) -> str:
    return f"`{names.get(a, f'FUN_{a & ~1:08x}')}` {_hex(a)}"


def _ranges(ids: Iterable[int]) -> str:
    ids = sorted(set(ids))
    out, i = [], 0
    while i < len(ids):
        j = i
        while j + 1 < len(ids) and ids[j + 1] == ids[j] + 1:
            j += 1
        out.append(str(ids[i]) if j == i else f"{ids[i]}–{ids[j]}")
        i = j + 1
    return ", ".join(out)


def _secs(t: float) -> str:
    return f"{t:.2f}"


def files_block(sounds: list[Sound], users: dict[int, set[int]], names: dict[int, str]) -> str:
    rows = [
        "| Id | File | Class | Format | Rate (Hz) | Ch | Bits / bitrate | Duration (s) | Bytes "
        "| ID3 title | Played by |",
        "|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for s in sounds:
        i = s.info
        if isinstance(i, WavInfo):
            fmt, rate, ch = f"WAV {i.format_name}", i.sample_rate, i.channels
            depth, title = f"{i.bits_per_sample}-bit", ""
        else:
            fmt, rate, ch = (
                f"{i.first.version} L3 {i.first.channel_mode}",
                i.first.sample_rate,
                i.first.channels,
            )
            depth = f"{i.bitrates[0]} kbit/s" + (" VBR" if i.vbr else " CBR")
            title = i.title or ""
        who = ", ".join(
            f"`{names.get(f, f'FUN_{f & ~1:08x}')}`" for f in sorted(users.get(s.id, ()))
        )
        rows.append(
            f"| {s.id} | `{s.file}` | {s.kind} | {fmt} | {rate} | {ch} | {depth} "
            f"| {_secs(i.duration)} | {s.size:,} | {title} | {who or '**never played**'} |"
        )
    bgm = [s for s in sounds if s.kind == "BGM"]
    sfx = [s for s in sounds if s.kind == "SFX"]
    rows.append("")
    rows.append(
        f"{len(sounds)} files: {len(bgm)} BGM ({_ranges(s.id for s in bgm)}), "
        f"{len(sfx)} SFX ({_ranges(s.id for s in sfx)}). "
        f"BGM {_secs(sum(s.info.duration for s in bgm))} s in total, "
        f"SFX {_secs(sum(s.info.duration for s in sfx))} s. "
        f"Never played: {_ranges(s.id for s in sounds if s.id not in users) or 'none'}."
    )
    return "\n".join(rows) + "\n"


def calls_block(got: Collected, names: dict[int, str]) -> str:
    entries = {e.address: e for e in ENTRY_POINTS}
    groups: dict[tuple, list[Call]] = defaultdict(list)
    for c in got.calls:
        e = entries[c.callee]
        if c.callee == FREE_BGM:  # one row per caller: the destructors free 22 ids each
            arg = ""
        elif e.arg_kind == "id":
            arg = "?" if c.arg is None else str(c.arg)
        elif e.arg is not None:
            arg = "?" if c.arg is None else f"{c.arg} {e.arg_kind}"
        else:
            arg = ""
        resume = "" if e.resume is None else ("?" if c.resume is None else str(c.resume))
        groups[(c.caller, c.callee, arg, resume)].append(c)
    rows = ["| Caller | Calls | Id / arg | Resume | Sites |", "|---|---|---|---|---|"]
    for (caller, callee, arg, resume), calls in sorted(groups.items(), key=lambda kv: kv[1][0]):
        if callee == FREE_BGM:
            arg = _ranges(c.arg for c in calls if c.arg is not None)
        rows.append(
            f"| {_fn(caller, names)} | `{entries[callee].name}` | {arg} | {resume} "
            f"| {', '.join(_hex(c.site) for c in calls)} |"
        )
    n_play = sum(1 for c in got.calls if c.callee in (PLAY, GAME_PLAY_SOUND))
    rows.append("")
    rows.append(
        f"{len(got.calls)} call sites in {len({c.caller for c in got.calls})} functions "
        f"({n_play} of `Play` or `Game_PlaySound`). armscan and Ghidra's call xrefs list the "
        f"same sites."
    )
    return "\n".join(rows) + "\n"


def scene_block(got: Collected, names: dict[int, str]) -> str:
    by: dict[int, list[SceneCommand]] = defaultdict(list)
    for s in got.scene:
        by[s.builder].append(s)
    rows = [
        "| Scene builder | Plays (step: id, resume) | Stops music at step "
        "| Vibrations (step: ms) |",
        "|---|---|---|---|",
    ]
    for b in sorted(by, key=lambda f: names.get(f, "")):
        cmds = sorted(by[b], key=lambda s: (s.step if s.step is not None else -1, s.site))
        plays = "; ".join(f"{s.step}: **{s.p1}**, {s.p2}" for s in cmds if s.command == SCENE_PLAY)
        stops = ", ".join(str(s.step) for s in cmds if s.command == SCENE_STOP_BGM)
        vib = ", ".join(f"{s.step}: {s.p2}" for s in cmds if s.command == SCENE_VIBRATE)
        rows.append(f"| {_fn(b, names)} | {plays or '–'} | {stops or '–'} | {vib or '–'} |")
    counts = Counter(s.command for s in got.scene)
    rows.append("")
    rows.append(
        f"{len(got.scene)} sound commands in {len(by)} scenes: "
        + ", ".join(f"{counts[c]} × {c} ({SCENE_COMMANDS[c]})" for c in sorted(SCENE_COMMANDS))
        + ". Each `Stops vibrating` follows a vibration and is not listed. Sites: "
        + ", ".join(_hex(s.site) for s in got.scene)
        + "."
    )
    return "\n".join(rows) + "\n"


def maps_block(maps: list[tuple[int, int]], tilesets: dict[int, str]) -> str:
    rows = ["| Area | Tileset | Map → BGM id (r = restart, otherwise resume) |", "|---|---|---|"]
    by: dict[int, list[int]] = defaultdict(list)
    for a, m in maps:
        by[a].append(m)
    for a in sorted(by):
        parts = []
        for m in sorted(by[a]):
            i, resume = map_bgm(a, m)
            parts.append(f"{m}: {i}{'' if resume else ' r'}")
        rows.append(f"| {a} | `{tilesets.get(a, '?')}` | {', '.join(parts)} |")
    used = sorted({map_bgm(a, m)[0] for a, m in maps})
    rows.append("")
    rows.append(f"{len(maps)} maps in {len(by)} areas use BGM {_ranges(used)}.")
    return "\n".join(rows) + "\n"


def users_of(got: Collected) -> dict[int, set[int]]:
    """id -> the functions that play it (direct calls with a constant id and scene builders)."""
    out: dict[int, set[int]] = defaultdict(set)
    for c in got.calls:
        if c.callee in (PLAY, GAME_PLAY_SOUND) and c.arg is not None:
            out[c.arg].add(c.caller)
    for s in got.scene:
        if s.command == SCENE_PLAY and s.p1 is not None:
            out[s.p1].add(s.builder)
    return out


def build(repo: Path) -> tuple[list[Sound], Collected, str]:
    """Everything ``generate`` computes: the inventory, the sites and the rendered doc."""
    from mtport import armscan, macho, mapprj

    sounds = inventory(repo / SOUND_DIR)
    image = macho.load(repo / SLICE)
    if bgm_ids_in_code(image) != BGM_IDS:
        raise AudioError(f"Sound_IsBGM compares {sorted(bgm_ids_in_code(image))}, not BGM_IDS")
    scan = armscan.scan(image, [a & ~1 for a in SCENE_ADDERS])
    got = collect(scan, gc.load_export(repo / XREFS_JSON)["xrefs"])
    check(sounds, got)
    functions = gc.load_export(repo / FUNCTIONS_JSON)["functions"]
    names = {parse_address(f["address"]): f["name"] for f in functions}
    maps = sorted(split_map_name(p.stem) for p in (repo / MAP_DIR).glob("*.map"))
    tilesets = {}
    for p in (repo / MAP_DIR).glob("*.mapprj"):
        proj = mapprj.decode(p.read_bytes(), p.name)
        tilesets[int(p.stem)] = ", ".join(proj.tilesets)
    blocks = {
        "files": files_block(sounds, users_of(got), names),
        "calls": calls_block(got, names),
        "scenes": scene_block(got, names),
        "maps": maps_block(maps, tilesets),
    }
    doc = render((repo / DOC).read_text(encoding="utf-8"), blocks)
    return sounds, got, doc


# -- command line ---------------------------------------------------------------------------


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.audio", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("names", help=f"write the Ghidra names file (default {NAMES_OUT})")
    p.add_argument("out", nargs="?", default=NAMES_OUT)
    p = sub.add_parser("info", help="describe WAV/MP3 files")
    p.add_argument("files", nargs="+", type=Path)
    p = sub.add_parser("generate", help=f"rewrite the generated blocks of {DOC}")
    p.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    try:
        if args.cmd == "names":
            out = Path(args.out)
            gc.write_text(out, names_json())
            print(f"wrote {out} ({len(NAMES)} names)")
            return 0
        if args.cmd == "info":
            for f in args.files:
                print(f"{f.name}: {describe(read_file(f))}")
            return 0
        sounds, got, doc = build(args.repo)
        gc.write_text(args.repo / DOC, doc)
        print(
            f"wrote the generated blocks of {DOC}: {len(sounds)} sounds, {len(got.calls)} call "
            f"sites, {len(got.scene)} scene commands"
        )
        return 0
    except (OSError, ValueError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        if isinstance(e, FileNotFoundError):
            print("run scripts/bootstrap and scripts/re-analyze first", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
