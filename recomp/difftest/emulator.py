"""The reference side of the differential test: the guest code run in Unicorn (ADR 0017).

The only module of the recompiler that imports Unicorn, a GPL-2.0 development dependency
(group ``difftest``): it runs the game's machine code inside Unicorn's emulator, never on
the host CPU, and nothing that ships imports it.

:class:`Emulator` maps the guest window (:mod:`recomp.difftest.inputs`) once, as a
Cortex-A15 (ARMv7-A with VFPv4-D32, the closest model Unicorn has to the game's Cortex-A8
targets) with the FPU on and FPSCR zero. ``__TEXT`` is read-only and executable, the rest
of the window readable and writable (and executable above ``__TEXT``, where the return
address is): the game never writes its code, so a case that would is dropped as an
``error`` (the C side, which does not check, never runs it).

:meth:`Emulator.run` loads one case, runs to the return address within :data:`BUDGET`
instructions, and returns an :class:`End`: how it stopped, r0-r15, NZCV, FPSCR, d0-d31 and
every byte of the window that differs from the case's start, then puts memory back. The run
stops as:

- ``return``: it reached ``RET``;
- ``fault``: a data access outside the window (address, and the PC of the access);
- ``budget``: still running after :data:`BUDGET` instructions (not a bounded run);
- ``error``: anything else Unicorn reports (an alignment fault, a write to ``__TEXT``, an
  undefined instruction, a jump outside the code), with its message.

**Two Unicorn 2.1.4 bugs shape this module** (found by this test, ADR 0017):

- A memory hook (``UC_HOOK_MEM_WRITE`` or ``UC_HOOK_MEM_READ``) on a load or store inside
  an IT block leaves a stale IT state behind: a later instruction outside the block is
  skipped as if its condition failed (case ``7:0xCAAF1:2135``: the ``mov r0, sb`` after a
  ``bge.w`` never ran). So nothing hooks memory accesses: the changed bytes come from
  comparing the writable part of the window with the case's start, 16 KiB, then 64 bytes at
  a time. Only the invalid-access hook remains (it fires only on a fault, to record the
  address).
- With the write hook installed, a run could also take another path after an earlier run
  on the same emulator (a stale translation block; case ``41:0xCAAF1:2603`` after a budget
  stop). The translation cache is flushed after every run (``ctl_flush_tb``), and a test
  checks a long-lived emulator against fresh ones.
"""

from __future__ import annotations

from dataclasses import dataclass

import unicorn as uc
from unicorn import arm_const as a

from recomp.difftest.inputs import HEAP, RET, WINDOW, Case

BUDGET = 50_000  # instructions per run
_BIG, _SMALL = 0x4000, 64  # window comparison granularity

_R = [getattr(a, f"UC_ARM_REG_R{n}") for n in range(13)] + [
    a.UC_ARM_REG_SP, a.UC_ARM_REG_LR, a.UC_ARM_REG_PC,
]  # fmt: skip
_D = [getattr(a, f"UC_ARM_REG_D{n}") for n in range(32)]


@dataclass
class End:
    status: str  # return | fault | budget | error
    regs: list[int]
    cpsr: int  # NZCV only
    fpscr: int
    d: list[int]
    mem: dict[int, int]  # guest address -> byte, for every byte that changed
    fault: tuple[int, int] | None = None  # (address, pc) of a fault
    error: str = ""


class Emulator:
    def __init__(self, pristine: bytes, text: tuple[int, int]) -> None:
        start, end = text
        if len(pristine) != WINDOW or not 0 < start < end <= WINDOW or (start | end) & 0xFFF:
            raise ValueError("pristine image or __TEXT does not fit the window")
        self.pristine = pristine
        self.ref = bytearray(pristine)  # the start state of the current case
        # The writable ranges, compared after each run in blocks of at most _BIG bytes.
        self.blocks = [
            (b, min(b + _BIG, hi))
            for lo, hi in ((0, start), (end, WINDOW))
            for b in range(lo, hi, _BIG)
        ]
        self.uc = uc.Uc(uc.UC_ARCH_ARM, uc.UC_MODE_THUMB)
        self.uc.ctl_set_cpu_model(a.UC_CPU_ARM_CORTEX_A15)
        self.uc.mem_map(0, start, uc.UC_PROT_READ | uc.UC_PROT_WRITE)
        self.uc.mem_map(start, end - start, uc.UC_PROT_READ | uc.UC_PROT_EXEC)
        self.uc.mem_map(end, WINDOW - end, uc.UC_PROT_ALL)
        self.uc.mem_write(0, pristine)
        cpacr = self.uc.reg_read(a.UC_ARM_REG_C1_C0_2)
        self.uc.reg_write(a.UC_ARM_REG_C1_C0_2, cpacr | (0xF << 20))  # cp10, cp11: full access
        self.uc.reg_write(a.UC_ARM_REG_FPEXC, 0x40000000)  # EN
        self.fault: tuple[int, int] | None = None
        self.uc.hook_add(uc.UC_HOOK_MEM_UNMAPPED, self._on_unmapped)

    def _on_unmapped(self, mu, access, address, _size, _value, _data) -> bool:
        if access in (uc.UC_MEM_READ_UNMAPPED, uc.UC_MEM_WRITE_UNMAPPED):
            self.fault = (address, mu.reg_read(a.UC_ARM_REG_PC))
        return False

    def run(self, ptr: int, case: Case, pool: list[bytes]) -> End:
        mu, ref = self.uc, self.ref
        heap = pool[case.pool]
        mu.mem_write(HEAP, heap)
        ref[HEAP : HEAP + len(heap)] = heap
        for addr, data in case.patches:
            mu.mem_write(addr, data)
            ref[addr : addr + len(data)] = data
        regs = list(zip(_R[:15], case.regs[:15], strict=True))
        regs += list(zip(_D, case.d, strict=True))
        mu.reg_write_batch(regs)
        cpsr = mu.reg_read(a.UC_ARM_REG_CPSR)
        # Keep the mode, T, A, I, F bits; clear Q, GE and an IT state left by a stopped run.
        mu.reg_write(a.UC_ARM_REG_CPSR, (cpsr & 0x1FF) | (case.cpsr & 0xF0000000))
        mu.reg_write(a.UC_ARM_REG_FPSCR, case.fpscr)
        self.fault = None
        status, error = "return", ""
        try:
            mu.emu_start(ptr, RET, count=BUDGET)
            if mu.reg_read(a.UC_ARM_REG_PC) != RET:
                status = "budget"
        except uc.UcError as exc:
            status = "fault" if self.fault is not None else "error"
            error = str(exc)
        values = mu.reg_read_batch(_R + _D)
        end = End(
            status,
            list(values[:16]),
            mu.reg_read(a.UC_ARM_REG_CPSR) & 0xF0000000,
            mu.reg_read(a.UC_ARM_REG_FPSCR),
            list(values[16:]),
            self._changes(),
            self.fault,
            error,
        )
        for addr, data in case.patches:
            ref[addr : addr + len(data)] = self.pristine[addr : addr + len(data)]
            mu.mem_write(addr, self.pristine[addr : addr + len(data)])
        mu.ctl_flush_tb()
        return end

    def _changes(self) -> dict[int, int]:
        """Bytes of the window that differ from the case's start; puts the start back."""
        mu, ref = self.uc, self.ref
        changed: dict[int, int] = {}
        for lo, hi in self.blocks:
            now, was = mu.mem_read(lo, hi - lo), ref[lo:hi]
            if now == was:
                continue
            for p in range(0, hi - lo, _SMALL):
                x, y = now[p : p + _SMALL], was[p : p + _SMALL]
                if x != y:
                    changed.update(
                        (lo + p + i, b) for i, (b, c) in enumerate(zip(x, y, strict=True)) if b != c
                    )
            mu.mem_write(lo, bytes(was))
        return changed


__all__ = ["BUDGET", "Emulator", "End"]
