"""Instruction census: every discovered instruction by *form* (#33; VFP/NEON #34).

A **form** is one way of using an instruction that the lifter handles differently, written as
a short key: the mnemonic (with ``s`` when it sets the flags), then the operand shapes, then
markers. ``adds r,r,#``, ``ldr r,[r,#-]!``, ``orrs r,r,r,lsl #``, ``b tail``,
``pop {..pc} ret ?``. What goes into the key (:func:`form`):

- ``a32`` first for an ARM-state instruction (PC reads + 8, ALU writes to PC interwork).
- Registers: ``pc`` (a read is a constant, a write is a jump) or ``r``.
- Immediates: ``#``, ``#c`` for the immediate of a flag-setting logical instruction whose
  expansion has a carry-out (ThumbExpandImm_C / ARMExpandImm_C sets C), ``#32`` for the
  shift amount 32 of ``lsr``/``asr`` (DecodeImmShift).
- Shifted registers: ``r,lsl #``, ``r,lsr #32``, ``r,rrx`` ...
- Memory: ``[pc,#]`` (a literal, folded), ``[r]``, ``[r,#]``, ``[r,#-]`` (negative),
  ``[r,r]``, ``[r,-r]``, ``[r,r,lsl #]``, ``[r,#]!`` (pre-indexed with writeback),
  ``[r],#`` (post-indexed).
- Register lists: ``{..}``, ``{..pc}`` (loads the PC), ``{..rn}`` (the base is in the list).
- Branch targets: ``label`` (inside the function: a goto, or a call target), ``tail``
  (a ``b``/``cbz`` that leaves the function: a tail call).
- Markers: ``ret`` (a return: ``bx lr``, ``pop {..pc}``, ``ldr pc, [sp], #4``),
  ``table:<kind>`` (a jump-table site discovery resolved), ``?`` (conditional: in an IT
  block or with a condition field).

Conditions are forms too: ``cond <name>`` for each condition code some instruction uses.
VFP forms are the mnemonic with its data type and the register kinds (``vadd.f32 s,s,s``,
``vmov r,r,s,s``, ``vmov.f32 s,#f``). Advanced SIMD forms the same, after ``neon``
(``neon vadd.f32 d,d,d``, ``neon vmov.i32 q,#``); an Advanced SIMD encoding the decoder does
not know is the one form ``neon <undecoded>``.

Each form's class is ``integer``, ``vfp``, ``neon`` or ``undefined`` (:func:`insn_class`).
The snippet suite must cover every form of the binary (#33 for the integer forms, #34 for
VFP and NEON): ``tests/recomp/integer_forms.txt`` and ``tests/recomp/vfp_forms.txt`` list
them, and ``tests/test_recomp_census.py`` checks that a passing snippet contains each one,
using :func:`function_forms` on the snippets.

``python -m recomp census`` discovers and lifts the image and writes
``work/recomp/census.json``, schema 1 (``ghidra_common.dumps`` layout, forms sorted by
class, then form)::

    {"schema": 1, "source": {"path", "arch", "sha256"},
     "counts": {"functions", "instructions", "forms", "integer_forms", "vfp_forms",
                "neon_forms", "undefined_forms", "unsupported_instructions",
                "unsupported_integer_instructions", "unsupported_forms", ...},
     "forms": [
      {"form": "adds r,r,#", "class": "integer", "count": 413, "functions": 120,
       "example": "0x2A3C4",          # the lowest address with this form
       "unsupported": 0}]}            # instances the lifter turned into a trap

The input is untrusted data: it is decoded, never executed.
"""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

from recomp import REPO
from recomp.decode.insn import (
    AL,
    COMPARES,
    COND_NAMES,
    LOGICAL,
    PC,
    DReg,
    FPImm,
    Imm,
    Insn,
    Label,
    Mem,
    QReg,
    Reg,
    RegList,
    Shifted,
    SReg,
    SysReg,
    VRegList,
)
from recomp.discover import Function, is_return, is_simd

CENSUS_OUT = REPO / "work" / "recomp" / "census.json"
CLASSES = ("integer", "vfp", "neon", "undefined")
_LOGIC_FLAGS = LOGICAL | {"orn", "tst", "teq", "mov"}  # their S forms set C from the shifter
_SHIFTS = frozenset({"lsl", "lsr", "asr", "ror"})
_MULTIPLE = frozenset({"ldm", "stm", "ldmdb", "stmdb"})


def insn_class(insn: Insn) -> str:
    """``integer``, ``vfp`` (VFP, incl. the core<->VFP moves and ``vmrs``), ``neon``
    (Advanced SIMD, decoded or not) or ``undefined``."""
    if not insn.known:
        return "neon" if is_simd(insn) else "undefined"
    if insn.simd:
        return "neon"
    return "vfp" if insn.mnemonic.startswith("v") else "integer"


def _imm(insn: Insn, op: Imm, index: int) -> str:
    m = insn.mnemonic
    if m in _SHIFTS and index == len(insn.operands) - 1:
        return "#32" if op.value == 32 else "#"
    if insn.setflags and m in _LOGIC_FLAGS and op.carry is not None:
        return "#c"
    return "#"


def _mem(op: Mem) -> str:
    base = "pc" if op.base == PC else "r"
    if op.index is not None:
        off = ("-r" if op.subtract else "r") + (",lsl #" if op.shift else "")
    elif op.disp < 0 or op.subtract:
        off = "#-"
    else:
        off = "#" if op.disp else ""
    if not op.pre:
        return f"[{base}],{off}"
    return f"[{base},{off}]" + ("!" if op.wback else "") if off else f"[{base}]"


def _operand(insn: Insn, op, index: int, fn: Function | None) -> str:
    if isinstance(op, Reg):
        return "pc" if op.n == PC else "r"
    if isinstance(op, Imm):
        return _imm(insn, op, index)
    if isinstance(op, Shifted):
        if op.kind == "rrx":
            return "r,rrx"
        return f"r,{op.kind} #32" if op.amount == 32 else f"r,{op.kind} #"
    if isinstance(op, Mem):
        return _mem(op)
    if isinstance(op, RegList):
        marks = ""
        if PC in op.regs:
            marks += "pc"
        base = insn.operands[0]
        if insn.mnemonic in _MULTIPLE and isinstance(base, Reg) and base.n in op.regs:
            marks += "rn"
        return "{.." + marks + "}"
    if isinstance(op, Label):
        if insn.mnemonic in ("b", "cbz", "cbnz") and fn is not None:
            inside = fn.contains(op.target) and op.target in fn.insns
            return "label" if inside else "tail"
        return "label"
    if isinstance(op, SReg):
        return "s"
    if isinstance(op, DReg):
        return "d"
    if isinstance(op, QReg):
        return "q"
    if isinstance(op, VRegList):
        return "{d..}" if op.double else "{s..}"
    if isinstance(op, FPImm):
        return "#f"
    if isinstance(op, SysReg):
        return op.name
    return type(op).__name__


def form(insn: Insn, fn: Function | None = None) -> str:
    """The form key of ``insn`` (see the module docstring). ``fn``, the function it belongs
    to, tells a local branch from a tail call and marks jump-table sites."""
    cls = insn_class(insn)
    if cls == "neon" and not insn.known:
        return "neon <undecoded>"
    if cls == "undefined":
        return "<undefined>"
    m = insn.mnemonic
    parts = ["a32"] if insn.arm else ["neon"] if insn.simd else []
    if m.startswith("it") and not insn.arm:
        return m  # it, itt, ite, ...: the conditions are their own forms
    if cls == "integer" and insn.setflags and m not in COMPARES:
        m += "s"
    parts.append(m + insn.dt)
    ops = [_operand(insn, op, i, fn) for i, op in enumerate(insn.operands)]
    if insn.wback and ops and isinstance(insn.operands[0], Reg):
        ops[0] += "!"
    if ops:
        parts.append(",".join(ops))
    if fn is not None and insn.address in fn.tables:
        parts.append(f"table:{fn.tables[insn.address].kind}")
    elif cls == "integer" and is_return(insn):
        parts.append("ret")
    if insn.cond != AL:
        parts.append("?")
    return " ".join(parts)


def conditions(insn: Insn) -> list[str]:
    """``cond <name>`` for the condition ``insn`` executes under, and for an ``it`` the
    conditions of the instructions it covers."""
    if insn.known and insn.mnemonic.startswith("it") and not insn.arm:
        return [f"cond {COND_NAMES[c]}" for c in insn.it_conditions()]
    if insn.cond != AL and insn.known:
        return [f"cond {COND_NAMES[insn.cond]}"]
    return []


def function_forms(fn: Function) -> dict[str, list[Insn]]:
    """Every form (and condition) of ``fn``'s instructions, with the instructions."""
    out: dict[str, list[Insn]] = {}
    for insn in fn.ordered():
        for key in (form(insn, fn), *conditions(insn)):
            out.setdefault(key, []).append(insn)
    return out


def form_class(key: str) -> str:
    """The class of a form key."""
    if key.startswith("cond "):
        return "integer"
    if key.startswith("neon "):
        return "neon"
    if key == "<undefined>":
        return "undefined"
    mnemonic = key.removeprefix("a32 ").split(" ", 1)[0]
    return "vfp" if mnemonic.startswith("v") else "integer"


@dataclass
class FormStats:
    form: str
    cls: str
    count: int = 0
    functions: set[int] = field(default_factory=set)
    example: int | None = None
    unsupported: int = 0

    def to_json(self) -> dict:
        return {
            "form": self.form,
            "class": self.cls,
            "count": self.count,
            "functions": len(self.functions),
            "example": None if self.example is None else f"0x{self.example:X}",
            "unsupported": self.unsupported,
        }


@dataclass
class Census:
    forms: dict[str, FormStats]
    functions: int
    instructions: int

    def of_class(self, cls: str) -> list[FormStats]:
        return [f for f in self.forms.values() if f.cls == cls]

    def counts(self) -> dict[str, int]:
        out = {"functions": self.functions, "instructions": self.instructions}
        out["forms"] = len(self.forms)
        for cls in CLASSES:
            out[f"{cls}_forms"] = len(self.of_class(cls))
            out[f"{cls}_instructions"] = sum(
                f.count for f in self.of_class(cls) if not f.form.startswith("cond ")
            )
        out["unsupported_instructions"] = sum(
            f.unsupported for f in self.forms.values() if not f.form.startswith("cond ")
        )
        out["unsupported_integer_instructions"] = sum(
            f.unsupported for f in self.of_class("integer") if not f.form.startswith("cond ")
        )
        out["unsupported_forms"] = sum(bool(f.unsupported) for f in self.forms.values())
        return out

    def to_json(self, source: dict) -> str:
        from mtport import ghidra_common as gc

        header = {"schema": 1, "source": source, "counts": self.counts()}
        rows = sorted(self.forms.values(), key=lambda f: (CLASSES.index(f.cls), f.form))
        return gc.dumps(header, "forms", [f.to_json() for f in rows])


def take(functions: Iterable[Function], unsupported: Iterable[int] = ()) -> Census:
    """The census of ``functions``. ``unsupported``: addresses the lifter trapped at."""
    traps = set(unsupported)
    forms: dict[str, FormStats] = {}
    n_fn = n_insn = 0
    for fn in functions:
        n_fn += 1
        n_insn += len(fn.insns)
        for key, insns in function_forms(fn).items():
            stats = forms.get(key)
            if stats is None:
                stats = forms[key] = FormStats(key, form_class(key))
            stats.count += len(insns)
            stats.functions.add(fn.ptr)
            first = min(i.address for i in insns)
            stats.example = first if stats.example is None else min(stats.example, first)
            if not key.startswith("cond "):
                stats.unsupported += sum(i.address in traps for i in insns)
    return Census(forms, n_fn, n_insn)


FORM_LIST_HEADER = """\
# The integer instruction forms of the armv7 slice (#33): every form `python -m recomp census`
# reports with class "integer", one per line (recomp/census.py). Form names only, no bytes or
# code. tests/test_recomp_census.py checks in CI that a passing snippet covers each one, and
# tests/test_recomp_census_real.py checks locally that this is the binary's list.
# Regenerate: python -m recomp census --forms tests/recomp/integer_forms.txt
"""


VFP_FORM_LIST_HEADER = """\
# The VFP and Advanced SIMD (NEON) instruction forms of the armv7 slice (#34): every form
# `python -m recomp census` reports with class "vfp" or "neon", one per line (recomp/census.py).
# Form names only, no bytes or code. tests/test_recomp_census.py checks in CI that a passing
# snippet covers each one, and tests/test_recomp_census_real.py checks locally that this is
# the binary's list. Regenerate: python -m recomp census --vfp-forms tests/recomp/vfp_forms.txt
"""


def _form_list(header: str, census: Census, *classes: str) -> str:
    rows = sorted((f for c in classes for f in census.of_class(c)), key=lambda f: f.form)
    return header + "".join(f"{f.form}\n" for f in rows)


def integer_form_list(census: Census) -> str:
    """The text of ``tests/recomp/integer_forms.txt``: the census's integer forms, sorted."""
    return _form_list(FORM_LIST_HEADER, census, "integer")


def vfp_form_list(census: Census) -> str:
    """The text of ``tests/recomp/vfp_forms.txt``: the census's VFP and NEON forms, sorted."""
    return _form_list(VFP_FORM_LIST_HEADER, census, "vfp", "neon")


def read_form_list(text: str) -> list[str]:
    """The forms of an ``integer_forms.txt`` (comment lines start with ``#``)."""
    return [line for line in text.splitlines() if line and not line.startswith("#")]


def run(slice_path: Path, out: Path) -> Census:
    """Discover and lift the whole image, write ``out`` (census.json), return the census."""
    from mtport import ghidra_common as gc
    from recomp.discover import discover
    from recomp.loader import Image
    from recomp.prototype import lift

    data = Path(slice_path).read_bytes()
    image = Image(data)
    found = discover(image)
    ptrs = sorted(found.functions)
    lifted = lift(image, ptrs, discovery=found)
    traps = {addr for f in lifted for addr, _ in f.unsupported}
    census = take((found.functions[p] for p in ptrs), traps)
    gc.write_text(out, census.to_json(gc.source_info(Path(slice_path), data, REPO)))
    return census


__all__ = [
    "CENSUS_OUT", "CLASSES", "Census", "FormStats", "conditions", "form", "form_class",
    "function_forms", "insn_class", "integer_form_list", "read_form_list", "run", "take",
    "vfp_form_list",
]  # fmt: skip
