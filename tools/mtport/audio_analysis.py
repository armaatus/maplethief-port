"""The code half of :mod:`mtport.audio`: the sound player's facts (:data:`BGM_IDS`,
:data:`ENTRY_POINTS`, the scene commands, ``PlayMapBGM`` transcribed), its Ghidra names
table, the ``sound/`` inventory and the call sites from one armscan sweep. The module
docstring of ``tools/mtport/audio.py`` describes it; that module re-exports every name
defined here.
"""

from __future__ import annotations

import re
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from mtport.audio_headers import AudioError, Mp3Info, WavInfo, read_file
from mtport.names import hexaddr, parse_address

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
