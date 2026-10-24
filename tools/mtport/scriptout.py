"""Loader and disassembler for the game's compiled scripts, ``script/**/*.out``.

Spec: docs/formats/script-out.md (file format, opcode table). VM internals and the native
API: docs/re/script-vm.md.

The scripts are **Lua 2.4 bytecode** as written by Lua 2.4's ``luac``, minus the chunk
header: a file is ``ESC`` (``0x1B``) followed by ``'F'`` function records, with no
``"Lua"`` signature, version byte or test word/float. The VM in the binary is a C++ port of
the Lua 2.4 interpreter (classes ``Lua``, ``TFunc``, ``Undump``, ``Table``, ``Symbol``,
``TaggedString``) with integer numbers instead of floats and the built-in library replaced
by 52 game natives. Layout of one function record::

    'F'  u16 size_hi  u16 size_lo          // code size = size_hi << 16 | size_lo
         u16 line                          // 0 = the main function
         main:  u16 len, char name[len]    // source name with its NUL ("Temp.dat")
         other: u16 marked                 // offset in main's code of its PUSHFUNCTION operand
         u8 code[size]
         { 'V' | 'S'  u16 head  u16 len, char name[len] }*   // global / string constant

Global and string operands are *threaded*: each ``V``/``S`` record names one symbol and
``head`` is the code offset of its first 16-bit operand; that operand holds the offset of the
next use, and 0 ends the chain. The loader (``Undump::LoadFunction`` ``0x9F359``) overwrites
each link with the symbol's index. ``PUSHFUNCTION``'s 4-byte operand is a stale compiler
pointer; the loader writes the function's slot number (its position among the file's
non-main functions, from 0) into its first byte.

This module parses a file into :class:`Chunk` (all bounds-checked: the files are untrusted),
decodes each function's code into :class:`Instruction` records with the threaded operands
resolved to names, recovers call sites with a small symbolic stack, and prints listings with
``Scripts.strings`` text next to dialogue keys.

Command line::

    python3 -m mtport.scriptout dis FILE [--strings Scripts.strings]   # listing
    python3 -m mtport.scriptout stats DIR [--strings ...]   # opcode / native / key coverage
    python3 -m mtport.scriptout names [OUT]   # Ghidra names (default work/re/names/scriptvm.json)
"""

from __future__ import annotations

import argparse
import json
import plistlib
import struct
import sys
from collections import Counter
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from mtport import ghidra_common as gc
from mtport.bytereader import ByteReader, FormatError
from mtport.names import hexaddr

ID_CHUNK = 0x1B
ID_FUN = ord("F")
ID_VAR = ord("V")
ID_STR = ord("S")
MAIN_LINE = 0  # a function record with line 0 is the main function
MAX_FUNCTIONS = 30  # the loader's function-slot table holds 30 entries (0x9CE81), unchecked
MULT_RET = 255  # CALLFUNC result count meaning "all results"
FIELDS_PER_FLUSH = 40  # STORELIST's block size (the 0x28 multiplier at 0x9C38E)
NAMES_OUT = "work/re/names/scriptvm.json"
# Script strings are UTF-8, except Korean text left in a few unused scripts, which is EUC-KR:
# the same fallback the game's C-string -> NSString helper (0x6DD1) uses.
TEXT_ENCODINGS = ("utf-8", "euc_kr")

# Object tags of the VM (Lua 2.4's lua_Type), as stored in each 8-byte stack slot.
TAG_NIL, TAG_NUMBER, TAG_STRING, TAG_ARRAY, TAG_FUNCTION, TAG_CFUNCTION = -1, -2, -3, -4, -5, -6
TAG_MARK, TAG_CMARK, TAG_LINE = -7, -8, -9


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


# -- file structure --------------------------------------------------------------------------


@dataclass(frozen=True)
class SymbolRef:
    """A ``V`` (global) or ``S`` (string constant) record: a name and its operand chain."""

    kind: str  # "V" or "S"
    head: int  # code offset of the first operand of the chain
    name: str


@dataclass(frozen=True)
class Function:
    """One ``F`` record. ``slot`` is None for main, else its function-slot number."""

    slot: int | None
    line: int
    source: str | None  # main only: the source name ("Temp.dat")
    marked: int | None  # others: offset of the PUSHFUNCTION operand in main's code
    code: bytes
    refs: tuple[SymbolRef, ...]
    offset: int  # file offset of the 'F' byte

    @property
    def is_main(self) -> bool:
        return self.slot is None


@dataclass(frozen=True)
class Instruction:
    offset: int
    opcode: Opcode | None  # None: an unknown opcode byte (size 1)
    raw: bytes
    operands: tuple[int, ...] = ()
    names: tuple[str, ...] = ()  # resolved global / string / function names
    target: int | None = None  # jump target offset

    @property
    def name(self) -> str:
        return self.opcode.name if self.opcode else f"UNKNOWN_{self.raw[0]:02X}"

    @property
    def end(self) -> int:
        return self.offset + len(self.raw)


@dataclass(frozen=True)
class Value:
    """A symbolic stack value, for call-site recovery."""

    kind: str  # nil, number, string, function, global, local, call, expr, array
    text: str

    @property
    def type(self) -> str:
        """The value's static type where it is known, else ``any``."""
        return {"number": "int", "string": "string", "nil": "nil", "function": "function"}.get(
            self.kind, "any"
        )


@dataclass(frozen=True)
class Call:
    offset: int  # CALLFUNC offset
    callee: str | None  # global name, when the called value is a global
    args: tuple[Value, ...]
    results: int  # CALLFUNC's result count (255 = all)


@dataclass
class Chunk:
    """A decoded ``.out`` file."""

    name: str
    functions: tuple[Function, ...]
    size: int
    _code: dict[int, tuple[Instruction, ...]] = field(default_factory=dict, repr=False)

    @property
    def main(self) -> Function:
        return self.functions[0]

    @property
    def subfunctions(self) -> tuple[Function, ...]:
        return self.functions[1:]

    def function_names(self) -> dict[int, str]:
        """slot -> the global each function is stored in by main (PUSHFUNCTION; STOREGLOBAL)."""
        out: dict[int, str] = {}
        code = self.instructions(self.main)
        for a, b in zip(code, code[1:], strict=False):
            if a.name == "PUSHFUNCTION" and b.name == "STOREGLOBAL":
                out.setdefault(a.operands[0], b.names[0])
        return out

    def instructions(self, func: Function) -> tuple[Instruction, ...]:
        key = id(func)
        if key not in self._code:
            self._code[key] = decode_code(func, self)
        return self._code[key]


def _string(r: ByteReader, what: str) -> str:
    """``u16 len`` + ``len`` bytes ending in NUL; the loader keeps ``len - 1`` bytes."""
    start = r.pos
    size = r.u16(f"{what} length")
    if size == 0:
        raise r.error(f"{what} has length 0 (no NUL)", start)
    raw = r.raw(size, what)
    if raw[-1] != 0 or 0 in raw[:-1]:
        raise r.error(f"{what} {raw!r} is not one NUL-terminated string", start)
    for encoding in TEXT_ENCODINGS:
        try:
            return raw[:-1].decode(encoding)
        except UnicodeDecodeError:
            pass
    raise r.error(f"{what} {raw!r} is neither UTF-8 nor EUC-KR", start)


def decode(data: bytes, name: str = "<out>") -> Chunk:
    """Parse a whole ``.out`` file. Raises :class:`FormatError` on any malformed input.

    Stricter than the game: it also checks every operand chain (in range, ending in 0, never
    revisiting an offset), that each ``V``/``S`` chain lands only on operands of the matching
    kind, that every PUSHFUNCTION operand of main is claimed by exactly one function, and that
    nothing follows the last function.
    """
    r = ByteReader(data, name)
    if r.u8("chunk id") != ID_CHUNK:
        raise r.error("not a script chunk (first byte is not ESC)", 0)
    functions: list[Function] = []
    while not r.at_end:
        start = r.pos
        tag = r.u8("record id")
        if tag != ID_FUN:
            raise r.error(f"expected an 'F' record, found 0x{tag:02X}", start)
        hi = r.u16("code size (high)")
        lo = r.u16("code size (low)")
        size = hi << 16 | lo
        line = r.u16("line")
        source = marked = None
        if line == MAIN_LINE:
            if functions:
                raise r.error("a second main function", start)
            source = _string(r, "source name")
        else:
            if not functions:
                raise r.error("first function is not main (line != 0)", start)
            marked = r.u16("PUSHFUNCTION offset")
        code = r.raw(size, "code")
        refs = []
        while not r.at_end and r.data[r.pos] in (ID_VAR, ID_STR):
            kind = chr(r.u8())
            head = r.u16(f"{kind} chain head")
            refs.append(SymbolRef(kind, head, _string(r, f"{kind} name")))
        slot = None if line == MAIN_LINE else len(functions) - 1
        functions.append(Function(slot, line, source, marked, code, tuple(refs), start))
    if not functions:
        raise r.error("no functions")
    if len(functions) - 1 > MAX_FUNCTIONS:
        raise r.error(f"{len(functions) - 1} functions, the slot table holds {MAX_FUNCTIONS}")
    chunk = Chunk(name, tuple(functions), len(data))
    _check(chunk)
    return chunk


def chains(func: Function, name: str = "<out>") -> dict[int, SymbolRef]:
    """Operand offset -> the ``V``/``S`` record whose chain passes through it."""
    out: dict[int, SymbolRef] = {}
    code = func.code
    for ref in func.refs:
        at = ref.head
        while at != 0:
            if at in out:
                raise FormatError(f"{name}: {ref.kind} {ref.name!r}: offset 0x{at:X} reused")
            if at + 2 > len(code):
                raise FormatError(f"{name}: {ref.kind} {ref.name!r}: 0x{at:X} outside the code")
            out[at] = ref
            at = struct.unpack_from("<H", code, at)[0]
    return out


def _operand_kinds(ins: Instruction) -> dict[int, str]:
    """Code offset -> operand kind for the threaded (g/s) operands of ``ins``."""
    out = {}
    if ins.opcode is None:
        return out
    if ins.opcode.name == "STORERECORD":
        for k in range(ins.operands[0]):
            out[ins.offset + 2 + 2 * k] = "s"
        return out
    at = ins.offset + 1
    for kind in ins.opcode.operands:
        if kind in "gs":
            out[at] = kind
        at += {"b": 1, "f": 4, "p": 4}.get(kind, 2)
    return out


def _check(chunk: Chunk) -> None:
    name = chunk.name
    for func in chunk.functions:
        code = chunk.instructions(func)  # decodes and checks threaded operands
        starts = {ins.offset for ins in code} | {len(func.code)}
        for ins in code:
            if ins.target is not None and ins.target not in starts:
                raise FormatError(f"{name}: {ins.name} at 0x{ins.offset:X} -> 0x{ins.target:X}")
            if ins.name == "PUSHFUNCTION" and not func.is_main:
                raise FormatError(f"{name}: PUSHFUNCTION outside main at 0x{ins.offset:X}")
    claimed: dict[int, int] = {}
    pushes = {i.offset + 1 for i in chunk.instructions(chunk.main) if i.name == "PUSHFUNCTION"}
    for func in chunk.subfunctions:
        assert func.marked is not None and func.slot is not None
        if func.marked not in pushes or func.marked in claimed:
            raise FormatError(
                f"{name}: function at 0x{func.offset:X}: marked 0x{func.marked:X} is not a free "
                "PUSHFUNCTION operand of main"
            )
        claimed[func.marked] = func.slot
    if set(claimed) != pushes:
        raise FormatError(
            f"{name}: PUSHFUNCTION operands with no function: {pushes - set(claimed)}"
        )


def decode_code(func: Function, chunk: Chunk | None = None) -> tuple[Instruction, ...]:
    """Decode a function's code by a linear sweep (Lua 2.4 code has no inline data).

    Raises :class:`FormatError` if an instruction runs past the end, or if the operand chains
    and the operands that need them do not match one to one.
    """
    name = chunk.name if chunk else "<out>"
    code = func.code
    links = chains(func, name)
    slots: dict[int, int] = {}
    if chunk is not None and func.is_main:
        slots = {f.marked: f.slot for f in chunk.subfunctions if f.marked is not None}
    out: list[Instruction] = []
    used: set[int] = set()
    pos = 0
    while pos < len(code):
        op = BY_CODE.get(code[pos])
        if op is None:
            out.append(Instruction(pos, None, code[pos : pos + 1]))
            pos += 1
            continue
        r = ByteReader(code, f"{name}: {op.name} at 0x{pos:X}", pos + 1)
        operands: list[int] = []
        names: list[str] = []
        target = None
        kinds = op.operands
        if kinds == "r":
            n = r.u8("record count")
            operands.append(n)
            kinds = "s" * n
        for kind in kinds:
            at = r.pos
            if kind == "b":
                operands.append(r.u8())
            elif kind == "f":
                operands.append(r.u32())
            elif kind == "p":
                raw = r.u32()
                slot = slots.get(at)
                operands.append(raw & 0xFF if slot is None else slot)
                names.append(f"f{operands[-1]}")
            else:
                value = r.u16()
                if kind in "gs":
                    ref = links.get(at)
                    want = "V" if kind == "g" else "S"
                    if ref is None or ref.kind != want:
                        raise r.error(f"operand has no {want} record", at)
                    used.add(at)
                    names.append(ref.name)
                    operands.append(0)  # the symbol index is assigned at load time
                    continue
                operands.append(value)
                if kind == "j":
                    target = r.pos + value
                elif kind == "u":
                    target = r.pos - value
        out.append(Instruction(pos, op, code[pos : r.pos], tuple(operands), tuple(names), target))
        pos = r.pos
    stray = set(links) - used
    if stray:
        at = min(stray)
        raise FormatError(f"{name}: {links[at].kind} {links[at].name!r} chain hits 0x{at:X}")
    return tuple(out)


def load(path: str | Path) -> Chunk:
    path = Path(path)
    return decode(path.read_bytes(), path.name)


# -- call sites ------------------------------------------------------------------------------


def calls(chunk: Chunk, func: Function) -> list[Call]:
    """Recover the call sites of ``func`` with a symbolic stack.

    Lua 2.4 keeps a function's locals at the bottom of its frame and every statement leaves
    the stack at the locals, so a linear walk is exact for straight-line expressions: a
    condition jumps only after its value is popped (IFFJMP) or keeps it (``and``/``or``:
    ONTJMP/ONFJMP then POP), and loops jump back with the stack balanced.
    """
    stack: list[Value] = []
    out: list[Call] = []
    names = chunk.function_names()

    def pop(n: int) -> list[Value]:
        n = max(n, 0)
        taken = stack[len(stack) - n :] if n else []
        del stack[len(stack) - n :]
        return [Value("expr", "?")] * (n - len(taken)) + taken

    for ins in chunk.instructions(func):
        op = ins.name
        if ins.opcode is None:
            continue
        if op == "PUSHNIL":
            stack.append(Value("nil", "nil"))
        elif op in ("PUSH0", "PUSH1", "PUSH2"):
            stack.append(Value("number", op[-1]))
        elif op in ("PUSHBYTE", "PUSHWORD"):
            stack.append(Value("number", str(ins.operands[0])))
        elif op == "PUSHSTRING":
            stack.append(Value("string", ins.names[0]))
        elif op == "PUSHFUNCTION":
            slot = ins.operands[0]
            stack.append(Value("function", names.get(slot, f"f{slot}")))
        elif op.startswith("PUSHLOCAL"):
            n = ins.operands[0] if ins.operands else int(op[9:])
            stack.append(Value("local", f"local{n}"))
        elif op == "PUSHGLOBAL":
            stack.append(Value("global", ins.names[0]))
        elif op == "CREATEARRAY":
            stack.append(Value("array", "{}"))
        elif op in ("ADJUST0", "ADJUST"):
            n = ins.operands[0] if ins.operands else 0
            del stack[n:]
            stack.extend(Value("nil", "nil") for _ in range(n - len(stack)))
        elif op == "CALLFUNC":
            nargs, nres = ins.operands
            args = pop(nargs)
            (fn,) = pop(1)
            out.append(
                Call(ins.offset, fn.text if fn.kind == "global" else None, tuple(args), nres)
            )
            text = f"{fn.text}()"
            stack.extend(Value("call", text) for _ in range(1 if nres == MULT_RET else nres))
        elif op == "STOREINDEXED":
            pop(1)  # value; the table and index stay below for the next store (Lua 2.4)
        elif op in ("STORELIST0", "STORELIST"):
            pop(ins.operands[-1])
        elif op == "STORERECORD":
            pop(ins.operands[0])
        elif op in ("RETCODE0", "RETCODE"):
            stack.clear()
        elif ins.opcode.pops is not None and ins.opcode.pushes is not None:
            pop(ins.opcode.pops)
            stack.extend(Value("expr", op) for _ in range(ins.opcode.pushes))
    return out


# -- strings tables --------------------------------------------------------------------------


def load_strings(path: str | Path) -> dict[str, str]:
    """A ``*.lproj/*.strings`` table (a binary or XML property list of string -> string)."""
    with open(path, "rb") as fh:
        try:
            table = plistlib.load(fh)
        except (plistlib.InvalidFileException, ValueError) as e:
            raise FormatError(f"{path}: not a property list: {e}") from None
    if not isinstance(table, dict) or not all(
        isinstance(k, str) and isinstance(v, str) for k, v in table.items()
    ):
        raise FormatError(f"{path}: not a table of strings")
    return table


def is_dialogue_key(text: str) -> bool:
    """Whether a string constant has the shape of a ``Scripts.strings`` key.

    Keys are ``<Prefix>.<function>.<n>`` with an alphanumeric/underscore prefix
    (``Chapter1_Quest_Manji.dialog.1``, ``ManufactureItem.Equip.12``).
    """
    parts = text.split(".")
    return (
        len(parts) == 3
        and parts[2].isdigit()
        and all(p and all(c.isalnum() or c == "_" for c in p) for p in parts[:2])
    )


# -- listing ---------------------------------------------------------------------------------


def _quote(text: str, limit: int = 0) -> str:
    if limit and len(text) > limit:
        text = text[: limit - 3] + "..."
    return json.dumps(text, ensure_ascii=False)


def listing(chunk: Chunk, strings: Mapping[str, str] | None = None) -> str:
    """A text disassembly of ``chunk``, with dialogue keys resolved through ``strings``."""
    names = chunk.function_names()
    lines = [f"; {chunk.name}: {chunk.size} bytes, {len(chunk.functions)} functions"]
    for func in chunk.functions:
        if func.is_main:
            title = f"main  ; source {_quote(func.source or '')}"
        else:
            assert func.slot is not None
            title = f"f{func.slot} {names.get(func.slot, '?')}  ; line {func.line}"
        lines += ["", f"function {title}, {len(func.code)} bytes"]
        targets = {i.target for i in chunk.instructions(func) if i.target is not None}
        call_at = {c.offset: c for c in calls(chunk, func)}
        for ins in chunk.instructions(func):
            if ins.offset in targets:
                lines.append(f"  L{ins.offset:04X}:")
            text = _operand_text(ins, names)
            comment = _comment(ins, call_at.get(ins.offset), strings)
            row = f"    {ins.offset:04X}  {ins.raw.hex(' '):<14} {ins.name:<13} {text}"
            lines.append((row.rstrip() + ("  ; " + comment if comment else "")).rstrip())
    return "\n".join(lines) + "\n"


def _operand_text(ins: Instruction, names: Mapping[int, str]) -> str:
    if ins.opcode is None:
        return ""
    op = ins.opcode.name
    if op == "PUSHFUNCTION":
        slot = ins.operands[0]
        return f"f{slot} ({names.get(slot, '?')})"
    if op in ("PUSHGLOBAL", "STOREGLOBAL"):
        return ins.names[0]
    if op in ("PUSHSTRING", "PUSHSELF"):
        return _quote(ins.names[0])
    if op == "STORERECORD":
        return " ".join(_quote(n) for n in ins.names)
    if ins.target is not None:
        return f"L{ins.target:04X}"
    if op == "PUSHFLOAT":
        return str(struct.unpack("<f", struct.pack("<I", ins.operands[0]))[0])
    return " ".join(str(v) for v in ins.operands)


def _comment(ins: Instruction, call: Call | None, strings: Mapping[str, str] | None) -> str:
    if ins.opcode is None:
        return "unknown opcode"
    if ins.opcode.name == "PUSHSTRING" and strings is not None:
        key = ins.names[0]
        if key in strings:
            return _quote(strings[key], 100)
        if is_dialogue_key(key):
            return "key not in Scripts.strings"
    if call is not None:
        args = ", ".join(_quote(a.text) if a.kind == "string" else a.text for a in call.args)
        native = NATIVE_BY_NAME.get(call.callee or "")
        tag = " [native]" if native else ""
        return f"{call.callee or '?'}({args}){tag}"
    if ins.opcode.name in DESYNC_OPCODES:
        return "the game misreads this opcode"
    return ""


# -- statistics over a directory -------------------------------------------------------------


@dataclass
class Stats:
    files: int = 0
    functions: int = 0
    instructions: int = 0
    unknown: int = 0
    opcodes: Counter = field(default_factory=Counter)
    native_calls: Counter = field(default_factory=Counter)
    native_arity: dict[str, Counter] = field(default_factory=dict)
    native_types: dict[str, list[Counter]] = field(default_factory=dict)
    other_calls: Counter = field(default_factory=Counter)
    keys: set[str] = field(default_factory=set)
    missing_keys: set[str] = field(default_factory=set)
    strings: set[str] = field(default_factory=set)

    def add(self, chunk: Chunk, table: Mapping[str, str] | None) -> None:
        self.files += 1
        defined = set(chunk.function_names().values())
        for func in chunk.functions:
            self.functions += 1
            for ins in chunk.instructions(func):
                self.instructions += 1
                self.opcodes[ins.name] += 1
                if ins.opcode is None:
                    self.unknown += 1
                if ins.name in ("PUSHSTRING", "STORERECORD", "PUSHSELF"):
                    for text in ins.names:
                        self.strings.add(text)
                        if is_dialogue_key(text) or (table is not None and text in table):
                            self.keys.add(text)
                            if table is not None and text not in table:
                                self.missing_keys.add(text)
            for call in calls(chunk, func):
                if call.callee in NATIVE_BY_NAME:
                    self.native_calls[call.callee] += 1
                    self.native_arity.setdefault(call.callee, Counter())[len(call.args)] += 1
                    types = self.native_types.setdefault(call.callee, [])
                    for k, arg in enumerate(call.args):
                        if k == len(types):
                            types.append(Counter())
                        types[k][arg.type] += 1
                else:
                    where = "local" if call.callee in defined else "extern"
                    self.other_calls[(where, call.callee)] += 1


def scan(root: str | Path, table: Mapping[str, str] | None = None) -> tuple[Stats, list[str]]:
    """Decode every ``*.out`` under ``root``; returns the statistics and the decode errors."""
    stats, errors = Stats(), []
    for path in sorted(Path(root).rglob("*.out")):
        try:
            chunk = decode(path.read_bytes(), str(path.relative_to(root)))
        except FormatError as e:
            errors.append(str(e))
            continue
        stats.add(chunk, table)
    return stats, errors


# -- Ghidra names ----------------------------------------------------------------------------

VM_NAMES: tuple[tuple[str, str, str], ...] = (
    # (function pointer, name, comment). The rest of the VM is in docs/re/script-vm.md.
    ("0x98EE1", "DataReader::u8", "next byte; 0x98EB9 is an identical copy"),
    ("0x98EB9", "DataReader::u8_copy", "same as DataReader::u8"),
    ("0x98F09", "DataReader::skip", "pos += n"),
    ("0x98F21", "DataReader::rewind", "pos = mark"),
    ("0x98F31", "DataReader::mark", "mark = pos"),
    ("0x98F41", "DataReader::remaining", "size - pos"),
    ("0x98F5D", "DataReader::u32", ""),
    ("0x98F95", "DataReader::u16", ""),
    ("0x98FC9", "DataReader::bytes", "new[] copy of the next n bytes"),
    ("0x990CD", "DataReader::DataReader", "(buffer, size)"),
    ("0x9912D", "Lua::stackSlot", "&stack[n - 1]"),
    ("0x99161", "Lua::error", "empty: VM errors are ignored"),
    ("0x9916D", "Lua::equalObjects", "EQOP"),
    ("0x99235", "Lua::pushIndexed", "PUSHINDEXED (arrays only)"),
    ("0x9930D", "Lua::storeIndexed0", "STOREINDEXED0"),
    ("0x993B5", "Lua::linkFunction", "TFunc onto the Lua+0xC list"),
    ("0x993D9", "Lua::getParam", "lua_getparam: slot of argument n, or -1"),
    ("0x9A1D5", "Lua::numberToString", "tostring coercion (%d)"),
    ("0x9A349", "Lua::toStringCheck", "string, or a number made a string"),
    ("0x9A391", "Lua::newReader", "DataReader over the script buffer"),
    ("0x9A4C5", "Lua::stringToNumber", "atol coercion"),
    ("0x9A521", "Lua::toNumberCheck", "number, or a string made a number"),
    ("0x9A569", "Lua::growStack", "doubles the 8-byte-slot stack"),
    ("0x9B37D", "Lua::callNative", "switch on the symbol index -> native handler"),
    ("0x9B621", "Lua::pushStringOrNil", ""),
    ("0x9B6B5", "Lua::comparison", "LTOP/LEOP/GTOP/GEOP"),
    ("0x9B821", "Lua::pushGlobal", "PUSHGLOBAL"),
    ("0x9B8AD", "Lua::setLine", "SETLINE: opens a LUA_T_LINE slot"),
    ("0x9B979", "Lua::adjustTop", "top = stack + base + n, filling with nil"),
    ("0x9BA11", "Lua::doCall", "do_call: CFUNCTION -> callNative, FUNCTION -> execute"),
    ("0x9BC11", "Lua::execute", "the interpreter loop: tbh over opcodes 0x00..0x42"),
    ("0x9CD65", "Lua::callFunction", "push a TFunc and call it with no arguments"),
    ("0x9CE2D", "Lua::runCallback", "calls and clears Lua+8 (regCallBack); from 0x35E89"),
    ("0x9CE81", "Lua::reset", "clears the stack, slot table, GC list, constants"),
    ("0x9D0A9", "Lua::loadBuffer", "reset, then undump a script buffer"),
    ("0x9D5C5", "Lua::Lua", "(owner world); first instance creates the symbol tables"),
    ("0x9DD65", "Stringtable::intern", "hash bucket lookup / insert"),
    ("0x9E165", "Table::constant", "TaggedString of constant n"),
    ("0x9E18D", "Table::symbol", "Symbol n"),
    ("0x9E1B5", "TaggedString::constantIndex", "assigns the next constant index"),
    ("0x9E201", "TaggedString::symbolIndex", "assigns the next symbol index"),
    ("0x9E361", "Table::createString", "luaI_createstring(mark, text)"),
    ("0x9E3DD", "Table::findConstantByName", "luaI_findconstantbyname"),
    ("0x9E401", "Table::findSymbolByName", "luaI_findsymbolbyname"),
    ("0x9E425", "Table::initConstants", ""),
    ("0x9E441", "Table::registerNatives", "65 names from 0x117950 as LUA_T_CFUNCTION = index"),
    ("0x9E4F5", "Table::resetToNatives", "drops every symbol after the 65 natives, constants"),
    ("0x9DA4D", "Stringtable::sweep", "frees strings marked < 3 (not natives)"),
    ("0x9E98D", "Table::Table", "8 string buckets, natives, constants"),
    ("0x9EC11", "TaggedString::TaggedString", "(mark, hash, text)"),
    ("0x9EC5D", "FunctionSlots::get", "PUSHFUNCTION: slot -> TFunc"),
    ("0x9EC85", "FunctionSlots::reset", ""),
    ("0x9ED09", "FunctionSlots::add", "returns the TFunc's slot number"),
    ("0x9ED59", "TFunc::unthread", "walk a V/S chain writing the symbol index"),
    ("0x9EDD1", "TFunc::rewind", "pc = 0"),
    ("0x9EDF1", "TFunc::jumpBack", "UPJMP/IFFUPJMP"),
    ("0x9EE21", "TFunc::jumpForward", "ONTJMP/ONFJMP/JMP/IFFJMP"),
    ("0x9EE45", "TFunc::fetchU32", ""),
    ("0x9EE71", "TFunc::fetchU16", ""),
    ("0x9EE95", "TFunc::fetchU8", ""),
    ("0x9F241", "TFunc::TFunc", ""),
    ("0x9F2A5", "Undump::loadBlock", ""),
    ("0x9F2C9", "Undump::loadString", "u16 size + size bytes (NUL dropped)"),
    ("0x9F33D", "Undump::loadNewString", ""),
    ("0x9F359", "Undump::loadFunction", "one 'F' record + its V/S records"),
    ("0x9F735", "Undump::loadChunk", "'F' records until another byte"),
    ("0x9F795", "Undump::undump", "ESC, then loadChunk; returns main"),
    ("0x9FB11", "Undump::Undump", "(reader)"),
    ("0x6DD1", "LocalizedCString", "C key -> [NSBundle localizedStringForKey:...table:]"),
    ("0x81369", "CMapleWorld::changeMap", "+0x98 area, +0x9C map, +0xA0 portal, +0xA4/+0xA8 = -1"),
)
DATA_NAMES: tuple[tuple[str, str, str], ...] = (
    ("0x117950", "g_scriptNativeNames", "65 char*: Lua 2.4 builtins then game natives"),
    ("0x1178EC", "g_luaStack", "stack base (8-byte TObject slots)"),
    ("0x1178F0", "g_luaTop", ""),
    ("0x1178F4", "g_luaStackLimit", ""),
)


def names_json() -> str:
    rows = [(a, "function", n, c) for a, n, c in VM_NAMES]
    for nat in NATIVES:
        if nat.handler is not None:
            sig = ", ".join(nat.params)
            ret = f" -> {nat.returns}" if nat.returns else ""
            comment = f"#{nat.index} {nat.name}({sig}){ret}: {nat.does}"
            rows.append((hexaddr(nat.handler), "function", f"ScriptNative_{nat.name}", comment))
    rows += [(a, "data", n, c) for a, n, c in DATA_NAMES]
    return gc.names_json("scriptvm", rows)


# -- command line ----------------------------------------------------------------------------


def _print_stats(stats: Stats, errors: Sequence[str], table: Mapping[str, str] | None) -> None:
    print(f"files {stats.files}, functions {stats.functions}, instructions {stats.instructions}")
    print(f"decode errors {len(errors)}, unknown opcodes {stats.unknown}")
    for e in errors:
        print(f"  error: {e}")
    print("opcodes: " + ", ".join(f"{k} {v}" for k, v in sorted(stats.opcodes.items())))
    unused = [op.name for op in OPCODES if op.name not in stats.opcodes]
    print("unused opcodes: " + " ".join(unused))
    print(f"natives called: {len(stats.native_calls)} of {len(NATIVES)}")
    for name, count in sorted(stats.native_calls.items()):
        arity = "/".join(str(n) for n in sorted(stats.native_arity[name]))
        types = ", ".join("|".join(sorted(t)) for t in stats.native_types[name])
        print(f"  {name}: {count} calls, {arity} args ({types})")
    print(f"natives never called: {[n.name for n in NATIVES if n.name not in stats.native_calls]}")
    print("other callees: " + ", ".join(f"{w}:{c} {n}" for (w, c), n in stats.other_calls.items()))
    if table is not None:
        print(f"dialogue keys {len(stats.keys)}, not in the table {len(stats.missing_keys)}")
        for key in sorted(stats.missing_keys):
            print(f"  missing: {key}")
        print(f"table entries never referenced: {len(set(table) - stats.keys)}")


def _iter_strings(paths: Iterable[str]) -> dict[str, str] | None:
    table: dict[str, str] | None = None
    for p in paths:
        table = {**(table or {}), **load_strings(p)}
    return table


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.scriptout", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    dis = sub.add_parser("dis", help="disassemble .out files")
    dis.add_argument("files", nargs="+", type=Path)
    dis.add_argument("--strings", action="append", default=[], help="a .strings table")
    st = sub.add_parser("stats", help="decode every .out under DIR and summarise")
    st.add_argument("root", type=Path)
    st.add_argument("--strings", action="append", default=[], help="a .strings table")
    names = sub.add_parser("names", help=f"write the Ghidra names file (default {NAMES_OUT})")
    names.add_argument("out", nargs="?", default=NAMES_OUT)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "dis":
            table = _iter_strings(args.strings)
            for path in args.files:
                sys.stdout.write(listing(load(path), table))
        elif args.cmd == "stats":
            table = _iter_strings(args.strings)
            stats, errors = scan(args.root, table)
            _print_stats(stats, errors, table)
            return 1 if errors or stats.unknown else 0
        else:
            out = Path(args.out)
            gc.write_text(out, names_json())
            count = len(VM_NAMES) + len(DATA_NAMES) + sum(1 for n in NATIVES if n.handler)
            print(f"wrote {out} ({count} names)")
    except (OSError, FormatError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
