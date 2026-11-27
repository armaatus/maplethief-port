"""SquashFS 4.0 images and type-2 AppImages, read-only (docs/formats/squashfs.md, ADR 0193).

An AppImage is an ELF program, the *runtime*, with a SquashFS image appended to it: the runtime
mounts the image (or extracts it) and starts its ``AppRun``. The release guard (mtport.release)
splits the Linux player AppImage into the runtime and the image's files with
:func:`appimage_members`, so that every rule sees each file. This module reads only what
``mksquashfs`` writes for us (zlib, the ``-comp gzip`` default) and refuses other compressors.
It follows the kernel's documentation of the format
(https://dr-emann.github.io/squashfs/ and linux/fs/squashfs/squashfs_fs.h). It never runs
anything: an image is untrusted data here.

CLI::

    python3 -m mtport.squashfs list IMAGE     every entry: kind, size and path
"""

from __future__ import annotations

import argparse
import stat
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"hsqs"
SUPERBLOCK = struct.Struct("<4sIIIIHHHHHHQQQQQQQQ")
COMPRESSION_ZLIB = 1
COMPRESSORS = {1: "zlib", 2: "lzma", 3: "lzo", 4: "xz", 5: "lz4", 6: "zstd"}
META_SIZE = 8192
META_UNCOMPRESSED = 0x8000
DATA_UNCOMPRESSED = 1 << 24
NO_FRAGMENT = 0xFFFFFFFF
NO_TABLE = 0xFFFFFFFFFFFFFFFF
# Inode types (basic and extended).
DIR, FILE, SYMLINK = 1, 2, 3
EXT_DIR, EXT_FILE, EXT_SYMLINK = 8, 9, 10
KINDS = {DIR: "dir", EXT_DIR: "dir", FILE: "file", EXT_FILE: "file"}
KINDS.update({SYMLINK: "symlink", EXT_SYMLINK: "symlink"})
MAX_ENTRIES = 1 << 20  # more entries than any of our images would have: a loop or a bomb
MAX_FILE = 2 << 30  # GitHub's limit per release asset

# AppImage type 2: "AI" and 2 at offset 8 of the runtime's ELF identification.
APPIMAGE_MAGIC = b"AI\x02"
ELF_MAGIC = b"\x7fELF"


class SquashfsError(ValueError):
    """The bytes are not a SquashFS image (or an AppImage) this reader understands."""


@dataclass(frozen=True)
class Entry:
    """One entry of the image: its path (no leading slash), kind, mode and a file's bytes."""

    path: str
    kind: str  # "dir", "file", "symlink" or "other"
    mode: int
    data: bytes = b""
    target: str = ""  # a symlink's target


@dataclass(frozen=True)
class Superblock:
    inode_count: int
    mtime: int
    block_size: int
    fragment_count: int
    compression: int
    flags: int
    id_count: int
    root_inode: int
    bytes_used: int
    id_table: int
    xattr_table: int
    inode_table: int
    directory_table: int
    fragment_table: int
    export_table: int


def superblock(data: bytes) -> Superblock:
    if len(data) < SUPERBLOCK.size:
        raise SquashfsError("shorter than a SquashFS superblock")
    f = SUPERBLOCK.unpack_from(data)
    magic, inodes, mtime, block_size, frags, comp, block_log, flags, ids, major, minor = f[:11]
    if magic != MAGIC:
        raise SquashfsError("no SquashFS magic 'hsqs'")
    if (major, minor) != (4, 0):
        raise SquashfsError(f"SquashFS {major}.{minor}, not 4.0")
    if block_size != 1 << block_log or not 4096 <= block_size <= 1 << 20:
        raise SquashfsError(f"block size {block_size} does not match its log {block_log}")
    if comp != COMPRESSION_ZLIB:
        name = COMPRESSORS.get(comp, f"#{comp}")
        raise SquashfsError(f"{name} compression; this reader reads zlib (mksquashfs -comp gzip)")
    sb = Superblock(inodes, mtime, block_size, frags, comp, flags, ids, *f[11:])
    if sb.bytes_used > len(data):
        raise SquashfsError(f"image of {sb.bytes_used} bytes, only {len(data)} present")
    return sb


class _Image:
    def __init__(self, data: bytes):
        self.data = data
        self.sb = superblock(data)
        self.blocks: dict[int, tuple[bytes, int]] = {}  # metadata block: (bytes, next block)

    def _meta_block(self, pos: int) -> tuple[bytes, int]:
        cached = self.blocks.get(pos)
        if cached is not None:
            return cached
        if pos + 2 > self.sb.bytes_used:
            raise SquashfsError(f"metadata block at {pos} past the image's end")
        (header,) = struct.unpack_from("<H", self.data, pos)
        size = header & 0x7FFF
        raw = self.data[pos + 2 : pos + 2 + size]
        if size == 0 or len(raw) != size:
            raise SquashfsError(f"metadata block at {pos} is cut short")
        block = raw if header & META_UNCOMPRESSED else self._inflate(raw, META_SIZE, pos)
        out = (block, pos + 2 + size)
        self.blocks[pos] = out
        return out

    def _inflate(self, raw: bytes, limit: int, pos: int) -> bytes:
        d = zlib.decompressobj()
        try:
            out = d.decompress(raw, limit)
        except zlib.error as e:
            raise SquashfsError(f"block at {pos}: {e}") from e
        if d.unconsumed_tail or not d.eof:
            raise SquashfsError(f"block at {pos} inflates past {limit} bytes or is cut short")
        return out

    def meta(self, start: int, block: int, offset: int, size: int) -> tuple[bytes, int, int]:
        """``size`` bytes of the metadata stream at ``start + block``, ``offset``; and where
        the stream continues (block, offset)."""
        out = bytearray()
        pos = start + block
        while True:
            data, nxt = self._meta_block(pos)
            if offset > len(data):
                raise SquashfsError(f"offset {offset} past metadata block at {pos}")
            take = data[offset : offset + size - len(out)]
            out += take
            offset += len(take)
            if len(out) == size:
                if offset == len(data):  # the stream continues at the next block
                    return bytes(out), nxt - start, 0
                return bytes(out), pos - start, offset
            pos, offset = nxt, 0

    def data_block(self, pos: int, word: int, size: int) -> bytes:
        length = word & ~DATA_UNCOMPRESSED & 0xFFFFFFFF
        if length == 0:
            return bytes(size)  # a sparse block
        raw = self.data[pos : pos + length]
        if len(raw) != length or pos + length > self.sb.bytes_used:
            raise SquashfsError(f"data block at {pos} past the image's end")
        if word & DATA_UNCOMPRESSED:
            return raw
        return self._inflate(raw, self.sb.block_size, pos)

    def fragment(self, index: int) -> bytes:
        sb = self.sb
        if index >= sb.fragment_count:
            raise SquashfsError(f"fragment {index} of {sb.fragment_count}")
        table_block = index // 512
        ptr_pos = sb.fragment_table + 8 * table_block
        if ptr_pos + 8 > sb.bytes_used:
            raise SquashfsError("fragment table past the image's end")
        (ptr,) = struct.unpack_from("<Q", self.data, ptr_pos)
        raw, _, _ = self.meta(ptr, 0, (index % 512) * 16, 16)
        start, word, _unused = struct.unpack("<QII", raw)
        return self.data_block(start, word, sb.block_size)

    def inode(self, ref: int) -> tuple[int, int, bytes, str]:
        """(type, mode, file bytes or dir listing ref packed, symlink target) of inode ``ref``."""
        sb = self.sb
        block, offset = ref >> 16, ref & 0xFFFF
        head, block, offset = self.meta(sb.inode_table, block, offset, 16)
        itype, perms = struct.unpack_from("<HH", head)
        rd = lambda n: self.meta(sb.inode_table, block, offset, n)  # noqa: E731
        if itype == DIR:
            body, _, _ = rd(16)
            start, _links, size, off, _parent = struct.unpack("<IIHHI", body)
            return itype, stat.S_IFDIR | perms, struct.pack("<IIQ", start, off, size), ""
        if itype == EXT_DIR:
            body, _, _ = rd(24)
            _links, size, start, _parent, _icount, off, _x = struct.unpack("<IIIIHHI", body)
            return itype, stat.S_IFDIR | perms, struct.pack("<IIQ", start, off, size), ""
        if itype in (SYMLINK, EXT_SYMLINK):
            body, block2, offset2 = rd(8)
            _links, size = struct.unpack("<II", body)
            if size > 4096:
                raise SquashfsError(f"symlink target of {size} bytes")
            target, _, _ = self.meta(sb.inode_table, block2, offset2, size)
            return itype, stat.S_IFLNK | perms, b"", target.decode("utf-8", "replace")
        if itype in (FILE, EXT_FILE):
            if itype == FILE:
                body, block2, offset2 = rd(16)
                start, frag, frag_off, size = struct.unpack("<IIII", body)
            else:
                body, block2, offset2 = rd(40)
                start, size, _sparse, _links, frag, frag_off, _x = struct.unpack("<QQQIIII", body)
            if size > MAX_FILE:
                raise SquashfsError(f"file of {size} bytes")
            bs = sb.block_size
            count = size // bs if frag != NO_FRAGMENT else -(-size // bs)
            words_raw, _, _ = self.meta(sb.inode_table, block2, offset2, 4 * count)
            words = struct.unpack(f"<{count}I", words_raw)
            out = bytearray()
            pos = start
            for word in words:
                out += self.data_block(pos, word, bs)
                pos += word & ~DATA_UNCOMPRESSED & 0xFFFFFFFF
            if frag != NO_FRAGMENT:
                tail = size - len(out)
                piece = self.fragment(frag)[frag_off : frag_off + tail]
                if len(piece) != tail:
                    raise SquashfsError(f"fragment {frag} too short for a file's tail")
                out += piece
            if len(out) < size:
                raise SquashfsError(f"file of {size} bytes has only {len(out)}")
            return itype, stat.S_IFREG | perms, bytes(out[:size]), ""
        return itype, perms, b"", ""

    def listing(self, packed: bytes) -> list[tuple[str, int]]:
        """(name, inode ref) of a directory listing packed by :meth:`inode`."""
        start, off, size = struct.unpack("<IIQ", packed)
        if size < 3:
            raise SquashfsError("directory size below 3")
        left = size - 3
        if left == 0:
            return []
        raw, _, _ = self.meta(self.sb.directory_table, start, off, left)
        out = []
        pos = 0
        while pos < len(raw):
            if pos + 12 > len(raw):
                raise SquashfsError("directory header cut short")
            count, iblock, _inum = struct.unpack_from("<III", raw, pos)
            pos += 12
            for _ in range(count + 1):
                if pos + 8 > len(raw):
                    raise SquashfsError("directory entry cut short")
                ioff, _delta, _itype, nsize = struct.unpack_from("<HhHH", raw, pos)
                pos += 8
                name = raw[pos : pos + nsize + 1]
                pos += nsize + 1
                if len(name) != nsize + 1:
                    raise SquashfsError("directory entry's name cut short")
                text = name.decode("utf-8", "replace")
                if text in (".", "..") or "/" in text or "\0" in text:
                    raise SquashfsError(f"unsafe name {text!r} in a directory")
                out.append((text, (iblock << 16) | ioff))
        return out


def entries(data: bytes) -> list[Entry]:
    """Every entry of the SquashFS image ``data``, depth first in the image's (sorted) order."""
    img = _Image(data)
    out: list[Entry] = []
    seen: set[int] = set()

    def walk(ref: int, prefix: str) -> None:
        if ref in seen:
            raise SquashfsError(f"directory {prefix or '/'} loops")
        seen.add(ref)
        itype, _mode, packed, _ = img.inode(ref)
        if itype not in (DIR, EXT_DIR):
            raise SquashfsError(f"{prefix or '/'} is not a directory")
        for name, child in img.listing(packed):
            if len(out) >= MAX_ENTRIES:
                raise SquashfsError(f"more than {MAX_ENTRIES} entries")
            path = f"{prefix}{name}"
            ctype, mode, body, target = img.inode(child)
            kind = KINDS.get(ctype, "other")
            if kind == "dir":
                out.append(Entry(path, "dir", mode))
                walk(child, path + "/")
            else:
                out.append(Entry(path, kind, mode, body, target))

    walk(img.sb.root_inode, "")
    return out


def appimage_offset(data: bytes) -> int:
    """Where the SquashFS image starts in the type-2 AppImage ``data``: the end of its runtime,
    as the runtime itself computes it (the ELF section header table's end)."""
    if data[:4] != ELF_MAGIC or data[8:11] != APPIMAGE_MAGIC:
        raise SquashfsError("not a type-2 AppImage (no ELF with 'AI' 2 at offset 8)")
    if data[4] == 2:  # ELFCLASS64
        if data[5] != 1:
            raise SquashfsError("big-endian ELF")
        (shoff,) = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    elif data[4] == 1:
        (shoff,) = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    else:
        raise SquashfsError("ELF of an unknown class")
    end = shoff + shentsize * shnum
    if data[end : end + 4] != MAGIC:
        raise SquashfsError(f"no SquashFS image at the runtime's end ({end})")
    return end


def appimage_parts(data: bytes) -> tuple[bytes, list[Entry]]:
    """The runtime's bytes and the entries of the image of the type-2 AppImage ``data``."""
    offset = appimage_offset(data)
    image = data[offset:]
    sb = superblock(image)
    trailing = image[sb.bytes_used :]
    # mksquashfs pads to 4 KiB with zeros; anything else after the image would escape the guard.
    if trailing.strip(b"\0"):
        raise SquashfsError(f"{len(trailing)} bytes after the SquashFS image are not padding")
    return data[:offset], entries(image)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="python3 -m mtport.squashfs", description=__doc__.split("\n")[0]
    )
    sub = p.add_subparsers(dest="cmd", required=True)
    ls = sub.add_parser("list", help="list an image's (or an AppImage's) entries")
    ls.add_argument("image", type=Path)
    args = p.parse_args(argv)
    data = args.image.read_bytes()
    try:
        if data[:4] == ELF_MAGIC:
            runtime, items = appimage_parts(data)
            print(f"runtime: {len(runtime)} bytes")
        else:
            items = entries(data)
    except SquashfsError as e:
        print(f"{args.image}: {e}", file=sys.stderr)
        return 1
    for e in items:
        extra = f" -> {e.target}" if e.kind == "symlink" else ""
        print(f"{e.kind:7} {oct(e.mode & 0o7777)[2:]:>4} {len(e.data):>10} {e.path}{extra}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
