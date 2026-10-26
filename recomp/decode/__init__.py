"""Thumb-2 / VFP decoder: bytes -> :class:`Insn` (ADR 0014).

Our own decoder, written from the ARM ARM's encoding tables, so the recompiler needs only the
standard library; Capstone decodes the same bytes in the tests as an independent oracle
(ADR 0003). IT state is explicit: the caller passes the condition an enclosing IT block gives
the instruction (``cond``), which discovery tracks along the fall-through path, so an
instruction decodes the same however it is reached.

Modules: ``insn`` (the model and Capstone-style text), ``bits`` (ThumbExpandImm and friends),
``thumb16`` and ``thumb32`` (integer encodings, #33), ``vfp`` (VFP and NEON, #34); ``arm``
(A32, for the entry stub) comes with #33.
"""

from __future__ import annotations

import struct

from recomp.decode.insn import AL, Insn, undefined
from recomp.decode.thumb16 import decode16
from recomp.decode.thumb32 import decode32


def is_wide(hw1: int) -> bool:
    """Whether the Thumb halfword ``hw1`` starts a 32-bit instruction."""
    return hw1 >> 11 in (0b11101, 0b11110, 0b11111)


def decode_thumb(code: bytes, offset: int, address: int, cond: int | None = None) -> Insn:
    """Decode the Thumb instruction at ``code[offset:]`` (guest address ``address``).
    ``cond``: the IT condition of this instruction, None outside an IT block."""
    if offset + 2 > len(code):
        return undefined(address, 2, 0)
    (hw1,) = struct.unpack_from("<H", code, offset)
    if not is_wide(hw1):
        return decode16(hw1, address, cond)
    if offset + 4 > len(code):
        return undefined(address, 2, hw1)
    (hw2,) = struct.unpack_from("<H", code, offset + 2)
    return decode32(hw1, hw2, address, cond)


def decode_linear(code: bytes, address: int, end: int | None = None) -> list[Insn]:
    """Decode ``code`` (at ``address``) front to back, applying IT blocks to the
    instructions they cover. For tests and listings; discovery decodes by control flow."""
    out: list[Insn] = []
    pos = 0
    limit = len(code) if end is None else end - address
    pending: list[int] = []
    while pos < limit:
        cond = pending.pop(0) if pending else None
        insn = decode_thumb(code, pos, address + pos, cond)
        out.append(insn)
        if insn.mnemonic.startswith("it") and insn.known and cond is None:
            pending = list(insn.it_conditions())
        pos += insn.size
    return out


__all__ = ["AL", "Insn", "decode_linear", "decode_thumb", "is_wide"]
