"""Audio packs: start one from your own copy of the game (#188, ADR 0106).

An audio pack is a folder of music and sound files that replace the game's, by the game's file
names (``audio_pack = DIR`` in settings.ini, or ``--audio-pack DIR``; ``maplethief/audio.h``).
The game's 44 sounds are ``sound/0.mp3`` ... ``sound/43.wav`` in its bundle: 18 music tracks
(BGM, MP3) and 26 effects (SFX, WAV), told apart by the game's code (``docs/re/audio.md``).

``python3 -m mtport.audiopack init DIR [--bundle APP]`` makes DIR (if needed) and writes
``DIR/audiopack.txt``: every replaceable name with its role, its length (seconds and sample
frames) and its format, and for the music the title its file's tag gives. It writes no sound:
put your own ``<id>.wav``, ``<id>.mp3`` or ``<id>.flac`` next to it (any of them replaces
either of the game's), and for a track an optional ``<id>.loop`` with its loop's start and end
frame (``start end``, end excluded) in your file's own frames. The list names the game's files
only; it copies none of them, and DIR is yours to keep outside the repository.

APP is the game's ``.app`` directory or its ``sound/`` folder (default
``work/ipa/Payload/MapleThief.app``, which ``scripts/bootstrap`` extracts). Its files are
untrusted data: their headers are parsed (``mtport.audio``), nothing else.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

from mtport.audio import AudioError, Mp3Info, WavInfo, inventory

LIST_NAME = "audiopack.txt"
DEFAULT_BUNDLE = Path("work/ipa/Payload/MapleThief.app")

HEADER = """\
# Audio pack: the game's sounds you can replace (python3 -m mtport.audiopack init).
#
# Put a file named like the game's next to this list: <id>.wav, <id>.mp3 or <id>.flac replaces
# the game's <id>.mp3 or <id>.wav (any of them for either). PCM WAV (8 to 32-bit), MP3 or FLAC,
# mono or stereo, 8,000 to 192,000 Hz, at most 128 MiB; an effect at most 60 s, a track at most
# 30 min.
# A file the game can't use is logged once and the game's own sound plays instead.
# Music loops as the game asks. <id>.loop (two numbers, "start end": sample frames of your file,
# end excluded) or a WAV's smpl loop sets the loop: the track plays to end, then from start.
# Then set audio_pack = <this folder> in settings.ini, or start with --audio-pack <this folder>.
#
# name      role  length (s)  frames     format
"""


def sound_dir(bundle: Path) -> Path:
    """The ``sound/`` folder of an app directory, or ``bundle`` itself if it is one."""
    return bundle if bundle.name == "sound" else bundle / "sound"


def frames_of(info: WavInfo | Mp3Info) -> int:
    return info.frames if isinstance(info, WavInfo) else info.samples


def rate_of(info: WavInfo | Mp3Info) -> int:
    return info.sample_rate if isinstance(info, WavInfo) else info.first.sample_rate


def channels_of(info: WavInfo | Mp3Info) -> int:
    return info.channels if isinstance(info, WavInfo) else info.first.channels


def listing(sounds: Path) -> str:
    """The list file's text for the game's ``sound/`` folder."""
    lines = [HEADER]
    for s in inventory(sounds):
        info = s.info
        kind = "WAV" if isinstance(info, WavInfo) else "MP3"
        ch = {1: "mono", 2: "stereo"}.get(channels_of(info), f"{channels_of(info)} ch")
        line = (
            f"{s.file:<9} {s.kind:<5} {info.duration:>10.2f}  {frames_of(info):<10} "
            f"{kind} {rate_of(info)} Hz {ch}"
        )
        title = info.title if isinstance(info, Mp3Info) else None
        if title:
            line += f'  "{title.strip()}"'
        lines.append(line.rstrip() + "\n")
    return "".join(lines)


def init(pack: Path, bundle: Path) -> Path:
    """Makes ``pack`` and writes its list; returns the list's path."""
    text = listing(sound_dir(bundle))
    pack.mkdir(parents=True, exist_ok=True)
    out = pack / LIST_NAME
    out.write_text(text, encoding="utf-8", newline="\n")
    return out


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.audiopack", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("init", help=f"write {LIST_NAME}, the replaceable names, into DIR")
    p.add_argument("pack", type=Path, metavar="DIR")
    p.add_argument(
        "--bundle",
        type=Path,
        default=DEFAULT_BUNDLE,
        metavar="APP",
        help=f"the game's .app directory or its sound/ folder (default {DEFAULT_BUNDLE})",
    )
    args = parser.parse_args(argv)
    sounds = sound_dir(args.bundle)
    if not sounds.is_dir():
        print(
            f"mtport.audiopack: {sounds} is no folder (scripts/bootstrap extracts the game)",
            file=sys.stderr,
        )
        return 2
    try:
        out = init(args.pack, args.bundle)
    except (AudioError, OSError) as e:
        print(f"mtport.audiopack: {e}", file=sys.stderr)
        return 2
    print(f"audiopack: wrote {out}: 44 names (18 BGM, 26 SFX)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
