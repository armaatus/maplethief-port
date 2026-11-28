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
    python3 -m mtport.scenes stage [SLICE] [--app APP]  # each scene's stage at 480 and at E (#396)
    python3 -m mtport.scenes widescreen [SLICE] [--app APP]  # docs/re/widescreen.md's block

``list``, ``generate``, ``check``, ``stage`` and ``widescreen`` need capstone
(``uv run --group tools``).

``stage`` (#396, docs/re/widescreen.md#scenes-at-the-fields-width) replays each scene's commands
a step a frame on the map it plays on (:data:`SCENE_MAPS`, widths from the bundle's ``.map``
files) and reports what a view of the field's width E would show that the 480-point view does
not: :func:`stage` and :func:`verdict`.
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
APP = "work/ipa/Payload/MapleThief.app"
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


# --- the stage at the field's width (#396) -------------------------------------------------------

NARROW = 480  # the original view, points
WIDE = 568  # MT_WIDESCREEN_WIDE

# The map each scene plays on, (area, map), and where that is decided
# (docs/re/widescreen.md#scenes-at-the-fields-width).
SCENE_MAPS: dict[int, tuple[int, int, str]] = {
    0: (2, 10, "LoadGame 0x86C1D, a new game"),
    1: (10, 5, "CMob::Die 0x73349, mob 69"),
    2: (2, 11, "scene 1's end (command 52)"),
    3: (3, 8, "QueueBossScene 0x80965, area 3 (MelodyMap)"),
    4: (3, 8, "CMob::Die, Malady (mob 64)"),
    5: (4, 17, "QueueBossScene, area 4 (WolfMap)"),
    6: (4, 17, "CMob::Die, mob 66"),
    7: (9, 5, "QueueBossScene, area 9 (no map script names it: never played)"),
    8: (9, 5, "CMob::Die, the Yeti (mob 68)"),
    9: (7, 4, "QueueBossScene, area 7 (HomunMap)"),
    10: (7, 4, "CMob::Die, mob 67"),
    11: (2, 0, "CQuestList::ClearAll 0x7C61D, chapter 1 (portal281: 2.0)"),
    12: (4, 1, "CQuestList::ClearAll, chapter 2 (Pirate: 4.1)"),
    13: (6, 0, "CQuestList::ClearAll, chapter 3 (Pirate_Orbis: 6.0)"),
    14: (10, 5, "QueueBossScene, area 10 (BalogMap)"),
    15: (1, 0, "chapter 1's PerionMap, runAnimation(15)"),
    16: (8, 4, "chapter 3's Humanoid3 (an NPC on 8.4), runAnimation(16)"),
    17: (8, 6, "scene 16's end (command 52)"),
    18: (7, 6, "scene 17's end (command 52)"),
    19: (8, 4, "scene 18's end (command 52)"),
    20: (1, 0, "chapter 2's PerionMap, runAnimation(20)"),
}
HERO_SPEED = 5  # CHero +0x20 after the ends' hero reset (commands 28, 52, 58)
WIDE_DOC = "docs/re/widescreen.md"
WIDE_BLOCK = "scene_stage"
# The scenes runtime/src/hooks/widescreen.cpp lists (k_wide_scenes): the ones :func:`verdict`
# finds wide, and the real-asset run that shows each one wide.
WIDE_SCENES: dict[int, str] = {
    2: "play.widescreen_scene_2_on_synthetic",
    11: "play.widescreen_scene_11_on_synthetic",
    13: "play.widescreen_scene_13_on_synthetic",
    15: "play.widescreen_scene_15_on_synthetic",
    16: "play.widescreen_scene_16_on_synthetic",
    19: "play.widescreen_scene_16_on_synthetic (after 17 and 18)",
    20: "play.widescreen_scene_20_on_synthetic",
}


@dataclass(frozen=True)
class Sighting:
    """An actor the view of width E shows and the 480-point view does not, over steps
    ``first`` ... ``last``."""

    actor: str  # "hero", "NPC 2", "sprite 4", "object 1"
    first: int
    last: int
    xs: tuple[int, int]  # its x, lowest and highest
    why: str  # "before its entrance", "after its exit", "off stage"
    parked: bool  # standing still at the last of those steps

    @property
    def hidden(self) -> bool:
        """The scene keeps it unseen at 480: before its entrance, never on stage, or standing
        where it went off."""
        return self.why != "after its exit" or self.parked


@dataclass(frozen=True)
class Stage:
    scene: int
    map_width: int
    view: int  # E: the map's width clamped to 480 ... WIDE
    steps: int
    camera_from: int | None  # the first step the scene places the camera (None: never)
    sightings: tuple[Sighting, ...]
    boxes: int  # speech boxes (command 50)
    pictures: tuple[tuple[int, int], ...]  # (step, slot) of each picture faded in at a set x


def _clamp(left: float, width: int, map_width: int) -> float:
    return max(0.0, min(left, float(max(0, map_width - width))))


def stage(cmds: Iterable[SceneCommand], scene: int, map_width: int, wide: int = WIDE) -> Stage:
    """Replays scene ``scene``'s commands a step a frame (docs/re/qol.md#how-a-scene-runs: the
    hero, the NPCs and the scene sprites move by their speed every frame, the camera follows the
    hero or scrolls) on a map ``map_width`` points wide, with a view of 480 points and one of E,
    and finds every actor the wide view shows and the narrow one does not. Positions are the
    actors' x (their feet); frames a speech box waits are not counted (only NPCs walk then)."""
    function = SCENES[scene][1]
    view = max(NARROW, min(map_width, wide))
    by_step: dict[int, list[SceneCommand]] = {}
    for c in cmds:
        if c.function == function:
            by_step.setdefault(c.step, []).append(c)
    last = max(by_step, default=0)
    widths = (NARROW, view)
    left: dict[int, float | None] = dict.fromkeys(widths)
    follow, scroll = False, 0
    camera_from: int | None = None
    hero: list = [None, 0, HERO_SPEED]  # x, vx, speed
    npcs: dict[int, list] = {}  # index -> [x, speed, direction (+1/-1), walking]
    sprites: dict[int, list] = {}  # index -> [x, vx, shown]
    objects: dict[int, int] = {}
    pictures: list[tuple[int, int]] = []
    placed: dict[int, bool] = {}  # picture slot -> its x set (command 31)
    seen: dict[str, list[tuple[int, int | None, bool, bool]]] = {}  # actor -> (step, x, n, w)
    boxes = 0
    for step in range(1, last + 1):
        for c in by_step.get(step, ()):
            n, p1, p2 = c.command, c.p1, c.p2
            if n == 5:
                hero[0] = p2
            elif n == 7 and p2 is not None:
                hero[2] = p2
            elif n == 9:
                hero[1] = hero[2] if p2 == 0 else -hero[2]
            elif n == 10:
                hero[1] = 0
            elif n == 12 and p1 is not None:
                npcs[p1] = [None, 0, 1, False]
            elif n == 13 and p1 in npcs:
                npcs[p1][0] = p2
            elif n == 15 and p1 in npcs and p2 is not None:
                npcs[p1][1] = p2
                npcs[p1][3] = npcs[p1][3] and p2 != 0
            elif n == 16 and p1 in npcs:
                npcs[p1][2], npcs[p1][3] = (1 if p2 == 0 else -1), True
            elif n == 17 and p1 in npcs:
                npcs[p1][3] = False
            elif n == 59:
                npcs.clear()
            elif n == 42 and p1 is not None:
                sprites[p1] = [None, 0, False]
            elif n == 43 and p1 in sprites:
                sprites[p1][0] = p2
            elif n == 45 and p1 in sprites and p2 is not None:
                sprites[p1][1] = p2
            elif n == 47 and p1 in sprites:
                sprites[p1][2] = True
            elif n == 48 and p1 in sprites:
                sprites[p1][2] = False
            elif n == 37 and p1 is not None and p2 is not None:
                objects[p1] = p2
            elif n == 21:
                follow = True
                camera_from = step if camera_from is None else camera_from
            elif n == 22 and p2 is not None:
                for w in widths:
                    left[w] = _clamp(p2 - w / 2, w, map_width)
                scroll = 0
                camera_from = step if camera_from is None else camera_from
            elif n == 24 and p2 is not None:
                scroll = p2
            elif n == 26:
                follow = False
            elif n == 27:
                follow, scroll = False, 0
            elif n == 30 and p1 is not None:
                placed[p1] = False
            elif n == 31 and p1 is not None:
                placed[p1] = True
            elif n == 35 and p1 is not None and placed.get(p1):
                pictures.append((step, p1))
            elif n == 50:
                boxes += 1
        # Paint, after the runner: everything with a speed moves, then the camera.
        if hero[0] is not None:
            hero[0] += hero[1]
        for npc in npcs.values():
            if npc[0] is not None and npc[3]:
                npc[0] += npc[1] * npc[2]
        for sprite in sprites.values():
            if sprite[0] is not None:
                sprite[0] += sprite[1]
        for w in widths:
            if follow and hero[0] is not None:
                left[w] = _clamp(hero[0] - w / 2, w, map_width)
            elif left[w] is not None:
                left[w] = _clamp(left[w] + scroll, w, map_width)
        if left[NARROW] is None or left[view] is None:
            continue  # the camera is still where the field left it
        actors: dict[str, int | None] = {"hero": hero[0]}
        actors |= {f"NPC {i}": npc[0] for i, npc in npcs.items()}
        actors |= {f"sprite {i}": s[0] for i, s in sprites.items() if s[2]}
        actors |= {f"object {i}": x for i, x in objects.items()}
        for name, x in actors.items():
            if x is None:
                continue
            inside = [left[w] <= x < left[w] + w for w in widths]  # type: ignore[operator]
            seen.setdefault(name, []).append((step, x, inside[0], inside[1]))
    return Stage(
        scene,
        map_width,
        view,
        last,
        camera_from,
        tuple(_sightings(seen)),
        boxes,
        tuple(pictures),
    )


def _sightings(seen: dict[str, list[tuple[int, int | None, bool, bool]]]) -> list[Sighting]:
    out = []
    for actor, frames in seen.items():
        narrow_steps = [s for s, _, n, _ in frames if n]
        run: list[tuple[int, int]] = []
        for step, x, n, w in [*frames, (-1, None, True, True)]:
            if w and not n and x is not None and (not run or run[-1][0] == step - 1):
                run.append((step, x))
                continue
            if run:
                first, last = run[0][0], run[-1][0]
                xs = [x for _, x in run]
                if any(s > last for s in narrow_steps):
                    why = "before its entrance"
                elif any(s < first for s in narrow_steps):
                    why = "after its exit"
                else:
                    why = "off stage"
                parked = len(xs) < 2 or xs[-1] == xs[-2]
                out.append(Sighting(actor, first, last, (min(xs), max(xs)), why, parked))
                run = []
            if w and not n and x is not None and step >= 0:
                run.append((step, x))
    return sorted(out, key=lambda s: (s.first, s.actor))


def verdict(st: Stage) -> tuple[bool, str]:
    """(wide, why) for a scene's stage: wide only on a map wider than 480 points, with no actor
    the scene keeps unseen at 480 inside the wider view."""
    if st.view <= NARROW:
        return False, f"its map is {st.map_width} points wide: the view is 480 either way"
    hidden = [s for s in st.sightings if s.hidden]
    if hidden:
        s = hidden[0]
        return False, (
            f"{s.actor} {s.why} at x {s.xs[0]}"
            + (f"-{s.xs[1]}" if s.xs[1] != s.xs[0] else "")
            + f", steps {s.first}-{s.last}, only in the wider view"
        )
    return True, "nothing staged where only the wider view shows it"


def map_widths(app: Path) -> dict[tuple[int, int], int]:
    """Each scene map's width in points, from the bundle's ``map/<area><map>.map``."""
    from mtport import mapfile, mapprj

    out = {}
    for area, number, _ in SCENE_MAPS.values():
        m = mapfile.load(app / "map" / f"{mapprj.map_stem(area, number)}.map")
        out[(area, number)] = m.width * 16
    return out


def stage_report(cmds: Sequence[SceneCommand], widths: dict[tuple[int, int], int]) -> str:
    """One line per scene: its map, E, the verdict and why, then its sightings."""
    lines = []
    for number, _f, name in SCENES:
        area, map_no, _ = SCENE_MAPS[number]
        st = stage(cmds, number, widths[(area, map_no)])
        wide, why = verdict(st)
        lines.append(
            f"{number} {name} on {area}.{map_no} ({st.map_width} points, E {st.view}): "
            f"{'wide' if wide else '480'}: {why}; camera from step {st.camera_from}, "
            f"{st.boxes} boxes, pictures at a set x {list(st.pictures)}"
        )
        for s in st.sightings:
            lines.append(
                f"  {s.actor}: steps {s.first}-{s.last}, x {s.xs[0]}..{s.xs[1]}, {s.why}"
                f"{', standing' if s.parked else ''}"
            )
    return "\n".join(lines) + "\n"


def stage_table(cmds: Sequence[SceneCommand], widths: dict[tuple[int, int], int]) -> str:
    """docs/re/widescreen.md's generated block: per scene, its map, E, the verdict and why, and
    the evidence."""
    lines = [
        "| Scene | Builder | Map (points) | E | Boxes | Verdict | Why | Evidence |",
        "|---|---|---|---|---|---|---|---|",
    ]
    wide_n = 0
    for number, f, name in SCENES:
        area, map_no, where = SCENE_MAPS[number]
        st = stage(cmds, number, widths[(area, map_no)])
        wide, why = verdict(st)
        wide_n += wide
        hidden = sum(1 for x in st.sightings if x.hidden)
        if wide:
            evidence = f"stage: {len(st.sightings)} sightings, none hidden; `{WIDE_SCENES[number]}`"
        elif st.view <= NARROW:
            evidence = "the map's width"
        else:
            evidence = f"stage: {hidden} hidden sightings"
        lines.append(
            f"| {number} | `{name}` `0x{f:X}` | {area}.{map_no} ({st.map_width}; {where}) | "
            f"{st.view} | {st.boxes} | {'**wide**' if wide else '480'} | {why} | {evidence} |"
        )
    lines.append("")
    lines.append(
        f"{wide_n} of {len(SCENES)} scenes wide. Map: where the scene plays, its width and what "
        "starts it there. Boxes: its speech boxes (command 50). A sighting is an actor (the "
        "hero, a scene NPC, a scene sprite, a moved map object) at an x the E-point view shows "
        "and the 480-point one does not, over consecutive steps; hidden: before its entrance, "
        "never on stage, or standing where it went off."
    )
    return "\n".join(lines) + "\n"


def wide_problems(cmds: Sequence[SceneCommand], widths: dict[tuple[int, int], int]) -> list[str]:
    """Where :data:`WIDE_SCENES` (runtime/src/hooks/widescreen.cpp's list) and the verdicts
    differ."""
    found = {n for n, _, _ in SCENES if verdict(stage(cmds, n, widths[SCENE_MAPS[n][:2]]))[0]}
    return [
        f"scene {n}: {'wide by its stage, not listed' if n in found else 'listed, not wide'}"
        for n in sorted(found ^ set(WIDE_SCENES))
    ]


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
        ("stage", "each scene's stage at 480 points and at the field's width E (#396)"),
        ("widescreen", f"rewrite {WIDE_DOC}'s generated block (#396)"),
    ):
        p = sub.add_parser(name, help=text)
        p.add_argument("slice", nargs="?", default=SLICE)
        if name == "generate":
            p.add_argument("--doc", default=DOC)
        if name in ("stage", "widescreen"):
            p.add_argument("--app", default=APP)
        if name == "widescreen":
            p.add_argument("--doc", default=WIDE_DOC)
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
        elif args.cmd == "stage":
            print(stage_report(cmds, map_widths(Path(args.app))), end="")
        elif args.cmd == "widescreen":
            widths = map_widths(Path(args.app))
            bad += wide_problems(cmds, widths)
            doc = Path(args.doc)
            text = gc.render_blocks(
                doc.read_text(encoding="utf-8"),
                {WIDE_BLOCK: stage_table(cmds, widths)},
                "python3 -m mtport.scenes widescreen",
                str(doc),
            )
            gc.write_text(doc, text)
            print(f"wrote {doc}")
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
