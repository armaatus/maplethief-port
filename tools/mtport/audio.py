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
import sys
from collections.abc import Sequence
from pathlib import Path

from mtport import ghidra_common as gc
from mtport.audio_analysis import (
    BGM_IDS,
    BOSS_BGM,
    BOSS_MAPS,
    ENTRY_POINTS,
    FIELD_BGM,
    FREE_BGM,
    GAME_PLAY_SOUND,
    IS_BGM,
    NAMED_BY_FLOW,
    NAMES,
    PLAY,
    PLAY_MAP_BGM,
    PLAYER_FUNCTIONS,
    SCENE_ADD,
    SCENE_ADD_CMD,
    SCENE_ADD_P2,
    SCENE_ADDERS,
    SCENE_COMMANDS,
    SCENE_RUNNER,
    SOUND_COUNT,
    TOWN_BGM,
    TOWN_DEFAULT,
    TOWN_EXTRA_MAPS,
    Call,
    Collected,
    Entry,
    SceneCommand,
    Sound,
    bgm_ids,
    bgm_ids_in_code,
    check,
    collect,
    inventory,
    map_bgm,
    map_bgm_calls,
    split_map_name,
)
from mtport.audio_doc import (
    BUNDLE,
    DOC,
    FUNCTIONS_JSON,
    MAP_DIR,
    NAMES_OUT,
    SLICE,
    SOUND_DIR,
    XREFS_JSON,
    build,
    calls_block,
    files_block,
    maps_block,
    names_json,
    render,
    scene_block,
    users_of,
)
from mtport.audio_headers import (
    CHANNEL_MODES,
    WAVE_FORMATS,
    AudioError,
    FrameHeader,
    Mp3Info,
    WavInfo,
    describe,
    frame_header,
    id3v2_text_frames,
    read_file,
    read_mp3,
    read_wav,
)

# mtport.audio is the public face of the sound files (audio_headers), the code facts and
# call sites (audio_analysis) and the outputs (audio_doc): it re-exports their names and
# keeps the command line.
__all__ = [
    "AudioError",
    "BGM_IDS",
    "BOSS_BGM",
    "BOSS_MAPS",
    "BUNDLE",
    "CHANNEL_MODES",
    "Call",
    "Collected",
    "DOC",
    "ENTRY_POINTS",
    "Entry",
    "FIELD_BGM",
    "FREE_BGM",
    "FUNCTIONS_JSON",
    "FrameHeader",
    "GAME_PLAY_SOUND",
    "IS_BGM",
    "MAP_DIR",
    "Mp3Info",
    "NAMED_BY_FLOW",
    "NAMES",
    "NAMES_OUT",
    "PLAY",
    "PLAYER_FUNCTIONS",
    "PLAY_MAP_BGM",
    "SCENE_ADD",
    "SCENE_ADDERS",
    "SCENE_ADD_CMD",
    "SCENE_ADD_P2",
    "SCENE_COMMANDS",
    "SCENE_RUNNER",
    "SLICE",
    "SOUND_COUNT",
    "SOUND_DIR",
    "SceneCommand",
    "Sound",
    "TOWN_BGM",
    "TOWN_DEFAULT",
    "TOWN_EXTRA_MAPS",
    "WAVE_FORMATS",
    "WavInfo",
    "XREFS_JSON",
    "bgm_ids",
    "bgm_ids_in_code",
    "build",
    "calls_block",
    "check",
    "collect",
    "describe",
    "files_block",
    "frame_header",
    "id3v2_text_frames",
    "inventory",
    "map_bgm",
    "map_bgm_calls",
    "maps_block",
    "names_json",
    "read_file",
    "read_mp3",
    "read_wav",
    "render",
    "scene_block",
    "split_map_name",
    "users_of",
]


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
