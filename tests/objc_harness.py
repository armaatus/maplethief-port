"""Run the Objective-C runtime (``runtime/src/objc/``, ADR 0026) on a Mach-O image's metadata.

:func:`guest_memory` maps a thin armv7 image the way the runtime's start-up does: every
segment at its VM address, every bound symbol given a zeroed 32-byte object after the image
(as ``mt_imports_place`` does for data imports, enough for a 20-byte ``class_t``), and every
bind written (``mt_imports_apply_binds``). :func:`run` compiles ``tests/objc/driver.c`` with the
runtime's C++ sources (:mod:`recomp_harness`'s host compiler, no C++ runtime library), writes
that memory and a command file, and returns the driver's parsed output: the load statistics
and the result of each lookup. The synthetic test runs it on ``objcfixtures.IMAGE``, the real
asset test on the armv7 slice.
"""

from __future__ import annotations

import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from mtport import macho
from recomp_harness import DIAG_RUNTIME, INCLUDE, OBJC_RUNTIME, RUNTIME_SRC, host_cc
from repo_paths import REPO

DRIVER = REPO / "tests" / "objc" / "driver.c"
SOURCES = [
    DRIVER,
    *OBJC_RUNTIME,
    RUNTIME_SRC / "hle" / "imports.cpp",  # mt_import_config_default
    *DIAG_RUNTIME,  # the unimplemented report (#53)
]
CLASS_SYMBOL = "_OBJC_CLASS_$_"
METACLASS_SYMBOL = "_OBJC_METACLASS_$_"
OBJECT_SIZE = 32  # per placed import
HEAP = 0x100000  # free guest memory after the import objects, for the runtime's allocator


@dataclass(frozen=True)
class GuestImage:
    memory: bytes
    symbols: dict[str, int]  # bound symbol -> its placed object
    heap: int  # first free address

    def host_class_objects(self) -> dict[str, tuple[int, int]]:
        """``name -> (class object, metaclass object)`` of every imported class; 0 where the
        image binds only one of the two."""
        out: dict[str, tuple[int, int]] = {}
        for symbol, addr in self.symbols.items():
            for prefix, index in ((CLASS_SYMBOL, 0), (METACLASS_SYMBOL, 1)):
                if symbol.startswith(prefix):
                    pair = list(out.get(symbol[len(prefix) :], (0, 0)))
                    pair[index] = addr
                    out[symbol[len(prefix) :]] = (pair[0], pair[1])
        return out


def guest_memory(image: macho.MachO) -> GuestImage:
    end = max(s.vmaddr + s.vmsize for s in image.segments)
    end = (end + 0xFFF) & ~0xFFF
    symbols: dict[str, int] = {}
    binds = [b for b in image.binds if b.kind != "weak"]
    for b in binds:
        symbols.setdefault(b.symbol, end + OBJECT_SIZE * len(symbols))
    heap = end + OBJECT_SIZE * len(symbols)
    memory = bytearray(heap + HEAP)
    for s in image.segments:
        if s.filesize:
            memory[s.vmaddr : s.vmaddr + s.filesize] = image.data[
                s.fileoff : s.fileoff + s.filesize
            ]
    for b in binds:
        struct.pack_into("<I", memory, b.address, (symbols[b.symbol] + b.addend) & 0xFFFFFFFF)
    return GuestImage(bytes(memory), symbols, heap)


def pointer_section(image: macho.MachO, sectname: str) -> tuple[int, int]:
    """``(address, entries)`` of a ``__DATA`` pointer section, ``(0, 0)`` if absent."""
    s = image.section("__DATA", sectname)
    return (s.addr, s.size // 4) if s is not None else (0, 0)


@dataclass(frozen=True)
class HostClass:
    name: str
    superclass: str | None
    size: int = 0
    instance_methods: tuple[str, ...] = ()
    class_methods: tuple[str, ...] = ()


@dataclass
class Result:
    load: dict[str, float] = field(default_factory=dict)
    # (class object, selector) -> (origin, guest IMP, owner object), or None for a miss
    lookups: dict[tuple[int, str], tuple[str, int, int] | None] = field(default_factory=dict)
    # the same for named lookups: (class name, "+" or "-", selector)
    named: dict[tuple[str, str, str], tuple[str, int, int] | None] = field(default_factory=dict)
    logs: list[str] = field(default_factory=list)
    bench_ns: float | None = None


def run(
    tmp_path: Path,
    image: macho.MachO,
    hosts: list[HostClass],
    lookups: list[tuple[int, str]],
    overrides: list[tuple[str, bool, str]] = (),
    named: list[tuple[str, str, str]] = (),
    bench: tuple[int, str, int] | None = None,
) -> tuple[GuestImage, Result]:
    """Loads ``image``'s metadata with ``hosts`` (superclasses first; their class objects are
    the image's bound ones where it has them), installs ``overrides`` (class, class method?,
    selector) and answers ``lookups`` (class or metaclass object, selector) and ``named``
    lookups (class name, "+" or "-", selector)."""
    guest = guest_memory(image)
    objects = guest.host_class_objects()
    lines = [f"heap {guest.heap:X}"]
    for h in hosts:
        cls, meta = objects.get(h.name, (0, 0))
        lines.append(f"host {h.name} {h.superclass or '-'} {h.size} {cls:X} {meta:X}")
        lines += [f"imethod {sel}" for sel in h.instance_methods]
        lines += [f"cmethod {sel}" for sel in h.class_methods]
    classlist = pointer_section(image, "__objc_classlist")
    catlist = pointer_section(image, "__objc_catlist")
    selrefs = pointer_section(image, "__objc_selrefs")
    lines.append(
        "image {:X} {} {:X} {} {:X} {}".format(*classlist, *catlist, *selrefs)  # noqa: UP032
    )
    lines += [f"override {name} {'+' if meta else '-'} {sel}" for name, meta, sel in overrides]
    lines += [f"lookup {cls:X} {sel}" for cls, sel in lookups]
    lines += [f"lookupn {name} {kind} {sel}" for name, kind, sel in named]
    if bench is not None:
        lines.append("bench {:X} {} {}".format(*bench))  # noqa: UP032
    (tmp_path / "memory.bin").write_bytes(guest.memory)
    (tmp_path / "commands.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    exe = tmp_path / ("driver.exe" if sys.platform == "win32" else "driver")
    host_cc().build(SOURCES, exe, [INCLUDE, RUNTIME_SRC])
    status, out = run_driver(exe, tmp_path)
    assert status == 0, out[-3000:]
    return guest, parse(out)


def run_driver(exe: Path, tmp_path: Path) -> tuple[int, str]:
    args = [str(exe), str(tmp_path / "memory.bin"), str(tmp_path / "commands.txt")]
    done = subprocess.run(args, capture_output=True, text=True, timeout=120)
    return done.returncode, done.stdout + done.stderr


def parse(out: str) -> Result:
    result = Result()
    for line in out.splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == "load":
            result.load = {k: float(v) for k, v in (w.split("=") for w in words[1:])}
        elif words[0] == "lookup":
            key = (int(words[1], 16), words[2])
            result.lookups[key] = (
                None if words[3] == "miss" else (words[3], int(words[4], 16), int(words[5], 16))
            )
        elif words[0] == "lookupn":
            result.named[(words[1], words[2], words[3])] = (
                None if words[4] == "miss" else (words[4], int(words[5], 16), int(words[6], 16))
            )
        elif words[0] == "log":
            result.logs.append(line[4:])
        elif words[0] == "bench":
            result.bench_ns = float(words[1])
    return result


__all__ = ["GuestImage", "HostClass", "Result", "guest_memory", "run"]
