"""The decoded-instruction model shared by the decoder, discovery and the lifters.

An :class:`Insn` holds what the ARM ARM's decode pseudo-code produces for one instruction:
the mnemonic (spelled as Capstone spells it, without the condition, ``s`` and ``.w``), the
operands in assembler order, the condition (from the encoding, or from the enclosing IT
block), whether it sets the flags, and the raw encoding. ``text()`` renders it in Capstone's
syntax, which is how the tests use Capstone as the decoder's oracle (ADR 0003, ADR 0014).

Operands are small frozen dataclasses: :class:`Reg`, :class:`Imm` (with the carry-out of a
modified-immediate expansion), :class:`Shifted` (register with an immediate shift),
:class:`Mem`, :class:`RegList`, :class:`SReg` / :class:`DReg` / :class:`QReg`,
:class:`Label` (an absolute branch target) and :class:`FPImm`.
"""

from __future__ import annotations

from dataclasses import dataclass

from recomp.decode.bits import arm_expand_imm_text

# Condition codes (ARM ARM A8.3), the values of the 4-bit cond field.
EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL = range(15)
COND_NAMES = ("eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le")

REG_NAMES = (
    "r0 r1 r2 r3 r4 r5 r6 r7 r8 sb sl fp ip sp lr pc".split()  # Capstone's spelling
)
SP, LR, PC = 13, 14, 15

# Mnemonics that always set the flags and so never take an "s".
COMPARES = frozenset({"cmp", "cmn", "tst", "teq"})
LOGICAL = frozenset({"and", "orr", "eor", "bic", "mvn"})  # Capstone prints these unsigned


def imm_text(value: int) -> str:
    """``#n`` the way Capstone prints an immediate: decimal up to 9, else hex."""
    if -9 <= value <= 9:
        return f"#{value}"
    return f"#-0x{-value:x}" if value < 0 else f"#0x{value:x}"


@dataclass(frozen=True, slots=True)
class Reg:
    n: int

    def text(self) -> str:
        return REG_NAMES[self.n]


@dataclass(frozen=True, slots=True)
class Imm:
    """An immediate. ``carry`` is the carry-out of ThumbExpandImm_C (0/1), or None when the
    expansion leaves the carry flag unchanged."""

    value: int
    carry: int | None = None

    def text(self) -> str:
        value = self.value - (1 << 32) if self.value >= 1 << 31 else self.value
        return imm_text(value)  # Capstone prints a 32-bit immediate as signed


@dataclass(frozen=True, slots=True)
class Shifted:
    """Register ``n`` shifted by a constant: ``kind`` is lsl/lsr/asr/ror/rrx; lsr and asr
    #32 are amount 32 (DecodeImmShift)."""

    n: int
    kind: str
    amount: int

    def text(self) -> str:
        if self.kind == "rrx":
            return f"{REG_NAMES[self.n]}, rrx"
        return f"{REG_NAMES[self.n]}, {self.kind} #{self.amount}"


@dataclass(frozen=True, slots=True)
class Mem:
    """``[base, #disp]`` / ``[base, index, lsl #shift]``, pre- or post-indexed. ``disp`` is
    signed (``add`` is folded into its sign); ``subtract`` marks ``[base, -index]``."""

    base: int
    disp: int = 0
    index: int | None = None
    shift: int = 0
    subtract: bool = False  # [base, -index], or the U=0 encoding of an offset of 0
    pre: bool = True
    wback: bool = False
    neg_hex: bool = False  # Capstone spells this form's negative offset in hex

    def text(self) -> str:
        base = REG_NAMES[self.base]
        if self.index is not None:
            off = ("-" if self.subtract else "") + REG_NAMES[self.index]
            if self.shift:
                off += f", lsl #{self.shift}"
        elif self.subtract and self.disp == 0:
            off = "#-0"
        elif self.disp < 0 and self.neg_hex:
            off = f"#-0x{-self.disp:x}"
        else:
            off = imm_text(self.disp)
        if not self.pre:
            return f"[{base}], {off}"
        shown = self.index is not None or self.disp or self.subtract or self.wback
        inner = f"{base}, {off}" if shown or self.base == PC else base
        return f"[{inner}]" + ("!" if self.wback else "")


@dataclass(frozen=True, slots=True)
class RegList:
    regs: tuple[int, ...]

    def text(self) -> str:
        return "{" + ", ".join(REG_NAMES[r] for r in self.regs) + "}"


@dataclass(frozen=True, slots=True)
class SReg:
    n: int

    def text(self) -> str:
        return f"s{self.n}"


@dataclass(frozen=True, slots=True)
class DReg:
    n: int

    def text(self) -> str:
        return f"d{self.n}"


@dataclass(frozen=True, slots=True)
class QReg:
    n: int

    def text(self) -> str:
        return f"q{self.n}"


@dataclass(frozen=True, slots=True)
class VRegList:
    """``{d8, d9}`` / ``{s0, s1}``: ``first`` and ``count`` registers of one kind."""

    double: bool
    first: int
    count: int

    def text(self) -> str:
        k = "d" if self.double else "s"
        return "{" + ", ".join(f"{k}{self.first + i}" for i in range(self.count)) + "}"


@dataclass(frozen=True, slots=True)
class Label:
    """An absolute code address (a branch target; Thumb bit not included)."""

    target: int

    def text(self) -> str:
        return f"#0x{self.target:x}"


@dataclass(frozen=True, slots=True)
class FPImm:
    """A VFP immediate as its bit pattern (32 or 64 bits)."""

    bits: int
    double: bool

    def text(self) -> str:
        import struct

        if self.double:
            value = struct.unpack("<d", self.bits.to_bytes(8, "little"))[0]
        else:
            value = struct.unpack("<f", self.bits.to_bytes(4, "little"))[0]
        return f"#{value:e}"


@dataclass(frozen=True, slots=True)
class SysReg:
    """A named special register operand (``fpscr``, ``apsr_nzcv``)."""

    name: str

    def text(self) -> str:
        return self.name


Operand = Reg | Imm | Shifted | Mem | RegList | SReg | DReg | QReg | VRegList | Label | FPImm


@dataclass(frozen=True, slots=True)
class Insn:
    """One decoded instruction.

    ``mnemonic`` is the base name (``add``, ``ldrb``, ``vadd``, ``it``); ``dt`` the VFP data
    type suffix (``.f32``, ``.f64.f32``); ``wide`` prints ``.w``. ``cond`` is AL unless the
    encoding or an IT block makes it conditional; ``in_it`` marks an instruction inside an
    IT block. ``wback`` is the ``rn!`` of ldm/stm. For ``it``, ``operands`` is
    ``(Imm(firstcond), Imm(mask))``. ``mnemonic == "<undefined>"`` is an encoding the
    decoder does not know (the lifter turns it into a trap). ``arm`` marks an A32
    instruction (:mod:`recomp.decode.arm`), whose condition is its cond field."""

    address: int
    size: int
    raw: int
    mnemonic: str
    operands: tuple = ()
    cond: int = AL
    setflags: bool = False
    wide: bool = False
    dt: str = ""
    wback: bool = False
    in_it: bool = False
    arm: bool = False  # A32 (ARM state): a read of the PC is the address + 8, not + 4

    @property
    def end(self) -> int:
        return self.address + self.size

    @property
    def pc(self) -> int:
        """The value a read of the PC returns: the address + 4 in Thumb, + 8 in ARM."""
        return self.address + (8 if self.arm else 4)

    @property
    def known(self) -> bool:
        return self.mnemonic != "<undefined>"

    def full_mnemonic(self) -> str:
        m = self.mnemonic
        if self.setflags and m not in COMPARES:
            m += "s"
        if self.cond != AL:
            m += COND_NAMES[self.cond]
        return m + self.dt + (".w" if self.wide else "")

    def text(self) -> str:
        """The instruction in Capstone's syntax (``mnemonic op_str``)."""
        if self.mnemonic.startswith("it"):
            first = self.operands[0].value
            return f"{self.mnemonic} {COND_NAMES[first]}"
        ops = []
        for i, op in enumerate(self.operands):
            if isinstance(op, Imm) and self._arm_modified_imm():
                t = arm_expand_imm_text(self.raw & 0xFFF)
            elif isinstance(op, Imm) and self.mnemonic in LOGICAL:
                t = imm_text(op.value)  # Capstone prints these unsigned, arithmetic signed
            else:
                t = op.text()
            if i == 0 and self.wback and isinstance(op, Reg):
                t += "!"
            ops.append(t)
        op_str = ", ".join(ops)
        m = self.full_mnemonic()
        return f"{m} {op_str}" if op_str else m

    def _arm_modified_imm(self) -> bool:
        """An A32 data-processing immediate (Capstone spells those its own way)."""
        return self.arm and self.raw >> 25 & 7 == 1 and self.mnemonic not in ("movw", "movt")

    def it_conditions(self) -> tuple[int, ...]:
        """For an ``it`` instruction: the conditions of the 1-4 instructions it covers."""
        first, mask = self.operands[0].value, self.operands[1].value
        count = 4 - ((mask & -mask).bit_length() - 1)
        conds = [first]
        for k in range(1, count):
            bit = (mask >> (4 - k)) & 1
            conds.append((first & 0b1110) | bit)
        return tuple(conds)


def undefined(address: int, size: int, raw: int) -> Insn:
    return Insn(address, size, raw, "<undefined>")
