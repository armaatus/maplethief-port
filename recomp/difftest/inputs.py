"""The guest window of the differential test and its seeded random inputs (#41).

Both sides (Unicorn and the C driver) map one window of :data:`WINDOW` bytes at guest
address 0, with nothing outside it::

    0x000000  page zero (zeros, mapped: a null pointer reads zeros on both sides)
    0x001000  __TEXT, __DATA at their link-time addresses (from recomp.loader), to 0x11B000
    HEAP      0x120000, HEAP_SIZE bytes of random words, half of them heap pointers
    SP0       0x134000: sp at entry (64 KiB of stack below it); STACK_ARGS bytes above it
              are random (stack arguments)
    RET       0x13F000: lr at entry (Thumb bit set); a return to it ends the run
    WINDOW    0x140000: an access at or past it faults on both sides

One :class:`Case` is the start state of one run. Everything is derived from a string seed
``"<seed>:<function pointer>:<index>"`` through :class:`random.Random` (a string seed is hashed
with SHA-512, so it gives the same numbers on every run), and a reported seed reproduces its
input exactly. Heap contents come from a
pool of :data:`POOL` images per function (seed ``"<seed>:<pointer>:pool"``), plus a few
random words per case, so the case files stay small.

Register values mix small and negative numbers, boundary values, random words and pointers
into the heap and ``__DATA``; a register the function uses as a memory base
(:func:`recomp.difftest.select.pointer_regs`) gets a heap pointer most of the time. NZCV
are random (the flags variant starts from them). For functions that touch VFP registers,
d0-d31 hold random doubles, pairs of singles, small integers and raw bits.
"""

from __future__ import annotations

import random
import struct
from dataclasses import dataclass, field

M32 = 0xFFFFFFFF
WINDOW = 0x140000
HEAP = 0x120000
HEAP_SIZE = 0x4000
SP0 = 0x134000
STACK_ARGS = 0x100
RET = 0x13F000
POOL = 32
PATCH_WORDS = 8  # random heap words per case on top of its pool image
FPSCR_IOS = 0x03000000  # DN | FZ, round to nearest: how iOS starts every thread (ADR 0022)

_SPECIAL = (0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000,
            0x80000001, 0xFFFFFFFE, M32)  # fmt: skip


@dataclass
class Case:
    """One start state: ``regs`` r0-r15 (r15 unused), NZCV in ``cpsr``, ``fpscr``,
    ``d`` d0-d31, the heap image ``pool`` and ``patches`` (guest address -> bytes)."""

    seed: str
    regs: list[int]
    cpsr: int
    fpscr: int
    d: list[int]
    pool: int
    patches: list[tuple[int, bytes]] = field(default_factory=list)


def case_seed(seed: int, ptr: int, index: int) -> str:
    return f"{seed}:0x{ptr:X}:{index}"


def pristine(image) -> bytes:
    """The window before any case: page zero, ``__TEXT`` and ``__DATA``, the rest zero."""
    buf = bytearray(WINDOW)
    for name in ("__TEXT", "__DATA"):
        addr, data = image.segment_bytes(name)
        if addr + len(data) > HEAP:
            raise ValueError(f"{name} reaches past the heap at {HEAP:#x}")
        buf[addr : addr + len(data)] = data
    return bytes(buf)


def text_range(image) -> tuple[int, int]:
    """``[start, end)`` of ``__TEXT``: read-only in the emulator (ADR 0017)."""
    seg = image.macho.segment("__TEXT")
    return seg.vmaddr, seg.vmaddr + seg.vmsize


def data_range(image) -> tuple[int, int]:
    seg = image.macho.segment("__DATA")
    return seg.vmaddr, seg.vmaddr + seg.vmsize


def heap_pointer(rng: random.Random) -> int:
    """An 8-byte aligned address in the heap, at least 0x800 bytes from its end."""
    return HEAP + 8 * rng.randrange((HEAP_SIZE - 0x800) // 8)


def value(rng: random.Random, data: tuple[int, int]) -> int:
    """A random register or memory word."""
    x = rng.random()
    if x < 0.25:
        return rng.randrange(64)
    if x < 0.35:
        return -rng.randrange(1, 64) & M32
    if x < 0.47:
        return rng.choice(_SPECIAL)
    if x < 0.62:
        return heap_pointer(rng)
    if x < 0.67:
        return rng.randrange(data[0], data[1]) & ~3
    return rng.getrandbits(32)


def heap_pool(seed: int, ptr: int) -> list[bytes]:
    """:data:`POOL` heap images of random words: half of them heap pointers (so pointer
    chains stay inside the window), an eighth small numbers (indices, counts, flags), the
    rest random bits."""
    rng = random.Random(f"{seed}:0x{ptr:X}:pool")
    words = HEAP_SIZE // 4
    slots = (HEAP_SIZE - 0x800) // 8
    pool = []
    for _ in range(POOL):
        kind = rng.randbytes(words)
        pick = struct.unpack(f"<{words}H", rng.randbytes(2 * words))
        out = list(struct.unpack(f"<{words}I", rng.randbytes(HEAP_SIZE)))
        for i in range(words):
            if kind[i] < 128:
                out[i] = HEAP + 8 * (pick[i] % slots)
            elif kind[i] < 160:
                out[i] = pick[i] & 15
        pool.append(struct.pack(f"<{words}I", *out))
    return pool


def _double(rng: random.Random) -> int:
    x = rng.random()
    if x < 0.3:
        return struct.unpack("<Q", struct.pack("<d", rng.uniform(-1000.0, 1000.0)))[0]
    if x < 0.6:
        a, b = (rng.uniform(-1000.0, 1000.0) for _ in range(2))
        return struct.unpack("<Q", struct.pack("<ff", a, b))[0]
    if x < 0.75:
        a, b = (float(rng.randrange(-8, 9)) for _ in range(2))
        return struct.unpack("<Q", struct.pack("<ff", a, b))[0]
    if x < 0.85:
        return struct.unpack("<Q", struct.pack("<d", float(rng.randrange(-8, 9))))[0]
    return rng.getrandbits(64)


def make_case(
    seed: int,
    ptr: int,
    index: int,
    pointer_regs: frozenset[int],
    vfp: bool,
    data: tuple[int, int],
) -> Case:
    """The start state of run ``index`` of the function ``ptr``."""
    s = case_seed(seed, ptr, index)
    rng = random.Random(s)
    regs = []
    for n in range(13):
        if n in pointer_regs and rng.random() < 0.85:
            regs.append(heap_pointer(rng))
        else:
            regs.append(value(rng, data))
    regs += [SP0, RET | 1, 0]
    cpsr = rng.getrandbits(4) << 28
    d = [_double(rng) if vfp else rng.getrandbits(64) for _ in range(32)]
    case = Case(s, regs, cpsr, FPSCR_IOS, d, rng.randrange(POOL))
    args = b"".join(struct.pack("<I", value(rng, data)) for _ in range(STACK_ARGS // 4))
    case.patches.append((SP0, args))
    for _ in range(PATCH_WORDS):
        addr = HEAP + 4 * rng.randrange(HEAP_SIZE // 4)
        case.patches.append((addr, struct.pack("<I", value(rng, data))))
    return case


__all__ = [
    "HEAP", "HEAP_SIZE", "M32", "POOL", "RET", "SP0", "STACK_ARGS", "WINDOW", "Case",
    "case_seed", "data_range", "heap_pointer", "heap_pool", "make_case", "pristine", "text_range",
    "value",
]  # fmt: skip
