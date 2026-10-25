"""The outputs of :mod:`mtport.audio`: the names file (:func:`names_json`) and the
generated blocks of ``docs/re/audio.md`` (:func:`build`, :func:`render`).
``mtport.audio`` re-exports every name defined here.
"""

from __future__ import annotations

from collections import Counter, defaultdict
from collections.abc import Iterable
from pathlib import Path

from mtport import ghidra_common as gc
from mtport.audio_analysis import (
    BGM_IDS,
    ENTRY_POINTS,
    FREE_BGM,
    GAME_PLAY_SOUND,
    NAMES,
    PLAY,
    SCENE_ADDERS,
    SCENE_COMMANDS,
    SCENE_PLAY,
    SCENE_STOP_BGM,
    SCENE_VIBRATE,
    Call,
    Collected,
    SceneCommand,
    Sound,
    bgm_ids_in_code,
    check,
    collect,
    inventory,
    map_bgm,
    split_map_name,
)
from mtport.audio_headers import AudioError, WavInfo
from mtport.names import hexaddr, parse_address

NAMES_OUT = "work/re/names/audio.json"
SLICE = "work/slices/MapleThief.armv7"
BUNDLE = "work/ipa/Payload/MapleThief.app"
SOUND_DIR = f"{BUNDLE}/sound"
MAP_DIR = f"{BUNDLE}/map"
FUNCTIONS_JSON = "work/ghidra/functions.json"
XREFS_JSON = "work/ghidra/xrefs.json"
DOC = "docs/re/audio.md"


def names_json() -> str:
    return gc.names_json("audio", NAMES)


# The player's own functions: calls between them are the player's internals, not uses.
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
