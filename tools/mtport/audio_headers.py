"""The sound-file half of :mod:`mtport.audio`: the WAV and MP3 header readers
(:func:`read_wav`, :func:`read_mp3`, :func:`read_file`, :func:`describe`) and
:class:`AudioError`. ``mtport.audio`` re-exports every name defined here.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

from mtport.bytereader import FormatError


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
