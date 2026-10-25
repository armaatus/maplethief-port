"""The instruction set of the script VM (:data:`OPCODES`) and its native functions
(:data:`NATIVES`), as tables for :mod:`mtport.scriptout`, which re-exports every name
defined here. The evidence for both is in docs/re/script-vm.md.
"""

from __future__ import annotations

from dataclasses import dataclass

# -- opcodes ---------------------------------------------------------------------------------

# Operand kinds: b = u8, w = u16, f = 4-byte float, p = 4-byte function slot,
# g = u16 global (threaded 'V'), s = u16 string constant (threaded 'S'),
# j = u16 forward jump, u = u16 backward jump, r = STORERECORD's u8 n + n string words.


@dataclass(frozen=True)
class Opcode:
    code: int
    name: str
    operands: str  # operand kinds, in order (see above)
    pops: int | None  # values taken from the stack (None: depends on the operands)
    pushes: int | None
    game: str = ""  # how the game's interpreter (0x9BC11) departs from Lua 2.4, if at all

    @property
    def fixed_size(self) -> int | None:
        """Instruction size in bytes, or None when it depends on the operands (STORERECORD)."""
        if "r" in self.operands:
            return None
        sizes = {"b": 1, "w": 2, "g": 2, "s": 2, "j": 2, "u": 2, "f": 4, "p": 4}
        return 1 + sum(sizes[k] for k in self.operands)


def _ops() -> tuple[Opcode, ...]:
    ops: list[Opcode] = [
        Opcode(0x00, "PUSHNIL", "", 0, 1),
        Opcode(0x01, "PUSH0", "", 0, 1),
        Opcode(0x02, "PUSH1", "", 0, 1),
        Opcode(0x03, "PUSH2", "", 0, 1),
        Opcode(0x04, "PUSHBYTE", "b", 0, 1),
        Opcode(0x05, "PUSHWORD", "w", 0, 1),
        Opcode(
            0x06,
            "PUSHFLOAT",
            "f",
            0,
            1,
            "no-op that does not skip its 4 operand bytes (numbers are integers)",
        ),
        Opcode(0x07, "PUSHSTRING", "s", 0, 1),
        Opcode(0x08, "PUSHFUNCTION", "p", 0, 1),
    ]
    ops += [Opcode(0x09 + i, f"PUSHLOCAL{i}", "", 0, 1) for i in range(10)]
    ops += [
        Opcode(0x13, "PUSHLOCAL", "b", 0, 1),
        Opcode(0x14, "PUSHGLOBAL", "g", 0, 1),
        Opcode(0x15, "PUSHINDEXED", "", 2, 1, "arrays only; indexes a C array of slots"),
        Opcode(0x16, "PUSHSELF", "s", 1, 2, "no-op that does not skip its 2 operand bytes"),
    ]
    ops += [
        Opcode(
            0x17 + i,
            f"STORELOCAL{i}",
            "",
            1,
            0,
            "stores to local (last operand - 23), not local n (bug at 0x9BFE2)",
        )
        for i in range(10)
    ]
    ops += [
        Opcode(0x21, "STORELOCAL", "b", 1, 0),
        Opcode(0x22, "STOREGLOBAL", "g", 1, 0),
        Opcode(0x23, "STOREINDEXED0", "", 3, 0),
        Opcode(0x24, "STOREINDEXED", "b", None, None),
        Opcode(0x25, "STORELIST0", "b", None, None),
        Opcode(0x26, "STORELIST", "bb", None, None),
        Opcode(0x27, "STORERECORD", "r", None, None),
        Opcode(0x28, "ADJUST0", "", None, None),
        Opcode(0x29, "ADJUST", "b", None, None),
        Opcode(0x2A, "CREATEARRAY", "w", 0, 1, "array of w + 1 slot pointers, not a hash"),
        Opcode(0x2B, "EQOP", "", 2, 1),
        Opcode(0x2C, "LTOP", "", 2, 1),
        Opcode(0x2D, "LEOP", "", 2, 1),
        Opcode(0x2E, "GTOP", "", 2, 1),
        Opcode(0x2F, "GEOP", "", 2, 1),
        Opcode(0x30, "ADDOP", "", 2, 1, "integer"),
        Opcode(0x31, "SUBOP", "", 2, 1, "integer"),
        Opcode(0x32, "MULTOP", "", 2, 1, "integer"),
        Opcode(0x33, "DIVOP", "", 2, 1, "integer (__divsi3)"),
        Opcode(0x34, "POWOP", "", 2, 1, "no-op: leaves both operands"),
        Opcode(0x35, "CONCOP", "", 2, 1, "into a 100-byte stack buffer (sprintf %s%s)"),
        Opcode(0x36, "MINUSOP", "", 1, 1),
        Opcode(0x37, "NOTOP", "", 1, 1),
        Opcode(0x38, "ONTJMP", "j", 0, 0),
        Opcode(0x39, "ONFJMP", "j", 0, 0),
        Opcode(0x3A, "JMP", "j", 0, 0),
        Opcode(0x3B, "UPJMP", "u", 0, 0),
        Opcode(0x3C, "IFFJMP", "j", 1, 0),
        Opcode(0x3D, "IFFUPJMP", "u", 1, 0),
        Opcode(0x3E, "POP", "", 1, 0),
        Opcode(0x3F, "CALLFUNC", "bb", None, None),
        Opcode(0x40, "RETCODE0", "", None, None),
        Opcode(0x41, "RETCODE", "b", None, None),
        Opcode(0x42, "SETLINE", "w", 0, 0),
    ]
    return tuple(ops)


OPCODES: tuple[Opcode, ...] = _ops()
BY_CODE: dict[int, Opcode] = {op.code: op for op in OPCODES}
BY_NAME: dict[str, Opcode] = {op.name: op for op in OPCODES}
assert [op.code for op in OPCODES] == list(range(len(OPCODES)))  # 0x00..0x42, no gaps

# Opcodes the game cannot run correctly: it would misread the instruction stream.
DESYNC_OPCODES = frozenset({"PUSHFLOAT", "PUSHSELF"})
JUMPS = frozenset({"ONTJMP", "ONFJMP", "JMP", "UPJMP", "IFFJMP", "IFFUPJMP"})


# -- natives ---------------------------------------------------------------------------------


@dataclass(frozen=True)
class Native:
    """A global the VM pre-registers as a C function (``Table::registerNatives`` ``0x9E441``).

    ``index`` is its symbol index (registration order) and also the ``case`` of the native
    dispatcher ``0x9B37D``; ``handler`` is the function pointer that case calls, or None when
    the case is missing (the call does nothing and returns no value). ``params`` gives the
    argument types the handler reads (``int``: the slot's value word, used as an integer;
    ``string``: the slot's value used as a string object), in order; extra arguments are
    ignored and missing ones read garbage. ``returns`` is what it pushes (None: nothing).
    """

    index: int
    name: str
    handler: int | None
    params: tuple[str, ...]
    returns: str | None
    does: str


def _natives() -> tuple[Native, ...]:
    i, s = "int", "string"
    rows = [
        # Lua 2.4's built-in library: registered, but only println/print have a case, and
        # both of those are empty.
        ("assert", None, (), None, "no-op (no dispatcher case)"),
        ("dofile", None, (), None, "no-op (no dispatcher case)"),
        ("dostring", None, (), None, "no-op (no dispatcher case)"),
        ("error", None, (), None, "no-op (no dispatcher case)"),
        ("getglobal", None, (), None, "no-op (no dispatcher case)"),
        ("next", None, (), None, "no-op (no dispatcher case)"),
        ("println", 0x9941D, (), None, "no-op (empty handler)"),
        ("print", 0x99425, (), None, "no-op (empty handler)"),
        ("setfallback", None, (), None, "no-op (no dispatcher case)"),
        ("setglobal", None, (), None, "no-op (no dispatcher case)"),
        ("tonumber", None, (), None, "no-op (no dispatcher case)"),
        ("tostring", None, (), None, "no-op (no dispatcher case)"),
        ("type", None, (), None, "no-op (no dispatcher case)"),
        # Game natives. world = Lua+4, the CMapleWorld that owns the VM; quests = world+0x58
        # (quest manager), msg = world+0x5C (dialogue box), hero = world+0x30. "Current NPC" is
        # the object world+0xB0 indexes in the map's object list (world+0x2C). kind: 0 = main
        # quest, else sub-quest; inventory kind: 0 = equipment, 1 = other items.
        (
            "regCallBack",
            0x99449,
            ("function",),
            None,
            "Lua+8 = arg; the dialogue box runs it (0x9CE2D)",
        ),
        ("regMsgHero", 0x9A0F1, (s, i), None, "msg: queue a hero line, Scripts.strings[key]"),
        ("regMsgNPC", 0x9A055, (s,), None, "msg: queue an NPC line, Scripts.strings[key]"),
        ("regSelList", 0x99F85, (s, i), None, "msg: queue a choice (key, value)"),
        ("initMsg", 0x99AF1, (), None, "msg: clear the queued lines and choices"),
        ("showMsg", 0x99F51, (i,), None, "msg: open the box in mode arg (msg+8)"),
        ("getLevel", 0x9B329, (), "int", "hero level (hero+0x80)"),
        ("getQuestState", 0x9B269, (i, i), "int", "state of quest (kind, id) (0x799B1)"),
        ("setNpcNotify", 0x9989D, (i,), None, "current NPC's quest icon; 5 = from current quest"),
        ("setClassUp", 0x997E1, (i, i), None, "hero job and advancement count (0x655AD)"),
        ("checkRequire", 0x9B1A9, (i, i), "int", "1 if quest (kind, id) requirements are met"),
        ("setQuest", 0x99E15, (i, i, s, s), None, "quests: prepare (kind, id, title key, msg key)"),
        ("startQuest", 0x99D65, (), None, "quests: accept the prepared quest (state = 1)"),
        ("clearQuest", 0x9942D, (), None, "no-op (empty handler)"),
        ("regRequireItem", 0x99CDD, (i, i, i), None, "prepared quest needs item (kind, id, n)"),
        ("regRequireMob", 0x99C61, (i, i), None, "prepared quest needs kills (mob, n)"),
        ("regRequireMeso", 0x99C2D, (i,), None, "prepared quest needs meso (0x7CF41)"),
        ("regRewardItem", 0x99B71, (i, i, i), None, "prepared quest gives item (kind, id, n)"),
        ("regRewardMeso", 0x99B3D, (i,), None, "prepared quest gives meso"),
        ("regRewardExp", 0x99B09, (i,), None, "prepared quest gives EXP"),
        ("loseRequire", None, (), None, "no-op (no dispatcher case)"),
        ("getReward", 0x9B099, (i, i), "int", "complete (kind, id): take needs, give rewards; 1/0"),
        ("getEvtIndex", 0x9B03D, (), "int", "msg: answer (modes 1/3: 1 = first button; 2: value)"),
        ("regMsgRequire", 0x99A75, (i, i), None, "msg: show the needs of quest (kind, id)"),
        ("regMsgReward", 0x999F9, (i, i), None, "msg: show the rewards of quest (kind, id)"),
        (
            "changeMap",
            0x99969,
            (i, i, i),
            None,
            "warp to (area, map, portal): world+0x98/+0x9C/+0xA0",
        ),
        ("showMarket", 0x998D1, (i,), None, "open shop arg (0x4628D)"),
        ("getMainQuest", 0x9A975, (), "int", "current main-quest id (quests+0x198), made current"),
        ("setCurrent", 0x99699, (i, i), None, "quests: make (kind, id) the current quest"),
        ("setNpcHide", 0x9985D, (i,), None, "current NPC object +6 = arg (hidden flag)"),
        ("regRequireQuest", 0x99BF9, (i,), None, "same call as regRequireMeso (0x7CF41)"),
        ("getItem", 0x9AADD, (i, i, i), "int", "give item (kind, id, n) if it fits: 1/0"),
        ("getMeso", 0x997A9, (i,), None, "hero meso += arg, with a notice"),
        ("getEquipState", 0x9A9D1, (i, i), "int", "1 if equipment slot arg1 holds item id arg2"),
        (
            "showPortalErrMsg",
            0x99765,
            (),
            None,
            "popup 0x11 (portal scripts call it when a condition fails)",
        ),
        ("setNPC", 0x99731, (i,), None, "msg: speaker portrait = NPC arg"),
        ("startBoss", 0x9971D, (), None, "world+0x1E4 = boss scene of the area (world+0x84)"),
        (
            "setBoss",
            0x99705,
            (),
            None,
            "msg: speaker sprite from the map's mobs (probably the boss)",
        ),
        ("updateNPCState", 0x99435, (), None, "msg+0x2D = 1: re-run NPC state scripts"),
        ("clearAll", 0x996ED, (), None, "end the chapter: reset quest states, chapter += 1"),
        ("getHeroClass", 0x9AF29, (), "int", "hero job (hero+0x88)"),
        ("getClassUpCount", 0x9AED5, (), "int", "hero job advancements (hero+0x8C)"),
        ("setNpcDir", 0x99909, (i,), None, "current NPC object +8 = (arg ? 2 : 0) (facing)"),
        ("checkItemCount", 0x9AF7D, (i, i), "int", "count of item (kind, id) in the inventory"),
        ("getQuestTitle", 0x9A795, (i, i), "string", "title key of quest (kind, id)"),
        ("removeQuest", 0x995F1, (i, i), None, "drop quest (kind, id), re-run NPC states"),
        (
            "makeEquipItem",
            0x99575,
            (i, i),
            None,
            "quests 0x7C2D9 (probably crafting, ManufactureItem_*)",
        ),
        ("invenFullNotify", 0x9966D, (), None, "popup 8 (inventory full)"),
        ("isInvenAvailable", 0x9A691, (i, i), "int", "1 if inventory kind has >= n free slots"),
        ("initMonCa", 0x99545, (i,), None, "start Monster Carnival arg (0x82271)"),
        ("runAnimation", 0x9950D, (i,), None, "world+0x1E4 = arg, play that scene (0x80935)"),
        ("removeItem", 0x99485, (i, i, i), None, "take item (kind, id, n) (0x63CC1)"),
    ]
    return tuple(Native(n, name, h, p, r, d) for n, (name, h, p, r, d) in enumerate(rows))


NATIVES: tuple[Native, ...] = _natives()
NATIVE_BY_NAME: dict[str, Native] = {n.name: n for n in NATIVES}
