"""The scene runner's commands, what each one changes, and every scene's state steps (#174).

``docs/re/qol.md#cutscene-skip`` has the evidence: the scene runner
``CStateGame::PaintExitToMenu`` ``0x83C71`` runs one step of a ``CAniManager`` command list per
``CStateGame::Paint`` in game mode 9 (``Scene_NextStep`` ``0x26A11``), unless a speech box waits
for a key, and switches on ``command - 1`` for each command of the step. :data:`COMMANDS`
classifies the 64 command numbers from that switch: ``picture`` and ``sound`` commands only change
what is drawn or heard, ``box`` commands are the speech box the runner waits on, ``state``
commands change the hero, the NPCs, the map, the scripts or the mobs, and ``end`` commands end
the scene (and change the map, the game mode or the boss). The port's ``skip_intros``
(``platform/src/scene_skip.h``) runs every command of every step, so nothing here is skipped; the
table is why it has to.

Command line::

    python3 -m mtport.scenes names [OUT]          # work/re/names/scenes.json
    python3 -m mtport.scenes list [SLICE]         # every builder's commands, one per line
    python3 -m mtport.scenes generate [SLICE]     # docs/re/qol.md's generated block
    python3 -m mtport.scenes check [SLICE]        # every command used is classified

``list``, ``generate`` and ``check`` need capstone (``uv run --group tools``).
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

from mtport import ghidra_common as gc

NAMES_OUT = "work/re/names/scenes.json"
DOC = "docs/re/qol.md"
SLICE = "work/slices/MapleThief.armv7"
BLOCK = "scenes"

# (address, kind, name, comment). Function pointers carry the Thumb bit. The runner, StartScene,
# QueueScene, QueueBossScene, CHero::SetAction, ResetSceneCamera, the adders and the argument
# readers are named by mtport.audio, mtport.autosave, mtport.screenshake and mtport.flow.
NAMES: tuple[tuple[str, str, str, str], ...] = (
    (
        "0x26A11",
        "function",
        "Scene_NextStep",
        "(list, box): the step to run now and step+1, or -1 (a speech box up, or no command)",
    ),
    (
        "0x2690D",
        "function",
        "Scene_NextCommand",
        "(list, step): the next command of the step, or -1",
    ),
    ("0x26AB1", "function", "Scene_Image", "the scene's image p2 (list +0x20)"),
    (
        "0x80A6D",
        "function",
        "CStateGame::FreeScene",
        "deletes the list (+0x38), the pictures, the speech box lines and the sprite speeds",
    ),
    (
        "0x83BE1",
        "function",
        "CStateGame::PlaceSceneBoss",
        "cmd 60: boss 0x40/0x42/0x43/0x44 placed through CMobs",
    ),
    ("0x7579D", "function", "CNpcs_StartBosses", "cmd 58 (p2 0): mobs with id 0x40-0x45 start"),
    ("0x758B5", "function", "CNpcs_Get", "(npcs, i): the NPC list's item i"),
    ("0x76EB5", "function", "CNpcs_AddSceneNpc", "cmd 12: a new NPC (0x168 bytes) of type p2"),
    ("0x76D89", "function", "CNpcs_RemoveSceneNpcs", "cmd 59: the scene's NPCs deleted"),
    ("0x6299D", "function", "CHero::SetFace", "+0x14C; /png/face/face<n-1>.png"),
    ("0x6FCBD", "function", "CNpc::Walk", "(npc, right): +0x14 facing, +0x18 speed"),
    ("0x6FD65", "function", "CNpc::Stop", "speed 0, action 0"),
    ("0x6F809", "function", "CNpc::SetAction", "+0x10 and its animation (+0x164)"),
    ("0x5E699", "function", "CEffects_Get", "(effects, i): the scene sprite i"),
    ("0x5E8ED", "function", "CEffects_AddSprite", "cmd 42: a scene sprite of image p2"),
    ("0x5E965", "function", "CEffects_AddAt", "cmd 49: effect p2 near the hero"),
    (
        "0x22EC5",
        "function",
        "Text_Wrap",
        "(owner, text, width, &count): new[] array of whole lines (speech box, CChatBox pages)",
    ),
)

# The kinds of command (docs/re/qol.md#cutscene-skip).
PICTURE, SOUND, BOX, STATE, END = "picture", "sound", "box", "state", "end"
KINDS = (PICTURE, SOUND, BOX, STATE, END)


@dataclass(frozen=True)
class Command:
    number: int  # Scene_AddCommand's `command` (the runner's switch case + 1)
    kind: str
    does: str
    target: str = ""  # for state and end: "hero", "NPCs", "map", "scripts", "mobs", "flow"


def _c(number: int, kind: str, does: str, target: str = "") -> Command:
    return Command(number, kind, does, target)


# The runner's switch at 0x83D26 (tbh on command - 1, 64 cases; 3, 4, 11 have none).
COMMANDS: dict[int, Command] = {
    c.number: c
    for c in (
        _c(1, PICTURE, "letterbox bars on (`+0x47C`: black 50-point strips, top and bottom)"),
        _c(2, PICTURE, "letterbox bars off"),
        _c(5, STATE, "hero x = p2", "hero"),
        _c(6, STATE, "hero y = p2", "hero"),
        _c(7, STATE, "hero walk speed (`+0x20`) = p2", "hero"),
        _c(8, STATE, "hero face = p2 (`CHero::SetFace`, `+0x14C`)", "hero"),
        _c(9, STATE, "hero walks: facing right if p2 = 0, else left; `vx` = ± speed", "hero"),
        _c(10, STATE, "hero stops: `vx` = `vy` = 0, stands", "hero"),
        _c(12, STATE, "an NPC of type p2 created (`CNpcs_AddSceneNpc`), the next index", "NPCs"),
        _c(13, STATE, "NPC p1 x = p2", "NPCs"),
        _c(14, STATE, "NPC p1 y = p2", "NPCs"),
        _c(15, STATE, "NPC p1 speed = p2 (0 also stops it)", "NPCs"),
        _c(16, STATE, "NPC p1 walks: right if p2 = 0", "NPCs"),
        _c(17, STATE, "NPC p1 stops, action p2 (3: stands still)", "NPCs"),
        _c(18, SOUND, "vibrate p2 ms"),
        _c(19, PICTURE, "camera shake on, amplitude p2 ([screen-shake.md](screen-shake.md))"),
        _c(20, PICTURE, "camera shake off"),
        _c(21, PICTURE, "the scene camera follows the hero (`+0x47D`)"),
        _c(22, PICTURE, "camera x centred on p2 (`CMMap +0x70`, clamped), scroll stopped"),
        _c(23, PICTURE, "camera y centred on p2 (`CMMap +0x74`), scroll stopped"),
        _c(24, PICTURE, "camera scroll x per frame = p2 (`+0x480`)"),
        _c(25, PICTURE, "camera scroll y per frame = p2 (`+0x484`)"),
        _c(26, PICTURE, "camera follow off"),
        _c(27, PICTURE, "camera follow and scroll off"),
        _c(
            28,
            END,
            "hero reset (face 0, facing right, speed 5, still); p1 = −1: to the main menu "
            "(`CStateMainMenu`, the Game Center ending); else a warp to map p1.p2 (`+0x98`, "
            "`+0x9C`, game mode 8)",
            "flow",
        ),
        _c(29, PICTURE, "fade in from colour p1 over p2 frames (`+0x4B8`, `+0x494`)"),
        _c(30, PICTURE, "picture p1 (0–4) = the scene's image p2 (`+0x498`)"),
        _c(31, PICTURE, "picture p1 x = p2 (`+0x4A0`)"),
        _c(32, PICTURE, "picture p1 y = p2 (`+0x4A4`)"),
        _c(33, PICTURE, "picture p1 `+0x4A8` = p2"),
        _c(34, PICTURE, "picture p1 removed"),
        _c(35, PICTURE, "picture p1 fades in (`+0x4B0` = 5)"),
        _c(36, PICTURE, "picture p1 fades out (`+0x4B0` = −5)"),
        _c(37, STATE, "map object p1 x = p2 (`CMMap +0x48`)", "map"),
        _c(38, STATE, "map object p1 y = p2", "map"),
        _c(39, STATE, "map object p1 `+0x0A` = p2", "map"),
        _c(40, STATE, "map object p1 `+0x06` = p2", "map"),
        _c(41, STATE, "map object p1 `+0x08` = p2", "map"),
        _c(42, PICTURE, "a scene sprite of image p2 (`CStateGame +0x48`)"),
        _c(43, PICTURE, "sprite p1 x = p2"),
        _c(44, PICTURE, "sprite p1 y = p2"),
        _c(45, PICTURE, "sprite p1 speed x = p2 per frame (`+0x4BC`)"),
        _c(46, PICTURE, "sprite p1 speed y = p2 per frame (`+0x4C0`)"),
        _c(47, PICTURE, "sprite p1 flag `+0x20` on"),
        _c(48, PICTURE, "sprite p1 still, flag `+0x20` off"),
        _c(49, PICTURE, "effect p2 near the hero (`CEffects_AddAt`)"),
        _c(50, BOX, "speech box: speaker p1, text p2, wrapped; waits for a key (`+0x1E9`)"),
        _c(51, BOX, "speech box closed, picture p2 cleared, `+0x1EC` = 0"),
        _c(
            52,
            END,
            "hero reset; the next scene (`+0x1E4`) = p1 and its map (2: 2.11, 17: 8.6, 18: "
            "7.6, 19: 8.4), game mode 8, key pad off",
            "flow",
        ),
        _c(53, PICTURE, "fade out to colour p1 over p2 frames"),
        _c(54, STATE, "`CStateGame::RunNpcStateScripts` `0x839BD`: quests, flags", "scripts"),
        _c(55, SOUND, "all music stopped, then music p1 (resumed if p2 = 1)"),
        _c(56, SOUND, "all music stopped"),
        _c(57, SOUND, "vibration stopped (empty)"),
        _c(
            58,
            END,
            "hero reset; p2 = 0: the boss starts (`CNpcs_StartBosses` `0x7579D`); game mode "
            "0, key pad on (`+0x3FC` = 1): the field of the same map",
            "flow",
        ),
        _c(59, STATE, "the scene's NPCs removed (`CNpcs_RemoveSceneNpcs` `0x76D89`)", "NPCs"),
        _c(60, STATE, "boss p2 placed (`CStateGame::PlaceSceneBoss` `0x83BE1`)", "mobs"),
        _c(61, PICTURE, "picture p1 `+0x4AC` = p2"),
        _c(62, BOX, "speech box parameter `+0x1EC` = p2"),
        _c(63, PICTURE, "scan lines over the picture (`+0x4CC` on)"),
        _c(64, PICTURE, "scan lines off"),
    )
}
# Command numbers the switch has no case for: they do nothing.
NO_CASE = frozenset((3, 4, 11))
COMMAND_COUNT = 64

# CStateGame::StartScene 0x853E5's switch: scene number -> builder (function pointer, name).
SCENES: tuple[tuple[int, int, str], ...] = (
    (0, 0x302F5, "makeAni1"),
    (1, 0x2BE55, "makeAniEndingZacum"),
    (2, 0x26F89, "makeAniEndingHenesis"),
    (3, 0x2FE21, "makeAniMelodyIn"),
    (4, 0x2F825, "makeAniMelodyEnd"),
    (5, 0x2F319, "makeAniWolfIn"),
    (6, 0x2EDD9, "makeAniWolfEnd"),
    (7, 0x2E5E1, "makeAniYetiIn"),
    (8, 0x2E03D, "makeAniYetiEnd"),
    (9, 0x2DA85, "makeAniHomunIn"),
    (10, 0x2D351, "makeAniHomunEnd"),
    (11, 0x2BA59, "makeAniChapter1"),
    (12, 0x2B65D, "makeAniChapter2"),
    (13, 0x2B261, "makeAniChapter3"),
    (14, 0x2CBD1, "makeAniBalogIn"),
    (15, 0x28521, "makeAniChapter1Perion"),
    (16, 0x291D9, "makeAniMagatiaHumanoid1"),
    (17, 0x2AAF5, "makeAniMagatiaHumanoid2"),
    (18, 0x29765, "makeAniMagatiaHumanoid3"),
    (19, 0x28D81, "makeAniMagatiaHumanoid4"),
    (20, 0x27AED, "makeAniChapter2Perion"),
)


@dataclass(frozen=True)
class SceneCommand:
    function: int  # the builder
    step: int
    command: int
    p1: int | None  # None: computed (a picture's place from the screen's size)
    p2: int | None
    site: int


def commands_of(scan) -> list[SceneCommand]:
    """Every command the scene builders add, from an :func:`mtport.armscan.scan` of the slice
    run with the scene adders as dispatchers, by builder, step and site. Raises ValueError for a
    step or a command that is not a constant."""
    from mtport.audio_analysis import SCENE_ADD, SCENE_ADD_P2, SCENE_ADDERS, _const, _signed

    adders = {a & ~1 for a in SCENE_ADDERS}
    out = []
    for s in scan.sends:
        if s.stub not in adders or s.function in SCENE_ADDERS:
            continue
        step, command = _const(s.regs[1]), _const(s.regs[2])
        if s.stub == SCENE_ADD & ~1:
            p1, p2 = _signed(_const(s.regs[3])), _signed(_const(s.stack[0])) if s.stack else None
        elif s.stub == SCENE_ADD_P2 & ~1:
            p1, p2 = 0, _signed(_const(s.regs[3]))
        else:
            p1 = p2 = 0
        if step is None or command is None:
            raise ValueError(f"0x{s.site:X}: a scene command's step or number not a constant")
        out.append(SceneCommand(s.function, step, command, p1, p2, s.site))
    return sorted(out, key=lambda c: (c.function, c.step, c.site))


def scan_slice(slice_path: Path) -> list[SceneCommand]:
    """:func:`commands_of` the armv7 slice."""
    from mtport import armscan, macho
    from mtport.audio_analysis import SCENE_ADDERS

    return commands_of(armscan.scan(macho.load(slice_path), [a & ~1 for a in SCENE_ADDERS]))


def problems(cmds: Iterable[SceneCommand]) -> list[str]:
    """What does not hold of the commands as docs/re/qol.md states it: each one classified,
    each builder a scene of StartScene's, each scene ending with exactly one end command, on its
    last step."""
    out = []
    by: dict[int, list[SceneCommand]] = {}
    for c in cmds:
        by.setdefault(c.function, []).append(c)
        if c.command not in COMMANDS:
            out.append(f"0x{c.site:X}: command {c.command} is not classified")
    known = {f for _, f, _ in SCENES}
    out += [f"0x{f:X}: not a builder of StartScene's" for f in by if f not in known]
    out += [f"0x{f:X} ({n}): no commands" for _, f, n in SCENES if f not in by]
    for f, cs in by.items():
        ends = [c for c in cs if COMMANDS.get(c.command, Command(0, "", "")).kind == END]
        last = max(c.step for c in cs)
        if len(ends) != 1 or ends[0].step != last:
            out.append(f"0x{f:X}: {len(ends)} end commands, not one on the last step {last}")
    return out


def _steps(steps: Iterable[int]) -> str:
    return ", ".join(str(s) for s in sorted(set(steps)))


def table(cmds: Iterable[SceneCommand]) -> str:
    """docs/re/qol.md's generated block: per scene, its steps, speech boxes, the steps of its
    state commands by what they change, and its end."""
    by: dict[int, list[SceneCommand]] = {}
    for c in cmds:
        by.setdefault(c.function, []).append(c)
    lines = [
        "| Scene | Builder | Steps | Speech boxes | State commands: steps | Ends (step: command) |",
        "|---|---|---|---|---|---|",
    ]
    total = {k: 0 for k in KINDS}
    for number, f, name in SCENES:
        cs = by.get(f, [])
        groups: dict[str, list[int]] = {}
        end = ""
        for c in cs:
            info = COMMANDS[c.command]
            total[info.kind] += 1
            if info.kind == STATE:
                groups.setdefault(info.target, []).append(c.step)
            elif info.kind == END:
                end = f"{c.step}: {c.command} ({c.p1}, {c.p2})"
        state = "; ".join(
            f"{t} {_steps(groups[t])}"
            for t in ("hero", "NPCs", "map", "scripts", "mobs")
            if t in groups
        )
        boxes = sum(1 for c in cs if c.command == 50)
        last = max((c.step for c in cs), default=0)
        lines.append(
            f"| {number} | `CAniManager::{name}` `0x{f:X}` | {last} | {boxes} | "
            f"{state or '–'} | {end} |"
        )
    n = sum(total.values())
    lines.append("")
    lines.append(
        f"{n} commands in {len(SCENES)} scenes: "
        + ", ".join(f"{total[k]} {k}" for k in KINDS)
        + ". Steps are the runner's frames (one per `Paint` while no box waits); "
        "an end command's arguments are (p1, p2)."
    )
    return "\n".join(lines) + "\n"


def names_json() -> str:
    return gc.names_json("scenes", NAMES)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.scenes", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    names = sub.add_parser("names", help=f"write the Ghidra names file (default {NAMES_OUT})")
    names.add_argument("out", nargs="?", default=NAMES_OUT)
    for name, text in (
        ("list", "every builder's commands"),
        ("generate", f"rewrite {DOC}'s generated block"),
        ("check", "fail if a command is unclassified or a scene's end is not its last step"),
    ):
        p = sub.add_parser(name, help=text)
        p.add_argument("slice", nargs="?", default=SLICE)
        if name == "generate":
            p.add_argument("--doc", default=DOC)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "names":
            out = Path(args.out)
            gc.write_text(out, names_json())
            print(f"wrote {out} ({len(NAMES)} names)")
            return 0
        cmds = scan_slice(Path(args.slice))
        bad = problems(cmds)
        if args.cmd == "list":
            for c in cmds:
                info = COMMANDS.get(c.command)
                print(
                    f"0x{c.function:X} step {c.step} cmd {c.command} ({c.p1}, {c.p2}) "
                    f"{info.kind if info else '?'}: {info.does if info else '?'} (0x{c.site:X})"
                )
        elif args.cmd == "generate":
            if bad:
                raise ValueError("; ".join(bad))
            doc = Path(args.doc)
            text = gc.render_blocks(
                doc.read_text(encoding="utf-8"),
                {BLOCK: table(cmds)},
                "python3 -m mtport.scenes generate",
                str(doc),
            )
            gc.write_text(doc, text)
            print(f"wrote {doc}")
        for p in bad:
            print(f"problem: {p}", file=sys.stderr)
        return 1 if bad else 0
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
