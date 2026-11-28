"""Flatpak single-file bundles, read-only (docs/formats/flatpak-bundle.md, ADR 0193).

``flatpak build-bundle`` writes an OSTree *static delta from scratch* with its parts inline: a
GVariant superblock (mtport.gvariant) that carries the commit, Flatpak's keys (the ref, the
app's ``metadata``, the runtime's repository) and, under ``deltas/.../<n>`` keys, the delta
parts. Each part is a compression byte (``x``: xz, ``0``: none) and a payload: a table of
modes, a table of xattrs, the raw bytes and a list of operations that cut the objects out of
the raw bytes. The objects are OSTree's: the commit names the root *dirtree* (file names to
checksums) and *dirmeta*; files are *content* objects.

The release guard (mtport.release) lists a bundle's files with :func:`members` so that every
rule sees each file, like an NRO's or an AppImage's. Every object is checked against its
checksum (SHA-256 of a metadata object's bytes; of a content object's header and bytes), the
commit against the superblock's, and the tree is walked from the commit, so the files listed
are exactly the files the bundle installs. Operations that need another commit (deltas that
are not from scratch, bsdiff) are refused. Input is untrusted: parsed, never run.

The layout follows OSTree's sources (src/libostree/ostree-repo-static-delta-private.h,
-processing.c, ostree-core.h).

CLI::

    python3 -m mtport.flatpak_bundle info BUNDLE    the ref, the app's metadata, the commit
    python3 -m mtport.flatpak_bundle list BUNDLE    every entry: kind, size and path
"""

from __future__ import annotations

import argparse
import hashlib
import lzma
import stat
import sys
from dataclasses import dataclass, field
from pathlib import Path

from mtport import gvariant
from mtport.gvariant import GVariantError

COMMIT = "(a{sv}aya(say)sstayay)"
SUPERBLOCK = f"(a{{sv}}tayay{COMMIT}aya(uayttay)a(yaytt))"
PART_PAYLOAD = "(a(uuu)aa(ayay)ayay)"
DIRTREE = "(a(say)a(sayay))"
DIRMETA = "(uuua(ayay))"
FILE_HEADER_PREFIX = 16  # uid, gid, mode, rdev: four u32
# OSTree object types.
OBJ_FILE, OBJ_DIRTREE, OBJ_DIRMETA, OBJ_COMMIT = 1, 2, 3, 4
OBJ_NAMES = {OBJ_FILE: "file", OBJ_DIRTREE: "dirtree", OBJ_DIRMETA: "dirmeta", OBJ_COMMIT: "commit"}
DELTA_PART_PREFIX = "deltas/"
MAX_UNPACKED = 4 << 30  # a part inflating past this is a bomb
MAX_ENTRIES = 1 << 20
PART_SLACK = 1 << 16  # how much more than a part's stated size its payload may unpack to


class BundleError(ValueError):
    """The bytes are not a Flatpak bundle this reader understands."""


@dataclass(frozen=True)
class Entry:
    """One entry of the bundle's tree: its path (no leading slash), kind, mode and bytes."""

    path: str
    kind: str  # "dir", "file" or "symlink"
    mode: int
    data: bytes = b""
    target: str = ""


@dataclass
class Bundle:
    ref: str
    metadata: str  # the app's metadata file, as the superblock carries it
    runtime_repo: str
    commit: str  # its checksum
    subject: str
    entries: list[Entry] = field(default_factory=list)
    extra: dict[str, bytes] = field(default_factory=dict)  # other superblock keys with bytes


def _varint(ops: bytes, pos: int) -> tuple[int, int]:
    value = shift = 0
    while True:
        if pos >= len(ops):
            raise BundleError("operation stream ends inside a number")
        b = ops[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        if not b & 0x80:
            return value, pos
        shift += 7
        if shift > 63:
            raise BundleError("number in the operation stream too long")


def _be32(value: int) -> int:
    """A u32 OSTree stored big-endian in a little-endian GVariant."""
    return int.from_bytes(value.to_bytes(4, "little"), "big")


def file_header(uid: int, gid: int, mode: int, target: str) -> bytes:
    """The serialised ``(uuuusa(ayay))`` header of a content object without xattrs, as
    OSTree checksums it: a big-endian u32 size, four zero bytes, then the header."""
    body = uid.to_bytes(4, "big") + gid.to_bytes(4, "big") + mode.to_bytes(4, "big") + bytes(4)
    body += target.encode("utf-8") + b"\0"
    end = len(body)  # the string's end: the tuple's one framing offset
    for osize in (1, 2, 4, 8):
        if end + osize < 1 << (8 * osize):
            header = body + end.to_bytes(osize, "little")
            break
    return len(header).to_bytes(4, "big") + bytes(4) + header


def _payload_objects(objects: bytes) -> list[tuple[int, str]]:
    if len(objects) % 33:
        raise BundleError("a part's object list is not 33-byte records")
    return [(objects[i], objects[i + 1 : i + 33].hex()) for i in range(0, len(objects), 33)]


def _apply_part(
    payload: bytes, objects: list[tuple[int, str]], store: dict[str, tuple[int, object]]
) -> None:
    """Run one part's operations, storing each object it writes by checksum: metadata objects
    as bytes, content objects as (mode, bytes, symlink target)."""
    try:
        modes, xattrs, raw, ops = gvariant.decode(PART_PAYLOAD, payload)
    except GVariantError as e:
        raise BundleError(f"part payload: {e}") from e
    pos = 0
    index = 0
    current: dict | None = None

    def raw_slice(length: int, offset: int) -> bytes:
        if offset + length > len(raw):
            raise BundleError("an operation reads past the part's raw bytes")
        return raw[offset : offset + length]

    def finish(content: bytes, mode: int, target_xattrs: int) -> None:
        nonlocal index
        objtype, checksum = objects[index]
        if objtype == OBJ_FILE:
            if xattrs[target_xattrs]:
                raise BundleError(f"file {checksum} has xattrs; this reader expects none")
            uid, gid = current["uid"], current["gid"]  # type: ignore[index]
            target = ""
            data = content
            if stat.S_ISLNK(mode):
                target, data = content.decode("utf-8", "replace"), b""
            elif not stat.S_ISREG(mode):
                raise BundleError(f"file {checksum} is neither a regular file nor a link")
            got = hashlib.sha256(file_header(uid, gid, mode, target) + data).hexdigest()
            store[checksum] = (objtype, (mode, data, target))
        else:
            got = hashlib.sha256(content).hexdigest()
            store[checksum] = (objtype, content)
        if got != checksum:
            raise BundleError(
                f"{OBJ_NAMES.get(objtype, objtype)} object {checksum} has checksum {got}"
            )
        index += 1

    def open_content() -> dict:
        nonlocal pos
        mode_i, pos = _varint(ops, pos)
        xattr_i, pos = _varint(ops, pos)
        if mode_i >= len(modes) or xattr_i >= len(xattrs):
            raise BundleError("an operation names a mode or xattrs the part lacks")
        uid, gid, mode = (_be32(v) for v in modes[mode_i])
        return {"uid": uid, "gid": gid, "mode": mode, "xattrs": xattr_i, "data": bytearray()}

    while pos < len(ops):
        op = ops[pos]
        pos += 1
        if index >= len(objects) and op != ord("c"):
            raise BundleError("more operations than the part's objects")
        objtype = objects[index][0] if index < len(objects) else 0
        if op == ord("S"):  # open, splice and close
            if objtype == OBJ_FILE:
                current = open_content()
                size, pos = _varint(ops, pos)
                offset, pos = _varint(ops, pos)
                finish(raw_slice(size, offset), current["mode"], current["xattrs"])
            else:
                size, pos = _varint(ops, pos)
                offset, pos = _varint(ops, pos)
                finish(raw_slice(size, offset), 0, 0)
            current = None
        elif op == ord("o"):  # open a content object, written by 'w' until 'c'
            if objtype != OBJ_FILE:
                raise BundleError("'open' of a metadata object")
            current = open_content()
            current["size"], pos = _varint(ops, pos)
        elif op == ord("w"):
            if current is None:
                raise BundleError("'write' with no open object")
            length, pos = _varint(ops, pos)
            offset, pos = _varint(ops, pos)
            current["data"] += raw_slice(length, offset)
            if len(current["data"]) > current["size"]:
                raise BundleError("an object written past its size")
        elif op == ord("c"):
            if current is None:
                raise BundleError("'close' with no open object")
            if len(current["data"]) != current["size"]:
                raise BundleError("an object closed before its size was written")
            finish(bytes(current["data"]), current["mode"], current["xattrs"])
            current = None
        elif op in (ord("r"), ord("R"), ord("B")):
            raise BundleError(
                f"operation {chr(op)!r} needs another commit: not a bundle from scratch"
            )
        else:
            raise BundleError(f"unknown delta operation {op:#x}")
    if current is not None or index != len(objects):
        raise BundleError(f"a part wrote {index} of its {len(objects)} objects")


def read(data: bytes) -> Bundle:
    """The bundle in ``data``: its keys and every entry of its tree."""
    try:
        meta, _ts, from_csum, to_csum, commit, _deps, parts, fallback = gvariant.decode(
            SUPERBLOCK, data
        )
        commit_raw = gvariant.tuple_slices(SUPERBLOCK, data)[4]
    except GVariantError as e:
        raise BundleError(f"not a Flatpak bundle: {e}") from e
    keys = {k: v for k, v in meta}
    if "flatpak" not in keys or "ref" not in keys:
        raise BundleError("not a Flatpak bundle (no 'flatpak' or 'ref' key)")
    if from_csum:
        raise BundleError("a delta from another commit, not a bundle from scratch")
    if fallback:
        raise BundleError("fallback objects outside the delta")
    if keys.get("ostree.endianness") and keys["ostree.endianness"].value != ord("l"):
        raise BundleError("a big-endian bundle; this reader reads little-endian ones")
    to_hex = to_csum.hex()
    if hashlib.sha256(commit_raw).hexdigest() != to_hex:
        raise BundleError("the commit does not match the superblock's checksum")

    store: dict[str, tuple[int, object]] = {}
    unpacked = 0
    for i, (_version, _csum, _size, usize, objects) in enumerate(parts):
        unpacked += usize
        if unpacked > MAX_UNPACKED:
            raise BundleError(f"parts unpack to more than {MAX_UNPACKED} bytes")
        part = None
        for k, v in meta:
            if k.startswith(DELTA_PART_PREFIX) and k.rsplit("/", 1)[-1] == str(i):
                part = v.value
        if not isinstance(part, tuple) or len(part) != 2:
            raise BundleError(f"part {i} is not inline")
        compression, body = part
        if compression == ord("x"):
            try:
                d = lzma.LZMADecompressor()
                body = d.decompress(body, max_length=usize + PART_SLACK + 1)
            except lzma.LZMAError as e:
                raise BundleError(f"part {i}: {e}") from e
        elif compression != 0:
            raise BundleError(f"part {i}: compression {compression!r}")
        # OSTree's usize is the payload's size before GVariant framing: a little short of it.
        if not usize <= len(body) <= usize + PART_SLACK:
            raise BundleError(f"part {i} unpacks to {len(body)} bytes, it says {usize}")
        _apply_part(body, _payload_objects(objects), store)

    _cmeta, _parent, _related, subject, _body, _cts, root_tree, root_meta = commit
    bundle = Bundle(
        ref=str(keys["ref"].value),
        metadata=str(keys["metadata"].value) if "metadata" in keys else "",
        runtime_repo=str(keys["runtime-repo"].value) if "runtime-repo" in keys else "",
        commit=to_hex,
        subject=subject,
    )
    for k, v in meta:
        if isinstance(v.value, bytes) and not k.startswith(DELTA_PART_PREFIX):
            bundle.extra[k] = v.value

    def get(csum: str, objtype: int) -> object:
        found = store.get(csum)
        if found is None or found[0] != objtype:
            raise BundleError(f"{OBJ_NAMES[objtype]} object {csum} is not in the bundle")
        return found[1]

    def walk(tree: str, prefix: str, depth: int) -> None:
        if depth > 64:
            raise BundleError("the tree is nested too deep")
        try:
            files, dirs = gvariant.decode(DIRTREE, get(tree, OBJ_DIRTREE))  # type: ignore[arg-type]
        except GVariantError as e:
            raise BundleError(f"dirtree {tree}: {e}") from e
        for name, csum in files:
            _check_name(name)
            mode, body, target = get(csum.hex(), OBJ_FILE)  # type: ignore[misc]
            kind = "symlink" if stat.S_ISLNK(mode) else "file"
            bundle.entries.append(Entry(prefix + name, kind, mode, body, target))
            if len(bundle.entries) > MAX_ENTRIES:
                raise BundleError(f"more than {MAX_ENTRIES} entries")
        for name, sub_tree, sub_meta in dirs:
            _check_name(name)
            try:
                _uid, _gid, mode, _x = gvariant.decode(DIRMETA, get(sub_meta.hex(), OBJ_DIRMETA))  # type: ignore[arg-type]
            except GVariantError as e:
                raise BundleError(f"dirmeta {sub_meta.hex()}: {e}") from e
            bundle.entries.append(Entry(prefix + name, "dir", _be32(mode)))
            walk(sub_tree.hex(), prefix + name + "/", depth + 1)

    get(root_meta.hex(), OBJ_DIRMETA)
    walk(root_tree.hex(), "", 0)
    return bundle


def _check_name(name: str) -> None:
    if name in ("", ".", "..") or "/" in name or "\0" in name:
        raise BundleError(f"unsafe name {name!r} in the tree")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="python3 -m mtport.flatpak_bundle", description=__doc__.split("\n")[0]
    )
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("info", "list"):
        s = sub.add_parser(name)
        s.add_argument("bundle", type=Path)
    args = p.parse_args(argv)
    try:
        b = read(args.bundle.read_bytes())
    except BundleError as e:
        print(f"{args.bundle}: {e}", file=sys.stderr)
        return 1
    if args.cmd == "info":
        print(f"ref: {b.ref}\ncommit: {b.commit}\nsubject: {b.subject}")
        print(f"runtime-repo: {b.runtime_repo}\nentries: {len(b.entries)}\n\n{b.metadata}")
        return 0
    for e in b.entries:
        extra = f" -> {e.target}" if e.kind == "symlink" else ""
        print(f"{e.kind:7} {oct(e.mode & 0o7777)[2:]:>4} {len(e.data):>10} {e.path}{extra}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
