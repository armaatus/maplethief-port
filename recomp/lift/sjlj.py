"""SjLj resume points: the ``setjmp`` hook and the resume label (#37, ADR 0014, ADR 0020).

A function with landing pads stores a resume address inside itself into its jump buffer
(``jbuf[1]``, at the function context + 0x24). Discovery (#32) makes that address an
internal entry of the function (``Function.entries``); its code is the dispatch to the
landing pads. Here each entry is matched with the store that writes it:

- the game's LLVM-GCC code: ``mov r4, pc; adds r4, r4, #7; ...; str r4, [r5, #4]``;
- Chartboost's clang code: ``ldr r1, [pc, #k]; orr r1, r1, #1; add r1, pc; ...;
  str r1, [sp, #0x70]``.

After that store the lifter emits ``if (MT_SJLJ_SETJMP(jbuf)) { goto L_<entry>; }`` (``jbuf``
is the guest address of ``jbuf[0]``, the store's address - 4) and gives the entry its label.
When the runtime's unwinder (``runtime/src/sjlj.c``) installs a landing pad of this
function, it restores ``r7`` and ``sp`` from the guest jump buffer and ``longjmp``s to the
host ``jmp_buf`` registered here, so the C function continues at the resume label with all
guest state in ``cpu``. The flag check treats the label as an entry with no flag defined.

The tracking is straight-line in address order and restarts at labels and entries, like
discovery's ``find_code_refs``; an entry no store matches is a lifting problem (its code
stays unreachable and is not emitted with a label).
"""

from __future__ import annotations

from dataclasses import dataclass

from recomp.decode.insn import AL, PC, Imm, Insn, Mem, Reg
from recomp.discover import Function, literal, written_regs


@dataclass(frozen=True)
class Hook:
    """At ``site`` (the store of the resume address), ``jbuf[0]`` is at
    ``r[base] + disp``; a longjmp continues at ``resume`` (Thumb bit clear)."""

    site: int
    resume: int
    base: int
    disp: int

    def c(self) -> str:
        reg = f"cpu->r[{self.base}]"
        addr = reg if self.disp == 0 else f"{reg} + 0x{self.disp & 0xFFFFFFFF:X}u"
        if self.disp < 0:
            addr = f"{reg} - 0x{-self.disp:X}u"
        return f"if (MT_SJLJ_SETJMP({addr})) {{ goto L_{self.resume:08X}; }}"


def _pc_value(insn: Insn, known: dict[int, int], const_word) -> int | None:
    """The value ``insn`` writes to its first register if it is computed from the PC."""
    m, ops = insn.mnemonic, insn.operands
    if insn.cond != AL or not ops or not isinstance(ops[0], Reg) or ops[0].n == PC:
        return None
    rd = ops[0].n
    imm = ops[-1].value if isinstance(ops[-1], Imm) else None
    src = ops[1].n if len(ops) == 3 and isinstance(ops[1], Reg) else rd
    if m == "mov" and len(ops) == 2 and ops[1] == Reg(PC):
        return insn.pc
    if m == "ldr" and (lit := literal(insn)) is not None and lit[1] == 4:
        word = const_word(lit[0], 4)
        return None if word is None else word | 0x1_0000_0000  # a constant, not yet PC-based
    if m in ("add", "orr") and imm is not None and src in known:
        v = known[src]
        return (v + imm if m == "add" else v | imm) & 0x1_FFFF_FFFF
    if m == "add" and len(ops) == 2 and ops[1] == Reg(PC) and rd in known:
        return (known[rd] + insn.pc) & 0xFFFFFFFF  # constant + pc: PC-based now
    return None


def find_hooks(fn: Function, const_word) -> tuple[dict[int, Hook], list[str]]:
    """The resume hooks of ``fn`` by store site, and a problem per entry without one.
    ``const_word(addr, size)`` reads immutable code bytes (the literal pools)."""
    if not fn.entries:
        return {}, []
    known: dict[int, int] = {}  # reg -> value; bit 32 set: a constant not yet based on pc
    hooks: dict[int, Hook] = {}
    targets = {e | 1 for e in fn.entries} if fn.thumb else set(fn.entries)
    for insn in fn.ordered():
        a = insn.address
        if a in fn.labels or a in fn.entries:
            known.clear()
        ops = insn.operands
        if (
            insn.mnemonic == "str"
            and insn.cond == AL
            and len(ops) == 2
            and isinstance(ops[0], Reg)
            and isinstance(ops[1], Mem)
        ):
            mem = ops[1]
            v = known.get(ops[0].n)
            if (
                v is not None
                and v in targets
                and mem.base != PC
                and mem.index is None
                and mem.pre
                and not mem.wback
            ):
                resume = v & ~1
                if all(h.resume != resume for h in hooks.values()):
                    hooks[a] = Hook(a, resume, mem.base, mem.disp - 4)
        value = _pc_value(insn, known, const_word)
        w = written_regs(insn)
        for r in list(known) if w is None else w:
            known.pop(r, None)
        if value is not None:
            known[ops[0].n] = value
    found = {h.resume for h in hooks.values()}
    problems = [
        f"{e:#x}: sjlj resume point without a jump-buffer store" for e in sorted(fn.entries)
        if e not in found
    ]  # fmt: skip
    return hooks, problems


__all__ = ["Hook", "find_hooks"]
