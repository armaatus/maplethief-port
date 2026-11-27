"""Import models for the differential test (#305, ADR 0017 addendum): the pure memory, string
and division imports, so that functions calling them can be compared too.

A model is the same small function on both sides: here in Python for the Unicorn run (the
emulator stops at the import's stub and calls it) and in ``models.c`` for the C run (the
driver's import table). Both follow one byte order, so a fault lands on the same address:
the guest window is ``[0, WINDOW)``, an access outside it faults, and every model reads and
writes one byte at a time in the order written below (``memcpy`` reads ``src[i]``, then writes
``dst[i]``, for ``i`` upward). A fault is reported at the address of the first bad byte with
the stub's address as its pc. A write into ``__TEXT`` is an ``error`` (Unicorn maps it
read-only and the case is dropped, as for the game's own stores). The models are not libc:
what is compared is the lifted code around the call, so all that matters is that both sides
compute the same thing. Results go in r0 (r0 and r1 for the 64-bit divisions); no other
register or flag changes.

Division by zero returns 0 (the remainder: the dividend), and ``INT_MIN / -1`` is
``INT_MIN``. ``memcmp`` and ``strcmp`` return the difference of the first differing bytes.

Standard library only (ADR 0014): the models run on a ``bytearray`` of the window, so they are
tested without Unicorn (``tests/test_recomp_difftest.py``).
"""

from __future__ import annotations

from collections.abc import Callable

from recomp.difftest.inputs import WINDOW

M32 = 0xFFFFFFFF
M64 = (1 << 64) - 1

# The order is the import id the lifted code passes (``MT_CALL_IMPORT(id)``) and the index
# of ``mt_import_table`` in models.c; a test checks that both list the same names.
MODELS: tuple[str, ...] = (
    "___divsi3", "___modsi3", "___udivsi3", "___umodsi3", "___divdi3", "___udivdi3",
    "_memcmp", "_memcpy", "_memset", "_strcat", "_strcmp", "_strcpy", "_strlen",
)  # fmt: skip
MODEL_IDS: dict[str, int] = {name: i for i, name in enumerate(MODELS)}
UNMODELED = len(MODELS)  # the id of every other import: such a function is no leaf


class Fault(Exception):
    """An access outside the window, at ``addr``."""

    def __init__(self, addr: int) -> None:
        super().__init__(f"fault at 0x{addr:X}")
        self.addr = addr


class TextWrite(Exception):
    """A write into ``__TEXT`` (read-only in the emulator)."""


PAGE = 0x1000


class Memory:
    """The window as a ``bytearray``, with the byte-order rules of the module docstring.

    ``load(addr, n)``, when given, fills the window a page at a time on first use (the
    emulator's memory: a model touches a few bytes of the 1.25 MiB window); without it
    ``data`` is the whole window."""

    def __init__(
        self,
        data: bytearray,
        text: tuple[int, int],
        load: Callable[[int, int], bytes] | None = None,
    ) -> None:
        self.data, self.text, self.load = data, text, load
        self.loaded: set[int] = set()  # page numbers already filled by ``load``
        self.dirty: list[tuple[int, int]] = []  # written [start, end) ranges

    def _fill(self, lo: int, hi: int) -> None:
        if self.load is None or lo >= hi:
            return
        for page in range(lo // PAGE, (hi + PAGE - 1) // PAGE):
            if page not in self.loaded:
                self.data[page * PAGE : (page + 1) * PAGE] = self.load(page * PAGE, PAGE)
                self.loaded.add(page)

    def valid(self, addr: int) -> int:
        """How many bytes from ``addr`` upward are inside the window."""
        return WINDOW - addr if addr < WINDOW else 0

    def read(self, addr: int, n: int) -> bytes:
        """``n`` bytes from ``addr``; raises at the first byte outside the window."""
        ok = self.valid(addr)
        if ok < n:
            raise Fault(addr + ok)
        self._fill(addr, addr + n)
        return bytes(self.data[addr : addr + n])

    def write(self, addr: int, data: bytes) -> None:
        ok = self.valid(addr)
        good = data[:ok]
        self._fill(addr, addr + len(good))  # a partly written page keeps its other bytes
        lo, hi = self.text
        if good and addr < hi and addr + len(good) > lo:
            raise TextWrite(f"write into __TEXT at 0x{max(addr, lo):X}")
        self.data[addr : addr + len(good)] = good
        if good:
            self.dirty.append((addr, addr + len(good)))
        if len(good) < len(data):
            raise Fault(addr + ok)

    def string_length(self, addr: int) -> int:
        """The index of the first zero byte at ``addr``; raises where the window ends first."""
        end = addr + self.valid(addr)
        at = addr
        while at < end:
            stop = min(end, (at // PAGE + 1) * PAGE)
            self._fill(at, stop)
            i = self.data.find(0, at, stop)
            if i >= 0:
                return i - addr
            at = stop
        raise Fault(end if end > addr else addr)


def _s32(v: int) -> int:
    return v - (1 << 32) if v & 0x80000000 else v


def _s64(v: int) -> int:
    return v - (1 << 64) if v >> 63 else v


def _quot(a: int, b: int) -> int:
    """C division: truncated toward zero."""
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def _copy(mem: Memory, dst: int, src: int, n: int) -> None:
    """``dst[i] = src[i]`` for ``i`` upward, one byte at a time (overlap included)."""
    k = min(n, mem.valid(src), mem.valid(dst))
    if dst <= src or dst >= src + k:
        mem.write(dst, mem.read(src, k))  # no byte is read after it is written
    else:
        for i in range(k):
            mem.write(dst + i, mem.read(src + i, 1))
    if k < n:
        mem.read(src + k, 1)  # the read faults first, then the write
        mem.write(dst + k, b"\0")


def _divsi3(r: list[int], mem: Memory) -> None:
    a, b = _s32(r[0]), _s32(r[1])
    r[0] = _quot(a, b) & M32 if b else 0


def _modsi3(r: list[int], mem: Memory) -> None:
    a, b = _s32(r[0]), _s32(r[1])
    r[0] = (a - _quot(a, b) * b) & M32 if b else r[0]


def _udivsi3(r: list[int], mem: Memory) -> None:
    r[0] = r[0] // r[1] if r[1] else 0


def _umodsi3(r: list[int], mem: Memory) -> None:
    r[0] = r[0] % r[1] if r[1] else r[0]


def _divdi3(r: list[int], mem: Memory) -> None:
    a, b = _s64(r[0] | r[1] << 32), _s64(r[2] | r[3] << 32)
    q = _quot(a, b) & M64 if b else 0
    r[0], r[1] = q & M32, q >> 32


def _udivdi3(r: list[int], mem: Memory) -> None:
    a, b = r[0] | r[1] << 32, r[2] | r[3] << 32
    q = a // b if b else 0
    r[0], r[1] = q & M32, q >> 32


def _memcmp(r: list[int], mem: Memory) -> None:
    a, b, n = r[0], r[1], r[2]
    k = min(n, mem.valid(a), mem.valid(b))
    x, y = mem.read(a, k), mem.read(b, k)
    for i in range(k):
        if x[i] != y[i]:
            r[0] = (x[i] - y[i]) & M32
            return
    if k < n:
        mem.read(a + k, 1)
        mem.read(b + k, 1)
    r[0] = 0


def _memcpy(r: list[int], mem: Memory) -> None:
    _copy(mem, r[0], r[1], r[2])


def _memset(r: list[int], mem: Memory) -> None:
    d, n = r[0], r[2]
    k = min(n, mem.valid(d))
    mem.write(d, bytes([r[1] & 0xFF]) * (k + (k < n)))


def _strcpy_at(mem: Memory, dst: int, src: int) -> None:
    if dst <= src or dst > src + mem.valid(src):
        # No byte of the source is overwritten before it is read.
        try:
            n = mem.string_length(src) + 1
        except Fault as f:
            n = f.addr - src  # copy what there is, then fault on the next read
            _copy(mem, dst, src, n)
            raise
        _copy(mem, dst, src, n)
        return
    i = 0
    while True:
        b = mem.read(src + i, 1)
        mem.write(dst + i, b)
        if b == b"\0":
            return
        i += 1


def _strcat(r: list[int], mem: Memory) -> None:
    d = r[0]
    _strcpy_at(mem, (d + mem.string_length(d)) & M32, r[1])


def _strcmp(r: list[int], mem: Memory) -> None:
    a, b = r[0], r[1]
    i = 0
    while True:
        x, y = mem.read(a + i, 1)[0], mem.read(b + i, 1)[0]
        if x != y or x == 0:
            r[0] = (x - y) & M32
            return
        i += 1


def _strcpy(r: list[int], mem: Memory) -> None:
    _strcpy_at(mem, r[0], r[1])


def _strlen(r: list[int], mem: Memory) -> None:
    r[0] = mem.string_length(r[0])


IMPLEMENTATIONS: dict[str, Callable[[list[int], Memory], None]] = {
    "___divsi3": _divsi3, "___modsi3": _modsi3, "___udivsi3": _udivsi3,
    "___umodsi3": _umodsi3, "___divdi3": _divdi3, "___udivdi3": _udivdi3,
    "_memcmp": _memcmp, "_memcpy": _memcpy, "_memset": _memset, "_strcat": _strcat,
    "_strcmp": _strcmp, "_strcpy": _strcpy, "_strlen": _strlen,
}  # fmt: skip


def call(name: str, regs: list[int], mem: Memory) -> None:
    """Run model ``name`` on r0-r3 in ``regs`` (changed in place) and ``mem``. Raises
    :class:`Fault` or :class:`TextWrite`."""
    IMPLEMENTATIONS[name](regs, mem)


__all__ = [
    "IMPLEMENTATIONS", "MODELS", "MODEL_IDS", "UNMODELED", "Fault", "Memory", "TextWrite",
    "call",
]  # fmt: skip
