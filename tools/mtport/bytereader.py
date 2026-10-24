"""Bounds-checked little-endian reader for the game's data files.

The shared base for the format parsers (``.map``, ``.att``, ``.obj``, ``.mapprj``, ...). The
game reads its files as one byte buffer with a cursor; this class does the same, but every
read is checked against the end of the data, because the files are untrusted input. A short
read raises :class:`FormatError` naming the file, the offset and what was being read.

The game's own helper for 16-bit fields (``0x1BC61`` in the armv7 slice) reads
``buf[off] | buf[off + 1] << 8`` and sign-extends it, so :meth:`ByteReader.i16` is the
faithful one; :meth:`ByteReader.u16` exists for fields the game treats as unsigned.
"""

from __future__ import annotations

import struct


class FormatError(ValueError):
    """The data does not match the format being parsed."""


class ByteReader:
    """A cursor over ``data``. ``name`` labels error messages (usually the file name)."""

    def __init__(self, data: bytes, name: str = "<data>", pos: int = 0) -> None:
        self.data = bytes(data)
        self.name = name
        if not 0 <= pos <= len(self.data):
            raise FormatError(f"{name}: start offset {pos} outside {len(self.data)} bytes")
        self.pos = pos

    def __len__(self) -> int:
        return len(self.data)

    @property
    def remaining(self) -> int:
        return len(self.data) - self.pos

    @property
    def at_end(self) -> bool:
        return self.pos >= len(self.data)

    def error(self, message: str, pos: int | None = None) -> FormatError:
        """A :class:`FormatError` for ``message`` at ``pos`` (default: the cursor)."""
        return FormatError(f"{self.name}: offset 0x{self.pos if pos is None else pos:X}: {message}")

    def need(self, size: int, what: str) -> None:
        """Raise unless ``size`` more bytes are available."""
        if size < 0 or size > self.remaining:
            raise self.error(f"{what} needs {size} bytes, {self.remaining} left")

    def raw(self, size: int, what: str = "bytes") -> bytes:
        """The next ``size`` bytes, as they are."""
        self.need(size, what)
        out = self.data[self.pos : self.pos + size]
        self.pos += size
        return out

    def _unpack(self, fmt: str, size: int, what: str) -> int:
        self.need(size, what)
        (value,) = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += size
        return value

    def u8(self, what: str = "u8") -> int:
        return self._unpack("<B", 1, what)

    def i8(self, what: str = "i8") -> int:
        return self._unpack("<b", 1, what)

    def u16(self, what: str = "u16") -> int:
        return self._unpack("<H", 2, what)

    def i16(self, what: str = "i16") -> int:
        return self._unpack("<h", 2, what)

    def u32(self, what: str = "u32") -> int:
        return self._unpack("<I", 4, what)

    def i32(self, what: str = "i32") -> int:
        return self._unpack("<i", 4, what)

    def pstr(self, what: str = "string") -> bytes:
        """A Pascal string: one length byte, then that many bytes (returned undecoded)."""
        start = self.pos
        size = self.u8(f"{what} length")
        if size > self.remaining:
            raise self.error(f"{what} of {size} bytes runs past the end", start)
        return self.raw(size, what)

    def expect_end(self, what: str = "data") -> None:
        """Raise if any bytes are left after the last field of ``what``."""
        if self.remaining:
            raise self.error(f"{self.remaining} trailing bytes after the {what}")
